"""The History app (WP42; apps.md, "History"; ui.md, "Screens": Any list).

Needs one badge with a dev build (VKDEMOAPPROVE is a dev hook). No network, no hands, no second
badge. The badge is provisioned with the test values if it is not provisioned already.

In order:
  1. A record exists: VKDEMOAPPROVE green is raised and approved with SELECT (the history
     feature logs every approval that closes, a confirmation included).
  2. Push apps/history with lib/vk.lua added as vk.lua, start it with common.launch. The app
     logs "HIST n <count>" with count >= 1.
  3. The list is drawn (shots/history_list_<theme>.png). SELECT opens the detail view
     (shots/history_detail_<theme>.png), which looks different.
  4. CANCEL closes the detail view: the app is still running and shows the list again.
     A second CANCEL returns to the launcher (VKSTATE app "" and screen "launcher").

History stays installed afterwards; the launcher is on the screen.
"""

import os
import re
import time

from common import launch, provision_test, to_launcher

_HERE = os.path.dirname(os.path.abspath(__file__))
APP_DIR = os.path.normpath(os.path.join(_HERE, "..", "..", "apps", "history"))
VK_LUA = os.path.normpath(os.path.join(_HERE, "..", "..", "lib", "vk.lua"))
SHOTS = os.path.join(_HERE, "shots")
APP = "history"

WIDTH, HEIGHT = 320, 240
ROW_BYTES = WIDTH * 2
SHOT_BYTES = ROW_BYTES * HEIGHT

# What upstream's runtime logs under "[lua]" when nothing is wrong.
_LUA_BENIGN = re.compile(r"\[lua\] (?:launching '|'[^']*' started|stopped '|Lua \d)")


def theme_name(badge):
    """The active theme's name; an empty or missing config key means receipt-light."""
    reply = badge.cmd("VKGET theme")[-1]
    name = reply[3:].strip() if reply.startswith("OK ") else ""
    return name or "receipt-light"


def body(shot):
    """Everything between the header (y 0..19, whose clock may tick between two screenshots) and
    the footer's rule (y 216)."""
    return shot[20 * ROW_BYTES:216 * ROW_BYTES]


def colours(pixels):
    return len({pixels[i:i + 2] for i in range(0, len(pixels), 2)})


def take(badge, name):
    shot = badge.shot(os.path.join(SHOTS, name))
    assert len(shot) == SHOT_BYTES, "%s is %d bytes, expected %d" % (name, len(shot), SHOT_BYTES)
    return shot


def run(badge):
    os.makedirs(SHOTS, exist_ok=True)
    to_launcher(badge)
    if badge.info().get("provisioned") != "1":
        provision_test(badge)
    theme = theme_name(badge)

    # 1. One approval, answered: the record the app must find.
    badge.clear_log()
    badge.ok("VKDEMOAPPROVE green")
    badge.wait_state(lambda s: s["modal"] and s["phase"] == "ARMED", timeout=5)
    badge.btn("a", "tap")
    badge.wait_log(r"demo approval green: approved", timeout=10)
    badge.wait_state(lambda s: not s["modal"], timeout=10)

    # 2. Push and start; the app says how many records it read.
    with open(VK_LUA, "rb") as handle:
        library = handle.read()
    written = badge.push(APP_DIR, APP, extra={"vk.lua": library})
    for name in ("app.ini", "main.lua", "config.lua", "vk.lua"):
        assert name in written, "the push did not write %s: %s" % (name, written)

    badge.clear_log()
    state = launch(badge, APP, timeout=20)
    assert state["app"] == APP and not state["modal"], "history is not running after its launch: %s" % state
    line = badge.wait_log(r"\[app\] HIST n \d+", timeout=10)
    count = int(re.search(r"HIST n (\d+)", line).group(1))
    assert count >= 1, "history read %d records after an approved demo approval" % count

    # 3. The list, then the detail view.
    time.sleep(0.5)  # the first frames
    listed = take(badge, "history_list_%s.png" % theme)
    assert colours(body(listed)) >= 3, "the history list looks blank: %d colours under the header" % (
        colours(body(listed)))
    badge.btn("a", "tap")
    time.sleep(0.3)
    detail = take(badge, "history_detail_%s.png" % theme)
    assert colours(body(detail)) >= 3, "the detail view looks blank: %d colours under the header" % (
        colours(body(detail)))
    assert body(detail) != body(listed), "SELECT on a row did not open the detail view: the screen is unchanged"
    assert badge.state()["app"] == APP, "history stopped when SELECT was pressed"

    # 4. CANCEL: back to the list, then out to the launcher.
    badge.btn("b", "tap")
    time.sleep(0.4)
    state = badge.state()
    assert state["app"] == APP, "the first CANCEL closed the app (app %r), not the detail view" % state["app"]
    again = badge.shot()
    assert body(again) == body(listed), "after CANCEL in the detail view the list was not drawn again as before"

    badge.btn("b", "tap")
    badge.wait_state(lambda s: s["app"] == "" and s.get("screen") == "launcher", timeout=5)

    failed = [entry for entry in badge.log() if "[lua]" in entry and not _LUA_BENIGN.search(entry)]
    assert not failed, "history logged a Lua error: %s" % failed[0]
    to_launcher(badge)
