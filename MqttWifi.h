#pragma once

// WiFi STA + MQTT client task. Callback only queues PlayCommand — no SD I/O.
// Requires library: PubSubClient (by Nick O'Leary) from Library Manager.
//
// Connect strategy for late RPi AP boot / flaky assoc:
//   1) Wait until WIFI_SSID is visible (scan), then WiFi.begin(channel)
//   2) setTxPower(8.5 dBm) — required on many ESP32-S3 boards to avoid AUTH_EXPIRE
//   3) On AUTH_EXPIRE: long backoff (Pi SoftAP ignores fast re-auth / stale stations)
//   4) After WIFI_RADIO_RESET_AFTER_FAILS outer failures: soft radio reset
//   5) mqttWifiIsOnline() is true only after WiFi + MQTT are up (setup waits on this)

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <PubSubClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "app_config.h"
#include "MqttCommand.h"
#include "I2sPcmOutput.h"

static WiFiClient g_mqttWifiClient;
static PubSubClient g_mqttClient(g_mqttWifiClient);
static MqttCommandState *g_mqttCmdState = nullptr;
static uint8_t g_wifiFailStreak = 0;
static volatile bool g_mqttWifiOnline = false;
static volatile bool g_wifiSawDisconnect = false;
static volatile uint8_t g_wifiLastDisconnectReason = 0;

static bool payloadHasState(const char *buf, const char *value)
{
  // Accept {"state":"alert"} with optional spaces: "state" ... "alert"
  const char *stateKey = strstr(buf, "\"state\"");
  if (stateKey == nullptr)
  {
    return false;
  }
  const char *colon = strchr(stateKey, ':');
  if (colon == nullptr)
  {
    return false;
  }
  return strstr(colon, value) != nullptr;
}

static bool payloadParseVolume(const char *buf, int &outPercent)
{
  const char *key = strstr(buf, "\"volume\"");
  if (key == nullptr)
  {
    return false;
  }
  const char *colon = strchr(key, ':');
  if (colon == nullptr)
  {
    return false;
  }
  colon++;
  while (*colon == ' ' || *colon == '\t')
  {
    colon++;
  }
  char *end = nullptr;
  long value = strtol(colon, &end, 10);
  if (end == colon)
  {
    return false;
  }
  if (value < 0)
  {
    value = 0;
  }
  if (value > 100)
  {
    value = 100;
  }
  outPercent = static_cast<int>(value);
  return true;
}

static void mqttMessageCallback(char *topic, byte *payload, unsigned int length)
{
  if (topic == nullptr)
  {
    return;
  }

  char buf[160];
  unsigned int n = length < sizeof(buf) - 1 ? length : sizeof(buf) - 1;
  memcpy(buf, payload, n);
  buf[n] = '\0';

  if (strcmp(topic, MQTT_VOLUME_TOPIC) == 0)
  {
    int percent = 0;
    if (payloadParseVolume(buf, percent))
    {
      I2sPcmOutput::setVolumePercent(static_cast<uint8_t>(percent));
      Serial.printf("MQTT: volume %d%%\n", percent);
    }
    else
    {
      Serial.printf("MQTT ignored volume payload: %s\n", buf);
    }
    return;
  }

  if (g_mqttCmdState == nullptr || strcmp(topic, MQTT_TOPIC) != 0)
  {
    return;
  }

  if (payloadHasState(buf, "\"alert\""))
  {
    Serial.println("MQTT: alert");
    mqttCommandSend(*g_mqttCmdState, PlayCommand::Alert);
  }
  else if (payloadHasState(buf, "\"idle\""))
  {
    Serial.println("MQTT: idle");
    mqttCommandSend(*g_mqttCmdState, PlayCommand::Idle);
  }
  else
  {
    Serial.printf("MQTT ignored payload: %s\n", buf);
  }
}

static const char *wifiStatusText(wl_status_t status)
{
  switch (status)
  {
  case WL_IDLE_STATUS:
    return "IDLE";
  case WL_NO_SSID_AVAIL:
    return "NO_SSID_AVAIL";
  case WL_SCAN_COMPLETED:
    return "SCAN_COMPLETED";
  case WL_CONNECTED:
    return "CONNECTED";
  case WL_CONNECT_FAILED:
    return "CONNECT_FAILED";
  case WL_CONNECTION_LOST:
    return "CONNECTION_LOST";
  case WL_DISCONNECTED:
    return "DISCONNECTED";
  default:
    return "UNKNOWN";
  }
}

static const char *wifiDisconnectReasonText(uint8_t reason)
{
  switch (reason)
  {
  case 2:
    return "AUTH_EXPIRE";
  case 8:
    return "ASSOC_LEAVE (local disconnect)";
  case 15:
    return "4WAY_HANDSHAKE_TIMEOUT (wrong password or WPA mismatch)";
  case 201:
    return "NO_AP_FOUND";
  case 204:
    return "HANDSHAKE_TIMEOUT";
  case 205:
    return "CONNECTION_FAIL";
  default:
    return "see esp_wifi_types.h";
  }
}

static void wifiSetCountryCode(const char *cc)
{
  if (cc == nullptr || cc[0] == '\0' || cc[1] == '\0')
  {
    return;
  }

  wifi_country_t country = {};
  country.cc[0] = cc[0];
  country.cc[1] = cc[1];
  country.schan = 1;
  country.nchan = 13;
  country.policy = WIFI_COUNTRY_POLICY_AUTO;
  const esp_err_t err = esp_wifi_set_country(&country);
  if (err != ESP_OK)
  {
    Serial.printf("WiFi: set country failed (%d)\n", (int)err);
  }
}

// IMPORTANT: WiFi.disconnect(wifioff, eraseap) — first arg true turns the radio OFF.
// Clean reconnect must keep the radio on (wifioff=false), or MAC becomes 00:00:00:00:00:00
// and association fails with AUTH_EXPIRE.
static void wifiCleanDisconnect()
{
  WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
  vTaskDelay(pdMS_TO_TICKS(200));
}

static bool wifiMacIsValid()
{
  const String mac = WiFi.macAddress();
  return mac.length() >= 17 && mac != "00:00:00:00:00:00";
}

static bool wifiWaitForValidMac(uint32_t timeoutMs = 2000)
{
  const uint32_t start = millis();
  while (!wifiMacIsValid() && (millis() - start) < timeoutMs)
  {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return wifiMacIsValid();
}

static void wifiApplyTxPower()
{
  // Lower TX power avoids AUTH_EXPIRE on many ESP32-S3 modules (RF frontend).
  WiFi.setTxPower(WIFI_TX_POWER);
  Serial.printf("WiFi: TX power -> enum %d\n", (int)WIFI_TX_POWER);
}

static void wifiSoftRadioReset()
{
  Serial.println("WiFi: soft radio reset (OFF -> STA)");
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
  vTaskDelay(pdMS_TO_TICKS(100));
  WiFi.mode(WIFI_OFF);
  vTaskDelay(pdMS_TO_TICKS(300));
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
#if defined(WIFI_COUNTRY_CODE)
  wifiSetCountryCode(WIFI_COUNTRY_CODE);
#endif
  vTaskDelay(pdMS_TO_TICKS(150));
  wifiApplyTxPower();
  if (!wifiWaitForValidMac())
  {
    Serial.println("WiFi: MAC still invalid after soft radio reset");
  }
}

static void wifiEnsureStaMode()
{
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);

  if (WiFi.getMode() != WIFI_STA)
  {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
#if defined(WIFI_COUNTRY_CODE)
    wifiSetCountryCode(WIFI_COUNTRY_CODE);
#endif
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  else
  {
    WiFi.setSleep(false);
  }

  // Keep radio on; only drop any half-open association.
  wifiCleanDisconnect();
  wifiApplyTxPower();
  if (!wifiWaitForValidMac())
  {
    Serial.println("WiFi: MAC invalid after STA ensure — forcing soft radio reset");
    wifiSoftRadioReset();
  }
}

static void wifiEventHandler(WiFiEvent_t event, WiFiEventInfo_t info)
{
  switch (event)
  {
  case ARDUINO_EVENT_WIFI_STA_START:
    Serial.println("WiFi: STA started");
    break;
  case ARDUINO_EVENT_WIFI_STA_CONNECTED:
    Serial.println("WiFi: associated with AP (waiting for IP)");
    break;
  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    Serial.printf("WiFi: got IP %s gw=%s dns=%s\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(),
                  WiFi.dnsIP().toString().c_str());
    break;
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    g_wifiSawDisconnect = true;
    g_wifiLastDisconnectReason = info.wifi_sta_disconnected.reason;
    g_mqttWifiOnline = false;
    Serial.printf("WiFi: disconnected reason=%u (%s)\n",
                  info.wifi_sta_disconnected.reason,
                  wifiDisconnectReasonText(info.wifi_sta_disconnected.reason));
    break;
  default:
    break;
  }
}

struct WifiTargetAp
{
  int channel = 0;
  int rssi = 0;
  uint8_t bssid[6] = {};
  bool hasBssid = false;
};

// Returns true if WIFI_SSID is in the current scan. Fills channel/rssi/BSSID when found.
static bool wifiScanFindsTargetSsid(WifiTargetAp *outTarget)
{
  const int n = WiFi.scanNetworks(false, false);
  if (n < 0)
  {
    Serial.printf("WiFi: scan failed (%d)\n", n);
    return false;
  }

  bool found = false;
  for (int i = 0; i < n; i++)
  {
    if (WiFi.SSID(i) != WIFI_SSID)
    {
      continue;
    }

    found = true;
    if (outTarget != nullptr)
    {
      outTarget->channel = WiFi.channel(i);
      outTarget->rssi = WiFi.RSSI(i);
      outTarget->hasBssid = false;
      const uint8_t *bssid = WiFi.BSSID(i);
      if (bssid != nullptr)
      {
        memcpy(outTarget->bssid, bssid, 6);
        outTarget->hasBssid = true;
      }
    }
    break;
  }
  WiFi.scanDelete();
  return found;
}

static bool wifiWaitForTargetSsid(uint32_t waitMs, WifiTargetAp *outTarget)
{
  const uint32_t start = millis();
  uint32_t scanNum = 0;

  while ((millis() - start) < waitMs)
  {
    scanNum++;
    Serial.printf("WiFi: waiting for SSID \"%s\" (scan #%u, %u ms left)...\n",
                  WIFI_SSID,
                  (unsigned)scanNum,
                  (unsigned)(waitMs - (millis() - start)));

    WifiTargetAp target;
    if (wifiScanFindsTargetSsid(&target))
    {
      Serial.printf("WiFi: SSID \"%s\" visible ch=%d rssi=%d bssid=%02X:%02X:%02X:%02X:%02X:%02X\n",
                    WIFI_SSID,
                    target.channel,
                    target.rssi,
                    target.bssid[0], target.bssid[1], target.bssid[2],
                    target.bssid[3], target.bssid[4], target.bssid[5]);
      if (outTarget != nullptr)
      {
        *outTarget = target;
      }
      return true;
    }

    const uint32_t elapsed = millis() - start;
    if (elapsed >= waitMs)
    {
      break;
    }

    uint32_t sleepMs = WIFI_SSID_POLL_MS;
    const uint32_t remaining = waitMs - elapsed;
    if (sleepMs > remaining)
    {
      sleepMs = remaining;
    }
    vTaskDelay(pdMS_TO_TICKS(sleepMs));
  }

  return false;
}

static bool wifiEnsureConnected()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    g_wifiFailStreak = 0;
    return true;
  }

  static bool eventsRegistered = false;
  if (!eventsRegistered)
  {
    WiFi.onEvent(wifiEventHandler);
    eventsRegistered = true;
  }

  const bool doSoftReset =
      (g_wifiFailStreak > 0) &&
      ((g_wifiFailStreak % WIFI_RADIO_RESET_AFTER_FAILS) == 0);

  if (doSoftReset)
  {
    Serial.printf("WiFi: %u consecutive failures — soft radio reset\n",
                  (unsigned)g_wifiFailStreak);
    wifiSoftRadioReset();
  }
  else
  {
    wifiEnsureStaMode();
  }

  if (!wifiMacIsValid())
  {
    Serial.printf("WiFi: abort connect — invalid STA MAC=%s\n", WiFi.macAddress().c_str());
    g_wifiFailStreak++;
    return false;
  }

  Serial.printf("  STA MAC=%s failStreak=%u\n",
                WiFi.macAddress().c_str(),
                (unsigned)g_wifiFailStreak);

  WifiTargetAp target;
  if (!wifiWaitForTargetSsid(WIFI_SSID_WAIT_MS, &target))
  {
    Serial.printf("WiFi: SSID \"%s\" not visible after %u ms (is RPi AP up?)\n",
                  WIFI_SSID,
                  (unsigned)WIFI_SSID_WAIT_MS);
    g_wifiFailStreak++;
    wifiCleanDisconnect();
    return false;
  }

  // Settle after scan; stagger boards so they don't auth-storm the Pi AP.
  // Pi SoftAP often ignores rapid re-auth while a stale station entry exists.
  vTaskDelay(pdMS_TO_TICKS(500));
  {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    const uint32_t staggerMs = (uint32_t)(mac[5] % 5u) * (uint32_t)WIFI_STAGGER_SLOT_MS;
    if (staggerMs > 0)
    {
      Serial.printf("WiFi: stagger %u ms before assoc\n", (unsigned)staggerMs);
      vTaskDelay(pdMS_TO_TICKS(staggerMs));
    }
  }

  Serial.printf("WiFi connecting to \"%s\" ch=%d (timeout %u ms, up to %u attempts)...\n",
                WIFI_SSID,
                target.channel,
                (unsigned)WIFI_CONNECT_TIMEOUT_MS,
                (unsigned)WIFI_ASSOC_MAX_ATTEMPTS);

  const uint32_t start = millis();
  uint8_t attempt = 0;
  while (WiFi.status() != WL_CONNECTED &&
         attempt < WIFI_ASSOC_MAX_ATTEMPTS &&
         (millis() - start) < WIFI_CONNECT_TIMEOUT_MS)
  {
    attempt++;
    if (attempt > 1)
    {
      Serial.printf("WiFi: re-begin assoc attempt %u/%u (waiting %u ms for Pi AP)...\n",
                    (unsigned)attempt,
                    (unsigned)WIFI_ASSOC_MAX_ATTEMPTS,
                    (unsigned)WIFI_ASSOC_RETRY_DELAY_MS);
      // Deauth + long wait: Pi brcmfmac keeps stale stations and ignores fast re-auth.
      wifiCleanDisconnect();
      vTaskDelay(pdMS_TO_TICKS(WIFI_ASSOC_RETRY_DELAY_MS));
      if ((attempt % 2u) == 1u)
      {
        wifiSoftRadioReset();
      }
    }

    g_wifiSawDisconnect = false;
    // Prefer SSID+password (+ channel). BSSID lock is more brittle with Pi SoftAP.
    if (target.channel > 0)
    {
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD, target.channel);
    }
    else
    {
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    wifiApplyTxPower();

    const uint32_t attemptStart = millis();
    wl_status_t lastStatus = WL_IDLE_STATUS;
    uint32_t lastLogMs = attemptStart;
    while (WiFi.status() != WL_CONNECTED &&
           (millis() - attemptStart) < WIFI_ASSOC_ATTEMPT_MS &&
           (millis() - start) < WIFI_CONNECT_TIMEOUT_MS)
    {
      const wl_status_t status = WiFi.status();
      const uint32_t now = millis();
      if (status != lastStatus || (now - lastLogMs) >= 2000)
      {
        Serial.printf("WiFi: still connecting attempt %u (%u ms) status=%d (%s)\n",
                      (unsigned)attempt,
                      (unsigned)(now - attemptStart),
                      (int)status,
                      wifiStatusText(status));
        lastStatus = status;
        lastLogMs = now;
      }

      // AUTH_EXPIRE: stop this attempt and back off (do not hammer the AP).
      if (g_wifiSawDisconnect && (now - attemptStart) >= 1000)
      {
        Serial.printf("WiFi: assoc attempt %u failed (reason=%u) — backing off\n",
                      (unsigned)attempt,
                      (unsigned)g_wifiLastDisconnectReason);
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(250));
    }

    if (WiFi.status() == WL_CONNECTED)
    {
      break;
    }
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    g_wifiFailStreak = 0;
    wifiApplyTxPower();
    Serial.printf("WiFi OK ip=%s rssi=%d ch=%d\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI(),
                  WiFi.channel());
    return true;
  }

  const wl_status_t failStatus = WiFi.status();
  Serial.printf("WiFi connect timeout after %u ms (%u attempts): status=%d (%s)\n",
                (unsigned)(millis() - start),
                (unsigned)attempt,
                (int)failStatus,
                wifiStatusText(failStatus));
  Serial.printf("  SSID=\"%s\" password len=%u STA MAC=%s\n",
                WIFI_SSID,
                (unsigned)strlen(WIFI_PASSWORD),
                WiFi.macAddress().c_str());
  if (failStatus == WL_CONNECT_FAILED || g_wifiLastDisconnectReason == 15)
  {
    Serial.println("WiFi: if handshake fails, verify hostapd passphrase matches app_config.h");
  }
  if (g_wifiLastDisconnectReason == 2)
  {
    Serial.println("WiFi: AUTH_EXPIRE — check TX power, Pi hostapd stale stations, passphrase");
  }

  g_wifiFailStreak++;
  g_mqttWifiOnline = false;
  wifiCleanDisconnect();
  return false;
}

static bool mqttEnsureConnected()
{
  if (g_mqttClient.connected())
  {
    return true;
  }

  g_mqttWifiOnline = false;

  // Full STA MAC — low 16 bits of getEfuseMac() are the OUI and collide across boards.
  char clientId[40];
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(clientId, sizeof(clientId), "%s%02X%02X%02X%02X%02X%02X",
           MQTT_CLIENT_ID_PREFIX, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  Serial.printf("MQTT connecting %s:%d as %s...\n", MQTT_HOST, MQTT_PORT, clientId);
  g_mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  g_mqttClient.setKeepAlive(MQTT_KEEPALIVE_S);
  g_mqttClient.setCallback(mqttMessageCallback);
  g_mqttClient.setBufferSize(256);

  if (g_mqttClient.connect(clientId))
  {
    g_mqttClient.subscribe(MQTT_TOPIC, MQTT_QOS);
    g_mqttClient.subscribe(MQTT_VOLUME_TOPIC, MQTT_QOS);
    Serial.printf("MQTT subscribed to %s qos=%u\n", MQTT_TOPIC, (unsigned)MQTT_QOS);
    Serial.printf("MQTT subscribed to %s (DISPLAY_ID=%d)\n", MQTT_VOLUME_TOPIC, DISPLAY_ID);
    return true;
  }

  Serial.printf("MQTT failed rc=%d\n", g_mqttClient.state());
  return false;
}

static void mqttWifiTask(void *param)
{
  g_mqttCmdState = static_cast<MqttCommandState *>(param);

  for (;;)
  {
    if (!wifiEnsureConnected())
    {
      g_mqttWifiOnline = false;
      vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS));
      continue;
    }

    if (!mqttEnsureConnected())
    {
      g_mqttWifiOnline = false;
      vTaskDelay(pdMS_TO_TICKS(MQTT_RECONNECT_DELAY_MS));
      continue;
    }

    if (!g_mqttWifiOnline)
    {
      g_mqttWifiOnline = true;
      Serial.println("Network online (WiFi + MQTT)");
    }

    g_mqttClient.loop();
    vTaskDelay(pdMS_TO_TICKS(MQTT_LOOP_DELAY_MS));
  }
}

inline bool mqttWifiIsOnline()
{
  return g_mqttWifiOnline;
}

inline bool startMqttWifiTask(MqttCommandState &state)
{
  if (!mqttCommandInit(state))
  {
    Serial.println("MQTT command queue alloc failed");
    return false;
  }

  TaskHandle_t handle = nullptr;
  BaseType_t ok = xTaskCreatePinnedToCore(
      mqttWifiTask,
      "mqttWifi",
      MQTT_TASK_STACK,
      &state,
      MQTT_TASK_PRIORITY,
      &handle,
      MQTT_TASK_CORE);

  if (ok != pdPASS)
  {
    Serial.println("mqttWifi task create failed");
    return false;
  }
  return true;
}
