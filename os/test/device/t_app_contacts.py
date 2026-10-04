"""The Contacts app (WP43; docs/os/apps/apps.md, "Contacts"), on one badge.

In order:
  1. Push apps/contacts with lib/vk.lua added as vk.lua, start it, read "CON list <n>".
  2. A badge that already holds contacts (t_con_single.py leaves one) is emptied through the app
     itself: DOWN to the first contact, RIGHT (the confirm line), SELECT. Each removal logs
     "CON removed <short>" and a new "CON list <n>". The list then logs "CON list 0".
  3. The empty list is on screen (saved as shots/contacts_list_<theme>.png, not uniform).
  4. SELECT on "Swap contacts" enters swap mode ("CON swap on"); the screen changes (saved as
     shots/contacts_swap_<theme>.png). CANCEL leaves it ("CON swap off") and the app still runs.
  5. CANCEL exits: VKSTATE app is "" and screen is "launcher".
  6. No Lua error was logged.

The swap itself (HELLO, CARD, accept on both sides) needs two badges and is t_con.py's job.
Needs one badge with a dev build. No hands, no network. Leaves the badge provisioned with the test
values, contacts installed, no contacts stored, and the launcher on the screen.
"""

import os
import re
import time

from common import launch, provision_test, to_launcher

_HERE = os.path.dirname(os.path.abspath(__file__))
APP_DIR = os.path.normpath(os.path.join(_HERE, "..", "..", "apps", "contacts"))
VK_LUA = os.path.normpath(os.path.join(_HERE, "..", "..", "lib", "vk.lua"))
SHOTS = os.path.join(_HERE, "shots")
APP = "contacts"

_LUA_ERROR = r"\[lua\] .*(?:error|stopping)"
_LIST = r"\[app\] CON list (\d+)"

# More contacts than this on the badge and the test gives up emptying the list.
MAX_REMOVALS = 40


def colours(picture):
    """How many different RGB565 values a screenshot holds."""
    return len({picture[i:i + 2] for i in range(0, len(picture), 2)})


def theme_name(badge):
    try:
        return badge.ok("VKGET theme").strip() or "receipt-light"   # empty = the default theme
    except Exception:  # noqa: BLE001  (an older build without the key: the name is only a file name)
        return "receipt-light"


def no_lua_error(badge):
    failed = [line for line in badge.log() if re.search(_LUA_ERROR, line)]
    assert not failed, "contacts logged a Lua error: %s" % failed[0]


def listed(badge, timeout=10):
    """The count in the first "CON list <n>" line since the last clear_log()."""
    return int(re.search(_LIST, badge.wait_log(_LIST, timeout=timeout)).group(1))


def on_launcher(state):
    return state["app"] == "" and state["screen"] == "launcher"


def run(badge):
    provision_test(badge)
    to_launcher(badge)

    # 1. The library is pushed into the app's own folder.
    with open(VK_LUA, "rb") as handle:
        library = handle.read()
    written = badge.push(APP_DIR, APP, extra={"vk.lua": library})
    assert "main.lua" in written and "config.lua" in written and "vk.lua" in written, (
        "the push did not write the app and the library: %s" % written)

    badge.clear_log()
    launch(badge, APP, timeout=20)
    count = listed(badge)

    # 2. Empty the list through the app: the cursor starts on "Swap contacts", the newest contact
    #    is the row under it.
    removals = 0
    while count > 0:
        assert removals < MAX_REMOVALS, "still %d contacts after %d removals" % (count, removals)
        no_lua_error(badge)
        badge.clear_log()
        badge.btn("down")
        badge.btn("right")       # the confirm line
        badge.btn("a")           # confirmed
        badge.wait_log(r"\[app\] CON removed \S+", timeout=5)
        after = listed(badge, timeout=5)
        assert after == count - 1, "removing one contact left %d of %d" % (after, count)
        count = after
        removals += 1
    if removals:
        # A fresh start, so that the line the brief names is the one the app logs on start.
        badge.stop()
        badge.wait_state(lambda s: s["app"] != APP, timeout=5)
        no_lua_error(badge)
        badge.clear_log()
        launch(badge, APP, timeout=20)
        count = listed(badge)
    assert count == 0, "the list is not empty: CON list %d" % count

    # 3. The empty list.
    os.makedirs(SHOTS, exist_ok=True)
    theme = theme_name(badge)
    time.sleep(0.3)
    list_shot = badge.shot(os.path.join(SHOTS, "contacts_list_%s.png" % theme))
    assert colours(list_shot) >= 2, "the contacts list is a uniform screen"

    # RIGHT on "Swap contacts" must not open a confirm line, and SELECT then must still swap.
    badge.btn("right")

    # 4. Swap mode, and back.
    no_lua_error(badge)
    badge.clear_log()
    badge.btn("a")
    badge.wait_log(r"\[app\] CON swap on", timeout=5)
    time.sleep(0.3)
    swap_shot = badge.shot(os.path.join(SHOTS, "contacts_swap_%s.png" % theme))
    assert swap_shot != list_shot, "the screen did not change when swap mode started"
    time.sleep(2.5)              # a few HELLO broadcasts with nobody answering
    state = badge.state()
    assert state["app"] == APP and not state["modal"], "contacts is not running in swap mode: %s" % state

    badge.btn("b")
    badge.wait_log(r"\[app\] CON swap off", timeout=5)
    assert listed(badge, timeout=5) == 0, "the list is not empty after leaving swap mode"
    state = badge.state()
    assert state["app"] == APP, "CANCEL in swap mode left the app instead of going back: %s" % state

    # 5. CANCEL on the list exits to the shell's launcher.
    badge.btn("b")
    badge.wait_state(on_launcher, timeout=5)

    # 6.
    no_lua_error(badge)
    to_launcher(badge)
