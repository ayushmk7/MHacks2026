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
        # A public value: the link the launcher shows as a QR code (t_barcode.py reads it back).
        "repo_url": "https://github.com/ayushmk7/MHacks2026",
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


def _settings_registry():
    """The rows of the shell's Settings list, in order, and the action rows among them, read from
    the registrations themselves (ui/shell.md, "Settings page registry"): every
    VK_SETTINGS_PAGE / VK_SETTINGS_ACTION(ident, "id", order, ...) line under src/vk/shell/pages/,
    sorted by order then id, as settings_list.cpp sorts them. An action row (SELECT acts in place
    or launches an app) never becomes a screen. A page added or deleted needs no edit here."""
    import re
    pages = os.path.normpath(os.path.join(_HERE, "..", "..", "src", "vk", "shell", "pages"))
    pattern = re.compile(r'^VK_SETTINGS_(PAGE|ACTION)\(\s*\w+\s*,\s*"([a-z_]+)"\s*,\s*(-?\d+)', re.M)
    rows = []
    for name in sorted(os.listdir(pages)):
        if name.endswith(".cpp"):
            with open(os.path.join(pages, name), "r", encoding="utf-8") as handle:
                rows += [(int(order), row_id, kind) for kind, row_id, order in pattern.findall(handle.read())]
    rows.sort(key=lambda row: (row[0], row[1]))
    return (tuple(row[1] for row in rows), tuple(row[1] for row in rows if row[2] == "ACTION"))


SETTINGS_ROWS, SETTINGS_ACTIONS = _settings_registry()
# Screens opened from a page, as {screen: page}. SELECT on the page opens them.
SETTINGS_SUBSCREENS = {"identity_new": "identity"}

# CANCEL taps to_launcher sends at most: the screen stack is at most 6 deep, and an open launcher
# folder takes one more.
LAUNCHER_TAPS = 7


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


def qr_decode(pixels, width=320, height=240, scale=3):
    """The text of the QR code in a screenshot (the RGB565 bytes badge.shot returns), as a phone
    would read it: "" when no code is found. None when no decoder is installed (it is OpenCV:
    `pip install opencv-python-headless` into the repository's .venv).

    The picture is turned to grey and enlarged `scale` times with nearest-neighbour first, so a
    module of 3 or 4 screen pixels is 9 or 12 in the picture the detector sees."""
    try:
        import cv2
        import numpy
    except ImportError:
        return None
    values = numpy.frombuffer(pixels, dtype="<u2").reshape(height, width).astype(numpy.uint32)
    red, green, blue = (values >> 11) & 0x1F, (values >> 5) & 0x3F, values & 0x1F
    grey = ((red * 255 // 31) * 299 + (green * 255 // 63) * 587 + (blue * 255 // 31) * 114) // 1000
    big = cv2.resize(grey.astype(numpy.uint8), (width * scale, height * scale), interpolation=cv2.INTER_NEAREST)
    return cv2.QRCodeDetector().detectAndDecode(big)[0]


def on_launcher(state):
    """True when the badge shows the shell's launcher: no app, screen "launcher"."""
    return state["app"] == "" and state.get("screen") == "launcher"


# ---- app manifests (docs/os/platform/app-host.md, "Manifest") -------------------------------------

APPS_DIR = os.path.normpath(os.path.join(_HERE, "..", "..", "apps"))
FOLDER_KEY = "folder:"   # how VKSTATE `menu` names a folder cell


def manifest(app_id):
    """os/apps/<id>/app.ini as a dict (keys in lower case, values trimmed); {} when there is none."""
    values = {}
    try:
        with open(os.path.join(APPS_DIR, app_id, "app.ini"), "r", encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if not line or line[0] in "#;" or "=" not in line:
                    continue
                key, value = line.split("=", 1)
                values[key.strip().lower()] = value.strip()
    except OSError:
        pass
    return values


def lua_apps(profile="dev"):
    """The Lua apps of os/apps that push-apps.sh installs for `profile` (release leaves out the
    ones whose manifest says profile=dev), sorted."""
    ids = []
    for name in sorted(os.listdir(APPS_DIR)):
        if not os.path.isfile(os.path.join(APPS_DIR, name, "app.ini")):
            continue
        if profile == "release" and manifest(name).get("profile") == "dev":
            continue
        ids.append(name)
    return ids


# ---- the launcher's grid and folders (docs/os/ui/shell.md, "Launcher") ----------------------------

LAUNCHER_COLS = 2


def launcher_cursor_to(badge, key, timeout=5):
    """Moves the launcher's cursor to the cell `key` of the grid on screen (an app id, or
    "folder:<name>"), with UP/DOWN/LEFT/RIGHT taps, and returns the state. VKSTATE `menu` lists
    the cells in grid order, `cursor` is the selected one."""
    state = badge.state()
    assert on_launcher(state), "launcher_cursor_to(%r): not on the launcher: %s" % (key, state.get("screen"))
    menu = state["menu"]
    assert key in menu, "launcher_cursor_to: %r is not on the launcher (folder %r): %s" % (
        key, state.get("folder"), menu)
    want = menu.index(key)
    rows = (want // LAUNCHER_COLS) - (state["cursor"] // LAUNCHER_COLS)
    for _ in range(abs(rows)):
        badge.btn("down" if rows > 0 else "up", "tap")
    deadline = time.monotonic() + timeout
    while True:
        state = badge.state()
        if state["cursor"] == want:
            return state
        assert time.monotonic() < deadline, "launcher_cursor_to(%r): cursor %d, wanted %d" % (
            key, state["cursor"], want)
        if state["cursor"] // LAUNCHER_COLS != want // LAUNCHER_COLS:
            time.sleep(0.1)   # the vertical taps are still being applied
            continue
        badge.btn("right" if state["cursor"] < want else "left", "tap")
        time.sleep(0.1)


def open_folder(badge, name):
    """From the launcher's top level, opens the folder `name`; returns the state inside it."""
    launcher_cursor_to(badge, FOLDER_KEY + name)
    badge.btn("a", "tap")
    return badge.wait_state(lambda s: on_launcher(s) and s.get("folder") == name, timeout=5)


def launcher_find(badge):
    """Every app the launcher lists, as {app_id: folder}, folder "" for the top level. Opens each
    folder of more than one app to read it, and ends on the top level."""
    state = to_launcher(badge)
    found = {key: "" for key in state["menu"] if not key.startswith(FOLDER_KEY)}
    for key in [k for k in state["menu"] if k.startswith(FOLDER_KEY)]:
        name = key[len(FOLDER_KEY):]
        launcher_cursor_to(badge, key)
        badge.btn("a", "tap")
        inner = badge.wait_state(lambda s: s.get("folder") == name or s["app"] != "", timeout=5)
        if inner["app"] != "":
            # A folder of one app launches it.
            found[inner["app"]] = name
            to_launcher(badge)
            continue
        for app_id in inner["menu"]:
            found[app_id] = name
        badge.btn("b", "tap")
        badge.wait_state(lambda s: on_launcher(s) and s.get("folder") == "", timeout=5)
    return found


def to_launcher(badge):
    """Leaves the shell's launcher on screen (VKSTATE app "" and screen "launcher"), at its top
    level (no folder open), and returns that state.

    An open approval is dismissed with CANCEL and the running app is stopped. Then CANCEL is
    tapped until the screen is the launcher: it leaves the error screen, a settings page and the
    Settings list one step at a time, and then an open folder. The state is read before every
    tap, because CANCEL on the launcher's top level itself opens Settings."""
    if badge.state()["modal"]:
        badge.btn("b", "tap")
        badge.wait_state(lambda s: not s["modal"], timeout=5)
    badge.stop()
    state = badge.wait_state(lambda s: s["app"] == "", timeout=5)
    for _ in range(LAUNCHER_TAPS):
        if on_launcher(state) and not state.get("folder"):
            return state
        if state["modal"]:
            badge.btn("b", "tap")
            state = badge.wait_state(lambda s: not s["modal"], timeout=5)
            continue
        badge.btn("b", "tap")
        time.sleep(0.2)
        state = badge.state()
    assert on_launcher(state) and not state.get("folder"), (
        "not on the launcher's top level after %d CANCEL taps: app %r, screen %r, folder %r" % (
            LAUNCHER_TAPS, state["app"], state.get("screen"), state.get("folder")))
    return state


def _settings_step(badge, key, taps):
    for _ in range(taps):
        badge.btn(key, "tap")


def goto_screen(badge, name):
    """Leaves the shell screen called `name` on top and returns the state.

    `name` is "launcher", "settings", a settings page (wifi, bluetooth, espnow, push, store,
    identity, display, leds, info, console, about) or "identity_new". The action rows (theme, wallet,
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
