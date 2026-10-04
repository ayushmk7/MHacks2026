"""The Dice app (apps/dice), on one badge.

In order:
  1. Push apps/dice with lib/vk.lua added as vk.lua and start it with common.launch. The app logs
     "DICE count <n>" (config.lua's `count`, 2 as shipped) and draws its screen.
  2. SELECT rolls: after the tumble the log has "DICE roll <d1> ... total <t>" with n values, each
     1..6, and t their sum. The settled screen is saved as shots/dice_<theme>.png; it is not blank
     and differs from the screen before the roll.
  3. UP adds a die ("DICE count <n+1>") unless n is already config.lua's `max`; the next roll then
     has that many values.
  4. CANCEL exits: VKSTATE app is "" and screen is "launcher".
  5. No Lua error was logged.

Needs one badge with a dev build. No hands, no network, no provisioning (the app asks for no
permission). Dice stays installed afterwards; the launcher is on the screen.
"""

import os
import re
import time

from common import launch, on_launcher, to_launcher

_HERE = os.path.dirname(os.path.abspath(__file__))
APP_DIR = os.path.normpath(os.path.join(_HERE, "..", "..", "apps", "dice"))
VK_LUA = os.path.normpath(os.path.join(_HERE, "..", "..", "lib", "vk.lua"))
SHOTS = os.path.join(_HERE, "shots")
APP = "dice"

WIDTH, HEIGHT = 320, 240
ROW_BYTES = WIDTH * 2
SHOT_BYTES = ROW_BYTES * HEIGHT

_LUA_ERROR = r"\[lua\] .*(?:error|stopping)"
_ROLL = r"\[app\] DICE roll ((?:\d+ )+)total (\d+)\s*$"

# config.lua's roll_ms is 600; the line is logged when the dice settle.
ROLL_TIMEOUT_S = 6


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


def no_lua_error(badge):
    failed = [line for line in badge.log() if re.search(_LUA_ERROR, line)]
    assert not failed, "dice logged a Lua error: %s" % failed[0]


def roll(badge, dice):
    """Presses SELECT and returns the values of the roll that settles. Checks the line."""
    badge.clear_log()
    badge.btn("a", "tap")
    line = badge.wait_log(_ROLL, timeout=ROLL_TIMEOUT_S)
    found = re.search(_ROLL, line)
    values = [int(value) for value in found.group(1).split()]
    total = int(found.group(2))
    assert len(values) == dice, "a roll of %d dice logged %d values: %s" % (dice, len(values), line)
    assert all(1 <= value <= 6 for value in values), "a die shows a value outside 1..6: %s" % line
    assert total == sum(values), "the total is not the sum of the dice: %s" % line
    return values


def run(badge):
    os.makedirs(SHOTS, exist_ok=True)
    to_launcher(badge)
    theme = theme_name(badge)

    # 1. Push and start.
    with open(VK_LUA, "rb") as handle:
        library = handle.read()
    written = badge.push(APP_DIR, APP, extra={"vk.lua": library})
    for name in ("app.ini", "main.lua", "config.lua", "vk.lua"):
        assert name in written, "the push did not write %s: %s" % (name, written)

    badge.clear_log()
    state = launch(badge, APP, timeout=20)
    assert state["app"] == APP and not state["modal"], "dice is not running after its launch: %s" % state
    line = badge.wait_log(r"\[app\] DICE count \d+", timeout=10)
    dice = int(re.search(r"DICE count (\d+)", line).group(1))
    assert 1 <= dice <= 9, "dice starts with %d dice" % dice
    time.sleep(0.5)  # the first frames
    before = badge.shot()
    assert len(before) == SHOT_BYTES, "the screenshot is %d bytes, expected %d" % (len(before), SHOT_BYTES)
    assert colours(body(before)) >= 3, "the dice screen looks blank: %d colours under the header" % (
        colours(body(before)))

    # 2. A roll.
    roll(badge, dice)
    time.sleep(0.4)  # the settled frame
    settled = badge.shot(os.path.join(SHOTS, "dice_%s.png" % theme))
    assert colours(body(settled)) >= 3, "the screen looks blank after a roll"
    assert body(settled) != body(before), "the screen did not change after a roll"
    assert badge.state()["app"] == APP, "dice stopped when SELECT was pressed"

    # 3. One die more, if config.lua's `max` allows it, then a roll with that many.
    badge.clear_log()
    badge.btn("up", "tap")
    try:
        line = badge.wait_log(r"\[app\] DICE count \d+", timeout=2)
    except AssertionError:
        line = None                      # already at `max`: UP changes nothing and logs nothing
    if line is not None:
        more = int(re.search(r"DICE count (\d+)", line).group(1))
        assert more == dice + 1, "UP changed the number of dice from %d to %d" % (dice, more)
        dice = more
    roll(badge, dice)

    # 4. CANCEL exits.
    badge.btn("b", "tap")
    badge.wait_state(on_launcher, timeout=5)

    # 5.
    no_lua_error(badge)
    to_launcher(badge)
