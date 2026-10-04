"""Screen dim and sleep (ui.md, "Screen dim and sleep"), with short timers set over USB.

Needs one badge with a dev build that has the screen service (VKINFO has a `display` field). No
network, no hands. The backlight is read from VKSTATE (`backlight`, the level the panel is driven
at) and the service's state from VKINFO (`display`: awake, dim or sleep).

In order:
  1. On the launcher, with dim_s 2 and sleep_s 5: awake and lit, then dim (a lower level that is
     not 0) after about 2 s, then off after about 5 s.
  2. An injected tap (VKBTN) wakes the screen at the level it had; the launcher is still on top.
     Injected buttons are added after hook H25 and are never swallowed (the swallowing of a real
     wake key is covered by the host suite test_power; see "By hand" below).
  3. Asleep again, an approval (VKDEMOAPPROVE green) lights the screen at once and keeps it lit past
     sleep_s; CANCEL closes it, and the backlight is back at the level it had before the sleep.
  4. With sleep_s 0 and dim_s 0 the screen stays awake.

Leaves dim_s, sleep_s and dim_pct as it found them, and the launcher on the screen.

By hand (T-SLEEP2): let the badge sleep on the launcher, press RIGHT once: the screen lights and
the cursor does not move; press RIGHT again: it moves. Hold CANCEL to wake a sleeping app: the
app is not force-quit.
"""

import time

from common import to_launcher

KEYS = ("dim_s", "sleep_s", "dim_pct")


def get(badge, key):
    reply = badge.cmd("VKGET %s" % key)[-1]
    assert reply.startswith("OK"), "VKGET %s -> %s" % (key, reply)
    return reply[3:].strip()


def put(badge, key, value):
    reply = badge.cmd("VKSET %s %s" % (key, value))[-1]
    assert reply == "OK", "VKSET %s %s -> %s" % (key, value, reply)


def display_state(badge):
    return badge.info().get("display", "")


def wait_display(badge, want, timeout):
    deadline = time.monotonic() + timeout
    while True:
        got = display_state(badge)
        if got == want:
            return badge.state()
        assert time.monotonic() < deadline, "display did not become %r within %g s (it is %r)" % (
            want, timeout, got)
        time.sleep(0.1)


def run(badge):
    to_launcher(badge)
    assert "display" in badge.info(), "VKINFO has no display field: no screen service in this build"
    before = {key: get(badge, key) for key in KEYS}
    try:
        put(badge, "dim_pct", "25")
        put(badge, "dim_s", "2")
        put(badge, "sleep_s", "5")

        # 1. Awake, dim, asleep. A tap first restarts the timer from a known moment (LEFT on the
        # launcher's first cell changes nothing).
        badge.btn("left", "tap")
        state = wait_display(badge, "awake", 2)
        lit = state["backlight"]
        assert lit > 0, "the screen is not lit at the start (backlight=%r)" % lit
        state = wait_display(badge, "dim", 4)
        assert 0 < state["backlight"] < lit, "dim: backlight %r, lit %r" % (state["backlight"], lit)
        state = wait_display(badge, "sleep", 6)
        assert state["backlight"] == 0, "asleep but the backlight is %r" % state["backlight"]
        assert state["screen"] == "launcher", "the screen changed while asleep: %r" % state["screen"]

        # 2. A tap wakes it at the level it had.
        badge.btn("left", "tap")
        state = wait_display(badge, "awake", 2)
        assert state["backlight"] == lit, "woke at %r, was %r" % (state["backlight"], lit)
        assert state["screen"] == "launcher", "waking changed the screen: %r" % state["screen"]

        # 3. An approval lights a sleeping screen and keeps it lit; closing it restores the level.
        wait_display(badge, "sleep", 8)
        badge.ok("VKDEMOAPPROVE green")
        state = badge.wait_state(lambda s: s["modal"], timeout=5)
        assert state["backlight"] > 0, "the approval opened on a dark screen"
        time.sleep(6)                                   # longer than sleep_s
        state = badge.state()
        assert state["modal"] and state["backlight"] > 0, "the screen slept under the approval: %s" % state
        assert display_state(badge) == "awake", "the service is not awake under the approval"
        badge.btn("b", "tap")
        badge.wait_state(lambda s: not s["modal"], timeout=5)
        time.sleep(0.5)
        state = badge.state()
        assert state["backlight"] == lit, "after the approval the backlight is %r, was %r" % (
            state["backlight"], lit)

        # 4. Both off: stays awake.
        put(badge, "dim_s", "0")
        put(badge, "sleep_s", "0")
        badge.btn("left", "tap")
        time.sleep(7)
        assert display_state(badge) == "awake", "dim_s 0 and sleep_s 0, but the screen did not stay awake"
        assert badge.state()["backlight"] == lit
    finally:
        for key, value in before.items():
            put(badge, key, value)
        badge.btn("left", "tap")
    to_launcher(badge)
