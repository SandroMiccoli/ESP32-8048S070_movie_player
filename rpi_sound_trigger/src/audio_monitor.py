"""Capture microphone/webcam/I2S audio and report level in dBFS."""

from __future__ import annotations

import threading
import time
from typing import Callable, List, Optional, Sequence, Tuple, Union

import numpy as np
import sounddevice as sd


DeviceSpec = Union[None, int, str]

# I2S MEMS cards (INMP441 via googlevoicehat overlay) often prefer 48 kHz / stereo.
_FALLBACK_RATES: Tuple[int, ...] = (48000, 44100, 32000, 16000)
_FALLBACK_CHANNELS: Tuple[int, ...] = (1, 2)


def list_input_devices() -> str:
    lines = []
    for i, dev in enumerate(sd.query_devices()):
        if int(dev.get("max_input_channels", 0)) > 0:
            lines.append(f"  [{i}] {dev['name']} (in={dev['max_input_channels']})")
    return "\n".join(lines) if lines else "  (no input devices found)"


def resolve_device(spec: DeviceSpec) -> Optional[Union[int, str]]:
    if spec is None:
        return None
    if isinstance(spec, int):
        return spec
    name = str(spec).strip().lower()
    if not name:
        return None
    for i, dev in enumerate(sd.query_devices()):
        if int(dev.get("max_input_channels", 0)) <= 0:
            continue
        if name in str(dev["name"]).lower():
            return i
    raise ValueError(
        f"No input device matching {spec!r}. Available:\n{list_input_devices()}"
    )


def rms_to_dbfs(rms: float) -> float:
    if rms <= 1e-12:
        return -120.0
    return float(20.0 * np.log10(rms))


def _unique_preserve(values: Sequence[int]) -> List[int]:
    seen = set()
    out: List[int] = []
    for v in values:
        if v in seen:
            continue
        seen.add(v)
        out.append(int(v))
    return out


class AudioMonitor:
    """Background InputStream that updates level_dbfs and fires on_trigger."""

    def __init__(
        self,
        device: DeviceSpec = None,
        sample_rate: int = 48000,
        channels: int = 2,
        block_ms: int = 50,
        threshold_dbfs: float = -30.0,
        cooldown_s: float = 2.5,
        on_trigger: Optional[Callable[[float], None]] = None,
    ) -> None:
        self.sample_rate = int(sample_rate)
        self.channels = int(channels)
        self.block_ms = int(block_ms)
        self.blocksize = max(1, int(self.sample_rate * self.block_ms / 1000.0))
        self.threshold_dbfs = float(threshold_dbfs)
        self.cooldown_s = float(cooldown_s)
        self.on_trigger = on_trigger
        self.device = resolve_device(device)

        self._lock = threading.Lock()
        self._level_dbfs = -120.0
        self._level_norm = 0.0  # 0..1 for VU (maps -60..0 dBFS)
        self._last_trigger_ts = 0.0
        self._stream: Optional[sd.InputStream] = None

    @property
    def level_dbfs(self) -> float:
        with self._lock:
            return self._level_dbfs

    @property
    def level_norm(self) -> float:
        with self._lock:
            return self._level_norm

    @property
    def last_trigger_ts(self) -> float:
        with self._lock:
            return self._last_trigger_ts

    def _open_stream(self, sample_rate: int, channels: int, callback) -> sd.InputStream:
        blocksize = max(1, int(sample_rate * self.block_ms / 1000.0))
        stream = sd.InputStream(
            device=self.device,
            samplerate=sample_rate,
            channels=channels,
            dtype="float32",
            blocksize=blocksize,
            callback=callback,
        )
        stream.start()
        return stream

    def start(self) -> None:
        if self._stream is not None:
            return

        def callback(indata, frames, time_info, status):  # noqa: ARG001
            if status:
                # Keep going; status is informational (overflow etc.)
                pass
            # INMP441 with L/R→GND puts signal on left (channel 0)
            mono = indata[:, 0] if indata.ndim > 1 else indata
            rms = float(np.sqrt(np.mean(np.square(mono.astype(np.float64)))))
            dbfs = rms_to_dbfs(rms)
            # Map -60..0 dBFS → 0..1 for the bar
            norm = max(0.0, min(1.0, (dbfs + 60.0) / 60.0))

            triggered = False
            now = time.monotonic()
            with self._lock:
                self._level_dbfs = dbfs
                self._level_norm = norm
                if dbfs >= self.threshold_dbfs and (now - self._last_trigger_ts) >= self.cooldown_s:
                    self._last_trigger_ts = now
                    triggered = True

            if triggered and self.on_trigger is not None:
                self.on_trigger(dbfs)

        rates = _unique_preserve((self.sample_rate, *_FALLBACK_RATES))
        channels_try = _unique_preserve((self.channels, *_FALLBACK_CHANNELS))
        errors: List[str] = []

        for rate in rates:
            for ch in channels_try:
                try:
                    self._stream = self._open_stream(rate, ch, callback)
                except Exception as exc:  # noqa: BLE001 — try next rate/channel combo
                    errors.append(f"rate={rate} ch={ch}: {exc}")
                    self._stream = None
                    continue
                self.sample_rate = rate
                self.channels = ch
                self.blocksize = max(1, int(rate * self.block_ms / 1000.0))
                print(f"[audio] opened device={self.device} rate={rate} channels={ch}")
                return

        detail = "\n  ".join(errors) if errors else "(no attempts)"
        raise RuntimeError(
            "Could not open audio input. Tried rates "
            f"{rates} and channels {channels_try}.\n  {detail}\n"
            f"Available inputs:\n{list_input_devices()}"
        )

    def stop(self) -> None:
        if self._stream is None:
            return
        try:
            self._stream.stop()
            self._stream.close()
        finally:
            self._stream = None
