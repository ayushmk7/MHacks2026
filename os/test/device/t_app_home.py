"""The Home app (WP40; apps.md, "Home"; ui.md, "Screens": Home).

Needs one badge with a dev build and NO network (Wi-Fi off, or not joined): the point of step 3
is that Home stays up and quiet when its balance fetch cannot work. No hands, no second badge.
The badge is provisioned with the test values if it is not provisioned already.

In order:
  1. Push apps/home with lib/vk.lua added as vk.lua, start it with common.launch.
  2. The app logs "HOME addr <short>" once; <short> is the first four and the last four
     characters of VKINFO pubkey, joined by "..".
  3. The screen is drawn: shots/home_<theme>.png holds more than paper and ink.
  4. For 10 s the log has no "[lua]" error line, the app is still running and no approval opened.
  5. CANCEL returns to the launcher (VKSTATE app "" and screen "launcher").

Home stays installed afterwards; the launcher is on the screen.
"""

import os
import re
import time

from common import launch, provision_test, to_launcher

_HERE = os.path.dirname(os.path.abspath(__file__))
APP_DIR = os.path.normpath(os.path.join(_HERE, "..", "..", "apps", "home"))
VK_LUA = os.path.normpath(os.path.join(_HERE, "..", "..", "lib", "vk.lua"))
SHOTS = os.path.join(_HERE, "shots")
APP = "home"

QUIET_S = 10

# What upstream's runtime logs under "[lua]" when nothing is wrong.
_LUA_BENIGN = re.compile(r"\[lua\] (?:launching '|'[^']*' started|stopped '|Lua \d)")


def theme_name(badge):
    """The active theme's name; an empty or missing config key means receipt-light."""
    reply = badge.cmd("VKGET theme")[-1]
    name = reply[3:].strip() if reply.startswith("OK ") else ""
    return name or "receipt-light"


def colours(picture):
    """How many different RGB565 values a screenshot holds."""
    return len({picture[i:i + 2] for i in range(0, len(picture), 2)})


def lua_errors(badge):
    return [line for line in badge.log() if "[lua]" in line and not _LUA_BENIGN.search(line)]


def run(badge):
    os.makedirs(SHOTS, exist_ok=True)
    to_launcher(badge)
    if badge.info().get("provisioned") != "1":
        provision_test(badge)

    # 1. Push and start.
    with open(VK_LUA, "rb") as handle:
        library = handle.read()
    written = badge.push(APP_DIR, APP, extra={"vk.lua": library})
    for name in ("app.ini", "main.lua", "config.lua", "vk.lua"):
        assert name in written, "the push did not write %s: %s" % (name, written)

    badge.clear_log()
    state = launch(badge, APP, timeout=20)
    assert state["app"] == APP and not state["modal"], "home is not running after its launch: %s" % state

    # 2. The address the app shows is this badge's.
    line = badge.wait_log(r"\[app\] HOME addr \S+", timeout=10)
    shown = re.search(r"HOME addr (\S+)", line).group(1)
    pubkey = badge.info().get("pubkey", "")
    assert len(pubkey) > 10, "VKINFO pubkey is %r" % pubkey
    expected = pubkey[:4] + ".." + pubkey[-4:]
    assert shown == expected, "home shows the address %r, VKINFO pubkey gives %r" % (shown, expected)

    # 3. The screen.
    time.sleep(0.5)  # the first frames
    shot = badge.shot(os.path.join(SHOTS, "home_%s.png" % theme_name(badge)))
    assert colours(shot) >= 3, "the home screen looks blank: %d colours" % colours(shot)

    # 4. Ten quiet seconds with no network: no Lua error, still running, nothing opened.
    deadline = time.monotonic() + QUIET_S
    while time.monotonic() < deadline:
        failed = lua_errors(badge)
        assert not failed, "home logged a Lua error: %s" % failed[0]
        state = badge.state()
        assert state["app"] == APP, "home stopped by itself (app %r); log: %s" % (state["app"], badge.log()[-5:])
        assert not state["modal"], "home opened an approval (title %r)" % state.get("title")
        time.sleep(0.5)
    failed = lua_errors(badge)
    assert not failed, "home logged a Lua error: %s" % failed[0]
    once = [entry for entry in badge.log() if "HOME addr " in entry]
    assert len(once) == 1, "home logged its address %d times, expected once" % len(once)

    # 5. CANCEL goes back to the launcher, which is the firmware's shell.
    badge.btn("b", "tap")
    badge.wait_state(lambda s: s["app"] == "" and s.get("screen") == "launcher", timeout=5)
    to_launcher(badge)
