"""On-badge settings and the setup checklist (docs/os/ui/shell.md, "Badge", "Setup", "Advanced",
"Editing a config key"), driven with injected buttons. One badge, a dev build with the commands
VKSETUP and VKKBD. No network, no hands. NOT YET RUN.

Navigation is by VKSTATE's `screen` and by VKSETUP / VKKBD, never by comparing pictures:

  VKSETUP  {"left", "autoopen", "setup_done", "cursor", "rows"}; rows is
           "identity=done,wi-fi=todo,clock=todo,wallet=todo,listener=optional,name=optional,theme=optional"

The three pages sit after About in the Settings list (orders 142, 144, 146; Restart is 150), so they
are found by walking UP from the first row (UP wraps to the last row) and opening each row until the
wanted screen is on top; every row passed on the way is a page, never an action row.

In order:
  1. VKSETUP's shape, and its first-boot answer against VKINFO (autoopen only when unprovisioned,
     never dismissed, and something left).
  2. Setup: the screen opens; DOWN walks the seven rows (VKSETUP cursor); on `Name`, SELECT opens
     the keyboard (title NAME, at most 32 characters: config key display_name's range); a name is
     typed with a held SELECT for a capital, a digit and a backspace; DONE writes display_name and
     the checklist row turns done. CANCEL leaves and sets setup_done (the dismissal).
  3. Badge: Time zone RIGHT twice -> utc_offset +00:30 from UTC, LEFT twice -> back; a VKSET of an
     offset that is not a quarter hour is refused (the key's rule); the picker opens on SELECT and
     CANCEL leaves it with nothing changed. Balance check RIGHT and LEFT step balance_poll_s inside
     its range. Start at boot: the picker opens and CANCEL changes nothing.
  4. Advanced: opens, scrolls, and leaves.
Screenshots in both themes: shots/setup_<setup|keyboard|badge|pick|advanced>_<theme>.png.

What it changes and puts back, whatever happens: display_name, utc_offset, balance_poll_s,
setup_done, theme. It never changes the autostart app, never touches a secure key, and never goes
near Identity's New identity.
"""

import json
import os
import struct
import time

from common import to_launcher

SHOTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "shots")
SHOT_BYTES = 320 * 240 * 2
THEMES = ("receipt-light", "receipt-dark")
REDRAW_S = 0.8                    # the shell looks at the theme every 500 ms

NAME = "Zz T9"                    # a capital by a held SELECT, the space key, a capital, a digit
RESTORED = ("display_name", "utc_offset", "balance_poll_s", "setup_done", "theme")
ROWS = ("identity", "wi-fi", "clock", "wallet", "listener", "name", "theme")
NAME_ROW = ROWS.index("name")

PAGES_FROM_BOTTOM = 6             # restart, advanced, setup, badge, about, console: all pages

COLS, KB_ROWS, ACTION_ROW = 7, 5, 4
LAYER_COL = {"abc": 0, "ABC": 1, "123": 2, "#+=": 3}
SPACE_COL, DONE_COL = 4, 6
HOLD_CAPS_MS = 700                # longer than HOLD_ALT_MS (450) in keyboard.cpp
HOLD_EXIT_MS = 1000               # longer than HOLD_EXIT_MS (700) in keyboard.cpp


# ---- commands -----------------------------------------------------------------------------------------

def _json(badge, command):
    reply = badge.cmd(command)[-1]
    assert reply.startswith("OK {"), "%s -> %s (flash a dev build with the setup page)" % (command, reply)
    return json.loads(reply[3:])


def setup_ui(badge):
    ui = _json(badge, "VKSETUP")
    ui["row_states"] = dict(part.split("=", 1) for part in ui["rows"].split(","))
    return ui


def kbd(badge):
    return _json(badge, "VKKBD")


def get(badge, key):
    reply = badge.cmd("VKGET %s" % key)[-1]
    assert reply.startswith("OK"), "VKGET %s -> %s" % (key, reply)
    return reply[3:] if len(reply) > 3 else ""


def put(badge, key, value):
    reply = badge.cmd(("VKSET %s %s" % (key, value)).rstrip())[-1]
    assert reply == "OK", "VKSET %s %s -> %s" % (key, value, reply)


def screen(badge, name, timeout=5):
    return badge.wait_state(lambda s: s.get("screen") == name and not s["app"], timeout=timeout)


def shots(badge, name):
    """One picture per theme; the theme is put back by run()."""
    pictures = []
    for theme in THEMES:
        put(badge, "theme", theme)
        time.sleep(REDRAW_S)
        picture = badge.shot(os.path.join(SHOTS, "setup_%s_%s.png" % (name, theme)))
        assert len(picture) == SHOT_BYTES, "%s: screenshot is %d bytes" % (name, len(picture))
        colours = set(struct.unpack("<%dH" % (len(picture) // 2), picture))
        assert len(colours) > 1, "%s in %s: the screen is one flat colour" % (name, theme)
        pictures.append(picture)
    assert pictures[0] != pictures[1], "%s looks the same in both themes" % name
    return pictures


# ---- navigation ---------------------------------------------------------------------------------------

def open_page(badge, name):
    """From the launcher: Settings, then UP from the first row (wrapping to the last) one row at a
    time, SELECT, until screen `name` is on top. Every row tried is a page; one that is not stops the
    test before anything else happens."""
    to_launcher(badge)
    badge.btn("b")
    screen(badge, "settings")
    for ups in range(1, PAGES_FROM_BOTTOM + 1):
        badge.btn("up")
        badge.btn("a")
        time.sleep(0.2)
        state = badge.state()
        assert state["app"] == "", "row %d from the bottom launched app %r" % (ups, state["app"])
        landed = state.get("screen")
        assert landed != "settings", "row %d from the bottom is an action row: stopping" % ups
        if landed == name:
            return state
        badge.btn("b")
        screen(badge, "settings")
    raise AssertionError("no row of the last %d opens screen %r" % (PAGES_FROM_BOTTOM, name))


# ---- typing (the same moves as t_wifi_setup.py, kept local) ----------------------------------------------

def _steps(here, there, count):
    forward = (there - here) % count
    return (0, forward) if forward <= count - forward else (1, count - forward)


def move_to(badge, row, col):
    state = kbd(badge)
    direction, presses = _steps(state["row"], row, KB_ROWS)
    for _ in range(presses):
        badge.btn(("down", "up")[direction])
    if row != ACTION_ROW:
        direction, presses = _steps(state["col"], col, COLS)
        for _ in range(presses):
            badge.btn(("right", "left")[direction])
        state = kbd(badge)
        assert (state["row"], state["col"]) == (row, col), "cursor at (%d,%d), wanted (%d,%d)" % (
            state["row"], state["col"], row, col)
        return state
    direction, _ = _steps(kbd(badge)["col"], col, COLS)
    for _ in range(COLS):
        state = kbd(badge)
        if state["col"] == col or (col == SPACE_COL and state["key"] == "space"):
            return state
        badge.btn(("right", "left")[direction])
    raise AssertionError("the cursor never reached action key %d: %s" % (col, kbd(badge)))


def find(rows, ch):
    for row, line in enumerate(rows):
        if ch in line:
            return row, line.index(ch)
    return None


def type_char(badge, ch, hold_ms=None):
    before = kbd(badge)
    assert before["open"], "the keyboard is not open: %s" % before
    if ch == " ":
        state = move_to(badge, ACTION_ROW, SPACE_COL)
    else:
        at = find(before["rows"], ch)
        if at is None:
            layers = ["abc"] if ch.islower() else ["ABC"] if ch.isupper() else ["123", "#+="]
            for layer in layers:
                move_to(badge, ACTION_ROW, LAYER_COL[layer])
                badge.btn("a")
                at = find(kbd(badge)["rows"], ch)
                if at is not None:
                    break
        assert at is not None, "no layer shows a key for %r" % ch
        state = move_to(badge, at[0], at[1])
        assert state["key"] == ch, "the key under the cursor is %r, wanted %r" % (state["key"], ch)
    if hold_ms is None:
        badge.btn("a")
    else:
        badge.btn("a", "hold", hold_ms)
    after = kbd(badge)
    assert after["len"] == before["len"] + 1, "typing %r: length %d -> %d" % (ch, before["len"], after["len"])
    return after


def type_text(badge, text):
    """Types `text`: a capital is its small letter with SELECT held, as a person would."""
    state = kbd(badge)
    # Start from an empty field: CANCEL erases one character per press.
    for _ in range(state["len"]):
        badge.btn("b")
    assert kbd(badge)["len"] == 0, "the field did not empty"
    typed = ""
    for ch in text:
        state = type_char(badge, ch.lower(), HOLD_CAPS_MS) if ch.isupper() else type_char(badge, ch)
        typed += ch
        assert state["text"] == typed, "after %r the field holds %r, wanted %r" % (ch, state["text"], typed)
    return state


def press_done(badge):
    state = move_to(badge, ACTION_ROW, DONE_COL)
    assert state["key"] == "DONE", "the key at the DONE column is %r" % state["key"]
    badge.btn("a")


# ---- the steps ------------------------------------------------------------------------------------------

def check_first_boot_rule(badge):
    ui = setup_ui(badge)
    assert tuple(ui["row_states"]) == ROWS, "VKSETUP rows: %s" % ui["rows"]
    for row, status in ui["row_states"].items():
        assert status in ("done", "todo", "wait", "optional"), "row %s is %r" % (row, status)
    required = [ui["row_states"][r] for r in ("identity", "wi-fi", "clock", "wallet")]
    assert ui["left"] == sum(1 for s in required if s != "done"), "left %d for %s" % (ui["left"], ui["rows"])
    info = badge.info()
    provisioned = info.get("provisioned") == "1"
    assert (ui["row_states"]["wallet"] == "done") == provisioned, "wallet row %s, provisioned=%s" % (
        ui["row_states"]["wallet"], info.get("provisioned"))
    if info.get("time") != "sntp":
        assert ui["row_states"]["clock"] != "done", "the clock row is done without SNTP: %s" % ui["rows"]
    expected = ui["setup_done"] == 0 and not provisioned and ui["left"] > 0
    assert ui["autoopen"] == expected, "autoopen %s, expected %s (%s)" % (ui["autoopen"], expected, ui)
    print("t_setup: %d step(s) left: %s" % (ui["left"], ui["rows"]))


def setup_screen(badge):
    open_page(badge, "setup")
    shots(badge, "setup")
    for i in range(1, len(ROWS)):
        badge.btn("down")
        assert setup_ui(badge)["cursor"] == i, "DOWN did not move the checklist cursor to %d" % i
    badge.btn("down")
    assert setup_ui(badge)["cursor"] == 0, "the checklist does not wrap"

    # The name, typed on the badge.
    for _ in range(NAME_ROW):
        badge.btn("down")
    badge.btn("a")
    screen(badge, "keyboard")
    state = kbd(badge)
    assert state["open"] and state["title"] == "NAME" and not state["secret"], "not the name field: %s" % state
    assert state["min"] == 0 and state["max"] == 32, "name limits %d..%d (display_name is 0..32)" % (
        state["min"], state["max"])
    state = type_text(badge, NAME + "x")
    badge.btn("b")                                    # backspace
    assert kbd(badge)["text"] == NAME, "backspace left %r" % kbd(badge)["text"]
    shots(badge, "keyboard")
    press_done(badge)
    screen(badge, "setup")
    assert get(badge, "display_name") == NAME, "display_name is %r" % get(badge, "display_name")
    assert setup_ui(badge)["row_states"]["name"] == "done", "the name row did not turn done"

    # CANCEL leaves and is the dismissal.
    badge.btn("b")
    screen(badge, "settings")
    assert get(badge, "setup_done") == "1", "leaving setup did not set setup_done"
    assert not setup_ui(badge)["autoopen"], "setup would still open by itself"


def badge_page(badge):
    open_page(badge, "badge")
    shots(badge, "badge")

    # Time zone: two quarter hours east, then back.
    put(badge, "utc_offset", "")
    badge.btn("down")
    badge.btn("right")
    badge.btn("right")
    assert get(badge, "utc_offset") == "+00:30", "utc_offset after RIGHT RIGHT: %r" % get(badge, "utc_offset")
    badge.btn("left")
    badge.btn("left")
    assert get(badge, "utc_offset") == "", "utc_offset after LEFT LEFT: %r" % get(badge, "utc_offset")
    # The key's rule: only quarter hours, -12:00 to +14:00.
    for bad in ("+05:31", "+15:00", "5:30"):
        reply = badge.cmd("VKSET utc_offset %s" % bad)[-1]
        assert reply == "ERR invalid", "VKSET utc_offset %s -> %s" % (bad, reply)
    # The picker, left with CANCEL.
    badge.btn("a")
    screen(badge, "pick")
    shots(badge, "pick")
    badge.btn("b")
    screen(badge, "badge")
    assert get(badge, "utc_offset") == "", "CANCEL in the picker changed utc_offset"

    # Balance check: one step each way, inside the key's range.
    badge.btn("down")                                  # Clock
    badge.btn("down")                                  # Balance check
    before = int(get(badge, "balance_poll_s"))
    badge.btn("right")
    after = int(get(badge, "balance_poll_s"))
    assert after > before or after == before == 3600, "RIGHT: balance_poll_s %d -> %d" % (before, after)
    badge.btn("left")
    back = int(get(badge, "balance_poll_s"))
    assert back <= after and 0 <= back <= 3600, "LEFT: balance_poll_s %d -> %d" % (after, back)

    # Start at boot: the picker opens over the apps; CANCEL changes nothing.
    badge.btn("down")
    badge.btn("a")
    screen(badge, "pick")
    badge.btn("b")
    screen(badge, "badge")
    badge.btn("b")
    screen(badge, "settings")


def advanced_page(badge):
    open_page(badge, "advanced")
    shots(badge, "advanced")
    for _ in range(12):                                # scrolls past the first screen and wraps
        badge.btn("down")
    assert badge.state().get("screen") == "advanced", "the page closed while scrolling"
    badge.btn("b")
    screen(badge, "settings")


def run(badge):
    os.makedirs(SHOTS, exist_ok=True)
    to_launcher(badge)
    saved = {key: get(badge, key) for key in RESTORED}
    try:
        check_first_boot_rule(badge)
        setup_screen(badge)
        badge_page(badge)
        advanced_page(badge)
    finally:
        # A keyboard or a picker left open by a failed step: leave it first.
        if badge.state().get("screen") == "keyboard":
            badge.btn("b", "hold", HOLD_EXIT_MS)
        for key, value in saved.items():
            put(badge, key, value)
        to_launcher(badge)
    for key, value in saved.items():
        assert get(badge, key) == value, "%s was not put back: %r, was %r" % (key, get(badge, key), value)
    assert badge.state().get("screen") == "launcher", "the test did not end on the launcher"
