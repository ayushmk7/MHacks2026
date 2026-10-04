"""Helpers shared by the scripted device tests (execution-plan.md, section 5.2).

A test is test/device/t_<name>.py with `def run(badge):`; `badge` is the Badge of scripts/vkdev.py.
vkdev.py puts this folder on the import path, so a test writes `from common import ...`.
"""

import json
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_SCRIPTS = os.path.normpath(os.path.join(_HERE, "..", "..", "scripts"))
if _SCRIPTS not in sys.path:
    sys.path.insert(0, _SCRIPTS)

from vkdev import b58dec, b58enc  # noqa: E402,F401  (re-exported: one base58 for the tool and the tests)

VECTORS_PATH = os.path.normpath(os.path.join(_HERE, "..", "host", "vectors.json"))

# How long past the configured hold time an injected hold lasts.
HOLD_MARGIN_MS = 300


def load_vectors():
    """test/host/vectors.json as a dict (test keys, messages, a record, a request, a proof)."""
    with open(VECTORS_PATH, "r", encoding="utf-8") as handle:
        return json.load(handle)


def test_config(vectors=None):
    """The values a badge is provisioned with for device tests (execution-plan.md, section 1)."""
    vectors = vectors or load_vectors()
    mint = vectors["tx"].get("mint") or b58enc(bytes.fromhex(vectors["tx"]["mint_hex"]))
    return {
        "issuer_key": vectors["vk"]["issuer"]["pubkey_b58"],
        "tokens": "%s:2:HACK:100.00:1000.00" % mint,
        "rpc_url": "http://127.0.0.1:8899",
        "approval_tmo_s": "10",
        "hold_ms": "1000",
        "record_ttl_s": "3600",
    }


def hold_ms(badge):
    """The badge's hold-SELECT time. 1000 (the test value) if the key cannot be read."""
    try:
        return int(badge.ok("VKGET hold_ms"))
    except (AssertionError, ValueError):
        return 1000


def provision_test(badge):
    """Leaves the badge provisioned with test_config(). A provisioned badge is reset first (VKRESET
    and an injected hold on its confirmation), because its secure keys cannot be changed silently."""
    if badge.info().get("provisioned") == "1":
        hold = hold_ms(badge) + HOLD_MARGIN_MS
        badge.ok("VKRESET")
        badge.wait_state(lambda s: s["modal"] and s["phase"] == "ARMED", timeout=5)
        badge.btn("a", "hold", hold)
        badge.wait_state(lambda s: not s["modal"], timeout=5)
        assert badge.info().get("provisioned") == "0", "VKRESET did not erase the wallet config"

    values = test_config()
    listed = [line.split()[1] for line in badge.cmd("VKKEYS")[:-1] if len(line.split()) > 1]
    for key in listed:
        if key in values:
            reply = badge.cmd("VKSET %s %s" % (key, values[key]))[-1]
            assert reply == "OK", "VKSET %s -> %s" % (key, reply)
    badge.ok("VKCOMMIT")
    assert badge.info().get("provisioned") == "1", "VKCOMMIT answered OK but provisioned is not 1"
    return values


# The rows of the shell's Settings list, in order (execution-plan.md, section 5.3; ui/shell.md,
# "Settings page registry"). theme, wallet and inbox are action rows: SELECT acts in place or
# launches an app, so they never become a screen.
SETTINGS_ROWS = ("theme", "wifi", "bluetooth", "espnow", "push", "store", "identity", "display",
                 "leds", "wallet", "inbox", "info", "console")
SETTINGS_ACTIONS = ("theme", "wallet", "inbox")
# Screens opened from a page, as {screen: page}. SELECT on the page opens them.
SETTINGS_SUBSCREENS = {"identity_new": "identity"}

# CANCEL taps to_launcher sends at most: the screen stack is at most 6 deep.
LAUNCHER_TAPS = 6


def assert_screen_lit(state, what):
    """The glass, which VKSHOT cannot see (it reads the canvas): the backlight is on and the canvas
    has been sent to the panel at least once. Returns the transfer count for assert_screen_sent."""
    assert "backlight" in state and "flushes" in state, "VKSTATE has no backlight/flushes: %s" % state
    assert state["backlight"] > 0, "%s: the backlight is off (backlight=%r): the screen is black" % (
        what, state["backlight"])
    assert state["flushes"] > 0, "%s: the canvas was never sent to the panel (flushes=0)" % what
    return state["flushes"]


def assert_screen_sent(badge, since, what):
    """After something changed the picture: the backlight is still on and at least one more canvas
    transfer reached the panel than at `since` (a VKSTATE `flushes` value). Returns the new count."""
    state = badge.wait_state(lambda s: s["flushes"] > since or s["backlight"] == 0, timeout=3)
    assert state["backlight"] > 0, "%s: the backlight is off (backlight=%r)" % (what, state["backlight"])
    return state["flushes"]


def on_launcher(state):
    """True when the badge shows the shell's launcher: no app, screen "launcher"."""
    return state["app"] == "" and state.get("screen") == "launcher"


def to_launcher(badge):
    """Leaves the shell's launcher on screen (VKSTATE app "" and screen "launcher") and returns
    that state.

    An open approval is dismissed with CANCEL and the running app is stopped. Then CANCEL is
    tapped until the screen is the launcher: it leaves the error screen, a settings page and the
    Settings list one step at a time. The state is read before every tap, because CANCEL on the
    launcher itself opens Settings."""
    if badge.state()["modal"]:
        badge.btn("b", "tap")
        badge.wait_state(lambda s: not s["modal"], timeout=5)
    badge.stop()
    state = badge.wait_state(lambda s: s["app"] == "", timeout=5)
    for _ in range(LAUNCHER_TAPS):
        if on_launcher(state):
            return state
        if state["modal"]:
            badge.btn("b", "tap")
            state = badge.wait_state(lambda s: not s["modal"], timeout=5)
            continue
        badge.btn("b", "tap")
        time.sleep(0.2)
        state = badge.state()
    assert on_launcher(state), "not on the launcher after %d CANCEL taps: app %r, screen %r" % (
        LAUNCHER_TAPS, state["app"], state.get("screen"))
    return state


def _settings_step(badge, key, taps):
    for _ in range(taps):
        badge.btn(key, "tap")


def goto_screen(badge, name):
    """Leaves the shell screen called `name` on top and returns the state.

    `name` is "launcher", "settings", a settings page (wifi, bluetooth, espnow, push, store,
    identity, display, leds, info, console) or "identity_new". The action rows (theme, wallet,
    inbox) are not screens: use settings_row() and press SELECT.

    The way there: to_launcher, CANCEL (the Settings list), DOWN to the page's row, SELECT. The
    list is expected to open with its cursor on the first row; if SELECT opens another page, the
    cursor was elsewhere, and the function goes back and moves by the difference.

    NEVER send SELECT on "identity_new": it replaces the badge's key."""
    state = to_launcher(badge)
    if name == "launcher":
        return state
    page = SETTINGS_SUBSCREENS.get(name, name)
    assert page == "settings" or (page in SETTINGS_ROWS and page not in SETTINGS_ACTIONS), (
        "goto_screen: %r is not a shell screen that can be opened from Settings" % name)

    badge.btn("b", "tap")
    state = badge.wait_state(lambda s: s.get("screen") == "settings", timeout=5)
    if page != "settings":
        want = SETTINGS_ROWS.index(page)
        _settings_step(badge, "down", want)
        for _ in range(2):
            badge.btn("a", "tap")
            time.sleep(0.2)
            state = badge.state()
            landed = state.get("screen")
            if landed == page:
                break
            assert state["app"] == "" and landed in SETTINGS_ROWS, (
                "goto_screen(%r): SELECT on the row expected to be %r gave app %r, screen %r" % (
                    name, page, state["app"], landed))
            # Another page opened: the cursor is on its row. Back to the list, move by the difference.
            badge.btn("b", "tap")
            badge.wait_state(lambda s: s.get("screen") == "settings", timeout=5)
            delta = want - SETTINGS_ROWS.index(landed)
            _settings_step(badge, "down" if delta > 0 else "up", abs(delta))
        assert state.get("screen") == page, "goto_screen(%r): on screen %r, wanted %r" % (
            name, state.get("screen"), page)
    if name != page:
        badge.btn("a", "tap")   # the page opens its sub-screen
        state = badge.wait_state(lambda s: s.get("screen") == name, timeout=5)
    return state


def settings_row(badge, row):
    """Opens the Settings list and puts its cursor on `row` (an id from SETTINGS_ROWS) without
    pressing SELECT. For the action rows: theme, wallet, inbox. Returns the state."""
    assert row in SETTINGS_ROWS, "settings_row: unknown row %r" % row
    state = goto_screen(badge, "settings")
    _settings_step(badge, "down", SETTINGS_ROWS.index(row))
    return state


def launch(badge, app_id, timeout=10):
    """Starts an app and returns the state once it is running. Approves the first-run consent
    screen (title "Allow app") with an injected hold if it appears."""
    badge.run(app_id)
    deadline = time.monotonic() + timeout
    consented = False
    while True:
        state = badge.state()
        if state["modal"] and state["title"] == "Allow app" and not consented:
            badge.wait_state(lambda s: s["phase"] == "ARMED" or not s["modal"], timeout=5)
            badge.btn("a", "hold", hold_ms(badge) + HOLD_MARGIN_MS)
            badge.wait_state(lambda s: not s["modal"], timeout=5)
            consented = True
        elif state["app"] == app_id and not state["modal"]:
            return state
        assert time.monotonic() < deadline, "%s did not start within %g s; state: %s" % (
            app_id, timeout, json.dumps(state))
        time.sleep(0.1)
