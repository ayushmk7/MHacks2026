"""The Pay app on one badge with nobody asking (WP41).

In order:
  1. Push apps/pay with lib/vk.lua added as vk.lua, and start it.
  2. With no request in range the app logs "PAY list 0" and draws the empty list ("No requests
     nearby"); the screen is saved as shots/pay_<theme>.png.
  3. It runs for 10 s with no "[lua]" error line: reading wallet.requests() every 500 ms.
  4. CANCEL returns to the launcher (VKSTATE app "" and screen "launcher").

Needs one badge with a dev build. No network, no hands, no second badge. If another badge in range
has a request open, step 2 fails and says so: close that request and run the test again. The
payment itself is t_pay_2.py.
"""

import os
import re
import time

from common import launch, provision_test, to_launcher

_HERE = os.path.dirname(os.path.abspath(__file__))
APP_DIR = os.path.normpath(os.path.join(_HERE, "..", "..", "apps", "pay"))
VK_LUA = os.path.normpath(os.path.join(_HERE, "..", "..", "lib", "vk.lua"))
SHOTS = os.path.join(_HERE, "shots")
APP = "pay"

_LUA_ERROR = r"\[lua\] .*(?:error|stopping)"
QUIET_S = 10


def theme_name(badge):
    """The active theme's name; config `theme` is empty until someone sets it."""
    try:
        name = badge.ok("VKGET theme").strip()
    except AssertionError:
        name = ""
    return name or "receipt-light"


def colours(pixels):
    return len({pixels[i:i + 2] for i in range(0, len(pixels), 2)})


def lua_errors(badge):
    return [line for line in badge.log() if re.search(_LUA_ERROR, line)]


def push_app(badge, folder, app_id):
    """Pushes an app folder with the shared library beside it (an app can only require files in
    its own folder)."""
    with open(VK_LUA, "rb") as handle:
        library = handle.read()
    written = badge.push(folder, app_id, extra={"vk.lua": library})
    assert "vk.lua" in written and "main.lua" in written and "config.lua" in written, (
        "the push did not write the app and the library: %s" % written)


def at_launcher(state):
    return state["app"] == "" and state["screen"] == "launcher"


def run(badge):
    provision_test(badge)
    to_launcher(badge)
    push_app(badge, APP_DIR, APP)

    # ---- 1, 2. The empty list ------------------------------------------------------------------
    badge.clear_log()
    launch(badge, APP, timeout=20)
    line = badge.wait_log(r"\[app\] PAY list \d+|%s" % _LUA_ERROR, timeout=10)
    assert "PAY list" in line, "pay failed on start: %s" % line
    assert line.rstrip().endswith("PAY list 0"), (
        "pay lists a request (%s): another badge in range has one open. Close it and run again" % line)

    os.makedirs(SHOTS, exist_ok=True)
    time.sleep(0.5)
    shot = badge.shot(os.path.join(SHOTS, "pay_%s.png" % theme_name(badge)))
    assert colours(shot) >= 3, "the pay screen is blank (%d colours)" % colours(shot)

    # ---- 3. Ten quiet seconds ------------------------------------------------------------------
    deadline = time.monotonic() + QUIET_S
    while time.monotonic() < deadline:
        failed = lua_errors(badge)
        assert not failed, "pay logged a Lua error: %s" % failed[0]
        time.sleep(0.5)
    state = badge.state()
    assert state["app"] == APP and not state["modal"], "pay is not running after %d s: %s" % (QUIET_S, state)

    # ---- 4. CANCEL exits -----------------------------------------------------------------------
    badge.btn("b", "tap")
    badge.wait_state(at_launcher, timeout=10)
    failed = lua_errors(badge)
    assert not failed, "pay logged a Lua error on exit: %s" % failed[0]
