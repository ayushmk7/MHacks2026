"""The BadgeOS shell: launcher, settings list, app exit, app error, delete confirmation
(docs/os/ui/shell.md, "Tests"; implementation-plan.md, WP37).

Needs one badge with a dev build. No network, no hands. The test navigates by VKSTATE `screen`,
never by comparing screenshots to a reference; it saves one screenshot per screen as
shots/shell_<screen>_<theme>.png (theme from VKGET theme, "receipt-light" when empty).

Not covered here, and why:
  - The offer and installing screens cannot be raised without an app-store broker (and
    DEFAULT_BROKER_URL is empty): they are compiled, not seen.
  - The boot screen cannot be captured over serial: no command is read during setup(). A person
    checks it (T-LED1).

Steps:
  1. After a reset, VKSTATE app is "" and screen is "launcher"; the launcher's screenshot is not
     uniform.
  2. Grid navigation: a RIGHT tap and a DOWN tap each change the screenshot; UP and LEFT put the
     cursor back on the first app.
  3. CANCEL opens `settings`; CANCEL again returns to `launcher`.
  4. nativetest is launched and CANCEL exits it: app "" and screen "launcher".
  5. A pushed app whose main.lua calls error() lands on `app_error`; CANCEL returns to `launcher`.
  6. A pushed Lua app is listed before the native apps. With the cursor on it, holding RIGHT for
     900 ms opens `app_delete`. CANCEL keeps the app; the same again and SELECT deletes it (LIST
     no longer names it). Both ways end on `launcher`.

It starts with a reset (the launcher's cursor is then on the first app, which the cursor moves in
steps 2 and 6 count from) and leaves the launcher on screen with both of its fixtures deleted.
"""

import os
import struct
import tempfile

from common import assert_screen_lit, assert_screen_sent, launch, launcher_cursor_to, on_launcher, to_launcher

_HERE = os.path.dirname(os.path.abspath(__file__))
SHOTS = os.path.join(_HERE, "shots")

NATIVE_APP = "nativetest"
ERR_APP = "shellerr"
DEL_APP = "shelldel"

ERR_FILES = {
    "app.ini": "name=Shell error\nversion=1.0.0\nauthor=BadgeOS tests\n"
               "description=Dev-only test fixture (t_shell): raises an error at once.\nmin_api=2\n",
    "main.lua": 'error("t_shell: this app fails on purpose")\n',
}
DEL_FILES = {
    "app.ini": "name=Shell delete\nversion=1.0.0\nauthor=BadgeOS tests\n"
               "description=Dev-only test fixture (t_shell): deleted from the launcher.\nmin_api=2\n",
    "main.lua": "function on_draw() end\n",
}


def _authed(badge, line):
    """One push-protocol command. Upstream drops the session whenever an app stops, so AUTH first."""
    reply = badge.cmd("AUTH " + badge.ok("VKPAIR"))[-1]
    assert reply.startswith("OK"), "AUTH -> %s" % reply
    return badge.cmd(line)


def _app_ids(badge):
    """The ids LIST names, in the order the launcher shows them (Lua apps, then native apps)."""
    reply = _authed(badge, "LIST")
    assert reply[-1].startswith("OK"), "LIST -> %s" % reply[-1]
    return [line.split()[1] for line in reply[:-1] if len(line.split()) >= 2]


def _push(badge, app_id, files):
    with tempfile.TemporaryDirectory() as folder:
        for name, text in files.items():
            with open(os.path.join(folder, name), "w", encoding="utf-8") as handle:
                handle.write(text)
        badge.push(folder, app_id)


def _delete_if_listed(badge, app_id):
    if app_id in _app_ids(badge):
        _authed(badge, "DEL " + app_id)


def _shot(badge, screen, theme):
    """Saves shots/shell_<screen>_<theme>.png and returns the pixels, which must not be one colour."""
    pixels = badge.shot(os.path.join(SHOTS, "shell_%s_%s.png" % (screen, theme)))
    colours = set(struct.unpack("<%dH" % (len(pixels) // 2), pixels))
    assert len(colours) > 1, "screen %s is blank (one colour, 0x%04X)" % (screen, next(iter(colours)))
    return pixels


def _wait_screen(badge, name, what, timeout=5):
    try:
        return badge.wait_state(lambda s: s["app"] == "" and s.get("screen") == name, timeout=timeout)
    except Exception:
        state = badge.state()
        raise AssertionError("%s: expected screen %r with no app; app=%r screen=%r" % (
            what, name, state.get("app"), state.get("screen")))


def run(badge):
    os.makedirs(SHOTS, exist_ok=True)

    # 1. Boot lands on the launcher with no app.
    badge.reset()
    try:
        badge.wait_state(on_launcher, timeout=10)
    except Exception:
        state = badge.state()
        raise AssertionError("after a reset the badge is not on the launcher: app=%r screen=%r" % (
            state.get("app"), state.get("screen")))
    theme = badge.ok("VKGET theme").strip() or "receipt-light"
    # The glass itself: a screenshot reads the canvas and would pass on a black screen.
    sent = assert_screen_lit(badge.state(), "the launcher after a reset")

    # Fixtures of an earlier, interrupted run would shift the app list.
    _delete_if_listed(badge, ERR_APP)
    _delete_if_listed(badge, DEL_APP)
    ids = _app_ids(badge)
    assert NATIVE_APP in ids, "LIST has no %s: %s" % (NATIVE_APP, ids)
    assert len(ids) >= 3, "the grid test needs three apps (the native ones); LIST has %s" % ids
    _wait_screen(badge, "launcher", "after the fixture clean-up")

    first = _shot(badge, "launcher", theme)

    # 2. Grid navigation. From app 0: RIGHT -> app 1; DOWN -> app 3, or the last app when there are
    # three. UP then LEFT is back on app 0 in both cases.
    badge.btn("right", "tap")
    assert_screen_sent(badge, sent, "the launcher after a RIGHT tap")
    right = badge.shot()
    assert right != first, "a RIGHT tap did not change the launcher"
    badge.btn("down", "tap")
    down = badge.shot()
    assert down != right, "a DOWN tap did not change the launcher"
    badge.btn("up", "tap")
    badge.btn("left", "tap")
    _wait_screen(badge, "launcher", "after grid navigation")

    # 3. Settings and back.
    badge.btn("b", "tap")
    _wait_screen(badge, "settings", "CANCEL on the launcher")
    _shot(badge, "settings", theme)
    badge.btn("b", "tap")
    _wait_screen(badge, "launcher", "CANCEL on the settings list")

    # 4. An app that exits returns to the launcher.
    launch(badge, NATIVE_APP)
    badge.btn("b", "tap")
    _wait_screen(badge, "launcher", "CANCEL in %s" % NATIVE_APP)

    # 5. An app that dies lands on the error screen.
    _push(badge, ERR_APP, ERR_FILES)
    badge.run(ERR_APP)
    _wait_screen(badge, "app_error", "launching an app that calls error()", timeout=10)
    _shot(badge, "app_error", theme)
    badge.btn("b", "tap")
    _wait_screen(badge, "launcher", "CANCEL on the error screen")
    _authed(badge, "DEL " + ERR_APP)
    assert ERR_APP not in _app_ids(badge), "DEL %s left it in LIST" % ERR_APP

    # 6. The delete confirmation, by holding RIGHT on a Lua app.
    _push(badge, DEL_APP, DEL_FILES)
    ids = _app_ids(badge)
    assert DEL_APP in ids, "the pushed %s is not in LIST: %s" % (DEL_APP, ids)
    index = ids.index(DEL_APP)
    assert index < ids.index(NATIVE_APP), "a Lua app is listed after a native app: %s" % ids
    _wait_screen(badge, "launcher", "after pushing %s" % DEL_APP)
    # The fixture has no category: it is a cell of the top level (VKSTATE `menu`).
    launcher_cursor_to(badge, DEL_APP)

    badge.btn("right", "hold", 900)
    _wait_screen(badge, "app_delete", "holding RIGHT on %s" % DEL_APP)
    _shot(badge, "app_delete", theme)
    badge.btn("b", "tap")
    _wait_screen(badge, "launcher", "CANCEL on the delete confirmation")
    assert DEL_APP in _app_ids(badge), "CANCEL on the delete confirmation deleted %s" % DEL_APP

    badge.btn("right", "hold", 900)
    _wait_screen(badge, "app_delete", "holding RIGHT on %s a second time" % DEL_APP)
    badge.btn("a", "tap")
    _wait_screen(badge, "launcher", "SELECT on the delete confirmation")
    assert DEL_APP not in _app_ids(badge), "SELECT on the delete confirmation left %s in LIST" % DEL_APP

    to_launcher(badge)
