#pragma once

// WiFi STA + MQTT client task. Callback only queues PlayCommand — no SD I/O.
// Requires library: PubSubClient (by Nick O'Leary) from Library Manager.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <PubSubClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "app_config.h"
#include "MqttCommand.h"

static WiFiClient g_mqttWifiClient;
static PubSubClient g_mqttClient(g_mqttWifiClient);
static MqttCommandState *g_mqttCmdState = nullptr;

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

static void mqttMessageCallback(char *topic, byte *payload, unsigned int length)
{
  if (g_mqttCmdState == nullptr || topic == nullptr)
  {
    return;
  }
  if (strcmp(topic, MQTT_TOPIC) != 0)
  {
    return;
  }

  char buf[160];
  unsigned int n = length < sizeof(buf) - 1 ? length : sizeof(buf) - 1;
  memcpy(buf, payload, n);
  buf[n] = '\0';

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

static void wifiPrepareSta()
{
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true, true);
  vTaskDelay(pdMS_TO_TICKS(300));
  WiFi.mode(WIFI_OFF);
  vTaskDelay(pdMS_TO_TICKS(100));
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
#if defined(WIFI_COUNTRY_CODE)
  wifiSetCountryCode(WIFI_COUNTRY_CODE);
#endif
  vTaskDelay(pdMS_TO_TICKS(100));
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
    Serial.printf("WiFi: disconnected reason=%u (%s)\n",
                  info.wifi_sta_disconnected.reason,
                  wifiDisconnectReasonText(info.wifi_sta_disconnected.reason));
    break;
  default:
    break;
  }
}

static void wifiLogScanForTargetSsid()
{
  wifiPrepareSta();
  Serial.println("WiFi: scanning for APs...");
  const int n = WiFi.scanNetworks(false, true);
  WiFi.scanDelete();
  if (n < 0)
  {
    Serial.printf("WiFi: scan failed (%d) — radio busy, retry after next disconnect\n", n);
    return;
  }

  Serial.printf("WiFi: found %d network(s)\n", n);
  bool foundTarget = false;
  for (int i = 0; i < n; i++)
  {
    const String ssid = WiFi.SSID(i);
    const bool isTarget = (ssid == WIFI_SSID);
    if (isTarget)
    {
      foundTarget = true;
    }
    Serial.printf("  [%d] \"%s\" ch=%d rssi=%d enc=%s%s\n",
                  i,
                  ssid.c_str(),
                  WiFi.channel(i),
                  WiFi.RSSI(i),
                  (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "open" : "secured",
                  isTarget ? " <-- target" : "");
  }

  if (!foundTarget)
  {
    Serial.printf("WiFi: target SSID \"%s\" NOT visible — check RPi AP is on and in range\n", WIFI_SSID);
  }
}

static void wifiLogConnectFailure(wl_status_t status)
{
  Serial.printf("WiFi connect failed: status=%d (%s)\n", (int)status, wifiStatusText(status));
  Serial.printf("  SSID=\"%s\" password len=%u\n", WIFI_SSID, (unsigned)strlen(WIFI_PASSWORD));
  Serial.printf("  STA MAC=%s\n", WiFi.macAddress().c_str());
  Serial.println("WiFi: if reason=15, verify RPi /etc/hostapd/hostapd.conf matches app_config.h");
  Serial.println("WiFi: on RPi run: sudo install/setup_ap.sh  (after editing config.yaml)");
  wifiLogScanForTargetSsid();
}

static bool wifiEnsureConnected()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    return true;
  }

  static bool eventsRegistered = false;
  if (!eventsRegistered)
  {
    WiFi.onEvent(wifiEventHandler);
    eventsRegistered = true;
  }

  Serial.printf("WiFi connecting to \"%s\" (timeout %u ms)...\n", WIFI_SSID, (unsigned)WIFI_CONNECT_TIMEOUT_MS);
  wifiPrepareSta();
  Serial.printf("  STA MAC=%s\n", WiFi.macAddress().c_str());
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t start = millis();
  wl_status_t lastStatus = WL_IDLE_STATUS;
  uint32_t lastLogMs = start;
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS)
  {
    const wl_status_t status = WiFi.status();
    const uint32_t now = millis();
    if (status != lastStatus || (now - lastLogMs) >= 2000)
    {
      Serial.printf("WiFi: still connecting (%u ms) status=%d (%s)\n",
                    (unsigned)(now - start),
                    (int)status,
                    wifiStatusText(status));
      lastStatus = status;
      lastLogMs = now;
    }
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.printf("WiFi OK ip=%s rssi=%d ch=%d\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI(),
                  WiFi.channel());
    return true;
  }

  Serial.printf("WiFi connect timeout after %u ms\n", (unsigned)WIFI_CONNECT_TIMEOUT_MS);
  wifiLogConnectFailure(WiFi.status());
  wifiPrepareSta();
  return false;
}

static bool mqttEnsureConnected()
{
  if (g_mqttClient.connected())
  {
    return true;
  }

  char clientId[40];
  uint64_t mac = ESP.getEfuseMac();
  snprintf(clientId, sizeof(clientId), "%s%04X", MQTT_CLIENT_ID_PREFIX, (unsigned)(mac & 0xFFFF));

  Serial.printf("MQTT connecting %s:%d as %s...\n", MQTT_HOST, MQTT_PORT, clientId);
  g_mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  g_mqttClient.setKeepAlive(MQTT_KEEPALIVE_S);
  g_mqttClient.setCallback(mqttMessageCallback);
  g_mqttClient.setBufferSize(256);

  if (g_mqttClient.connect(clientId))
  {
    g_mqttClient.subscribe(MQTT_TOPIC, 0);
    Serial.printf("MQTT subscribed to %s\n", MQTT_TOPIC);
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
      vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS));
      continue;
    }

    if (!mqttEnsureConnected())
    {
      vTaskDelay(pdMS_TO_TICKS(MQTT_RECONNECT_DELAY_MS));
      continue;
    }

    g_mqttClient.loop();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
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
