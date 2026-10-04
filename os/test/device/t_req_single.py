"""Payment requests on one badge (WP23): what the payee's firmware does with nobody listening.

Drives the reqtest app (apps/reqtest), which opens config.count requests when it starts (one per
frame) and logs "RT open <req_id>" or "RT err <reason>" for each, then "RT status <state> <proofs>"
once a second.

In order:
  1. With the clock unset, request_open is refused: "RT err no_time" (the expiry is a real time).
  2. After VKTIME the request opens: one "[vk] sign pay-req" line per request, status "open 0".
  3. Stopping the app closes its requests: a relaunch opens two again (if the first two were still
     active the firmware would answer busy), with fresh ids.
  4. A third open is refused with busy; an amount with too many decimals with bad_arg.

Needs one badge with a dev build. No network, no hands, no second badge. The test provisions the
badge with the test values (common.provision_test), resets it once (to unset the clock) and leaves
the clock set to the laptop's time.

The two-badge half (T-REQ1 to T-REQ4, T-CHK1) is t_req.py.
"""

import os
import re
import time

from common import launch, provision_test, to_launcher

_HERE = os.path.dirname(os.path.abspath(__file__))
REQTEST_DIR = os.path.normpath(os.path.join(_HERE, "..", "..", "apps", "reqtest"))
REQTEST = "reqtest"

_RT = r"\[app\] RT "
_LUA_ERROR = r"\[lua\] .*(?:error|stopping)"

# A software key signs in about a second, and the app opens its requests one per frame.
START_TIMEOUT_S = 20


def config_lua(amount="10.00", count=1, name=None, ttl_s=None, symbol=None):
    """The text of apps/reqtest/config.lua for one run."""
    rows = ['  amount = "%s",' % amount, "  count = %d," % count]
    if name is not None:
        rows.append('  name = "%s",' % name)
    if ttl_s is not None:
        rows.append("  ttl_s = %d," % ttl_s)
    if symbol is not None:
        rows.append('  symbol = "%s",' % symbol)
    return "return {\n%s\n}\n" % "\n".join(rows)


def stop_reqtest(badge):
    """Stops reqtest if it is the running app."""
    if badge.state()["app"] == REQTEST:
        badge.stop()
        badge.wait_state(lambda s: s["app"] != REQTEST, timeout=5)


def start_reqtest(badge, **config):
    """Stops the app, pushes it with a config.lua made from `config`, clears the log and starts
    it. Returns once the app has logged the outcome of every open it was asked for, as the list
    of outcomes: ("open", req_id) or ("err", reason)."""
    stop_reqtest(badge)
    badge.push(REQTEST_DIR, REQTEST, extra={"config.lua": config_lua(**config)})
    badge.clear_log()
    launch(badge, REQTEST, timeout=START_TIMEOUT_S)
    return outcomes(badge, config.get("count", 1))


def outcomes(badge, count, timeout=START_TIMEOUT_S):
    """The first `count` 'RT open' / 'RT err' lines since the last clear_log(), in order."""
    pattern = re.compile(_RT + r"(open|err) (\S+)")
    deadline = time.monotonic() + timeout
    while True:
        log = badge.log()
        failed = [line for line in log if re.search(_LUA_ERROR, line)]
        assert not failed, "reqtest failed: %s" % failed[0]
        found = [match.groups() for match in (pattern.search(line) for line in log) if match]
        if len(found) >= count:
            return found[:count]
        assert time.monotonic() < deadline, "reqtest logged %d of %d request outcomes within %g s: %s" % (
            len(found), count, timeout, found)
        time.sleep(0.2)


def sign_lines(badge, domain):
    """The '[vk] sign <domain> <n> bytes <ms> ms' lines since the last clear_log(), as (bytes, ms)."""
    pattern = re.compile(r"\[vk\] sign %s (\d+) bytes (\d+) ms" % re.escape(domain))
    return [(int(m.group(1)), int(m.group(2))) for m in (pattern.search(line) for line in badge.log()) if m]


def wait_status(badge, timeout=5):
    """The next 'RT status <state> <proofs>' line, as (state, proofs)."""
    line = badge.wait_log(_RT + r"status \S+ \d+", timeout=timeout)
    match = re.search(_RT + r"status (\S+) (\d+)", line)
    return match.group(1), int(match.group(2))


def run(badge):
    to_launcher(badge)
    provision_test(badge)

    # ---- 1. No clock: no_time ------------------------------------------------------------------
    badge.reset()  # the only way back to time=none; the config survives
    info = badge.info()
    assert info.get("provisioned") == "1", "the badge lost its provisioning over a reset: %s" % info
    if info.get("time") == "none":
        result = start_reqtest(badge, amount="10.00", count=1)
        if result == [("open", result[0][1])] and badge.info().get("time") != "none":
            print("note: the clock synced during the check, so no_time is not asserted")
        else:
            assert result == [("err", "no_time")], "with the clock unset request_open gave %s" % (result,)
            assert not sign_lines(badge, "pay-req"), "a request was signed although the clock is unset"
        stop_reqtest(badge)
    else:
        # A saved Wi-Fi network was in range and SNTP answered before the test could look.
        print("note: VKINFO time=%s right after the reset, so no_time is not asserted" % info.get("time"))

    # ---- 2. With the clock: the request opens --------------------------------------------------
    badge.ok("VKTIME %d" % int(time.time()))
    assert badge.info().get("time") == "sntp", "VKTIME did not set the clock"

    first = start_reqtest(badge, amount="10.00", count=2, name="Test Shop")
    assert [kind for kind, _ in first] == ["open", "open"], "two requests should open: %s" % (first,)
    first_ids = [value for _, value in first]
    assert all(re.fullmatch(r"[0-9a-f]{16}", req_id) for req_id in first_ids), "req_id is not 16 hex: %s" % first_ids
    assert first_ids[0] != first_ids[1], "two requests share one req_id: %s" % first_ids

    signed = sign_lines(badge, "pay-req")
    assert len(signed) == 2, "each request is signed exactly once; the log has %d pay-req signatures" % len(signed)
    # "pay-req:" (8) + the frame up to the end of the name (62 + 9 for "Test Shop").
    assert all(size == 8 + 62 + len("Test Shop") for size, _ in signed), "signed sizes: %s" % (signed,)
    print("pay-req sign ms: %s" % [ms for _, ms in signed])

    state, proofs = wait_status(badge)
    assert (state, proofs) == ("open", 0), "status of an open request nobody challenged: %s %d" % (state, proofs)

    # The app keeps running while the firmware rebroadcasts; nothing here needs a listener.
    time.sleep(2.5)
    assert badge.state()["app"] == REQTEST, "reqtest stopped by itself: %s" % badge.log()[-5:]

    # ---- 3. Stopping the app closes its requests -----------------------------------------------
    second = start_reqtest(badge, amount="10.00", count=2, name="Test Shop")  # stops, then relaunches
    assert [kind for kind, _ in second] == ["open", "open"], (
        "after a stop the relaunched app could not open two requests (the old ones were not closed): %s"
        % (second,))
    second_ids = [value for _, value in second]
    assert not set(first_ids) & set(second_ids), "a req_id was reused: %s then %s" % (first_ids, second_ids)

    # ---- 4. A third open is busy; a malformed amount is bad_arg --------------------------------
    third = start_reqtest(badge, amount="10.00", count=3)
    assert [kind for kind, _ in third[:2]] == ["open", "open"], "the first two of three should open: %s" % (third,)
    assert third[2] == ("err", "busy"), "the third open should be refused with busy: %s" % (third,)
    assert len(sign_lines(badge, "pay-req")) == 2, "the refused third request was signed"

    bad = start_reqtest(badge, amount="10.001", count=1)  # three decimals for a two-decimal token
    assert bad == [("err", "bad_arg")], "an amount with too many decimals gave %s" % (bad,)
    zero = start_reqtest(badge, amount="0.00", count=1)
    assert zero == [("err", "bad_arg")], "a zero amount gave %s" % (zero,)

    stop_reqtest(badge)
    to_launcher(badge)
