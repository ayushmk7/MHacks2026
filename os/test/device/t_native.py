"""T-APP5 and the badge half of T-APP6: the native app runtime (WP31).

T-APP5  hello_native (compiled into the firmware) is in the app list after the Lua apps, launches,
        draws its own screen, exits on CANCEL, and launches again as a new object. A pushed DEL of
        a native id answers "delete failed" and removes nothing (hook H11).
T-APP6  A native app without `sign` in its BADGE_APP permissions gets `denied` from
        vk::wallet::begin. The app is the temporary zz_denytest the integrator adds for the run
        (execution-plan.md, brief I4): its on_start calls begin, logs "[deny] begin=<reason>" and
        exits. When the build does not contain it, this half is reported as not run and the test
        still passes on T-APP5.

Also: a pushed folder named like a built-in app does not block or replace it (the native app
wins), and begin from a native app's on_stop is refused like begin from its on_start.

Needs one badge with a dev build. No network, no hands. T-APP6 needs a provisioned badge (begin
answers `not_provisioned` before it looks at permissions), so the test provisions one that is not.
"""

import collections
import os
import shutil
import struct
import tempfile

from common import launch, provision_test, to_launcher

SHOTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "shots")
SHOT_BYTES = 320 * 240 * 2

APP = "hello_native"
APP_NAME = "Hello (C++)"
DENY_APP = "zz_denytest"

# What hello_native draws: upstream's theme::BG (#0B0B12) and theme::GREEN (#14F195) as RGB565.
BG = 0x0842
GREEN = 0x1792


def _authed(badge, line):
    """One push-protocol command. Upstream drops the session whenever an app stops, so AUTH first."""
    reply = badge.cmd("AUTH " + badge.ok("VKPAIR"))[-1]
    assert reply.startswith("OK"), "AUTH -> %s" % reply
    return badge.cmd(line)


def _app_list(badge):
    """LIST as [(id, size, name)], in the order the launcher shows them."""
    reply = _authed(badge, "LIST")
    assert reply[-1].startswith("OK"), "LIST -> %s" % reply[-1]
    rows = []
    for line in reply[:-1]:
        parts = line.split(None, 3)  # "+", id, size, name
        if len(parts) >= 3:
            rows.append((parts[1], int(parts[2]), parts[3] if len(parts) > 3 else ""))
    return rows


def _colours(pixels):
    assert len(pixels) == SHOT_BYTES, "screenshot is %d bytes" % len(pixels)
    return collections.Counter(struct.unpack("<%dH" % (len(pixels) // 2), pixels))


def _hello_is_on_screen(badge, name):
    pixels = badge.shot(os.path.join(SHOTS, name))
    colours = _colours(pixels)
    assert len(colours) > 1, "T-APP5: the screen is blank (one colour, 0x%04X)" % next(iter(colours))
    total = sum(colours.values())
    assert colours[BG] > 0.8 * total, (
        "T-APP5: %d %% of the screen is hello_native's background; the app did not fill it"
        % (100 * colours[BG] // total))
    assert colours[GREEN] > 100, "T-APP5: only %d pixels of hello_native's green text" % colours[GREEN]
    return pixels


def _start_hello(badge):
    badge.clear_log()
    state = launch(badge, APP)
    assert state["app"] == APP, "VKSTATE app is %r" % state["app"]
    assert state["native"] is True, "VKSTATE native is %r while %s runs" % (state["native"], APP)
    badge.wait_log(r"native app '%s' started" % APP, timeout=2)


def _cancel_hello(badge):
    badge.btn("b", "tap")  # CANCEL: badge::exit()
    badge.wait_state(lambda s: s["app"] != APP, timeout=5)
    badge.wait_log(r"native app '%s' stopped" % APP, timeout=2)


def t_app5(badge):
    to_launcher(badge)
    launcher = badge.shot()

    # Hook H11: the native app is in the app list, with every Info field set, after the Lua apps
    # (which have files, so a size).
    rows = _app_list(badge)
    ids = [row[0] for row in rows]
    assert APP in ids, "LIST has no %s: %s" % (APP, ids)
    at = ids.index(APP)
    assert rows[at][1] == 0 and rows[at][2] == APP_NAME, "LIST row of %s: %r" % (APP, rows[at])
    later_lua = [row[0] for row in rows[at:] if row[1] > 0]
    assert not later_lua, "Lua apps listed after a native app: %s" % later_lua

    # Launch: running, native, and its own picture on the screen.
    _start_hello(badge)
    first = _hello_is_on_screen(badge, "native_hello.png")
    assert first != launcher, "T-APP5: the screen is still the launcher's"

    # CANCEL exits.
    _cancel_hello(badge)
    assert badge.state()["app"] != APP

    # Launch again: a new object is created (the log has a second "started"), and it draws again.
    _start_hello(badge)
    _hello_is_on_screen(badge, "native_hello_again.png")
    _cancel_hello(badge)

    # Hook H11, removeApp is not hooked: DEL of a native id fails and the app stays.
    reply = _authed(badge, "DEL " + APP)[-1]
    assert reply == "ERR delete failed", "DEL %s -> %s" % (APP, reply)
    assert APP in [row[0] for row in _app_list(badge)], "%s is gone from LIST after DEL" % APP

    # A STOP sent while it runs stops it like any app.
    _start_hello(badge)
    badge.stop()
    badge.wait_state(lambda s: s["app"] != APP, timeout=5)
    badge.wait_log(r"native app '%s' stopped" % APP, timeout=2)
    to_launcher(badge)


def t_shadow(badge):
    """A pushed folder whose id is a built-in app's cannot block it or replace it: the native app
    starts, the log says the folder is ignored, and DEL removes the folder (app-host.md, step 1)."""
    to_launcher(badge)
    folder = tempfile.mkdtemp(prefix="vk-shadow-")
    try:
        with open(os.path.join(folder, "main.lua"), "w") as handle:
            handle.write('print("SHADOW ran")\nfunction update() end\n')
        badge.push(folder, APP)
    finally:
        shutil.rmtree(folder, ignore_errors=True)
    try:
        _start_hello(badge)        # asserts native true and the "started" line
        log = "\n".join(badge.log())
        assert "ignoring pushed app '%s'" % APP in log, "no 'ignoring pushed app' line in the log:\n%s" % log
        assert "SHADOW ran" not in log, "the pushed folder's main.lua ran in place of the built-in app"
        _cancel_hello(badge)
    finally:
        reply = _authed(badge, "DEL " + APP)[-1]
    assert reply == "OK deleted", "DEL of the pushed %s folder -> %s" % (APP, reply)
    assert APP in [row[0] for row in _app_list(badge)], "%s is gone from LIST after the folder was deleted" % APP
    to_launcher(badge)


def t_app6(badge):
    """True when the check ran."""
    if DENY_APP not in [row[0] for row in _app_list(badge)]:
        print("T-APP6 not run: this build has no %s (the integrator adds it for the batch 4 run)" % DENY_APP)
        return False
    if badge.info().get("provisioned") != "1":
        provision_test(badge)
    to_launcher(badge)

    badge.clear_log()
    badge.run(DENY_APP)  # its on_start calls begin, logs the reason and exits: too short-lived for launch()
    line = badge.wait_log(r"\[deny\] begin=\S+", timeout=10)
    assert "begin=denied" in line, "T-APP6: a native app without `sign` got %r from begin, expected denied" % line
    # The same call from on_stop(): hook H8b has already told the stop listeners, so the grant slot
    # is cleared; the answer must still come from the app's BADGE_APP line (app-host.md, step 1).
    line = badge.wait_log(r"\[deny\] stop=\S+", timeout=10)
    assert "stop=denied" in line, "T-APP6: begin from on_stop gave %r, expected denied" % line
    state = badge.wait_state(lambda s: s["app"] != DENY_APP, timeout=5)
    assert not state["modal"], "T-APP6: an approval opened for an app without `sign`: %s" % state["title"]
    to_launcher(badge)
    return True


def run(badge):
    os.makedirs(SHOTS, exist_ok=True)
    t_app5(badge)
    t_shadow(badge)
    t_app6(badge)
