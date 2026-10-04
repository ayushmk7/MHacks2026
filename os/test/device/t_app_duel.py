"""The Duel app on one badge (WP45): title, choosing a stake, an invitation nobody answers.

In order:
  1. Push apps/duel with lib/vk.lua added as vk.lua, start it: it logs "DUEL title" and the title
     screen is drawn (saved as shots/duel_<theme>.png; not a blank screen).
  2. DOWN chooses another stake: the app logs "DUEL stake <text>" with a stake of its config.lua,
     and the screen changes.
  3. SELECT invites: "DUEL invite <game id>". With no other badge running Duel the invitation
     times out back to the title: "DUEL invite timeout" within config.invite_timeout_s, the app
     still running and no Lua error logged.
  4. CANCEL on the title exits to the launcher (VKSTATE app "" and screen "launcher").

Needs one badge with a dev build. No network, no hands, no second badge. No other badge running
Duel may be in range: an accepted invitation starts a duel instead of timing out. The test
provisions the badge with the test values and sets its clock (Duel refuses to invite while the
badge could not settle a stake: "DUEL not ready <reason>"). It leaves duel installed.

The two-badge half (invite, accept, play, settle) is t_duel_2.py.
"""

import os
import re
import time

from common import launch, provision_test, to_launcher

_HERE = os.path.dirname(os.path.abspath(__file__))
DUEL_DIR = os.path.normpath(os.path.join(_HERE, "..", "..", "apps", "duel"))
VK_LUA = os.path.normpath(os.path.join(_HERE, "..", "..", "lib", "vk.lua"))
SHOTS = os.path.join(_HERE, "shots")
DUEL = "duel"

_D = r"\[app\] DUEL "
_LUA_ERROR = r"\[lua\] .*(?:error|stopping)"


def config_value(name, default):
    """An integer tunable of apps/duel/config.lua."""
    with open(os.path.join(DUEL_DIR, "config.lua"), "r", encoding="utf-8") as handle:
        match = re.search(r"^\s*%s\s*=\s*(\d+)" % re.escape(name), handle.read(), re.MULTILINE)
    return int(match.group(1)) if match else default


def config_stakes():
    """The stakes of apps/duel/config.lua, in order."""
    with open(os.path.join(DUEL_DIR, "config.lua"), "r", encoding="utf-8") as handle:
        match = re.search(r"^\s*stakes\s*=\s*\{([^}]*)\}", handle.read(), re.MULTILINE)
    assert match, "apps/duel/config.lua has no stakes list"
    return re.findall(r'"([^"]+)"', match.group(1))


def push_duel(badge):
    with open(VK_LUA, "rb") as handle:
        badge.push(DUEL_DIR, DUEL, extra={"vk.lua": handle.read()})


def theme_name(badge):
    try:
        return badge.ok("VKGET theme").strip() or "receipt-light"
    except Exception:
        return "receipt-light"


def colours(picture):
    """How many different RGB565 values a screenshot holds."""
    return len({picture[i:i + 2] for i in range(0, len(picture), 2)})


def no_lua_error(badge):
    failed = [line for line in badge.log() if re.search(_LUA_ERROR, line)]
    assert not failed, "duel stopped with a Lua error: %s" % failed[0]


def run(badge):
    to_launcher(badge)
    provision_test(badge)
    badge.ok("VKTIME %d" % int(time.time()))

    # ---- 1. The title screen -------------------------------------------------------------------
    push_duel(badge)
    badge.clear_log()
    launch(badge, DUEL)
    badge.wait_log(_D + r"title", timeout=10)
    time.sleep(0.3)   # one frame, so the title is on the screen
    os.makedirs(SHOTS, exist_ok=True)
    first = badge.shot(os.path.join(SHOTS, "duel_%s.png" % theme_name(badge)))
    assert colours(first) >= 2, "the duel title screen is blank"

    # ---- 2. A stake can be chosen --------------------------------------------------------------
    stakes = config_stakes()
    badge.btn("down")
    line = badge.wait_log(_D + r"stake \S+", timeout=5)
    chosen = re.search(_D + r"stake (\S+)", line).group(1)
    assert chosen in stakes, "the app chose stake %r, which is not in config.lua (%s)" % (chosen, stakes)
    assert badge.shot() != first, "the title screen did not change when a stake was chosen"

    # ---- 3. An invitation with nobody around times out back to the title -----------------------
    badge.clear_log()
    badge.btn("a")
    time.sleep(0.5)
    refused = [line for line in badge.log() if re.search(_D + r"not ready", line)]
    assert not refused, "duel refused to invite on a provisioned badge with the clock set: %s" % refused[0]
    badge.wait_log(_D + r"invite [0-9a-f]{16}", timeout=5)
    assert badge.shot() != first, "the inviting screen is the same as the title screen"
    badge.wait_log(_D + r"invite timeout", timeout=config_value("invite_timeout_s", 15) + 10)
    badge.wait_log(_D + r"title", timeout=5)
    no_lua_error(badge)
    state = badge.state()
    assert state["app"] == DUEL and not state["modal"], "duel is not running after the timeout: %s" % state

    # ---- 4. CANCEL exits to the launcher -------------------------------------------------------
    badge.btn("b")
    badge.wait_state(lambda s: s["app"] == "" and s["screen"] == "launcher", timeout=5)
    no_lua_error(badge)
