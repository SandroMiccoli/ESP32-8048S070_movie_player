"""MQTT publisher for display trigger events."""

from __future__ import annotations

import json
import threading
import time
from typing import Optional

import paho.mqtt.client as mqtt


class MqttPublisher:
    def __init__(
        self,
        host: str = "192.168.4.1",
        port: int = 1883,
        topic: str = "displays/trigger",
        client_id: str = "rpi-sound-trigger",
    ) -> None:
        self.host = host
        self.port = int(port)
        self.topic = topic
        self._connected = False
        self._lock = threading.Lock()
        self._last_error = ""

        self._client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=client_id,
            protocol=mqtt.MQTTv311,
        )
        self._client.on_connect = self._on_connect
        self._client.on_disconnect = self._on_disconnect

    def _on_connect(self, client, userdata, flags, reason_code, properties):  # noqa: ARG002
        ok = reason_code == 0 or str(reason_code) in ("Success", "0")
        with self._lock:
            self._connected = bool(ok)
            self._last_error = "" if ok else f"connect: {reason_code}"

    def _on_disconnect(self, client, userdata, flags, reason_code, properties):  # noqa: ARG002
        with self._lock:
            self._connected = False
            if reason_code not in (0, None) and str(reason_code) not in ("Success", "0"):
                self._last_error = f"disconnect: {reason_code}"

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

    def publish_alert(self, level_dbfs: Optional[float] = None) -> bool:
        payload = {
            "state": "alert",
            "ts": int(time.time()),
        }
        if level_dbfs is not None:
            payload["level_dbfs"] = round(float(level_dbfs), 1)

        info = self._client.publish(self.topic, json.dumps(payload), qos=0, retain=False)
        try:
            info.wait_for_publish(timeout=2.0)
            return bool(info.is_published())
        except Exception as exc:
            with self._lock:
                self._last_error = str(exc)
            return False
