"""Wi-Fi set-up on the badge (docs/os/ui/shell.md, "Wi-Fi"; docs/os/ui/text-entry.md): the status
page, the scan list, the on-screen keyboard and the join screen, driven with injected buttons.

Needs one badge with a dev build that has the commands VKKBD and VKWIFIUI. No hands. It never
joins a real network: the one join it starts is to a name that does not exist, and the result it
waits for is "not found".

Navigation is by VKSTATE's `screen` field and by the two dev commands, never by comparing pictures:

  VKKBD      {"open", "title", "layer", "row", "col", "key", "len", "min", "max", "secret", "shown",
              "rows", "text"}; "text" is there only for a field that is not secret
  VKWIFIUI   {"scanning", "scan_failed", "networks", "rows", "cursor", "row", "join", "ssid",
              "saved", "mode"}

In order:
  1. Settings -> Wi-Fi: screen `wifi`. SELECT on its first row: screen `wifi_scan`, a scan runs
     and ends; the list's last row is "OTHER...".
  2. CANCEL semantics of the keyboard: OTHER... opens screen `keyboard`; CANCEL on the empty
     field leaves. Again: two characters, CANCEL erases one, a held CANCEL leaves with one typed.
  3. The name of a network that does not exist, typed through all four layers (abc, ABC, 123,
     #+=) with the space key, a SELECT hold for a capital and a backspace; the text is read back
     after every key (the field is not secret). DONE.
  4. The password field: secret, so VKKBD gives no text, only the length. DONE under 8 characters
     is refused; show and hide; DONE at 8.
  5. Screen `wifi_join`: "connecting", then "not_found" within the join timeout. SELECT tries
     again with the same password ("connecting"); CANCEL stops it and the scan list is back.
  6. Nothing was saved, and the password is in no VKSTATE reply and in no log line.

A screenshot of each screen is saved in both themes as shots/wifi_setup_<screen>_<theme>.png.

What it changes: the join to nothing turns the radio off for its duration, so a badge that was on
a network leaves it; on the way out the badge goes back to its saved networks by itself, and the
test waits for that. The theme is put back.
"""

import json
import os
import struct
import time

from common import goto_screen, to_launcher

SHOTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "shots")
SHOT_BYTES = 320 * 240 * 2
THEMES = ("receipt-light", "receipt-dark")
REDRAW_S = 0.8                    # the shell looks at the theme every 500 ms

# A name no network has, through every layer: lower case, the space key, upper case, a digit, a
# symbol only the #+= layer holds, lower case again.
NAME = "zz T9~x"
PASSWORD = "nope-0000"            # typed into the secret field; never joined with anything real
SHORT = 3                         # characters typed before the DONE that must be refused

SCAN_TIMEOUT_S = 20
JOIN_MARGIN_S = 10                # on top of config key wifi_join_s
REJOIN_TIMEOUT_S = 40             # for a badge that was on a network to be back on it

COLS, ROWS, ACTION_ROW = 7, 5, 4  # the keyboard's grid (keyboard_core.h)
LAYER_COL = {"abc": 0, "ABC": 1, "123": 2, "#+=": 3}
SPACE_COL, SHOW_COL, DONE_COL = 4, 5, 6
HOLD_CAPS_MS = 700                # longer than HOLD_ALT_MS (450) in keyboard.cpp
HOLD_EXIT_MS = 1000               # longer than HOLD_EXIT_MS (700) in keyboard.cpp
REVEAL_S = 1.5                    # longer than REVEAL_MS (1200): a typed character is masked after it
FIELD_ROWS = (42, 63)             # the screen rows of the text line (FIELD_TOP, FIELD_H in keyboard.cpp)


# ---- the two dev commands ---------------------------------------------------------------------------

def _json(badge, command):
    reply = badge.cmd(command)[-1]
    assert reply.startswith("OK {"), (
        "%s -> %s (the dev build on the badge has no %s: flash a build with the Wi-Fi set-up)"
        % (command, reply, command))
    return json.loads(reply[3:])


def kbd(badge):
    return _json(badge, "VKKBD")


def wifi_ui(badge):
    return _json(badge, "VKWIFIUI")


def wait_ui(badge, pred, timeout, what):
    deadline = time.monotonic() + timeout
    while True:
        ui = wifi_ui(badge)
        if pred(ui):
            return ui
        assert time.monotonic() < deadline, "%s: not within %g s; VKWIFIUI: %s" % (what, timeout, ui)
        time.sleep(0.2)


def screen(badge, name, timeout=5):
    return badge.wait_state(lambda s: s.get("screen") == name and not s["app"], timeout=timeout)


def get(badge, key):
    reply = badge.cmd("VKGET %s" % key)[-1]
    assert reply.startswith("OK"), "VKGET %s -> %s" % (key, reply)
    return reply[3:].strip()


def put(badge, key, value):
    reply = badge.cmd(("VKSET %s %s" % (key, value)).strip())[-1]
    assert reply == "OK", "VKSET %s %s -> %s" % (key, value, reply)


def shots(badge, name):
    """One picture of the screen per theme. Returns them in the order of THEMES."""
    pictures = []
    for theme in THEMES:
        put(badge, "theme", theme)
        time.sleep(REDRAW_S)
        picture = badge.shot(os.path.join(SHOTS, "wifi_setup_%s_%s.png" % (name, theme)))
        assert len(picture) == SHOT_BYTES, "%s: screenshot is %d bytes" % (name, len(picture))
        colours = set(struct.unpack("<%dH" % (len(picture) // 2), picture))
        assert len(colours) > 1, "%s in %s: the screen is one flat colour" % (name, theme)
        pictures.append(picture)
    assert pictures[0] != pictures[1], "%s looks the same in both themes" % name
    return pictures


def text_line(picture):
    """The pixels of the keyboard's text line in a screenshot."""
    return picture[FIELD_ROWS[0] * 320 * 2:FIELD_ROWS[1] * 320 * 2]


# ---- typing -------------------------------------------------------------------------------------------

def _steps(here, there, count):
    """(key index 0 = forward / 1 = back, presses) for the short way round a ring of `count`."""
    forward = (there - here) % count
    return (0, forward) if forward <= count - forward else (1, count - forward)


def move_to(badge, row, col):
    """Moves the keyboard's cursor to (row, col), the short way round, and checks it got there. On
    the action row the space key of a plain field is two columns wide: either of them counts."""
    state = kbd(badge)
    direction, presses = _steps(state["row"], row, ROWS)
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
    want = kbd(badge)
    direction, _ = _steps(want["col"], col, COLS)
    for _ in range(COLS):
        state = kbd(badge)
        assert state["row"] == ACTION_ROW, "cursor on row %d, wanted the action row" % state["row"]
        if state["col"] == col or (col == SPACE_COL and state["key"] == "space"):
            return state
        badge.btn(("right", "left")[direction])
    raise AssertionError("the cursor never reached action key %d: %s" % (col, kbd(badge)))


def find(rows, ch):
    for row, line in enumerate(rows):
        if ch in line:
            return row, line.index(ch)
    return None


def switch_layer(badge, name):
    state = move_to(badge, ACTION_ROW, LAYER_COL[name])
    assert state["key"] == name, "the key at column %d is %r, wanted %r" % (LAYER_COL[name], state["key"], name)
    badge.btn("a")
    state = kbd(badge)
    assert state["layer"] == name, "SELECT on %r left layer %r" % (name, state["layer"])
    assert (state["row"], state["col"]) == (ACTION_ROW, LAYER_COL[name]), "the cursor left the layer key"
    return state


def type_char(badge, ch, hold_ms=None):
    """Types one character with the direction keys and SELECT, switching layer when the character
    is not on the layer that is showing. Returns the state after it."""
    before = kbd(badge)
    assert before["open"], "the keyboard is not open: %s" % before
    if ch == " ":
        state = move_to(badge, ACTION_ROW, SPACE_COL)
        assert state["key"] == "space", "the key at the space column is %r" % state["key"]
    else:
        at = find(before["rows"], ch)
        if at is None:
            if ch.islower():
                wanted = ["abc"]
            elif ch.isupper():
                wanted = ["ABC"]
            else:
                wanted = ["123", "#+="]
            for name in wanted:
                at = find(switch_layer(badge, name)["rows"], ch)
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


def press_done(badge):
    state = move_to(badge, ACTION_ROW, DONE_COL)
    assert state["key"] == "DONE", "the key at the DONE column is %r" % state["key"]
    badge.btn("a")


# ---- the steps ----------------------------------------------------------------------------------------

def open_scan(badge):
    state = goto_screen(badge, "wifi")
    assert state["screen"] == "wifi" and state["app"] == "", "not on the Wi-Fi page: %s" % state
    shots(badge, "status")
    badge.btn("a")                                   # the first row: Scan for networks
    screen(badge, "wifi_scan")
    ui = wait_ui(badge, lambda u: not u["scanning"], SCAN_TIMEOUT_S, "the scan did not end")
    assert screen(badge, "wifi_scan"), "the scan list closed by itself"
    if ui["scan_failed"]:
        print("t_wifi_setup: the scan did not finish (scan_failed); the list holds OTHER... only")
    else:
        print("t_wifi_setup: %d network(s) in range" % ui["networks"])
    assert ui["rows"] == ui["networks"] + 1, "the list has %d rows for %d networks" % (ui["rows"], ui["networks"])
    return ui


def open_other(badge):
    """From the scan list: the cursor on OTHER... (the last row: UP from the first wraps to it),
    SELECT, and the keyboard asking for the name."""
    ui = wifi_ui(badge)
    for _ in range(ui["rows"] + 1):
        if ui["row"] == "OTHER..." and ui["cursor"] == ui["rows"] - 1:
            break
        badge.btn("up")
        ui = wifi_ui(badge)
    assert ui["row"] == "OTHER..." and ui["cursor"] == ui["rows"] - 1, "the cursor never reached OTHER...: %s" % ui
    badge.btn("a")
    screen(badge, "keyboard")
    state = kbd(badge)
    assert state["open"] and state["title"] == "NETWORK NAME", "not the name field: %s" % state
    assert not state["secret"] and state["text"] == "" and state["len"] == 0, "the name field is not empty: %s" % state
    assert (state["min"], state["max"]) == (1, 32), "name limits %d..%d" % (state["min"], state["max"])
    assert state["layer"] == "abc" and state["key"] == "a", "the keyboard does not start on 'a': %s" % state
    return state


def cancel_semantics(badge):
    # CANCEL on an empty field leaves.
    open_other(badge)
    badge.btn("b")
    screen(badge, "wifi_scan")
    assert not kbd(badge)["open"], "the keyboard is still open after CANCEL on an empty field"

    # CANCEL with text erases one character; held, it leaves whatever is typed.
    open_other(badge)
    type_char(badge, "a")
    state = type_char(badge, "b")
    assert state["text"] == "ab", "typed %r" % state["text"]
    badge.btn("b")
    state = kbd(badge)
    assert state["open"] and state["text"] == "a", "CANCEL did not erase one character: %s" % state
    assert badge.state()["screen"] == "keyboard", "one CANCEL with text typed left the keyboard"
    badge.btn("b", "hold", HOLD_EXIT_MS)
    screen(badge, "wifi_scan")
    assert not kbd(badge)["open"], "the keyboard is still open after a held CANCEL"


def type_name(badge):
    open_other(badge)
    typed = ""
    layers = set()
    for ch in NAME:
        if ch == "T":
            # A capital without the ABC layer: 't' with SELECT held.
            state = type_char(badge, "t", hold_ms=HOLD_CAPS_MS)
        else:
            state = type_char(badge, ch)
        typed += ch
        layers.add(state["layer"])
        assert state["text"] == typed, "after %r the field holds %r, wanted %r" % (ch, state["text"], typed)
    # The ABC layer by its key as well, then a backspace.
    state = type_char(badge, "Q")
    layers.add(state["layer"])
    assert state["text"] == NAME + "Q", "after 'Q' the field holds %r" % state["text"]
    badge.btn("b")
    state = kbd(badge)
    assert state["text"] == NAME and state["len"] == len(NAME), "backspace left %r" % state["text"]
    assert layers == {"abc", "ABC", "123", "#+="}, "the name used only the layers %s" % sorted(layers)
    shots(badge, "name")
    press_done(badge)


def type_password(badge):
    # The name was accepted: the same screen, now asking for the password.
    deadline = time.monotonic() + 5
    while True:
        state = kbd(badge)
        if state["open"] and state.get("title") == "PASSWORD":
            break
        assert time.monotonic() < deadline, "the password field did not open: %s" % state
        time.sleep(0.1)
    assert badge.state()["screen"] == "keyboard", "the password field is not screen `keyboard`"
    assert state["secret"] and not state["shown"], "the password field is not masked: %s" % state
    assert "text" not in state, "VKKBD gives the text of a secret field: %s" % state
    assert state["len"] == 0 and (state["min"], state["max"]) == (8, 63), "password limits: %s" % state

    for ch in PASSWORD[:SHORT]:
        type_char(badge, ch)
    press_done(badge)                                # too short: refused
    state = kbd(badge)
    assert state["open"] and state["len"] == SHORT, "DONE with %d characters was not refused: %s" % (SHORT, state)
    assert badge.state()["screen"] == "keyboard", "DONE with %d characters left the keyboard" % SHORT

    for ch in PASSWORD[SHORT:]:
        state = type_char(badge, ch)
        assert "text" not in state, "VKKBD gives the text of a secret field: %s" % state
    assert state["len"] == len(PASSWORD), "the field holds %d characters, typed %d" % (state["len"], len(PASSWORD))

    time.sleep(REVEAL_S)                             # the character typed last is masked by now
    masked = shots(badge, "password")
    state = move_to(badge, ACTION_ROW, SHOW_COL)
    assert state["key"] == "show", "the key at the show column is %r" % state["key"]
    badge.btn("a")
    state = kbd(badge)
    assert state["shown"] and "text" not in state, "show: %s" % state
    time.sleep(0.3)
    # The same theme as the last masked picture: only the text line is compared.
    assert text_line(badge.shot()) != text_line(masked[-1]), "the text line looks the same shown and masked"
    badge.btn("a")
    assert not kbd(badge)["shown"], "hide did not mask the field again"
    time.sleep(0.3)
    assert text_line(badge.shot()) == text_line(masked[-1]), "hide did not draw the text line masked again"
    press_done(badge)


def join_nothing(badge):
    screen(badge, "wifi_join")
    ui = wifi_ui(badge)
    assert ui["ssid"] == NAME, "joining %r, typed %r" % (ui["ssid"], NAME)
    assert ui["join"] in ("connecting", "not_found"), "the join did not start: %s" % ui
    if ui["join"] == "connecting":
        shots(badge, "connecting")
    limit = int(get(badge, "wifi_join_s")) + JOIN_MARGIN_S
    ui = wait_ui(badge, lambda u: u["join"] != "connecting", limit, "the join did not end (hard timeout)")
    assert ui["join"] == "not_found", (
        "a join to a network that does not exist ended as %r, wanted 'not_found'" % ui["join"])
    assert badge.state()["screen"] == "wifi_join", "the result is not on screen `wifi_join`"
    shots(badge, "not_found")

    # SELECT: the same join again, with the password kept.
    badge.btn("a")
    ui = wait_ui(badge, lambda u: u["join"] in ("connecting", "not_found"), 5, "the retry did not start")
    assert ui["ssid"] == NAME, "the retry joins %r" % ui["ssid"]
    # CANCEL: stop (or leave the result), back on the list.
    badge.btn("b")
    screen(badge, "wifi_scan")
    ui = wifi_ui(badge)
    assert ui["join"] == "idle", "after CANCEL the join is %r" % ui["join"]
    return ui


def no_secret(badge, where):
    text = json.dumps(badge.state())
    assert PASSWORD not in text, "%s: the password is in VKSTATE" % where
    for line in badge.log():
        assert PASSWORD not in line, "%s: the password is in a log line: %s" % (where, line)


def run(badge):
    os.makedirs(SHOTS, exist_ok=True)
    to_launcher(badge)
    theme_before = get(badge, "theme")
    was_joined = badge.info().get("wifi") == "1"
    badge.clear_log()

    try:
        ui = open_scan(badge)
        saved_before = ui["saved"]
        shots(badge, "scan")

        cancel_semantics(badge)
        type_name(badge)
        type_password(badge)
        no_secret(badge, "after typing")
        ui = join_nothing(badge)
        no_secret(badge, "after the join")
        assert ui["saved"] == saved_before, "a join that failed changed the saved networks: %d -> %d" % (
            saved_before, ui["saved"])

        badge.btn("b")
        screen(badge, "wifi")
        badge.btn("b")
        screen(badge, "settings")
    finally:
        # A keyboard left open by a failed step: CANCEL erases there, so leave it with the hold.
        if badge.state().get("screen") == "keyboard":
            badge.btn("b", "hold", HOLD_EXIT_MS)
        put(badge, "theme", theme_before)
        to_launcher(badge)

    assert badge.state().get("screen") == "launcher", "the test did not end on the launcher"
    if was_joined:
        deadline = time.monotonic() + REJOIN_TIMEOUT_S
        while badge.info().get("wifi") != "1" and time.monotonic() < deadline:
            time.sleep(1)
        if badge.info().get("wifi") == "1":
            print("t_wifi_setup: back on the network it was on")
        else:
            print("t_wifi_setup: NOT back on its network after %d s: Settings > Wi-Fi" % REJOIN_TIMEOUT_S)
