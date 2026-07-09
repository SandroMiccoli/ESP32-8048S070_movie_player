#!/usr/bin/env python3
"""RPi sound-level trigger: mic → MQTT alert for ESP32 displays."""

from __future__ import annotations

import argparse
import signal
import sys
import time
from pathlib import Path

from typing import Optional

import yaml

# Allow running as `python src/main.py` from rpi_sound_trigger/
sys.path.insert(0, str(Path(__file__).resolve().parent))

from audio_monitor import AudioMonitor, list_input_devices
from mqtt_publisher import MqttPublisher
from vu_display import VuDisplay


def load_config(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as f:
        return yaml.safe_load(f) or {}


def default_config_path() -> Path:
    return Path(__file__).resolve().parent.parent / "config.yaml"


def main() -> int:
    parser = argparse.ArgumentParser(description="RPi mic → MQTT display trigger")
    parser.add_argument(
        "-c",
        "--config",
        type=Path,
        default=default_config_path(),
        help="Path to config.yaml",
    )
    parser.add_argument(
        "--list-devices",
        action="store_true",
        help="List ALSA/input devices and exit",
    )
    parser.add_argument(
        "--no-display",
        action="store_true",
        help="Force headless (no pygame VU)",
    )
    args = parser.parse_args()

    if args.list_devices:
        print("Input devices:")
        print(list_input_devices())
        return 0

    cfg = load_config(args.config)
    audio_cfg = cfg.get("audio", {})
    mqtt_cfg = cfg.get("mqtt", {})
    disp_cfg = cfg.get("display", {})

    publisher = MqttPublisher(
        host=mqtt_cfg.get("host", "192.168.4.1"),
        port=int(mqtt_cfg.get("port", 1883)),
        topic=mqtt_cfg.get("topic", "displays/trigger"),
        client_id=mqtt_cfg.get("client_id", "rpi-sound-trigger"),
    )

    display: Optional[VuDisplay] = None

    def on_trigger(level_dbfs: float) -> None:
        ok = publisher.publish_alert(level_dbfs)
        print(f"[trigger] level={level_dbfs:.1f} dBFS publish={'ok' if ok else 'FAIL'}")
        if display is not None:
            display.note_trigger(level_dbfs)

    monitor = AudioMonitor(
        device=audio_cfg.get("device"),
        sample_rate=int(audio_cfg.get("sample_rate", 16000)),
        channels=int(audio_cfg.get("channels", 1)),
        block_ms=int(audio_cfg.get("block_ms", 50)),
        threshold_dbfs=float(audio_cfg.get("threshold_dbfs", -25.0)),
        cooldown_s=float(audio_cfg.get("cooldown_s", 2.5)),
        on_trigger=on_trigger,
    )

    stop = False

    def handle_signal(signum, frame):  # noqa: ARG001
        nonlocal stop
        stop = True
        if display is not None:
            display.stop()

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    print(f"Config: {args.config}")
    print(f"MQTT {publisher.host}:{publisher.port} topic={publisher.topic}")
    print(f"Threshold {monitor.threshold_dbfs} dBFS, cooldown {monitor.cooldown_s}s")
    print("Input devices:\n" + list_input_devices())

    publisher.connect()
    monitor.start()

    use_display = bool(disp_cfg.get("enabled", True)) and not args.no_display
    if use_display:
        display = VuDisplay(
            monitor=monitor,
            publisher=publisher,
            width=int(disp_cfg.get("width", 800)),
            height=int(disp_cfg.get("height", 480)),
            fps=int(disp_cfg.get("fps", 15)),
            fullscreen=bool(disp_cfg.get("fullscreen", False)),
            threshold_dbfs=monitor.threshold_dbfs,
        )
        try:
            display.run()
        finally:
            monitor.stop()
            publisher.disconnect()
        return 0

    print("Headless mode — Ctrl+C to stop")
    try:
        while not stop:
            print(
                f"\r level={monitor.level_dbfs:7.1f} dBFS  mqtt={'ok' if publisher.connected else 'down'}    ",
                end="",
                flush=True,
            )
            time.sleep(0.2)
    finally:
        print()
        monitor.stop()
        publisher.disconnect()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
