"""Detect whether a physical display is connected (HDMI / DSI / etc.)."""

from __future__ import annotations

import os
from pathlib import Path


def drm_connectors_connected() -> list[str]:
    """Return DRM connector names whose status is 'connected' (e.g. HDMI-A-1)."""
    drm_root = Path("/sys/class/drm")
    if not drm_root.is_dir():
        return []

    connected: list[str] = []
    for status_path in sorted(drm_root.glob("card*-*/status")):
        try:
            status = status_path.read_text(encoding="utf-8").strip().lower()
        except OSError:
            continue
        if status == "connected":
            connected.append(status_path.parent.name)
    return connected


def has_graphical_session() -> bool:
    """True if a GUI session env looks available (X11 or Wayland)."""
    if os.environ.get("WAYLAND_DISPLAY") or os.environ.get("DISPLAY"):
        return True
    # Common Pi desktop defaults when started from systemd
    if Path("/tmp/.X11-unix/X0").exists():
        return True
    runtime = Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"))
    if (runtime / "wayland-0").exists() or (runtime / "wayland-1").exists():
        return True
    return False


def screen_available() -> bool:
    """
    True when a physical screen appears connected.

    Prefers DRM connector status (works headless-friendly on modern Pi OS).
    Falls back to graphical-session env if DRM is unavailable.
    """
    connectors = drm_connectors_connected()
    if connectors:
        return True
    # No DRM nodes, or all disconnected — only treat as present if a GUI session exists
    # and DRM couldn't be queried at all (e.g. unusual setups).
    drm_root = Path("/sys/class/drm")
    if drm_root.is_dir() and any(drm_root.glob("card*-*/status")):
        return False
    return has_graphical_session()


def describe_display() -> str:
    connectors = drm_connectors_connected()
    if connectors:
        return "connected: " + ", ".join(connectors)
    if has_graphical_session():
        display = os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY") or "session"
        return f"no DRM connector connected; GUI session present ({display})"
    return "no screen detected"
