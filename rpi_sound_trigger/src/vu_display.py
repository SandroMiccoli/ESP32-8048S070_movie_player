"""Touch-screen BOCAS control UI: mic VU, threshold, MQTT status, volume sliders."""

from __future__ import annotations

import time
from dataclasses import dataclass
from typing import TYPE_CHECKING, List, Optional, Sequence, Tuple

import pygame

if TYPE_CHECKING:  # keeps the UI importable without sounddevice/paho installed
    from audio_monitor import AudioMonitor
    from mqtt_publisher import MqttPublisher

# White on black. Colour is reserved for meaning only: yellow threshold,
# green/red level + MQTT state.
WHITE = (255, 255, 255)
BLACK = (0, 0, 0)
YELLOW = (230, 190, 0)
GREEN = (0, 170, 0)
RED = (220, 0, 0)

DBFS_MIN = -60.0
DBFS_MAX = 0.0


@dataclass
class BocaVolume:
    id: int
    label: str
    topic: str
    percent: int = 80


def _dbfs_to_norm(dbfs: float) -> float:
    return max(0.0, min(1.0, (float(dbfs) - DBFS_MIN) / (DBFS_MAX - DBFS_MIN)))


def _norm_to_dbfs(norm: float) -> float:
    return DBFS_MIN + max(0.0, min(1.0, float(norm))) * (DBFS_MAX - DBFS_MIN)


def _load_mono_font(size: int, bold: bool = False) -> pygame.font.Font:
    """Prefer Consolas; fall back to common monospace faces on Linux/Pi."""
    names = (
        "consolas",
        "dejavusansmono",
        "liberationmono",
        "freemono",
        "couriernew",
        "monospace",
    )
    return pygame.font.SysFont(",".join(names), size, bold=bold)


def _fit_text(font: pygame.font.Font, text: str, max_width: int) -> str:
    """Trim text with an ellipsis until it fits max_width pixels."""
    if max_width <= 0 or font.size(text)[0] <= max_width:
        return text
    trimmed = text
    while trimmed and font.size(trimmed + "...")[0] > max_width:
        trimmed = trimmed[:-1]
    return trimmed + "..." if trimmed else ""


class VuDisplay:
    def __init__(
        self,
        monitor: AudioMonitor,
        publisher: MqttPublisher,
        width: int = 800,
        height: int = 480,
        fps: int = 15,
        fullscreen: bool = False,
        threshold_dbfs: float = -25.0,
        bocas: Optional[Sequence[BocaVolume]] = None,
    ) -> None:
        self.monitor = monitor
        self.publisher = publisher
        self.width = width
        self.height = height
        self.fps = max(1, int(fps))
        self.fullscreen = fullscreen
        self.threshold_dbfs = float(threshold_dbfs)
        self.bocas: List[BocaVolume] = list(bocas) if bocas else [
            BocaVolume(1, "BOCA 1", "displays/boca1/volume", 80),
            BocaVolume(2, "BOCA 2", "displays/boca2/volume", 80),
            BocaVolume(3, "BOCA 3", "displays/boca3/volume", 80),
        ]
        self._last_trigger_msg = "—"
        self._running = False

        # Interaction state
        self._drag_threshold = False
        self._drag_volume_idx: Optional[int] = None
        self._last_volume_pub: List[Tuple[int, float]] = [(b.percent, 0.0) for b in self.bocas]

        # Layout rects (filled in _layout())
        self._margin = 16
        self._bar_rect = pygame.Rect(0, 0, 0, 0)
        self._slider_tracks: List[pygame.Rect] = []
        self._slider_hit: List[pygame.Rect] = []

    def note_trigger(self, level_dbfs: float) -> None:
        self._last_trigger_msg = (
            f"alert @ {level_dbfs:.1f} dBFS ({time.strftime('%H:%M:%S')})"
        )

    def stop(self) -> None:
        self._running = False

    def run(self) -> None:
        pygame.init()
        # USB touchscreens usually appear as a mouse; keep the cursor for aiming.
        pygame.mouse.set_visible(True)

        flags = pygame.FULLSCREEN if self.fullscreen else 0
        size = (0, 0) if self.fullscreen else (self.width, self.height)
        screen = pygame.display.set_mode(size, flags)
        self.width, self.height = screen.get_size()
        pygame.display.set_caption("BOCAS")
        clock = pygame.time.Clock()

        self._build_fonts()
        self._layout()

        for i in range(len(self.bocas)):
            self._publish_volume(i, force=True)

        self._running = True
        while self._running:
            for event in pygame.event.get():
                self._handle_event(event)

            self._draw(screen)
            pygame.display.flip()
            clock.tick(self.fps)

        pygame.quit()

    def _build_fonts(self) -> None:
        h = self.height
        self.font_title = _load_mono_font(max(34, h // 9), bold=True)
        self.font_clock = _load_mono_font(max(26, h // 14), bold=True)
        self.font_body = _load_mono_font(max(16, h // 26))
        self.font_label = _load_mono_font(max(16, h // 26), bold=True)
        self.font_pct = _load_mono_font(max(14, h // 30), bold=True)
        self.font_small = _load_mono_font(max(12, h // 34))

    def _layout(self) -> None:
        w, h = self.width, self.height
        margin = max(16, w // 40)
        self._margin = margin
        body_h = self.font_body.get_height()

        self._header_y = margin // 2
        header_bottom = self._header_y + max(
            self.font_title.get_height(), self.font_clock.get_height()
        )

        # Level readout and threshold label share the row above the meter.
        self._level_row_y = header_bottom + max(10, h // 26)
        bar_top = self._level_row_y + body_h + max(8, h // 60)
        bar_h = max(36, h // 9)
        self._bar_rect = pygame.Rect(margin, bar_top, w - 2 * margin, bar_h)
        self._marker_overhang = max(10, h // 40)

        # One status row under the meter: last trigger left, MQTT status right.
        self._status_y = self._bar_rect.bottom + self._marker_overhang + max(10, h // 34)
        self._error_y = self._status_y + body_h + 4

        # Volume sliders fill the rest, labels on their own row above the tracks.
        label_h = self.font_label.get_height()
        label_gap = max(6, h // 60)
        track_bottom = h - margin
        self._slider_label_y = self._error_y + self.font_small.get_height() + max(
            14, h // 20
        )
        track_top = self._slider_label_y + label_h + label_gap
        # Cap the track height so the meter stays the focal point and the
        # BOCA labels keep clear space above the tracks
        max_track_h = int(h * 0.34)
        if track_bottom - track_top > max_track_h:
            track_top = track_bottom - max_track_h
            self._slider_label_y = track_top - label_gap - label_h
        track_h = max(60, track_bottom - track_top)

        track_w = max(48, w // 13)
        n = max(1, len(self.bocas))
        self._slider_tracks = []
        self._slider_hit = []
        for i in range(n):
            cx = int((i + 0.5) * w / n)
            track = pygame.Rect(0, 0, track_w, track_h)
            track.centerx = cx
            track.top = track_top
            hit = pygame.Rect(0, 0, max(track_w + 60, int(w / n) - 20), track_h + 48)
            hit.centerx = cx
            hit.centery = track.centery
            self._slider_tracks.append(track)
            self._slider_hit.append(hit)

    def _handle_event(self, event: pygame.event.Event) -> None:
        if event.type == pygame.QUIT:
            self._running = False
            return
        if event.type == pygame.KEYDOWN and event.key in (pygame.K_ESCAPE, pygame.K_q):
            self._running = False
            return

        if event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
            self._pointer_down(event.pos)
        elif event.type == pygame.MOUSEMOTION:
            buttons = event.buttons if hasattr(event, "buttons") else (0, 0, 0)
            if buttons and buttons[0]:
                self._pointer_drag(event.pos)
        elif event.type == pygame.MOUSEBUTTONUP and event.button == 1:
            self._pointer_up()
        elif event.type == pygame.FINGERDOWN:
            self._pointer_down(self._finger_to_pos(event))
        elif event.type == pygame.FINGERMOTION:
            self._pointer_drag(self._finger_to_pos(event))
        elif event.type == pygame.FINGERUP:
            self._pointer_up()

    def _finger_to_pos(self, event: pygame.event.Event) -> Tuple[int, int]:
        return (int(event.x * self.width), int(event.y * self.height))

    def _pointer_down(self, pos: Tuple[int, int]) -> None:
        if self._threshold_hit(pos):
            self._drag_threshold = True
            self._set_threshold_from_x(pos[0])
            return
        for i, hit in enumerate(self._slider_hit):
            if hit.collidepoint(pos):
                self._drag_volume_idx = i
                self._set_volume_from_y(i, pos[1])
                return

    def _pointer_drag(self, pos: Tuple[int, int]) -> None:
        if self._drag_threshold:
            self._set_threshold_from_x(pos[0])
        elif self._drag_volume_idx is not None:
            self._set_volume_from_y(self._drag_volume_idx, pos[1])

    def _pointer_up(self) -> None:
        if self._drag_volume_idx is not None:
            self._publish_volume(self._drag_volume_idx, force=True)
        self._drag_threshold = False
        self._drag_volume_idx = None

    def _threshold_hit(self, pos: Tuple[int, int]) -> bool:
        thr_x = self._threshold_x()
        pad = max(24, self._bar_rect.height)
        hit = pygame.Rect(
            thr_x - pad,
            self._bar_rect.top - pad // 2,
            pad * 2,
            self._bar_rect.height + pad,
        )
        return hit.collidepoint(pos) or self._bar_rect.collidepoint(pos)

    def _threshold_x(self) -> int:
        return self._bar_rect.x + int(_dbfs_to_norm(self.threshold_dbfs) * self._bar_rect.width)

    def _set_threshold_from_x(self, x: int) -> None:
        rel = (x - self._bar_rect.x) / max(1, self._bar_rect.width)
        self.threshold_dbfs = round(_norm_to_dbfs(rel), 1)
        self.monitor.threshold_dbfs = self.threshold_dbfs

    def _set_volume_from_y(self, idx: int, y: int) -> None:
        track = self._slider_tracks[idx]
        # Top = 100%, bottom = 0%
        rel = 1.0 - (y - track.top) / max(1, track.height)
        percent = int(round(max(0.0, min(1.0, rel)) * 100.0))
        if percent != self.bocas[idx].percent:
            self.bocas[idx].percent = percent
            self._publish_volume(idx, force=False)

    def _publish_volume(self, idx: int, force: bool = False) -> None:
        boca = self.bocas[idx]
        now = time.monotonic()
        last_pct, last_ts = self._last_volume_pub[idx]
        if not force and percent_unchanged_and_recent(boca.percent, last_pct, now, last_ts):
            return
        self.publisher.publish_volume(boca.topic, boca.percent)
        self._last_volume_pub[idx] = (boca.percent, now)

    def _draw(self, screen: pygame.Surface) -> None:
        screen.fill(BLACK)
        self._draw_header(screen)
        self._draw_meter(screen)
        self._draw_status(screen)
        self._draw_sliders(screen)

    def _draw_header(self, screen: pygame.Surface) -> None:
        margin = self._margin
        title = self.font_title.render("BOCAS", True, WHITE)
        screen.blit(title, (margin, self._header_y))

        clock_txt = self.font_clock.render(time.strftime("%H:%M:%S"), True, WHITE)
        # Baseline-align the clock with the bottom of the title
        clock_y = self._header_y + title.get_height() - clock_txt.get_height()
        screen.blit(clock_txt, (self.width - margin - clock_txt.get_width(), clock_y))

    def _draw_meter(self, screen: pygame.Surface) -> None:
        margin = self._margin
        bar = self._bar_rect
        level = self.monitor.level_dbfs
        over = level >= self.threshold_dbfs

        # Row above the bar: level readout (left) + threshold value (near marker)
        level_txt = self.font_body.render(f"Level: {level:6.1f} dBFS", True, WHITE)
        screen.blit(level_txt, (margin, self._level_row_y))

        thr_x = self._threshold_x()
        thr_txt = self.font_body.render(
            f"threshold {self.threshold_dbfs:.1f} dBFS", True, YELLOW
        )
        thr_min_x = margin + level_txt.get_width() + max(16, self.width // 40)
        thr_max_x = self.width - margin - thr_txt.get_width()
        thr_label_x = max(thr_min_x, min(thr_x - thr_txt.get_width() // 2, thr_max_x))
        if thr_max_x >= thr_min_x:
            screen.blit(thr_txt, (thr_label_x, self._level_row_y))

        pygame.draw.rect(screen, WHITE, bar, width=3)
        fill_w = int(self.monitor.level_norm * (bar.width - 6))
        if fill_w > 0:
            fill = pygame.Rect(bar.x + 3, bar.y + 3, fill_w, bar.height - 6)
            pygame.draw.rect(screen, RED if over else GREEN, fill)

        # Threshold marker: line across the bar with a grab triangle below it
        marker_bot = bar.bottom + self._marker_overhang
        pygame.draw.line(screen, YELLOW, (thr_x, bar.top), (thr_x, marker_bot), 4)
        tri_h = self._marker_overhang
        pygame.draw.polygon(
            screen,
            YELLOW,
            [
                (thr_x, bar.bottom),
                (thr_x - tri_h, marker_bot),
                (thr_x + tri_h, marker_bot),
            ],
        )

    def _draw_status(self, screen: pygame.Surface) -> None:
        margin = self._margin
        y = self._status_y
        mqtt_ok = self.publisher.connected

        # MQTT status: right-aligned, state word coloured
        prefix = self.font_body.render("MQTT Status: ", True, WHITE)
        state = self.font_body.render(
            "Connected" if mqtt_ok else "Disconnected", True, GREEN if mqtt_ok else RED
        )
        mqtt_x = self.width - margin - prefix.get_width() - state.get_width()
        screen.blit(prefix, (mqtt_x, y))
        screen.blit(state, (mqtt_x + prefix.get_width(), y))

        # Last trigger: left-aligned on the same line, truncated if needed
        trigger_room = mqtt_x - margin - max(16, self.width // 40)
        trig_txt = self.font_body.render(
            _fit_text(
                self.font_body, f"Last trigger: {self._last_trigger_msg}", trigger_room
            ),
            True,
            WHITE,
        )
        screen.blit(trig_txt, (margin, y))

        err = self.publisher.last_error
        if err and not mqtt_ok:
            err_txt = self.font_small.render(
                _fit_text(self.font_small, err, self.width - 2 * margin), True, RED
            )
            screen.blit(err_txt, (margin, self._error_y))

    def _draw_sliders(self, screen: pygame.Surface) -> None:
        for i, boca in enumerate(self.bocas):
            track = self._slider_tracks[i]

            label = self.font_label.render(boca.label, True, WHITE)
            screen.blit(
                label, (track.centerx - label.get_width() // 2, self._slider_label_y)
            )

            pygame.draw.rect(screen, WHITE, track, width=3)
            fill_h = int((track.height - 6) * (boca.percent / 100.0))
            if fill_h > 0:
                pygame.draw.rect(
                    screen,
                    WHITE,
                    pygame.Rect(
                        track.x + 3, track.bottom - 3 - fill_h, track.width - 6, fill_h
                    ),
                )

            handle_h = max(8, self.height // 60)
            handle = pygame.Rect(0, 0, track.width + 16, handle_h)
            handle.centerx = track.centerx
            handle.centery = track.bottom - 3 - fill_h
            handle.top = max(track.top, min(handle.top, track.bottom - handle_h))
            pygame.draw.rect(screen, WHITE, handle)

            pct_text = f"{boca.percent}%"
            pct_w, pct_h = self.font_pct.size(pct_text)
            gap = 4
            if handle.top - track.top >= pct_h + 2 * gap:
                # Room on the unfilled (black) part above the handle
                pct_y = handle.top - gap - pct_h
                pct_col = WHITE
            else:
                # Knock the value out of the white fill below the handle
                pct_y = min(handle.bottom + gap, track.bottom - 3 - pct_h)
                pct_col = BLACK
            pct = self.font_pct.render(pct_text, True, pct_col)
            screen.blit(pct, (track.centerx - pct_w // 2, pct_y))


def percent_unchanged_and_recent(
    percent: int, last_pct: int, now: float, last_ts: float, min_interval: float = 0.12
) -> bool:
    """True → skip publish (unchanged, or still inside the drag throttle window)."""
    if percent == last_pct:
        return True
    return (now - last_ts) < min_interval
