"""pygame VU meter for continuous microphone amplitude on an attached screen."""

from __future__ import annotations

import time
import pygame

from audio_monitor import AudioMonitor
from mqtt_publisher import MqttPublisher


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
    ) -> None:
        self.monitor = monitor
        self.publisher = publisher
        self.width = width
        self.height = height
        self.fps = max(1, int(fps))
        self.fullscreen = fullscreen
        self.threshold_dbfs = threshold_dbfs
        self._last_trigger_msg = "—"
        self._running = False

    def note_trigger(self, level_dbfs: float) -> None:
        self._last_trigger_msg = f"alert @ {level_dbfs:.1f} dBFS ({time.strftime('%H:%M:%S')})"

    def run(self) -> None:
        pygame.init()
        pygame.mouse.set_visible(False)

        flags = pygame.FULLSCREEN if self.fullscreen else 0
        size = (0, 0) if self.fullscreen else (self.width, self.height)
        screen = pygame.display.set_mode(size, flags)
        self.width, self.height = screen.get_size()
        pygame.display.set_caption("RPi Sound Trigger — VU")
        clock = pygame.time.Clock()

        title_size = max(28, self.height // 16)
        body_size = max(20, self.height // 22)
        font = pygame.font.SysFont("dejavusans", title_size)
        font_sm = pygame.font.SysFont("dejavusans", body_size)

        margin = max(24, self.width // 32)
        bar_x = margin
        bar_h = max(64, self.height // 6)
        bar_y = self.height // 2 - bar_h // 2
        bar_w = self.width - 2 * margin

        # Threshold mark on the same -60..0 mapping as AudioMonitor.level_norm
        thr_norm = max(0.0, min(1.0, (self.threshold_dbfs + 60.0) / 60.0))
        thr_x = bar_x + int(thr_norm * bar_w)

        self._running = True
        while self._running:
            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    self._running = False
                elif event.type == pygame.KEYDOWN and event.key in (pygame.K_ESCAPE, pygame.K_q):
                    self._running = False

            level = self.monitor.level_dbfs
            norm = self.monitor.level_norm
            mqtt_ok = self.publisher.connected

            screen.fill((18, 18, 22))

            # Track background
            pygame.draw.rect(screen, (40, 40, 50), (bar_x, bar_y, bar_w, bar_h), border_radius=8)
            fill_w = int(norm * bar_w)
            color = (50, 200, 90) if level < self.threshold_dbfs else (230, 70, 60)
            if fill_w > 0:
                pygame.draw.rect(screen, color, (bar_x, bar_y, fill_w, bar_h), border_radius=8)

            # Threshold line
            pygame.draw.line(screen, (255, 220, 60), (thr_x, bar_y - 10), (thr_x, bar_y + bar_h + 10), 3)

            title = font.render("Microphone level", True, (240, 240, 245))
            screen.blit(title, (margin, margin))

            level_txt = font.render(f"{level:.1f} dBFS", True, (240, 240, 245))
            screen.blit(level_txt, (margin, bar_y + bar_h + 24))

            thr_txt = font_sm.render(f"threshold {self.threshold_dbfs:.1f} dBFS", True, (255, 220, 60))
            thr_label_x = min(thr_x + 8, self.width - margin - thr_txt.get_width())
            screen.blit(thr_txt, (thr_label_x, bar_y - body_size - 14))

            mqtt_color = (50, 200, 90) if mqtt_ok else (230, 70, 60)
            mqtt_txt = font_sm.render(
                f"MQTT: {'connected' if mqtt_ok else 'disconnected'}",
                True,
                mqtt_color,
            )
            screen.blit(mqtt_txt, (margin, self.height - margin - body_size * 2 - 16))

            trig_txt = font_sm.render(f"Last trigger: {self._last_trigger_msg}", True, (180, 180, 190))
            screen.blit(trig_txt, (margin, self.height - margin - body_size))

            err = self.publisher.last_error
            if err:
                err_txt = font_sm.render(err[:80], True, (230, 70, 60))
                screen.blit(err_txt, (self.width // 2, self.height - margin - body_size * 2 - 16))

            pygame.display.flip()
            clock.tick(self.fps)

        pygame.quit()

    def stop(self) -> None:
        self._running = False
