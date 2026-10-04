"""T-APP1 to T-APP4 and T-APP7: permissions, first-run consent and the API version
(testing.md, Apps and permissions; platform/app-host.md; implementation-plan.md, WP30).

Needs one badge with a dev build. No network, no hands. Pushes the four fixture apps in
test/device/fixtures/ (noperm, needsign, minapi99, nonet); they stay installed afterwards.

  T-APP1  noperm has no permissions= line and calls wallet.begin_solana: the call raises
          "permission 'sign' not granted (add it to permissions= in app.ini)", caught or not, while
          a wallet function that needs no permission still works. No consent is asked.
  T-APP2  needsign has permissions=sign. The first launch shows the firmware confirmation: title
          "Allow app", headline "NEW PERMISSIONS", amber, hold, one line "May / request payments",
          and the app is not running. CANCEL: nothing is launched and nothing is remembered, so the
          next launch asks again. A tap of SELECT does not approve. A hold does: the app starts and
          holds `sign` but not `net`. A second launch starts with no prompt.
  T-APP3  needsign is pushed again with permissions=sign,net: the launch asks again; after the hold
          the app holds `net` as well, and the launch after that has no prompt.
  T-APP4  minapi99 has min_api=99: the launch is refused with "needs a newer Badge OS (API 99)" and
          its code never runs.
  T-APP7  nonet has an empty permissions= line and uses badge.http.get: the call raises an error
          naming `net`, caught or not; badge.wifi is closed as well, badge.system is not.

The test starts by erasing stored consent (VKRESET, through provision_test), so it can be run again
at any time. It leaves the badge provisioned with the test values and the launcher on the screen.
"""

import os
import time

from common import HOLD_MARGIN_MS, hold_ms, provision_test, to_launcher

_HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES_DIR = os.path.join(_HERE, "fixtures")

NOT_GRANTED = "permission '%s' not granted (add it to permissions= in app.ini)"

# needsign's app.ini as T-APP3 pushes it: the same app with one more permission.
NEEDSIGN_INI_SIGN_NET = (
    "name=Needs sign\n"
    "version=1.0.1\n"
    "author=Badge OS tests\n"
    "description=Dev-only test fixture (T-APP3): now asks for sign and net.\n"
    "permissions=sign,net\n"
    "min_api=2\n"
)

# How long a launch that must not show a prompt is watched for one.
START_TIMEOUT_S = 10


def stop_if_running(badge, app_id):
    """Closes an open confirmation with CANCEL and stops `app_id` if it is the running app."""
    state = badge.state()
    if state["modal"]:
        if state["phase"] != "RESULT":
            badge.btn("b", "tap")
        state = badge.wait_state(lambda s: not s["modal"], timeout=5)
    if state["app"] == app_id:
        badge.stop()
        badge.wait_state(lambda s: s["app"] != app_id, timeout=5)


def push_fixture(badge, app_id, extra=None):
    """Pushes test/device/fixtures/<app_id> (with `extra` files over it) and clears the log."""
    stop_if_running(badge, app_id)
    badge.push(os.path.join(FIXTURES_DIR, app_id), app_id, extra)
    badge.clear_log()


def logged(badge, text):
    return any(text in line for line in badge.log())


def expect_consent(badge, app_id, what):
    """RUN `app_id` and return the state once the consent confirmation is ARMED, after checking
    every field of it (app-host.md, Consent)."""
    badge.run(app_id)
    state = badge.wait_state(lambda s: s["modal"] and s["phase"] == "ARMED", timeout=10)
    assert state["title"] == "Allow app", "%s: the confirmation's title is %r" % (what, state["title"])
    assert state["headline"] == "NEW PERMISSIONS", "%s: the headline is %r" % (what, state["headline"])
    assert state["severity"] == "amber", "%s: the severity is %r" % (what, state["severity"])
    assert state["select"] == "hold", "%s: the select rule is %r" % (what, state["select"])
    assert state["big"] == "Needs sign", "%s: the big text is %r, not the app's name" % (what, state["big"])
    assert state["lines"] == [["May", "request payments"]], (
        "%s: the lines are %r (one per consent permission: sign)" % (what, state["lines"]))
    assert state["app"] != app_id, "%s: %s is running behind its own consent screen" % (what, app_id)
    assert not logged(badge, "NS start"), "%s: the app's code ran before the user approved" % what
    return state


def expect_no_prompt(badge, app_id, what):
    """RUN `app_id` and return once it is running; fails if a confirmation shows up on the way."""
    badge.run(app_id)
    deadline = time.monotonic() + START_TIMEOUT_S
    while True:
        state = badge.state()
        assert not state["modal"], "%s: a confirmation was shown (%r / %r)" % (
            what, state["title"], state["headline"])
        if state["app"] == app_id:
            return state
        assert time.monotonic() < deadline, "%s: %s did not start within %d s" % (what, app_id, START_TIMEOUT_S)
        time.sleep(0.05)


def approve(badge, app_id, hold):
    """Holds SELECT on the open confirmation and waits for the app to be running."""
    badge.btn("a", "hold", hold)
    badge.wait_state(lambda s: not s["modal"], timeout=5)
    badge.wait_state(lambda s: s["app"] == app_id and not s["modal"], timeout=10)
    badge.wait_log(r"\[app\] NS start", timeout=5)


def refused_call(badge, app_id, tag, permission, what):
    """The checks T-APP1 and T-APP7 share: the caught call logged the message, the uncaught one
    stopped the app with it, and no confirmation was shown."""
    message = NOT_GRANTED % permission
    line = badge.wait_log(r"\[app\] %s pcall " % tag, timeout=10)
    assert "%s pcall false " % tag in line and message in line, (
        "%s: the caught call did not raise %r: %s" % (what, message, line))

    line = badge.wait_log(r"\[app\] %s free " % tag, timeout=5)
    assert line.rstrip().endswith("%s free true" % tag), (
        "%s: a function that needs no permission was refused too: %s" % (what, line))

    # Not caught, from on_start: upstream logs the error and stops the app.
    line = badge.wait_log(r"\[lua\] on_start: .*permission '%s' not granted" % permission, timeout=10)
    assert message in line, "%s: the uncaught error is %s" % (what, line)
    badge.wait_log(r"\[lua\] stopping '%s' after error" % app_id, timeout=5)
    state = badge.wait_state(lambda s: s["app"] != app_id, timeout=5)
    assert not state["modal"], "%s: a confirmation is open: %s" % (what, state["title"])
    assert not logged(badge, "%s reached" % tag), "%s: the call returned instead of raising" % what


def app1(badge):
    push_fixture(badge, "noperm")
    badge.run("noperm")
    refused_call(badge, "noperm", "NP", "sign", "T-APP1")
    to_launcher(badge)


def app2(badge, hold):
    push_fixture(badge, "needsign")

    # First launch: the confirmation, and the app is not running.
    expect_consent(badge, "needsign", "T-APP2 first launch")

    # CANCEL: nothing is launched and nothing is stored.
    badge.btn("b", "tap")
    badge.wait_state(lambda s: not s["modal"], timeout=5)
    time.sleep(0.5)
    state = badge.state()
    assert state["app"] != "needsign" and not state["modal"], (
        "T-APP2: after CANCEL on the consent screen the state is %s" % state)
    assert not logged(badge, "NS start"), "T-APP2: the app ran although consent was rejected"
    to_launcher(badge)

    # So the next launch asks again. A tap is not a hold.
    badge.clear_log()
    expect_consent(badge, "needsign", "T-APP2 launch after a rejection")
    badge.btn("a", "tap")
    state = badge.wait_state(lambda s: s["phase"] in ("ARMED", "IDLE"), timeout=5)
    assert state["modal"] and state["app"] != "needsign", (
        "T-APP2: a tap of SELECT closed the consent screen, which needs a hold: %s" % state)

    # The hold approves and the app starts, with `sign` and nothing else.
    approve(badge, "needsign", hold)
    line = badge.wait_log(r"\[app\] NS poll ", timeout=5)
    assert line.rstrip().endswith("NS poll true idle"), (
        "T-APP2: wallet.poll() of an app that holds `sign` gave: %s" % line)
    line = badge.wait_log(r"\[app\] NS net ", timeout=5)
    assert line.rstrip().endswith("NS net false"), "T-APP2: badge.wifi is open to an app without `net`: %s" % line

    # Second launch: no prompt.
    stop_if_running(badge, "needsign")
    badge.clear_log()
    expect_no_prompt(badge, "needsign", "T-APP2 second launch")
    badge.wait_log(r"\[app\] NS start", timeout=5)
    stop_if_running(badge, "needsign")


def app3(badge, hold):
    # The same app with a changed permissions line.
    push_fixture(badge, "needsign", extra={"app.ini": NEEDSIGN_INI_SIGN_NET})
    expect_consent(badge, "needsign", "T-APP3 launch after the permissions changed")
    approve(badge, "needsign", hold)
    line = badge.wait_log(r"\[app\] NS net ", timeout=5)
    assert line.rstrip().endswith("NS net true"), "T-APP3: badge.wifi is closed to an app with `net`: %s" % line
    line = badge.wait_log(r"\[app\] NS poll ", timeout=5)
    assert line.rstrip().endswith("NS poll true idle"), "T-APP3: wallet.poll() gave: %s" % line

    # The new list is what is remembered now.
    stop_if_running(badge, "needsign")
    badge.clear_log()
    expect_no_prompt(badge, "needsign", "T-APP3 second launch")
    stop_if_running(badge, "needsign")


def app4(badge):
    push_fixture(badge, "minapi99")
    badge.run("minapi99")
    line = badge.wait_log(r"needs a newer Badge OS", timeout=10)
    assert "needs a newer Badge OS (API 99)" in line, "T-APP4: the refusal is: %s" % line
    time.sleep(0.5)
    state = badge.state()
    assert state["app"] != "minapi99" and not state["modal"], "T-APP4: after the refusal the state is %s" % state
    assert not logged(badge, "M99 start"), "T-APP4: the app's code ran"
    to_launcher(badge)   # upstream shows the refusal on its error screen


def app7(badge):
    push_fixture(badge, "nonet")
    badge.run("nonet")
    refused_call(badge, "nonet", "NN", "net", "T-APP7")
    line = badge.wait_log(r"\[app\] NN wifi ", timeout=5)
    assert line.rstrip().endswith("NN wifi false"), "T-APP7: badge.wifi is open to an app without `net`: %s" % line
    to_launcher(badge)


def run(badge):
    # Stored consent is erased by VKRESET, which provision_test sends only to a provisioned badge.
    was_provisioned = badge.info().get("provisioned") == "1"
    provision_test(badge)
    if not was_provisioned:
        provision_test(badge)
    hold = hold_ms(badge) + HOLD_MARGIN_MS

    to_launcher(badge)
    try:
        app1(badge)
        app2(badge, hold)
        app3(badge, hold)
        app4(badge)
        app7(badge)
    finally:
        to_launcher(badge)
