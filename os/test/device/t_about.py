"""Settings -> About (shell.md, "About"): the page with the QR code of config key `repo_url`.

Needs one badge with a dev build that has the About page (the boot line says pages=14). No
network, no hands. To read the code back the laptop needs OpenCV in the repository's .venv
(`pip install opencv-python-headless`); without it the decode step is skipped and the test says so.

In order:
  1. `repo_url` is set (the test value of common.test_config() when the badge has none).
  2. From the launcher: CANCEL opens `settings`, DOWN to the last row, SELECT opens `about`
     (VKSTATE screen == "about").
  3. In receipt-light and in receipt-dark: the screenshot shots/shell_about_<theme>.png holds a
     QR code that decodes to exactly `repo_url`, and the code's patch is the light theme's paper
     in both (a phone reads dark on light only). The size of one module is printed.
  4. With `repo_url` empty the page says so and holds no code; the link is then put back and the
     code returns, without leaving the page.
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
LIGHT_INK = 0x18C2          # receipt-light INK (#1B1A17)
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


def module_px(rows):
    """The width of one QR module in screen pixels. The first row of the body that holds the
    code's ink is the top of the code, and the first run of ink in it is the top bar of the left
    finder pattern, which is 7 modules wide."""
    for row in rows:
        if LIGHT_INK not in row:
            continue
        start = row.index(LIGHT_INK)
        run = 0
        while start + run < len(row) and row[start + run] == LIGHT_INK:
            run += 1
        assert run % 7 == 0, "the finder pattern's bar is %d px wide, not 7 whole modules" % run
        return run // 7
    return 0


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

        # 3. Both themes: the code reads back as the link, on a light patch.
        decoder = True
        for theme in THEMES:
            put(badge, "theme", theme)
            time.sleep(REDRAW_S)
            shot = badge.shot(os.path.join(SHOTS, "shell_about_%s.png" % theme))
            rows = body_pixels(shot)
            light = sum(row.count(LIGHT_PAPER) for row in rows)
            assert light > 8000, "%s: the code's patch is not the light paper (%d such pixels)" % (theme, light)
            size = module_px(rows)
            assert size >= 3, "%s: a module is %d px wide, too small for a phone" % (theme, size)
            text = qr_decode(shot)
            if text is None:
                decoder = False
                print("t_about: %s: no QR decoder installed (opencv); module %d px" % (theme, size))
            else:
                assert text == url, "%s: the code decodes to %r, expected %r" % (theme, text, url)
                print("t_about: %s: decoded %r, module %d px" % (theme, text, size))

        # 4. No link: the page says so, with no code. Then the link again, on the same page.
        put(badge, "repo_url", "")
        time.sleep(REDRAW_S)
        empty = badge.shot()
        # The theme is receipt-dark here, so nothing else on the page is the light paper.
        assert sum(row.count(LIGHT_PAPER) for row in body_pixels(empty)) == 0, (
            "with no link the body still holds the code's light patch")
        if decoder:
            assert qr_decode(empty) == "", "with no link a code is still on the screen"
        assert badge.state()["screen"] == "about", "the page closed when the link was removed"
        put(badge, "repo_url", url)
        time.sleep(REDRAW_S)
        again = badge.shot()
        assert again != empty, "the code did not come back when the link was set again"
        if decoder:
            assert qr_decode(again) == url, "the code that came back does not decode to the link"

        # 5. Back.
        badge.btn("b", "tap")
        badge.wait_state(lambda s: s["app"] == "" and s.get("screen") == "settings", timeout=5)
    finally:
        put(badge, "repo_url", url)
        put(badge, "theme", theme_before)
    to_launcher(badge)
