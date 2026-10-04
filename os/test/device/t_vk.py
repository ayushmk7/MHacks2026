"""lib/vk.lua on the badge (WP35; implementation-plan.md, review focus 4: no network).

Drives the vktest app (apps/vktest), which runs the library's JSON table and frame helpers on the
badge's own Lua, reads badge.theme, then calls every network helper and the payer flow. With no
network each helper must give nil and a message and the flow must end in "failed": nothing may
hang, and the app must still be running afterwards.

In order:
  1. Push vktest with lib/vk.lua added as vk.lua, start it, wait for "VT done".
  2. The log has: VT json ok, VT frames ok, VT theme ok <name>, VT <helper> nil <message> for rpc,
     blockhash, confirm, record, send_tx, report and feed, VT pay failed <reason>.
  3. The app is still running, with no approval open, and no Lua error was logged.
  4. The screen (drawn with vk.ui) is saved as shots/vktest_<theme>.png in both themes; the two
     pictures differ, so vk.ui follows badge.theme.

Needs one badge with a dev build and NO network: Wi-Fi off, or not joined. (With a route the app
logs "VT net up" and skips the steps that would post to the laptop or open an approval; the test
then fails and says so.) No hands, no second badge. It provisions the badge with the test values
and leaves vktest installed, the theme as it found it, and the launcher on the screen.
"""

import os
import re
import time

from common import launch, provision_test, to_launcher

_HERE = os.path.dirname(os.path.abspath(__file__))
VKTEST_DIR = os.path.normpath(os.path.join(_HERE, "..", "..", "apps", "vktest"))
VK_LUA = os.path.normpath(os.path.join(_HERE, "..", "..", "lib", "vk.lua"))
SHOTS = os.path.join(_HERE, "shots")
VKTEST = "vktest"

_VT = r"\[app\] VT "
_LUA_ERROR = r"\[lua\] .*(?:error|stopping)"

# Twelve steps 100 ms apart, each immediate with no route. A badge that is joined to a network
# whose node does not answer spends one HTTP timeout (4 s) on each read-only helper.
DONE_TIMEOUT_S = 60

# Every helper that must fail with a message when there is no network.
HELPERS = ("rpc", "blockhash", "confirm", "record", "send_tx", "report", "feed")
THEMES = ("receipt-light", "receipt-dark")


def vt_lines(badge):
    """What the app has logged since the last clear_log(): the text after 'VT ', in order."""
    pattern = re.compile(_VT + r"(.*)$")
    return [m.group(1).strip() for m in (pattern.search(line) for line in badge.log()) if m]


def wait_done(badge, timeout=DONE_TIMEOUT_S):
    """Returns the app's VT lines once it has logged 'VT done'. Fails at once on a Lua error, and
    if an approval opens (the payer flow must not get that far without a network)."""
    deadline = time.monotonic() + timeout
    while True:
        log = badge.log()
        failed = [line for line in log if re.search(_LUA_ERROR, line)]
        assert not failed, "vktest stopped with a Lua error: %s" % failed[0]
        lines = vt_lines(badge)
        if "done" in lines:
            return lines
        state = badge.state()
        assert not state["modal"], "vktest opened an approval (title %r); VT lines so far: %s" % (
            state.get("title"), lines)
        assert state["app"] == VKTEST, "vktest is no longer running (app %r); VT lines so far: %s" % (
            state["app"], lines)
        assert time.monotonic() < deadline, "vktest did not log 'VT done' within %g s; VT lines: %s" % (
            timeout, lines)
        time.sleep(0.3)


def one(lines, regex, what):
    """The first VT line that matches `regex` (re.fullmatch)."""
    for line in lines:
        if re.fullmatch(regex, line):
            return line
    raise AssertionError("%s: no VT line matching %r in %s" % (what, regex, lines))


def colours(picture):
    """How many different RGB565 values a screenshot holds."""
    return len({picture[i:i + 2] for i in range(0, len(picture), 2)})


def run(badge):
    provision_test(badge)
    to_launcher(badge)

    # 1. The library is pushed into the app's own folder: an app can only be pushed files there.
    with open(VK_LUA, "rb") as handle:
        library = handle.read()
    written = badge.push(VKTEST_DIR, VKTEST, extra={"vk.lua": library})
    assert "vk.lua" in written and "main.lua" in written and "config.lua" in written, (
        "the push did not write the app and the library: %s" % written)

    badge.clear_log()
    launch(badge, VKTEST, timeout=20)
    lines = wait_done(badge)

    # 2. What the app found.
    broken = [line for line in lines if " FAIL" in line or "stuck" in line]
    assert not broken, "vktest reports a failure: %s" % broken
    assert "net up" not in lines, (
        "the badge has a network route, so vktest skipped its no-network steps; turn Wi-Fi off "
        "(Settings -> Wi-Fi) and run this test again. VT lines: %s" % lines)
    one(lines, r"json ok", "the JSON table on the badge's Lua")
    one(lines, r"frames ok", "the frame helpers")
    theme = one(lines, r"theme ok \S+", "badge.theme")[len("theme ok "):]
    for name in HELPERS:
        one(lines, r"%s nil \S.*" % name, "vk.%s with no network must return nil and a message" % name)
    one(lines, r"pay failed \S.*", "vk.pay with no network must end in failed with a reason")
    assert lines[-1] == "done", "vktest logged something after 'VT done': %s" % lines

    # 3. Still running, nothing open, and it stays that way.
    time.sleep(1.0)
    state = badge.state()
    assert state["app"] == VKTEST and not state["modal"], "vktest is not running after its steps: %s" % state
    failed = [line for line in badge.log() if re.search(_LUA_ERROR, line)]
    assert not failed, "vktest logged a Lua error: %s" % failed[0]

    # 4. The screen, drawn with vk.ui, in both themes.
    os.makedirs(SHOTS, exist_ok=True)
    other = THEMES[1] if theme != THEMES[1] else THEMES[0]
    first = badge.shot(os.path.join(SHOTS, "vktest_%s.png" % theme))
    assert colours(first) >= 3, "the vktest screen is blank in %s (%d colours)" % (theme, colours(first))
    try:
        badge.ok("VKSET theme %s" % other)
        time.sleep(1.2)   # the theme is read again every 500 ms; the app redraws every frame
        second = badge.shot(os.path.join(SHOTS, "vktest_%s.png" % other))
        assert colours(second) >= 3, "the vktest screen is blank in %s (%d colours)" % (other, colours(second))
        assert second != first, "the vktest screen did not change with the theme: vk.ui does not follow badge.theme"
    finally:
        badge.ok("VKSET theme %s" % theme)   # the name the badge itself reported at the start
        time.sleep(0.6)

    state = badge.state()
    assert state["app"] == VKTEST, "vktest stopped while the theme was changed: %s" % state

    badge.stop()
    badge.wait_state(lambda s: s["app"] != VKTEST, timeout=5)
    to_launcher(badge)
