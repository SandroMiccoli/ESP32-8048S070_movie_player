"""MQTT publisher for display trigger events."""

from __future__ import annotations

import json
import threading
import time
from typing import Dict, Optional


import paho.mqtt.client as mqtt


class MqttPublisher:
    def __init__(
        self,
        host: str = "192.168.4.1",
        port: int = 1883,
        topic: str = "displays/trigger",
        client_id: str = "rpi-sound-trigger",
        qos: int = 1,
    ) -> None:
        self.host = host
        self.port = int(port)
        self.topic = topic
        self.qos = 1 if int(qos) >= 1 else 0
        self._connected = False
        self._lock = threading.Lock()
        self._last_error = ""
        # topic → percent; republished (retained) whenever we (re)connect to the broker
        # and whenever a display announces online so late joiners get the current volume.
        self._volumes: Dict[str, int] = {}

        self._client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=client_id,
            protocol=mqtt.MQTTv311,
        )
        self._client.on_connect = self._on_connect
        self._client.on_disconnect = self._on_disconnect
        self._client.on_message = self._on_message

    def set_volumes(self, volumes: Dict[str, int]) -> None:
        """Register per-topic volume percents used for connect / online pushes."""
        with self._lock:
            self._volumes = {
                str(topic): max(0, min(100, int(pct))) for topic, pct in volumes.items()
            }

    def update_volume(self, topic: str, percent: int) -> None:
        """Keep the registry in sync when a UI slider changes."""
        with self._lock:
            self._volumes[str(topic)] = max(0, min(100, int(percent)))

    def _on_connect(self, client, userdata, flags, reason_code, properties):  # noqa: ARG002
        ok = reason_code == 0 or str(reason_code) in ("Success", "0")
        with self._lock:
            self._connected = bool(ok)
            self._last_error = "" if ok else f"connect: {reason_code}"
        if ok:
            # Displays publish here when they join MQTT; we answer with their volume.
            client.subscribe("displays/+/online", qos=self.qos)
            self.publish_all_volumes()

    def _on_disconnect(self, client, userdata, flags, reason_code, properties):  # noqa: ARG002
        with self._lock:
            self._connected = False
            if reason_code not in (0, None) and str(reason_code) not in ("Success", "0"):
                self._last_error = f"disconnect: {reason_code}"

    def _on_message(self, client, userdata, msg):  # noqa: ARG002
        topic = msg.topic or ""
        # displays/boca1/online → push displays/boca1/volume
        if not topic.startswith("displays/") or not topic.endswith("/online"):
            return
        parts = topic.split("/")
        if len(parts) != 3:
            return
        volume_topic = f"displays/{parts[1]}/volume"
        with self._lock:
            percent = self._volumes.get(volume_topic)
        if percent is None:
            return
        ok = self.publish_volume(volume_topic, percent, retain=True)
        print(
            f"[volume] {parts[1]} online → {percent}% "
            f"({volume_topic}) {'ok' if ok else 'FAIL'}"
        )

    @property
    def connected(self) -> bool:
        with self._lock:
            return self._connected

    @property
    def last_error(self) -> str:
        with self._lock:
            return self._last_error

    def connect(self) -> None:
        self._client.connect_async(self.host, self.port, keepalive=30)
        self._client.loop_start()

    def disconnect(self) -> None:
        try:
            self._client.loop_stop()
            self._client.disconnect()
        except Exception:
            pass
        with self._lock:
            self._connected = False

    def _publish_json(
        self, topic: str, payload: dict, retain: bool = False, wait: bool = True
    ) -> bool:
        info = self._client.publish(
            topic, json.dumps(payload), qos=self.qos, retain=retain
        )
        if not wait:
            return True
        try:
            info.wait_for_publish(timeout=2.0)
            return bool(info.is_published())
        except Exception as exc:
            with self._lock:
                self._last_error = str(exc)
            return False

    def publish_alert(self, level_dbfs: Optional[float] = None) -> bool:
        payload = {
            "state": "alert",
            "ts": int(time.time()),
        }
        if level_dbfs is not None:
            payload["level_dbfs"] = round(float(level_dbfs), 1)

        # QoS 1: broker retries until each connected subscriber ACKs delivery.
        return self._publish_json(self.topic, payload, retain=False, wait=True)

    def publish_volume(self, topic: str, percent: int, retain: bool = True) -> bool:
        """Publish 0–100 volume for one BOCA display. Retained so late joiners pick it up."""
        pct = max(0, min(100, int(percent)))
        with self._lock:
            self._volumes[str(topic)] = pct
        payload = {
            "volume": pct,
            "ts": int(time.time()),
        }
        # Non-blocking: slider drags must stay snappy on the touch UI.
        return self._publish_json(topic, payload, retain=retain, wait=False)

    def publish_all_volumes(self) -> None:
        """Push current volume for every registered BOCA (retained)."""
        with self._lock:
            items = list(self._volumes.items())
        for topic, percent in items:
            self.publish_volume(topic, percent, retain=True)
            print(f"[volume] seed {topic} → {percent}%")
