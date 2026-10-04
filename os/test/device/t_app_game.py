"""The Game app (WP44; docs/os/apps/apps.md, "Game"), on one badge.

In order:
  1. Push apps/game with lib/vk.lua added as vk.lua, start it: "GAME title", the title screen is
     drawn (saved as shots/game_title_<theme>.png, not blank).
  2. SELECT on Play starts a run ("GAME play"); the score advances once a second ("GAME score 1",
     "GAME score 2", about a second apart); the playfield is saved as shots/game_play_<theme>.png.
  3. Nobody moves the player, and a share of the blocks fall straight at it (config
     spawn.aim_percent), so the run ends ("GAME over <n>") and the title returns ("GAME title").
  4. With config key shop_address empty the app logs "GAME shop closed" and the title is Play only.
     With shop_address set (to the test issuer key: well formed, but no shop's record), restarted,
     it logs "GAME shop open" and lists the items. DOWN, SELECT on the first shop item starts a
     purchase ("GAME shop <id> <price>"); the badge has no network, so the purchase ends in a
     failure the screen shows ("GAME buy failed <reason>", saved as
     shots/game_buy_failed_<theme>.png). No approval opens and the app keeps running.
  5. CANCEL goes back to the title; CANCEL again exits: VKSTATE app is "" and screen is "launcher".
  6. No Lua error was logged.

Needs one badge with a dev build on which the first shop item was never bought (an owned item
cannot be bought again). No hands. No network is needed; with a network the purchase still fails
(the issuer key has no shop record). Leaves the badge provisioned with the test values,
shop_address empty, game installed, and the launcher on the screen.

The evil-game checks are not in this file: they need the registry (a signed record for the shop)
over a network, a funded token account, and shop_address set to that shop. For the integrator
or a later test, with apps/evilgame assembled as scripts/push-apps.sh does (its app.ini says
include=game: apps/game's files except config.lua, plus evilgame's app.ini and config.lua, plus
vk.lua):
  - evil = "amount": start evilgame, DOWN, SELECT on "Buy sword". The game's own screen says
    "Buy sword: 5.00 HACK". The firmware approval that opens (VKSTATE modal) must show the true
    amount, 500.00 HACK, with the amber headline VERIFIED - NOT PRESENT and the shop's verified
    name, and must ask for a hold because 500.00 is over the cap (100.00 under test provisioning).
    CANCEL there gives "GAME buy failed cancelled" and the item stays locked.
  - evil = "recipient" (the transfer goes to the badge's own address, not the shop's): the approval
    must be red, headline WRONG RECIPIENT, and cannot be signed: an injected hold on SELECT does
    nothing, and closing it gives "GAME buy failed mismatch".
  - In both cases wallet.history (the History app) records the attempt with app "evilgame".
  - The honest game against the same registry: "Buy shield" opens an amber approval showing
    5.00 HACK; a hold signs it, the log has "GAME buy done shield", and after a restart of the app
    the row reads "owned".
"""

import os
import re
import time

from common import launch, provision_test, to_launcher

_HERE = os.path.dirname(os.path.abspath(__file__))
APP_DIR = os.path.normpath(os.path.join(_HERE, "..", "..", "apps", "game"))
VK_LUA = os.path.normpath(os.path.join(_HERE, "..", "..", "lib", "vk.lua"))
SHOTS = os.path.join(_HERE, "shots")
APP = "game"

_LUA_ERROR = r"\[lua\] .*(?:error|stopping)"

# An idle player is hit by an aimed block within a few seconds; unlocked shields add a few more.
GAME_OVER_TIMEOUT_S = 120
# The first step of a purchase with no network waits for the balance fetch to give up (about 3 s);
# a network whose node does not answer costs one HTTP timeout per step.
BUY_TIMEOUT_S = 60


def colours(picture):
    """How many different RGB565 values a screenshot holds."""
    return len({picture[i:i + 2] for i in range(0, len(picture), 2)})


def theme_name(badge):
    try:
        return badge.ok("VKGET theme").strip() or "receipt-light"   # empty = the default theme
    except Exception:  # noqa: BLE001  (an older build without the key: the name is only a file name)
        return "receipt-light"


def on_launcher(state):
    return state["app"] == "" and state["screen"] == "launcher"


def no_lua_error(badge):
    failed = [line for line in badge.log() if re.search(_LUA_ERROR, line)]
    assert not failed, "game logged a Lua error: %s" % failed[0]


def running(badge, what):
    state = badge.state()
    assert state["app"] == APP and not state["modal"], "%s: game is not running: %s" % (what, state)


def run(badge):
    provision_values = provision_test(badge)
    to_launcher(badge)
    # The shop is closed until step 4 opens it.
    assert badge.cmd("VKSET shop_address")[-1] == "OK", "VKSET shop_address (empty)"

    with open(VK_LUA, "rb") as handle:
        library = handle.read()
    written = badge.push(APP_DIR, APP, extra={"vk.lua": library})
    assert "main.lua" in written and "config.lua" in written and "vk.lua" in written, (
        "the push did not write the app and the library: %s" % written)

    # 1. The title.
    badge.clear_log()
    launch(badge, APP, timeout=20)
    badge.wait_log(r"\[app\] GAME shop closed", timeout=10)
    badge.wait_log(r"\[app\] GAME title", timeout=10)
    os.makedirs(SHOTS, exist_ok=True)
    theme = theme_name(badge)
    time.sleep(0.3)
    title = badge.shot(os.path.join(SHOTS, "game_title_%s.png" % theme))
    # With the shop closed the title is one selected row (Play): ink and paper only.
    assert colours(title) >= 2, "the title screen is blank (%d colours)" % colours(title)

    # 2. Play: the score counts seconds.
    no_lua_error(badge)
    badge.clear_log()
    badge.btn("a")
    badge.wait_log(r"\[app\] GAME play", timeout=5)
    badge.wait_log(r"\[app\] GAME score 1\b", timeout=5)
    first = time.monotonic()
    playfield = badge.shot(os.path.join(SHOTS, "game_play_%s.png" % theme))
    assert playfield != title, "the screen did not change when the run started"
    # The idle player may be hit before the next second: then the run is over and step 3 says so.
    line = badge.wait_log(r"\[app\] GAME (?:score 2\b|over \d+)", timeout=6)
    if "score 2" in line:
        # badge.shot() takes about a second, so only an upper bound is meaningful here.
        gap = time.monotonic() - first
        assert gap < 5.0, "the score took %.1f s to go from 1 to 2" % gap

    # 3. Game over, then the title again. The first "GAME title" was logged before the clear in
    #    step 2, so the one found here is the return.
    line = badge.wait_log(r"\[app\] GAME over (\d+)", timeout=GAME_OVER_TIMEOUT_S)
    final = int(re.search(r"GAME over (\d+)", line).group(1))
    scores = [int(m.group(1)) for m in (re.search(r"\[app\] GAME score (\d+)", l) for l in badge.log()) if m]
    assert scores == list(range(1, len(scores) + 1)), "the score did not count 1, 2, 3, ...: %s" % scores
    assert final == (scores[-1] if scores else 0), "GAME over %d after scores %s" % (final, scores)
    badge.wait_log(r"\[app\] GAME title", timeout=10)
    running(badge, "after game over")

    # 4. The shop. Without a shop address the title is Play only (step 1 saw "GAME shop closed").
    #    With one (config key shop_address; any well-formed address: there is no network and no
    #    registry record), the Buy rows appear and a purchase that cannot succeed fails where the
    #    player can see it.
    no_lua_error(badge)
    badge.stop()
    badge.wait_state(on_launcher, timeout=5)
    assert badge.cmd("VKSET shop_address %s" % provision_values["issuer_key"])[-1] == "OK", "VKSET shop_address"
    badge.clear_log()
    launch(badge, APP, timeout=20)
    badge.wait_log(r"\[app\] GAME shop open", timeout=10)
    badge.wait_log(r"\[app\] GAME title", timeout=10)
    time.sleep(0.3)
    back_on_title = badge.shot(os.path.join(SHOTS, "game_shop_%s.png" % theme))
    assert back_on_title != title, "the title did not change when the shop opened"
    badge.clear_log()
    badge.btn("down")
    badge.btn("a")
    badge.wait_log(r"\[app\] GAME shop \S+ \S+", timeout=5)
    line = badge.wait_log(r"\[app\] GAME buy (?:failed \S.*|done \S+)", timeout=BUY_TIMEOUT_S)
    assert "buy failed" in line, "a purchase with no network and no shop record did not fail: %s" % line
    time.sleep(0.3)
    running(badge, "after the failed purchase")
    failure = badge.shot(os.path.join(SHOTS, "game_buy_failed_%s.png" % theme))
    assert failure != back_on_title and colours(failure) >= 3, "the failure is not shown on the screen"
    time.sleep(1.5)
    running(badge, "a while after the failed purchase")

    # 5. CANCEL: back to the title, then out.
    badge.btn("b")
    badge.wait_log(r"\[app\] GAME title", timeout=5)
    running(badge, "after leaving the shop")
    badge.btn("b")
    badge.wait_state(on_launcher, timeout=5)

    # 6.
    no_lua_error(badge)
    to_launcher(badge)
    assert badge.cmd("VKSET shop_address")[-1] == "OK", "VKSET shop_address (empty) did not close the shop"
