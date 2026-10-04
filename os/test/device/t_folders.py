"""The launcher's folders (docs/os/ui/shell.md, "Launcher"; docs/os/platform/app-host.md,
"Manifest"), on one badge.

What the launcher should show is worked out from the manifests themselves, never from a list:
apps/<id>/app.ini for the Lua apps the badge has (LIST), the last BADGE_APP argument in
src/native_apps/ for the native ones. An app with hidden=1 is nowhere; an app with category=<c> is
in folder <c>; every other app is a cell of the top level.

In order:
  1. The top level (VKSTATE `menu`) is the uncategorised apps, then one "folder:<c>" cell per
     category, alphabetical. Screenshot shots/folders_top_<theme>.png.
  2. Each folder of two or more apps: SELECT opens it (VKSTATE `folder`, the title is its name),
     its cells are exactly its apps; CANCEL is back on the top level with the cursor on the folder.
     Screenshot shots/folders_<c>_<theme>.png.
  3. A folder of one app: SELECT starts that app; CANCEL returns to the launcher.
  4. An app started from inside a folder returns to that folder when it exits.
  5. badge.system.launcher_apps() (what Home lists) is the same set, with the same folders: a
     hidden fixture app logs it.
  6. Manifest changes are seen at once, also when the number of apps does not change: a fixture
     pushed with category=zzcat makes a ZZCAT folder; pushed again without it, it is a top-level
     cell; again with hidden=1, it is gone; DEL removes it.

Needs one badge with a dev build and the apps installed (scripts/push-apps.sh). No network, no
hands. Leaves the launcher's top level on screen and both fixtures deleted.
"""

import os
import re
import tempfile
import time

from common import (FOLDER_KEY, launch, launcher_cursor_to, manifest, on_launcher, open_folder,
                    to_launcher)

_HERE = os.path.dirname(os.path.abspath(__file__))
SHOTS = os.path.join(_HERE, "shots")
NATIVE_DIR = os.path.normpath(os.path.join(_HERE, "..", "..", "src", "native_apps"))

LIST_APP = "zzfoldlist"
MOVE_APP = "zzfoldmove"
MOVE_FOLDER = "zzcat"

LIST_FILES = {
    "app.ini": "name=Folder list\nversion=1.0.0\nauthor=BadgeOS tests\n"
               "description=Dev-only test fixture (t_folders): logs badge.system.launcher_apps().\n"
               "min_api=2\nhidden=1\n",
    "main.lua": 'for _, a in ipairs(badge.system.launcher_apps()) do\n'
                '  badge.log("FOLD " .. a.id .. " " .. (a.category ~= "" and a.category or "-"))\n'
                'end\nbadge.log("FOLD done")\n'
                'function on_button(key, pressed) if key == "b" and pressed then badge.system.exit() end end\n',
}
MOVE_MAIN = 'function on_button(key, pressed) if key == "b" and pressed then badge.system.exit() end end\n'


def _move_ini(extra):
    return ("name=Folder move\nversion=1.0.0\nauthor=BadgeOS tests\n"
            "description=Dev-only test fixture (t_folders).\nmin_api=2\n" + extra)


def _authed(badge, line):
    """One push-protocol command. Upstream drops the session whenever an app stops, so AUTH first."""
    reply = badge.cmd("AUTH " + badge.ok("VKPAIR"))[-1]
    assert reply.startswith("OK"), "AUTH -> %s" % reply
    return badge.cmd(line)


def _listed(badge):
    """LIST as {id: size}."""
    reply = _authed(badge, "LIST")
    assert reply[-1].startswith("OK"), "LIST -> %s" % reply[-1]
    return {line.split()[1]: int(line.split()[2]) for line in reply[:-1] if len(line.split()) >= 3}


def _push(badge, app_id, files):
    with tempfile.TemporaryDirectory() as folder:
        for name, text in files.items():
            with open(os.path.join(folder, name), "w", encoding="utf-8") as handle:
                handle.write(text)
        badge.push(folder, app_id)


def _native_keys():
    """{id: launcher keys as a dict} of every BADGE_APP line under src/native_apps/. Its string
    arguments are the id, name, version, permissions and the optional launcher keys."""
    found = {}
    for root, _, names in os.walk(NATIVE_DIR):
        for name in names:
            if name.endswith(".cpp"):
                with open(os.path.join(root, name), "r", encoding="utf-8") as handle:
                    text = handle.read()
                for match in re.finditer(r'^BADGE_APP\((.*)\);', text, re.M):
                    args = re.findall(r'"([^"]*)"', match.group(1))
                    keys = args[4] if len(args) >= 5 else ""
                    found[args[0]] = dict(kv.split("=", 1) for kv in keys.split(";") if "=" in kv)
    return found


def _expected(badge):
    """{app_id: folder} the launcher must show ("" = top level), from the manifests of the apps the
    badge has. Lua apps without a local folder (pushed by another test) are taken as uncategorised."""
    natives = _native_keys()
    want = {}
    for app_id, size in _listed(badge).items():
        if app_id in (LIST_APP, MOVE_APP):
            continue                                  # this test's own fixtures, checked apart
        keys = natives.get(app_id) if size == 0 and app_id in natives else manifest(app_id)
        if keys.get("hidden", "0").lower() in ("1", "true"):
            continue
        want[app_id] = keys.get("category", "").lower()
    return want


def _top_level(want, menu):
    apps = [key for key in menu if not key.startswith(FOLDER_KEY)]
    folders = [key[len(FOLDER_KEY):] for key in menu if key.startswith(FOLDER_KEY)]
    assert sorted(apps) == sorted(a for a, f in want.items() if f == ""), (
        "top-level apps %s, expected %s" % (sorted(apps), sorted(a for a, f in want.items() if f == "")))
    assert folders == sorted(set(f for f in want.values() if f)), (
        "folders %s, expected %s (alphabetical)" % (folders, sorted(set(f for f in want.values() if f))))
    first_folder = next((i for i, key in enumerate(menu) if key.startswith(FOLDER_KEY)), len(menu))
    assert all(key.startswith(FOLDER_KEY) for key in menu[first_folder:]), "an app cell after a folder: %s" % menu
    return folders


def _members(want, folder):
    return sorted(a for a, f in want.items() if f == folder)


def run(badge):
    os.makedirs(SHOTS, exist_ok=True)
    theme = badge.ok("VKGET theme").strip() or "receipt-light"
    listed = _listed(badge)
    for fixture in (LIST_APP, MOVE_APP):
        if fixture in listed:
            _authed(badge, "DEL " + fixture)

    # 1. The top level.
    want = _expected(badge)
    state = to_launcher(badge)
    assert state["folder"] == "", "VKSTATE folder is %r on the top level" % state["folder"]
    folders = _top_level(want, state["menu"])
    assert folders, "no app declares a category: there is no folder to test"
    badge.shot(os.path.join(SHOTS, "folders_top_%s.png" % theme))
    print("top level: %s" % " ".join(state["menu"]))

    # 2 and 3. Every folder.
    for name in folders:
        members = _members(want, name)
        launcher_cursor_to(badge, FOLDER_KEY + name)
        if len(members) == 1:
            badge.btn("a", "tap")
            started = badge.wait_state(lambda s: s["app"] != "", timeout=10)
            assert started["app"] == members[0], "SELECT on the one-app folder %r started %r, not %r" % (
                name, started["app"], members[0])
            to_launcher(badge)
            print("folder %s: one app, SELECT starts %s" % (name, members[0]))
            continue
        badge.btn("a", "tap")
        inside = badge.wait_state(lambda s: on_launcher(s) and s["folder"] == name, timeout=5)
        assert sorted(inside["menu"]) == members, "folder %r holds %s, expected %s" % (
            name, sorted(inside["menu"]), members)
        badge.shot(os.path.join(SHOTS, "folders_%s_%s.png" % (name, theme)))
        badge.btn("b", "tap")
        back = badge.wait_state(lambda s: on_launcher(s) and s["folder"] == "", timeout=5)
        assert back["menu"][back["cursor"]] == FOLDER_KEY + name, (
            "CANCEL in folder %r left the cursor on %r" % (name, back["menu"][back["cursor"]]))
        print("folder %s: %s" % (name, " ".join(inside["menu"])))

    # 4. An app started inside a folder comes back to it.
    multi = next((f for f in folders if len(_members(want, f)) > 1), None)
    if multi:
        inside = open_folder(badge, multi)
        app = sorted(inside["menu"])[0]
        launcher_cursor_to(badge, app)
        badge.btn("a", "tap")
        badge.wait_state(lambda s: s["app"] == app and not s["modal"], timeout=10)
        badge.stop()
        state = badge.wait_state(lambda s: on_launcher(s), timeout=5)
        assert state["folder"] == multi, "after %s exited the launcher shows folder %r, not %r" % (
            app, state["folder"], multi)
        to_launcher(badge)

    # 5. badge.system.launcher_apps(): what Home lists.
    _push(badge, LIST_APP, LIST_FILES)
    badge.clear_log()
    launch(badge, LIST_APP)
    badge.wait_log(r"FOLD done", timeout=5)
    got = {}
    for line in badge.log():
        match = re.search(r"\[app\] FOLD (\S+) (\S+)\s*$", line)
        if match:
            got[match.group(1)] = "" if match.group(2) == "-" else match.group(2)
    want = _expected(badge)
    assert got == want, "launcher_apps() %s, expected %s" % (got, want)
    to_launcher(badge)
    _authed(badge, "DEL " + LIST_APP)

    # 6. A manifest change is seen at once.
    _push(badge, MOVE_APP, {"app.ini": _move_ini("category=%s\n" % MOVE_FOLDER), "main.lua": MOVE_MAIN})
    state = badge.wait_state(lambda s: on_launcher(s) and FOLDER_KEY + MOVE_FOLDER in s["menu"], timeout=5)
    _push(badge, MOVE_APP, {"app.ini": _move_ini(""), "main.lua": MOVE_MAIN})
    state = badge.wait_state(lambda s: MOVE_APP in s["menu"] and FOLDER_KEY + MOVE_FOLDER not in s["menu"],
                             timeout=5)
    _push(badge, MOVE_APP, {"app.ini": _move_ini("hidden=1\n"), "main.lua": MOVE_MAIN})
    state = badge.wait_state(lambda s: MOVE_APP not in s["menu"], timeout=5)
    _authed(badge, "DEL " + MOVE_APP)
    assert MOVE_APP not in _listed(badge), "DEL %s left it in LIST" % MOVE_APP
    to_launcher(badge)
    time.sleep(0.2)
