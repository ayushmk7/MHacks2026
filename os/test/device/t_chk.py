"""T-CHK2 to T-CHK9, the replayed request, and review focus 1: the firmware checks who is being
paid and shows the right verdict for every kind of payment (testing.md, Acceptance tests;
wallet/checks.md; implementation-plan.md, WP21).

Needs one badge with a dev build. No network, no hands. Every case is built on the laptop
(test/device/fixtures.py): the message with the badge's own key as account 0, the registry record
signed with the test issuer key of vectors.json, the payment request signed with the test device
key. The badge is provisioned to that issuer by common.provision_test (token HACK, 2 decimals,
cap 100.00, max 1000.00, record_ttl_s 3600) and its clock is set with VKTIME. The checktest app
(apps/checktest) passes each case to wallet.begin_solana.

Each case asserts the severity, the headline, the select rule, the lines the verdict names, and
what wallet.poll() returns after the approval closes. A screenshot of each approval is saved as
shots/chk_<case>.png.

  T-CHK2  valid record, no request                  amber VERIFIED - NOT PRESENT, hold (then signed)
  T-CHK3  no record                                 red UNVERIFIED RECIPIENT, dev override in the dev profile only
  T-CHK4  one bit of the record signature flipped   red UNVERIFIED RECIPIENT, not overridable
  T-CHK5  revoked record                            red REVOKED
  T-CHK6  transfer to another account               red WRONG RECIPIENT, line Expected
  T-CHK7  request for 10.00, transfer for 500.00    red WRONG AMOUNT, line Requested 10.00 HACK
  T-CHK8  150.00 (above cap); 1500.00 (above max)   hold with line Limit; red OVER LIMIT
  replay  valid record and request, no presence     amber VERIFIED - NOT PRESENT; the app still runs
                                                    after verifying both inside one callback
  T-CHK9  clock past the record's expiry;           red EXPIRED;
          then a reset and a fresh record           amber CLOCK UNSYNCED

T-CHK1 (green, VERIFIED - PRESENT) needs a second badge to answer the presence challenge; it is
in t_req.py.

T-CHK9 moves the badge's clock forward and resets the badge. The test puts the clock back
(VKTIME now) and leaves the badge provisioned with the test values and the launcher on the
screen. It prints the "[vk] verify" times (measurement M2) and the time from begin_solana to the
first frame of the approval (M3).
"""

import hashlib
import os
import statistics
import time

from common import HOLD_MARGIN_MS, hold_ms, provision_test, to_launcher
from fixtures import (CHECKTEST, Keys, badge_pubkey, cancel, ct_ticks, ct_wait, install_checktest, lines_of, log_ms,
                      lua_errors, open_case, poll_result, set_time, short_address, verify)

_HERE = os.path.dirname(os.path.abspath(__file__))
SHOTS = os.path.join(_HERE, "shots")

SIGN_LINE = r"\[vk\] sign solana (\d+) bytes \d+ ms"
RECORD_LIFE_S = 3600        # how long after "now" the test records expire
ISSUED_AGO_S = 5            # how long before "now" they were issued


class Run:
    """What every case needs, and the measurements collected on the way."""

    def __init__(self, badge):
        self.badge = badge
        self.keys = Keys()
        self.own = badge_pubkey(badge)
        self.hold = hold_ms(badge) + HOLD_MARGIN_MS
        self.dev = badge.info().get("profile") == "dev"
        self.verify_ms = []
        self.open_ms = []        # (number of signatures verified, ms from begin_solana to first draw)

    def record(self, **overrides):
        """A record issued a few seconds ago that expires in an hour."""
        now = int(time.time())
        return self.keys.record(now - ISSUED_AGO_S, now + RECORD_LIFE_S, **overrides)

    def open(self, name, msg, record=None, sig=None, req=None):
        """Runs one case up to the open approval and returns its VKSTATE. Saves a screenshot."""
        state = open_case(self.badge, {"msg_hex": msg, "record_hex": record, "sig_hex": sig, "req_hex": req})
        verified = log_ms(self.badge, r"\[vk\] verify (\d+) ms")
        self.verify_ms += verified
        called = log_ms(self.badge, r"\[app\] CT call (\d+)")
        drawn = log_ms(self.badge, r"\[vk\] approval first draw at (\d+) ms")
        if called and drawn:
            after = [ms for ms in drawn if ms >= called[-1]]   # not the consent prompt's first draw
            if after:
                self.open_ms.append((len(verified), after[0] - called[-1]))
        os.makedirs(SHOTS, exist_ok=True)
        self.badge.shot(os.path.join(SHOTS, "chk_%s.png" % name))
        assert state["title"] == "Pay", "%s: title %r" % (name, state["title"])
        return state

    def refused(self, name, reason):
        """A hold of SELECT must do nothing; CANCEL closes; poll() reports why it was red."""
        self.badge.btn("a", "hold", self.hold)
        state = self.badge.state()
        assert state["modal"] and state["phase"] == "ARMED", "%s: a hold of SELECT gave phase %s" % (name, state["phase"])
        answer = cancel(self.badge)
        assert answer == ("err", reason), "%s: poll() gave %r, expected %s" % (name, answer, reason)
        assert not log_ms(self.badge, SIGN_LINE), "%s: the badge signed a blocked payment" % name


def expect(state, name, severity, headline, select, big, sub, red_reason="ok", dev_override=False):
    assert state["severity"] == severity, "%s: severity %s" % (name, state["severity"])
    assert state["headline"] == headline, "%s: headline %r" % (name, state["headline"])
    assert state["select"] == select, "%s: select %s" % (name, state["select"])
    assert state["big"] == big, "%s: big %r" % (name, state["big"])
    assert state["sub"] == sub, "%s: sub %r" % (name, state["sub"])
    assert state["red_reason"] == red_reason, "%s: red_reason %s" % (name, state["red_reason"])
    assert state["dev_override"] is dev_override, "%s: dev_override %r" % (name, state["dev_override"])


def expect_line(state, name, label, value):
    lines = lines_of(state)
    assert lines.get(label) == value, "%s: line %s is %r, expected %r (lines %r)" % (
        name, label, lines.get(label), value, state["lines"])


def chk2(run):
    """Valid record, no request: amber, and the hold signs."""
    keys = run.keys
    msg = keys.transfer(run.own)
    record, sig = run.record()
    state = run.open("T-CHK2", msg, record, sig)
    expect(state, "T-CHK2", "amber", "VERIFIED - NOT PRESENT", "hold", "10.00 HACK", "to " + keys.merchant_name)
    expect_line(state, "T-CHK2", "Account", short_address(keys.merchant_ata))
    expect_line(state, "T-CHK2", "Kind", "merchant")

    run.badge.btn("a", "tap")
    state = run.badge.state()
    assert state["modal"] and state["phase"] == "ARMED", "T-CHK2: a tap of SELECT gave phase %s" % state["phase"]
    run.badge.btn("a", "hold", run.hold)
    kind, signature = poll_result(run.badge)
    assert kind == "sig", "T-CHK2: after the hold poll() gave %s" % signature
    assert verify(run.own, msg, signature), "T-CHK2: the signature does not verify with the badge's key"


def chk3(run):
    """No record: red, and the dev override exists only in the dev profile."""
    msg = run.keys.transfer(run.own)
    state = run.open("T-CHK3", msg)
    expect(state, "T-CHK3", "red", "UNVERIFIED RECIPIENT", "hold" if run.dev else "disabled", "10.00 HACK",
           "to unverified recipient", "unverified", dev_override=run.dev)
    expect_line(state, "T-CHK3", "Account", short_address(run.keys.merchant_ata))
    assert "Kind" not in lines_of(state), "T-CHK3: a Kind line without a record: %r" % state["lines"]
    answer = cancel(run.badge)
    assert answer == ("err", "unverified"), "T-CHK3: poll() gave %r" % (answer,)


def chk4(run):
    """A record whose issuer signature has one bit flipped."""
    record, sig = run.record()
    bad = bytearray(sig)
    bad[10] ^= 0x01
    state = run.open("T-CHK4", run.keys.transfer(run.own), record, bytes(bad))
    expect(state, "T-CHK4", "red", "UNVERIFIED RECIPIENT", "disabled", "10.00 HACK", "to unverified recipient",
           "unverified")
    run.refused("T-CHK4", "unverified")


def chk5(run):
    """A revoked record, correctly signed."""
    record, sig = run.record(status="revoked")
    state = run.open("T-CHK5", run.keys.transfer(run.own), record, sig)
    expect(state, "T-CHK5", "red", "REVOKED", "disabled", "10.00 HACK", "to " + run.keys.merchant_name, "revoked")
    run.refused("T-CHK5", "revoked")


def chk6(run):
    """The merchant's record, but the transfer pays another account."""
    keys = run.keys
    other = hashlib.sha256(b"badge-os-test-other-account").digest()
    record, sig = run.record()
    state = run.open("T-CHK6", keys.transfer(run.own, dest=other), record, sig)
    expect(state, "T-CHK6", "red", "WRONG RECIPIENT", "disabled", "10.00 HACK", "to " + keys.merchant_name, "mismatch")
    expect_line(state, "T-CHK6", "Expected", short_address(keys.merchant_ata))
    expect_line(state, "T-CHK6", "Account", short_address(other))
    run.refused("T-CHK6", "mismatch")


def chk7(run):
    """The merchant asked for 10.00; the transfer is for 500.00."""
    keys = run.keys
    record, sig = run.record()
    req = keys.req(1000, int(time.time()) + 600)
    state = run.open("T-CHK7", keys.transfer(run.own, 50000), record, sig, req)
    expect(state, "T-CHK7", "red", "WRONG AMOUNT", "disabled", "500.00 HACK", "to " + keys.merchant_name, "mismatch")
    expect_line(state, "T-CHK7", "Requested", "10.00 HACK")
    run.refused("T-CHK7", "mismatch")


def chk8(run):
    """Above the cap the approval is a hold with a Limit line; above the max it is blocked."""
    keys = run.keys
    record, sig = run.record()
    state = run.open("T-CHK8-cap", keys.transfer(run.own, 15000), record, sig)
    expect(state, "T-CHK8 cap", "amber", "VERIFIED - NOT PRESENT", "hold", "150.00 HACK", "to " + keys.merchant_name)
    expect_line(state, "T-CHK8 cap", "Limit", "over 100.00 HACK")
    answer = cancel(run.badge)
    assert answer == ("err", "cancelled"), "T-CHK8 cap: poll() gave %r" % (answer,)

    record, sig = run.record()
    state = run.open("T-CHK8-max", keys.transfer(run.own, 150000), record, sig)
    assert state["severity"] == "red" and state["headline"] == "OVER LIMIT", "T-CHK8 max: %s %r" % (
        state["severity"], state["headline"])
    assert state["select"] == "disabled" and state["dev_override"] is False, "T-CHK8 max: select %s, dev_override %r" % (
        state["select"], state["dev_override"])
    assert state["red_reason"] == "over_cap", "T-CHK8 max: red_reason %s" % state["red_reason"]
    assert state["big"] == "1500.00 HACK", "T-CHK8 max: big %r" % state["big"]
    run.refused("T-CHK8 max", "over_cap")


def replay(run):
    """A valid record and a valid request with no presence proof (what a replayed REQ gives), and
    review focus 1: both signatures are verified inside one Lua callback and the app survives."""
    keys = run.keys
    record, sig = run.record()
    req = keys.req(1000, int(time.time()) + 600)
    state = run.open("replay", keys.transfer(run.own), record, sig, req)
    expect(state, "replay", "amber", "VERIFIED - NOT PRESENT", "hold", "10.00 HACK", "to " + keys.merchant_name)
    expect_line(state, "replay", "Kind", "merchant")
    before = ct_ticks(run.badge)
    answer = cancel(run.badge)
    assert answer == ("err", "cancelled"), "replay: poll() gave %r" % (answer,)

    # Review focus 1: the app is still running and still gets its frames.
    ct_wait(run.badge, r"tick %d\b" % ((before[-1] if before else 0) + 1), timeout=5)
    state = run.badge.state()
    assert state["app"] == CHECKTEST, "the app did not survive begin_solana with a record and a request: %s" % state
    errors = lua_errors(run.badge)
    assert not errors, "the Lua runtime reported: %r" % errors


def chk9(run):
    """The clock: past the record's expiry, then never synced."""
    badge, keys = run.badge, run.keys
    msg = keys.transfer(run.own)

    # First half: the badge's clock is moved one minute past the record's expiry.
    now = int(time.time())
    record, sig = keys.record(now - ISSUED_AGO_S, now + RECORD_LIFE_S)
    set_time(badge, now + RECORD_LIFE_S + 60)
    try:
        state = run.open("T-CHK9-expired", msg, record, sig)
        expect(state, "T-CHK9 expired", "red", "EXPIRED", "disabled", "10.00 HACK", "to " + keys.merchant_name, "expired")
        run.refused("T-CHK9 expired", "expired")
    finally:
        set_time(badge)

    # Second half: after a reset the clock has no source, and a fresh record raises it to the
    # floor: the payment can be amber at best.
    to_launcher(badge)
    badge.reset()
    info = badge.info()
    record, sig = run.record()
    state = run.open("T-CHK9-unsynced", msg, record, sig)
    if state["time"] == "floor":
        expect(state, "T-CHK9 unsynced", "amber", "CLOCK UNSYNCED", "hold", "10.00 HACK", "to " + keys.merchant_name)
    else:
        # A saved Wi-Fi network was in range and SNTP answered before the record was checked.
        assert info.get("wifi") == "1" and state["time"] == "sntp", (
            "T-CHK9 unsynced: after a reset and a verified record the clock source is %r (VKINFO wifi=%s time=%s)" % (
                state["time"], info.get("wifi"), info.get("time")))
        print("note: Wi-Fi synced the clock after the reset, so CLOCK UNSYNCED cannot be shown on this badge now")
        expect(state, "T-CHK9 synced", "amber", "VERIFIED - NOT PRESENT", "hold", "10.00 HACK", "to " + keys.merchant_name)
    answer = cancel(badge)
    assert answer == ("err", "cancelled"), "T-CHK9 unsynced: poll() gave %r" % (answer,)


def run(badge):
    provision_test(badge)
    set_time(badge)
    to_launcher(badge)
    install_checktest(badge)
    case = Run(badge)
    try:
        chk2(case)
        chk3(case)
        chk4(case)
        chk5(case)
        chk6(case)
        chk7(case)
        chk8(case)
        replay(case)
        chk9(case)
    finally:
        try:
            set_time(badge)
        finally:
            to_launcher(badge)

    if case.verify_ms:
        print("M2 verify ms: %s (median %d)" % (case.verify_ms, statistics.median(case.verify_ms)))
    for verified, ms in case.open_ms:
        print("M3 begin_solana to first draw, %d signature(s) verified: %d ms" % (verified, ms))
