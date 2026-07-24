#!/usr/bin/env python3
"""Render the BOCAS touch UI to PNG files without a screen or mic (layout check).

Usage:  python tools/render_preview.py [out_dir]
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
os.environ.setdefault("SDL_AUDIODRIVER", "dummy")

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))

import pygame  # noqa: E402

from vu_display import BocaVolume, VuDisplay  # noqa: E402


class FakeMonitor:
    def __init__(self, level_dbfs: float, threshold_dbfs: float) -> None:
        self.level_dbfs = level_dbfs
        self.threshold_dbfs = threshold_dbfs

    @property
    def level_norm(self) -> float:
        return max(0.0, min(1.0, (self.level_dbfs + 60.0) / 60.0))


class FakePublisher:
    def __init__(self, connected: bool, last_error: str = "") -> None:
        self.connected = connected
        self.last_error = last_error

    def publish_volume(self, topic: str, percent: int) -> bool:  # noqa: ARG002
        return True


def render(name: str, out_dir: Path, size, level, threshold, connected, volumes, error="") -> None:
    monitor = FakeMonitor(level, threshold)
    publisher = FakePublisher(connected, error)
    ui = VuDisplay(
        monitor=monitor,
        publisher=publisher,
        width=size[0],
        height=size[1],
        threshold_dbfs=threshold,
        bocas=[
            BocaVolume(1, "BOCA 1", "displays/boca1/volume", volumes[0]),
            BocaVolume(2, "BOCA 2", "displays/boca2/volume", volumes[1]),
            BocaVolume(3, "BOCA 3", "displays/boca3/volume", volumes[2]),
        ],
    )
    if not connected:
        ui.note_trigger(-24.1)
    else:
        ui.note_trigger(level)

    surface = pygame.display.set_mode(size)
    ui.width, ui.height = size
    ui._build_fonts()
    ui._layout()
    ui._draw(surface)

    out = out_dir / f"{name}.png"
    pygame.image.save(surface, str(out))
    print(f"wrote {out}")


def main() -> int:
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent / "preview"
    out_dir.mkdir(parents=True, exist_ok=True)

    pygame.init()
    render("800x480_quiet", out_dir, (800, 480), -42.0, -25.0, True, (80, 45, 100))
    render("800x480_over", out_dir, (800, 480), -12.4, -25.0, False, (0, 100, 62),
           error="connect: Connection refused")
    render("1024x600_quiet", out_dir, (1024, 600), -33.0, -18.0, True, (25, 70, 95))
    pygame.quit()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
