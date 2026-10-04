"""The approval engine and screen on a badge (WP12), driven with the sample approvals of VKDEMOAPPROVE.

Needs one badge with a dev build, provisioned with common.provision_test (approval_tmo_s 10,
hold_ms 1000; other values work, the test reads both). No network, no hands.

In order:
  1. green, amber, red: VKSTATE of each, a screenshot saved as shots/approval_<severity>_<theme>.png,
     then the select rule of each (press / hold / closed) and the answer the caller gets.
  2. The launcher is repainted after an approval closes over it (hook H20).
  3. T-APR2: SELECT held before the approval opens never approves; the phase stays WAIT_RELEASE.
  4. A pushed STOP and a pushed RUN during the approval are applied only after it closes (hook H4;
     the serial half of T-REQ6).
  5. T-APR3: an unanswered approval closes after approval_tmo_s.

Run it once per theme (VKSET theme receipt-dark / receipt-light) to get both sets of pictures.

Not here:
  - T-LED1 (hands): watch the LEDs during boot. They fill in order as the stages complete (with two
    LEDs: the first flashes below 50 %, then is solid while the second flashes) and all are lit at
    "Ready".
  - T-LED2 (hands): open VKDEMOAPPROVE green, amber and red and look at the LEDs. Green pulses
    slowly, amber pulses faster, red is solid; approving flashes green three times, refusing blinks
    red once.
  - `DEL <running app>` while an approval is open (implementation-plan.md, review focus 2): it
    needs a signing approval owned by an app, which the demo confirmation is not. It belongs with
    the signtest app's tests.
"""

import os
import time

from common import HOLD_MARGIN_MS, hold_ms, launch, to_launcher

_HERE = os.path.dirname(os.path.abspath(__file__))
SHOTS = os.path.join(_HERE, "shots")
HELLO = os.path.normpath(os.path.join(_HERE, "..", "..", "apps", "hello"))

SHOT_BYTES = 320 * 240 * 2

# What VKDEMOAPPROVE raises (src/vk/features/devtools/demo_approve.cpp).
DEMO = {
    "green": {"select": "press", "headline": "VERIFIED - PRESENT", "red_reason": "ok"},
    "amber": {"select": "hold", "headline": "VERIFIED - NOT PRESENT", "red_reason": "ok"},
    "red": {"select": "disabled", "headline": "UNVERIFIED RECIPIENT", "red_reason": "unverified"},
}
DEMO_LINES = [["Account", "2awX..6wrr"], ["Kind", "merchant"]]


def theme_name(badge):
    """The active theme's name; an empty or missing config key means receipt-light."""
    reply = badge.cmd("VKGET theme")[-1]
    name = reply[3:].strip() if reply.startswith("OK ") else ""
    return name or "receipt-light"


def timeout_s(badge):
    try:
        return int(badge.ok("VKGET approval_tmo_s"))
    except (AssertionError, ValueError):
        return 10


def open_demo(badge, severity):
    """Raises the sample approval and returns the state once it is ARMED (keys seen up)."""
    badge.ok("VKDEMOAPPROVE " + severity)
    return badge.wait_state(lambda s: s["modal"] and s["phase"] == "ARMED", timeout=3)


def wait_closed(badge, timeout=5):
    return badge.wait_state(lambda s: not s["modal"], timeout=timeout)


def same_fraction(first, second):
    """The share of pixels that are equal in two screenshots."""
    assert len(first) == len(second) == SHOT_BYTES, "screenshots are %d and %d bytes" % (len(first), len(second))
    equal = sum(1 for i in range(0, SHOT_BYTES, 2) if first[i:i + 2] == second[i:i + 2])
    return equal / (SHOT_BYTES // 2)


def check_demo_state(state, severity):
    want = DEMO[severity]
    assert state["modal"] is True, "%s: modal is %r" % (severity, state["modal"])
    assert state["phase"] == "ARMED", "%s: phase %s" % (severity, state["phase"])
    assert state["severity"] == severity, "%s: severity %s" % (severity, state["severity"])
    assert state["select"] == want["select"], "%s: select %s" % (severity, state["select"])
    assert state["title"] == "Pay", "%s: title %r" % (severity, state["title"])
    assert state["headline"] == want["headline"], "%s: headline %r" % (severity, state["headline"])
    assert state["big"] == "10.00 HACK", "%s: big %r" % (severity, state["big"])
    assert state["sub"] == "to Demo Merchant", "%s: sub %r" % (severity, state["sub"])
    assert state["lines"] == DEMO_LINES, "%s: lines %r" % (severity, state["lines"])
    assert state["red_reason"] == want["red_reason"], "%s: red_reason %s" % (severity, state["red_reason"])
    assert state["dev_override"] is False, "%s: dev_override %r" % (severity, state["dev_override"])


def severities(badge, theme, hold):
    """Step 1: every severity, its screenshot, its select rule and its answer."""
    to_launcher(badge)
    launcher = badge.shot()
    os.makedirs(SHOTS, exist_ok=True)

    for severity in ("green", "amber", "red"):
        badge.clear_log()
        check_demo_state(open_demo(badge, severity), severity)

        # A second approval cannot open over the first.
        busy = badge.cmd("VKDEMOAPPROVE green")[-1]
        assert busy == "ERR busy", "%s: a second VKDEMOAPPROVE answered %s" % (severity, busy)

        picture = badge.shot(os.path.join(SHOTS, "approval_%s_%s.png" % (severity, theme)))
        assert len(picture) == SHOT_BYTES, "%s: screenshot is %d bytes" % (severity, len(picture))
        assert same_fraction(picture, launcher) < 0.5, "%s: the approval is not drawn over the launcher" % severity

        if severity == "green":
            # PRESS: one tap of SELECT approves.
            badge.btn("a", "tap")
            state = badge.state()
            assert state["phase"] in ("RESULT", "IDLE"), "green: phase %s after SELECT" % state["phase"]
            answer = "approved"
        elif severity == "amber":
            # HOLD: a tap is a hold released early; the full hold approves.
            badge.btn("a", "tap")
            state = badge.state()
            assert state["modal"] and state["phase"] == "ARMED", "amber: a tap of SELECT gave phase %s" % state["phase"]
            badge.btn("a", "hold", hold)
            answer = "approved"
        else:
            # RED is closed: neither a tap nor a hold of SELECT does anything; CANCEL closes it.
            badge.btn("a", "tap")
            state = badge.state()
            assert state["modal"] and state["phase"] == "ARMED", "red: a tap of SELECT gave phase %s" % state["phase"]
            badge.btn("a", "hold", hold)
            state = badge.state()
            assert state["modal"] and state["phase"] == "ARMED", "red: a hold of SELECT gave phase %s" % state["phase"]
            badge.btn("b", "tap")
            answer = "refused"

        closed = wait_closed(badge)
        assert closed["phase"] == "IDLE" and closed["title"] == "", "%s: state after closing: %s" % (severity, closed)
        badge.wait_log(r"demo approval %s: %s" % (severity, answer), timeout=3)

    # CANCEL on a green approval refuses it.
    badge.clear_log()
    open_demo(badge, "green")
    badge.btn("b", "tap")
    wait_closed(badge)
    badge.wait_log(r"demo approval green: refused", timeout=3)


def launcher_repaint(badge):
    """Step 2: after an approval closes over the launcher, the launcher is on the screen again."""
    to_launcher(badge)
    before = badge.shot()
    open_demo(badge, "green")
    during = badge.shot()
    assert same_fraction(during, before) < 0.5, "the approval did not replace the launcher on the screen"
    badge.btn("b", "tap")
    wait_closed(badge)
    time.sleep(0.3)
    after = badge.shot()
    assert same_fraction(after, during) < 0.5, "the approval is still on the screen after it closed"
    alike = same_fraction(after, before)
    assert alike > 0.9, "the launcher was not repainted after the approval closed (%.0f %% as before)" % (alike * 100)


def fresh_press(badge):
    """Step 3, T-APR2. Runs over the hello app, where a SELECT press does nothing else."""
    badge.btn("a", "press")
    try:
        badge.ok("VKDEMOAPPROVE green")
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            state = badge.state()
            assert state["modal"], "T-APR2: the approval closed while SELECT was held from before it opened"
            assert state["phase"] == "WAIT_RELEASE", "T-APR2: phase %s while SELECT is held from before" % state["phase"]
            time.sleep(0.1)
    finally:
        badge.btn("a", "release")
    state = badge.wait_state(lambda s: s["phase"] != "WAIT_RELEASE", timeout=3)
    assert state["modal"] and state["phase"] == "ARMED", "T-APR2: after the release: %s" % state["phase"]
    badge.clear_log()
    badge.btn("b", "tap")
    wait_closed(badge)
    badge.wait_log(r"demo approval green: refused", timeout=3)


def stop_and_run_while_modal(badge):
    """Step 4: a pushed STOP and a pushed RUN wait for the approval to close."""
    state = badge.state()
    assert state["app"] == "hello" and not state["modal"], "hello is not running: %s" % state

    # STOP while the approval is up.
    open_demo(badge, "green")
    badge.stop()
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        state = badge.state()
        assert state["modal"], "the approval closed by itself"
        assert state["app"] == "hello", "STOP was applied while the approval was open (app %r)" % state["app"]
        time.sleep(0.1)
    badge.btn("b", "tap")
    wait_closed(badge)
    stopped = badge.wait_state(lambda s: s["app"] != "hello", timeout=5)
    idle_app = stopped["app"]   # "" with upstream's launcher, the home app's id once there is one

    # RUN while the approval is up.
    time.sleep(0.5)
    open_demo(badge, "green")
    badge.run("hello")
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        state = badge.state()
        assert state["modal"], "the approval closed by itself"
        assert state["app"] == idle_app, "RUN was applied while the approval was open (app %r)" % state["app"]
        time.sleep(0.1)
    badge.btn("b", "tap")
    wait_closed(badge)
    badge.wait_state(lambda s: s["app"] == "hello", timeout=5)


def unanswered(badge):
    """Step 5, T-APR3."""
    limit = timeout_s(badge)
    badge.clear_log()
    started = time.monotonic()
    open_demo(badge, "amber")
    time.sleep(max(0.0, limit - 2.0 - (time.monotonic() - started)))
    state = badge.state()
    assert state["modal"] and state["phase"] == "ARMED", (
        "T-APR3: the approval closed before approval_tmo_s (%d s): %s" % (limit, state["phase"]))
    time.sleep(max(0.0, limit + 2.0 - (time.monotonic() - started)))
    state = badge.state()
    assert not state["modal"], "T-APR3: still open %d s after it was raised (phase %s)" % (limit + 2, state["phase"])
    badge.wait_log(r"demo approval amber: refused", timeout=3)


def run(badge):
    theme = theme_name(badge)
    hold = hold_ms(badge) + HOLD_MARGIN_MS

    severities(badge, theme, hold)
    launcher_repaint(badge)

    badge.push(HELLO)
    launch(badge, "hello")
    try:
        fresh_press(badge)
        stop_and_run_while_modal(badge)
    finally:
        to_launcher(badge)

    unanswered(badge)
    to_launcher(badge)
