"""Settings -> About (shell.md, "About"): the page with the link of config key `repo_url` as text
(the QR code of it is on the launcher, t_barcode.py).

Needs one badge with a dev build that has the About page (the boot line says pages=14). No
network, no hands. With OpenCV in the repository's .venv the screenshots are also checked for no
QR code; without it that check is skipped.

In order:
  1. `repo_url` is set (the test value of common.test_config() when the badge has none).
  2. From the launcher: CANCEL opens `settings`, DOWN to the last row, SELECT opens `about`
     (VKSTATE screen == "about").
  3. In receipt-light and in receipt-dark: the screenshot shots/shell_about_<theme>.png holds no
     QR code, and in receipt-dark no light patch.
  4. With `repo_url` empty the page changes (it says so); the link is then put back and the page
     changes again, without leaving the page.
  5. CANCEL returns to `settings`.

Leaves `repo_url` and the theme as it found them (the link stays set if the test set it), and the
launcher on the screen.
"""

import os
import struct
import time

from common import goto_screen, qr_decode, test_config, to_launcher

SHOTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "shots")
WIDTH, HEIGHT = 320, 240
THEMES = ("receipt-light", "receipt-dark")
LIGHT_PAPER = 0xF77C        # receipt-light PAPER (#F3EFE4) as RGB565
BODY_X = 147                # the body column: right of the perforation
REDRAW_S = 0.8              # the page looks at the link, and the shell at the theme, every 500 ms


def get(badge, key):
    reply = badge.cmd("VKGET %s" % key)[-1]
    assert reply.startswith("OK"), "VKGET %s -> %s" % (key, reply)
    return reply[3:].strip()


def put(badge, key, value):
    reply = badge.cmd(("VKSET %s %s" % (key, value)).strip())[-1]
    assert reply == "OK", "VKSET %s %s -> %s" % (key, value, reply)


def body_pixels(shot):
    """The RGB565 values of the body column between the header and the footer, row by row."""
    values = struct.unpack("<%dH" % (WIDTH * HEIGHT), shot)
    return [values[y * WIDTH + BODY_X:(y + 1) * WIDTH] for y in range(20, 216)]


def run(badge):
    os.makedirs(SHOTS, exist_ok=True)
    to_launcher(badge)
    theme_before = get(badge, "theme")

    # 1. A link to show.
    url = get(badge, "repo_url")
    if not url:
        url = test_config()["repo_url"]
        put(badge, "repo_url", url)

    try:
        # 2. The page.
        state = goto_screen(badge, "about")
        assert state["screen"] == "about" and state["app"] == "", "not on About: %s" % state

        # 3. Both themes: the link as text, no code.
        for theme in THEMES:
            put(badge, "theme", theme)
            time.sleep(REDRAW_S)
            shot = badge.shot(os.path.join(SHOTS, "shell_about_%s.png" % theme))
            if theme == "receipt-dark":
                light = sum(row.count(LIGHT_PAPER) for row in body_pixels(shot))
                assert light == 0, "%s: the body holds a light patch (%d such pixels)" % (theme, light)
            text = qr_decode(shot)
            if text is None:
                print("t_about: %s: no QR decoder installed (opencv)" % theme)
            else:
                assert text == "", "%s: the page still shows a code (%r)" % (theme, text)

        # 4. No link: the page says so. Then the link again, on the same page.
        shown = badge.shot()
        put(badge, "repo_url", "")
        time.sleep(REDRAW_S)
        empty = badge.shot()
        assert empty != shown, "the page did not change when the link was removed"
        assert badge.state()["screen"] == "about", "the page closed when the link was removed"
        put(badge, "repo_url", url)
        time.sleep(REDRAW_S)
        again = badge.shot()
        assert again != empty, "the link did not come back when it was set again"

        # 5. Back.
        badge.btn("b", "tap")
        badge.wait_state(lambda s: s["app"] == "" and s.get("screen") == "settings", timeout=5)
    finally:
        put(badge, "repo_url", url)
        put(badge, "theme", theme_before)
    to_launcher(badge)
