"""Settings pages, group 1 (WP37; docs/os/ui/shell.md, "Settings pages"): Theme, Wi-Fi, Bluetooth,
ESP-NOW, App push.

Navigation is by VKSTATE's `screen` field, never by comparing screenshots. From the launcher,
CANCEL opens `settings` with the cursor on the first row; the rows are in the order of their
`order` value (theme 10, wifi 20, bluetooth 30, espnow 40, push 50), so row n is n DOWN taps away.

  1. wifi, bluetooth, espnow, push: SELECT opens the screen with that name; a screenshot is saved
     as shots/shell_<id>_<theme>.png and is not one flat colour; CANCEL returns to `settings`.
  2. Theme (an action row): SELECT changes config key `theme`, the screen stays `settings` and the
     picture changes; further SELECTs come back to the first theme (one more, with two themes).
  3. ESP-NOW: SELECT toggles the radio (the picture changes); a second SELECT puts it back.

Not done here, because the test tooling or the test network depends on them: starting the hotspot,
disconnecting or forgetting Wi-Fi (the page is opened and left; no row is selected), and
"New pairing code". The Bluetooth and App push pages are opened and left without a SELECT.

Needs one badge with a dev build. No network, no hands. Run it once per theme to get both sets of
pictures.
"""

import os
import struct
import time

from common import to_launcher

SHOTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "shots")
SHOT_BYTES = 320 * 240 * 2

# (screen id, DOWN taps from the top of the Settings list)
PAGES = [("wifi", 1), ("bluetooth", 2), ("espnow", 3), ("push", 4)]
MAX_THEMES = 8


def theme_name(badge):
    """The active theme's name; an empty or missing config key means receipt-light."""
    reply = badge.cmd("VKGET theme")[-1]
    name = reply[3:].strip() if reply.startswith("OK ") else ""
    return name or "receipt-light"


def _not_uniform(pixels, what):
    assert len(pixels) == SHOT_BYTES, "%s: screenshot is %d bytes" % (what, len(pixels))
    colours = set(struct.unpack("<%dH" % (len(pixels) // 2), pixels))
    assert len(colours) > 1, "%s: the screen is one flat colour (0x%04X)" % (what, next(iter(colours)))


def _screen(badge, name, timeout=5):
    return badge.wait_state(lambda s: s.get("screen") == name and not s["app"], timeout=timeout)


def open_settings(badge):
    """The Settings list, entered from the launcher, so its cursor is on the first row (Theme)."""
    to_launcher(badge)
    _screen(badge, "launcher")
    badge.btn("b", "tap")
    _screen(badge, "settings")


def open_page(badge, page, downs):
    open_settings(badge)
    for _ in range(downs):
        badge.btn("down", "tap")
    badge.btn("a", "tap")
    try:
        _screen(badge, page)
    except Exception as error:
        raise AssertionError("SELECT on Settings row %d did not open screen '%s': %s"
                             % (downs + 1, page, error))


def close_page(badge, page):
    badge.btn("b", "tap")
    try:
        _screen(badge, "settings")
    except Exception as error:
        raise AssertionError("CANCEL on '%s' did not return to 'settings': %s" % (page, error))


def pages(badge, theme):
    for page, downs in PAGES:
        open_page(badge, page, downs)
        time.sleep(0.3)   # one repaint
        picture = badge.shot(os.path.join(SHOTS, "shell_%s_%s.png" % (page, theme)))
        _not_uniform(picture, page)
        assert badge.state().get("screen") == page, "'%s' closed by itself" % page
        close_page(badge, page)


def theme_row(badge, first):
    open_settings(badge)          # the cursor is on Theme
    before = badge.shot()
    _not_uniform(before, "settings")

    badge.btn("a", "tap")
    time.sleep(0.6)               # the framework repaints a theme change within 500 ms
    second = theme_name(badge)
    try:
        assert second != first, "SELECT on Theme left config key theme at '%s'" % first
        assert badge.state().get("screen") == "settings", (
            "SELECT on Theme left the Settings list: screen '%s'" % badge.state().get("screen"))
        after = badge.shot()      # not saved: shell_settings_<theme>.png belongs to t_shell.py
        assert after != before, "the Settings list looks the same in '%s' and '%s'" % (first, second)
    finally:
        # Back to the first theme: one more SELECT with two themes registered.
        presses = 0
        while theme_name(badge) != first and presses < MAX_THEMES:
            badge.btn("a", "tap")
            time.sleep(0.3)
            presses += 1
    assert theme_name(badge) == first, "cycling Theme never came back to '%s'" % first
    assert badge.state().get("screen") == "settings", "cycling Theme left the Settings list"
    close = badge.shot()
    assert close != after, "the Settings list did not repaint when the theme came back to '%s'" % first


def espnow_toggle(badge):
    open_page(badge, "espnow", 3)
    time.sleep(0.3)
    original = badge.shot()
    badge.btn("a", "tap")         # on -> off, or off -> on
    try:
        time.sleep(0.5)
        toggled = badge.shot()
        assert toggled != original, "SELECT on ESP-NOW did not change the screen"
        assert badge.state().get("screen") == "espnow", "SELECT on ESP-NOW left the page"
    finally:
        badge.btn("a", "tap")     # back to the state the badge was in
        time.sleep(0.5)
    restored = badge.shot()
    assert restored != toggled, "the second SELECT on ESP-NOW did not change the screen back"
    close_page(badge, "espnow")


def run(badge):
    theme = theme_name(badge)
    try:
        pages(badge, theme)
        theme_row(badge, theme)
        espnow_toggle(badge)
    finally:
        to_launcher(badge)
    assert badge.state().get("screen") == "launcher", "the test did not end on the launcher"
