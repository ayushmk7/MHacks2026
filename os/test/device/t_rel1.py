"""T-REL1, scripted: every shipped app returns to the launcher on CANCEL and on the force-quit hold
(docs/os/testing/testing.md, "Release gate").

Needs one badge with a dev build and the apps installed (scripts/push-apps.sh). No network.

For each app:
  1. launch it; tap CANCEL up to five times; VKSTATE must reach app "" and screen "launcher";
  2. launch it again; hold CANCEL for 1.7 s (upstream's force-quit); the same.
After each return the glass is checked too: backlight on, and the launcher was sent to the panel.

An app that is not installed is reported and skipped; the test fails if none of the shipped Lua
apps is installed. The by-hand half of T-REL1 (real keys, every screen of every app) stays with a
person.
"""

import time

from common import assert_screen_lit, launch, lua_apps, on_launcher, provision_test, to_launcher

# Every shipped app: the Lua apps push-apps.sh installs for a release (their manifests say which),
# and every native app the build lists (LIST shows a native app with size 0). Nothing is listed here.
LUA_APPS = tuple(lua_apps("release"))
CANCEL_TAPS = 5
FORCE_QUIT_MS = 1700


def _listed(badge):
    """LIST as {id: size}."""
    reply = badge.cmd("AUTH " + badge.ok("VKPAIR"))[-1]
    assert reply.startswith("OK"), "AUTH -> %s" % reply
    listing = badge.cmd("LIST")
    assert listing[-1].startswith("OK"), "LIST -> %s" % listing[-1]
    return {line.split()[1]: int(line.split()[2]) for line in listing[:-1] if len(line.split()) >= 3}


def _back_by_cancel(badge, app_id):
    state = badge.state()
    for _ in range(CANCEL_TAPS):
        if on_launcher(state):
            break
        badge.btn("b", "tap")
        time.sleep(0.3)
        state = badge.state()
    assert on_launcher(state), "%s: %d CANCEL taps did not reach the launcher: app %r, screen %r" % (
        app_id, CANCEL_TAPS, state["app"], state.get("screen"))
    assert_screen_lit(state, "%s after CANCEL" % app_id)


def _back_by_hold(badge, app_id):
    badge.btn("b", "hold", FORCE_QUIT_MS)
    try:
        state = badge.wait_state(on_launcher, timeout=5)
    except AssertionError:
        state = badge.state()
        raise AssertionError("%s: a %d ms CANCEL hold did not reach the launcher: app %r, screen %r" % (
            app_id, FORCE_QUIT_MS, state["app"], state.get("screen")))
    assert_screen_lit(state, "%s after the force-quit hold" % app_id)


def run(badge):
    if badge.info().get("provisioned") != "1":
        provision_test(badge)
    to_launcher(badge)
    listed = _listed(badge)
    installed = set(listed)
    native_apps = tuple(sorted(a for a, size in listed.items() if size == 0 and a not in LUA_APPS))
    apps = [a for a in LUA_APPS + native_apps if a in installed]
    missing = [a for a in LUA_APPS if a not in installed]
    if missing:
        print("T-REL1: not installed, skipped: %s" % ", ".join(missing))
    assert any(a in installed for a in LUA_APPS), "none of the shipped Lua apps is installed: run scripts/push-apps.sh"

    for app_id in apps:
        launch(badge, app_id)
        time.sleep(0.5)
        _back_by_cancel(badge, app_id)

        launch(badge, app_id)
        time.sleep(0.5)
        _back_by_hold(badge, app_id)
        to_launcher(badge)
    print("T-REL1: %d apps return to the launcher on CANCEL and on a %d ms hold: %s" % (
        len(apps), FORCE_QUIT_MS, ", ".join(apps)))
