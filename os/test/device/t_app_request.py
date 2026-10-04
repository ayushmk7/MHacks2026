"""The Request app on one badge with nobody paying (WP41).

In order, with the clock set (VKTIME):
  1. Push apps/request with lib/vk.lua added as vk.lua, and start it: "REQ amount <text>".
  2. UP changes the amount: a second "REQ amount <text>" with another text. The amount screen is
     saved as shots/request_amount_<theme>.png.
  3. SELECT opens the request: "REQ open <req_id>" (16 hex characters). The waiting screen is
     drawn (shots/request_waiting_<theme>.png) and differs from the amount screen.
  4. CANCEL closes the request ("REQ closed") and the app keeps running; a second CANCEL returns
     to the launcher (VKSTATE app "" and screen "launcher").
Then with the clock unset (a reset is the only way back to time=none):
  5. SELECT is refused with no_time ("REQ err no_time"); the app shows the reason
     (shots/request_no_time_<theme>.png), keeps running and logs no Lua error. CANCEL exits.

Needs one badge with a dev build. No network, no hands, no second badge. If a saved Wi-Fi network
is in range the clock may sync before step 5 can look; the step then prints a note and asserts
only that the app does not crash. The test leaves the clock set to the laptop's time. The payment
itself is t_pay_2.py.
"""

import os
import re
import time

from common import launch, provision_test, to_launcher
from fixtures import set_time
from t_app_pay import at_launcher, colours, lua_errors, push_app, theme_name

_HERE = os.path.dirname(os.path.abspath(__file__))
APP_DIR = os.path.normpath(os.path.join(_HERE, "..", "..", "apps", "request"))
SHOTS = os.path.join(_HERE, "shots")
APP = "request"

_REQ = r"\[app\] REQ "
_LUA_ERROR = r"\[lua\] .*(?:error|stopping)"
ROW_BYTES = 320 * 2

# request_open is one signature (about 0.2 s with a software key), from the frame after SELECT.
OPEN_TIMEOUT_S = 15


def req_wait(badge, pattern, timeout=10):
    """The text after 'REQ ' of the first log line since the last clear_log() that matches."""
    line = badge.wait_log("%s(?:%s)|%s" % (_REQ, pattern, _LUA_ERROR), timeout=timeout)
    found = re.search(_REQ + r"(.*)", line)
    assert found, "request failed: %s" % line
    return found.group(1).strip()


def body(shot):
    """Everything between the header (its clock may tick between two screenshots) and the footer."""
    return shot[20 * ROW_BYTES:216 * ROW_BYTES]


def start(badge):
    """Starts the app with an empty log; returns the amount it shows first."""
    badge.clear_log()
    launch(badge, APP, timeout=20)
    return req_wait(badge, r"amount \S+").split()[1]


def run(badge):
    provision_test(badge)
    to_launcher(badge)
    set_time(badge)
    assert badge.info().get("time") == "sntp", "VKTIME did not set the clock"
    push_app(badge, APP_DIR, APP)
    theme = theme_name(badge)
    os.makedirs(SHOTS, exist_ok=True)

    # ---- 1, 2. The amount ----------------------------------------------------------------------
    first = start(badge)
    badge.clear_log()
    badge.btn("up", "tap")
    second = req_wait(badge, r"amount \S+").split()[1]
    assert second != first, "UP did not change the amount: %s then %s" % (first, second)
    assert re.fullmatch(r"\d+(\.\d+)?", second), "the amount is not a decimal string: %r" % second
    time.sleep(0.3)
    amount_shot = badge.shot(os.path.join(SHOTS, "request_amount_%s.png" % theme))
    assert colours(amount_shot) >= 3, "the amount screen is blank (%d colours)" % colours(amount_shot)

    # ---- 3. Open: the waiting screen -----------------------------------------------------------
    badge.clear_log()
    badge.btn("a", "tap")
    opened = req_wait(badge, r"open \S+|err \S+", timeout=OPEN_TIMEOUT_S)
    assert re.fullmatch(r"open [0-9a-f]{16}", opened), "SELECT did not open a request: REQ %s" % opened
    time.sleep(0.8)
    waiting_shot = badge.shot(os.path.join(SHOTS, "request_waiting_%s.png" % theme))
    assert colours(waiting_shot) >= 3, "the waiting screen is blank (%d colours)" % colours(waiting_shot)
    assert body(waiting_shot) != body(amount_shot), "the waiting screen is the amount screen"
    state = badge.state()
    assert state["app"] == APP and not state["modal"], "request is not running while waiting: %s" % state

    # ---- 4. CANCEL closes, then exits ----------------------------------------------------------
    badge.clear_log()
    badge.btn("b", "tap")
    assert req_wait(badge, r"closed") == "closed"
    time.sleep(0.3)
    assert badge.state()["app"] == APP, "the first CANCEL left the app instead of closing the request"
    badge.btn("b", "tap")
    badge.wait_state(at_launcher, timeout=10)
    failed = lua_errors(badge)
    assert not failed, "request logged a Lua error: %s" % failed[0]

    # ---- 5. No clock: no_time ------------------------------------------------------------------
    badge.reset()  # the only way back to time=none; the config and the pushed app survive
    try:
        unset = badge.info().get("time") == "none"
        if not unset:
            print("note: VKINFO time=%s right after the reset, so no_time is not asserted" % badge.info().get("time"))
        start(badge)
        badge.clear_log()
        badge.btn("a", "tap")
        outcome = req_wait(badge, r"open \S+|err \S+", timeout=OPEN_TIMEOUT_S)
        if unset and outcome.startswith("open") and badge.info().get("time") != "none":
            print("note: the clock synced during the check, so no_time is not asserted")
        elif unset:
            assert outcome == "err no_time", "with the clock unset SELECT gave: REQ %s" % outcome
            time.sleep(0.5)
            no_time_shot = badge.shot(os.path.join(SHOTS, "request_no_time_%s.png" % theme))
            assert colours(no_time_shot) >= 3, "the screen is blank after no_time"
        time.sleep(1.0)
        state = badge.state()
        assert state["app"] == APP and not state["modal"], "request stopped after SELECT: %s" % state
        failed = lua_errors(badge)
        assert not failed, "request logged a Lua error: %s" % failed[0]
        if outcome.startswith("open"):
            badge.btn("b", "tap")           # close the request first
            time.sleep(0.3)
        badge.btn("b", "tap")
        badge.wait_state(at_launcher, timeout=10)
    finally:
        set_time(badge)
        to_launcher(badge)
