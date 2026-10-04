"""Wallet core on one badge: the request memo, the wallet reset, the daily limit and the automatic
rows of the signature log (wallet/checks.md checks 12a and 3a, "Daily limit"; wallet/stores.md
"History", "Reset"; wallet/solana-payments.md "Request memo").

Needs one badge with a dev build. No network, no hands, no second badge. Uses the checktest app
(apps/checktest) with cases built by test/device/fixtures.py, and the reqtest app (apps/reqtest)
through t_req_single's helpers. What the host suites already prove (test_checks, test_payment,
test_stores, test_contacts, test_domains) is not repeated; this checks the same rules on the
badge's own screen, LittleFS and clock.

  WC1  A payment for a request without the request memo: red WRONG MEMO, line Request <req_id>,
       poll() mismatch, nothing signed. The same payment with the memo: amber VERIFIED - NOT
       PRESENT (no presence proof on one badge).
  WC2  VKRESET, approved on the badge: "[vk] history: erased" and "[contact] contacts erased" are
       logged and wallet.history is empty afterwards (the reset's own approval row included), but
       for checktest's consent confirmation (the reset erased the consent list too).
  WC3  day_limit HACK:15.00, confirmed on the badge: a 10.00 payment signs (hold); a second 10.00
       is red DAILY LIMIT with line Today "10.00 of 15.00 HACK", poll() over_daily, nothing
       signed; after a reboot it is still red (the total comes from the log). day_limit cleared
       (confirmed): amber again.
  WC4  A payment request opened by reqtest: one "[vk] sign pay-req", then within FLUSH_QUIET_MS
       plus a margin "[vk] history auto flush 1 ok", and the newest history row is that signature
       (outcome "signed", amount "0").

It leaves the badge provisioned with the test values, day_limit unset, the clock set and the
launcher on the screen. WC2 erases the badge's history and contacts.
"""

import os
import time

from common import HOLD_MARGIN_MS, hold_ms, provision_test, to_launcher
from fixtures import (Keys, badge_pubkey, cancel, ct_wait, install_checktest, lines_of, log_ms, open_case,
                      poll_result, set_time, start_case, stop_app, verify)
from t_req_single import sign_lines, start_reqtest, stop_reqtest

_HERE = os.path.dirname(os.path.abspath(__file__))
SHOTS = os.path.join(_HERE, "shots")

SIGN_SOLANA = r"\[vk\] sign solana (\d+) bytes \d+ ms"
FLUSH_QUIET_MS = 2000          # vk::history::FLUSH_QUIET_MS
DAY_LIMIT = "HACK:15.00"


def newest_history(badge):
    """The checktest app's view of the newest history row: "<outcome> <amount> <symbol>" or "none"."""
    start_case(badge, {"history": True})
    return ct_wait(badge, r"hist \S.*", timeout=10)[len("hist "):].strip()


def approve_confirmation(badge, what):
    """Holds SELECT on the confirmation on the screen and waits for it to close."""
    state = badge.wait_state(lambda s: s["modal"] and s["phase"] == "ARMED", timeout=5)
    assert state["severity"] == "amber" and state["select"] == "hold", "%s: confirmation is %s / %s" % (
        what, state["severity"], state["select"])
    badge.btn("a", "hold", hold_ms(badge) + HOLD_MARGIN_MS)
    badge.wait_state(lambda s: not s["modal"], timeout=5)


def set_secure(badge, key, value):
    """VKSET of a secure key on a provisioned badge, approved on the badge."""
    stop_app(badge)
    to_launcher(badge)
    reply = badge.cmd(("VKSET %s %s" % (key, value)).rstrip())[-1]
    assert reply == "OK pending", "VKSET %s %s -> %s" % (key, value, reply)
    approve_confirmation(badge, "VKSET %s" % key)
    assert badge.ok("VKGET %s" % key) == value, "%s is not %r after the approved change" % (key, value)


class Case:
    def __init__(self, badge):
        self.badge = badge
        self.keys = Keys()
        self.own = badge_pubkey(badge)
        self.hold = hold_ms(badge) + HOLD_MARGIN_MS

    def record(self):
        now = int(time.time())
        return self.keys.record(now - 5, now + 3600)

    def open(self, name, msg, req=None):
        record, sig = self.record()
        self.badge.clear_log()
        state = open_case(self.badge, {"msg_hex": msg, "record_hex": record, "sig_hex": sig, "req_hex": req})
        os.makedirs(SHOTS, exist_ok=True)
        self.badge.shot(os.path.join(SHOTS, "wc_%s.png" % name))
        assert state["title"] == "Pay", "%s: title %r" % (name, state["title"])
        return state

    def refused(self, name, reason):
        self.badge.btn("a", "hold", self.hold)
        state = self.badge.state()
        assert state["modal"] and state["phase"] == "ARMED", "%s: a hold of SELECT gave phase %s" % (name, state["phase"])
        answer = cancel(self.badge)
        assert answer == ("err", reason), "%s: poll() gave %r, expected %s" % (name, answer, reason)
        assert not log_ms(self.badge, SIGN_SOLANA), "%s: the badge signed a blocked payment" % name


def wc1_memo(case):
    keys = case.keys
    req_id = os.urandom(8)
    req = keys.req(1000, int(time.time()) + 600, req_id=req_id)

    state = case.open("wrong_memo", keys.transfer(case.own), req)
    assert state["severity"] == "red" and state["headline"] == "WRONG MEMO", "WC1: %s %r" % (
        state["severity"], state["headline"])
    assert state["red_reason"] == "mismatch" and state["select"] == "disabled", "WC1: %s %s" % (
        state["red_reason"], state["select"])
    assert lines_of(state).get("Request") == req_id.hex(), "WC1: lines %r" % state["lines"]
    case.refused("WC1 no memo", "mismatch")

    state = case.open("wrong_memo_other", keys.transfer(case.own, memo=os.urandom(8).hex()), req)
    assert state["headline"] == "WRONG MEMO", "WC1 other memo: %r" % state["headline"]
    case.refused("WC1 other memo", "mismatch")

    state = case.open("request_memo", keys.transfer(case.own, memo=req_id.hex()), req)
    assert state["severity"] == "amber" and state["headline"] == "VERIFIED - NOT PRESENT", "WC1 memo: %s %r" % (
        state["severity"], state["headline"])
    assert lines_of(state).get("Memo") is not None or len(state["lines"]) == 4, "WC1 memo: lines %r" % state["lines"]
    answer = cancel(case.badge)
    assert answer == ("err", "cancelled"), "WC1 memo: poll() gave %r" % (answer,)


def wc2_reset(badge):
    stop_app(badge)
    to_launcher(badge)
    badge.clear_log()
    reply = badge.cmd("VKRESET")[-1]
    assert reply == "OK pending", "VKRESET -> %s" % reply
    approve_confirmation(badge, "VKRESET")
    badge.wait_log(r"\[vk\] history: erased", timeout=5)
    badge.wait_log(r"\[contact\] contacts erased", timeout=5)
    assert badge.info().get("provisioned") == "0", "VKRESET was approved but the badge is still provisioned"
    provision_test(badge)
    set_time(badge)
    install_checktest(badge)
    # The reset also erased the consent list, so checktest asks again; its consent confirmation
    # is then the only row (an "approved" confirmation), or there is none.
    entry = newest_history(badge)
    assert entry == "none" or entry.startswith("approved 0"), "WC2: after VKRESET the newest history row is %r" % entry


def wc3_daily_limit(case):
    badge, keys = case.badge, case.keys
    set_secure(badge, "day_limit", DAY_LIMIT)
    msg = keys.transfer(case.own, 1000)

    state = case.open("day_first", msg)
    assert state["severity"] == "amber" and state["select"] == "hold", "WC3 first: %s %s %r" % (
        state["severity"], state["select"], state["headline"])
    badge.btn("a", "hold", case.hold)
    kind, sig = poll_result(badge)
    assert kind == "sig" and verify(case.own, msg, sig), "WC3: the first 10.00 was not signed: %s" % kind

    def second(name):
        state = case.open(name, keys.transfer(case.own, 1000))
        assert state["severity"] == "red" and state["headline"] == "DAILY LIMIT", "WC3 %s: %s %r" % (
            name, state["severity"], state["headline"])
        assert state["red_reason"] == "over_daily" and state["dev_override"] is False, "WC3 %s: %s %r" % (
            name, state["red_reason"], state["dev_override"])
        assert lines_of(state).get("Today") == "10.00 of 15.00 HACK", "WC3 %s: lines %r" % (name, state["lines"])
        case.refused("WC3 " + name, "over_daily")

    second("day_over")

    # The total survives a reboot: it is read from the signature log.
    stop_app(badge)
    to_launcher(badge)
    badge.reset()
    set_time(badge)
    second("day_over_rebooted")

    set_secure(badge, "day_limit", "")
    state = case.open("day_cleared", keys.transfer(case.own, 1000))
    assert state["severity"] == "amber", "WC3 cleared: %s %r" % (state["severity"], state["headline"])
    assert cancel(badge) == ("err", "cancelled")


def wc4_auto_rows(badge):
    stop_app(badge)
    to_launcher(badge)
    found = start_reqtest(badge, amount="10.00", count=1)
    assert found and found[0][0] == "open", "WC4: reqtest could not open a request: %s" % found
    assert len(sign_lines(badge, "pay-req")) == 1, "WC4: expected one pay-req signature"
    badge.wait_log(r"\[vk\] history auto flush 1 ok \d+ ms", timeout=FLUSH_QUIET_MS / 1000.0 + 8)
    stop_reqtest(badge)
    to_launcher(badge)
    entry = newest_history(badge)
    assert entry.startswith("signed 0"), "WC4: the newest history row is %r, expected the pay-req signature" % entry


def run(badge):
    assert badge.info().get("profile") == "dev", "t_wallet_core needs a dev build (VKTIME, the flush log line)"
    to_launcher(badge)
    provision_test(badge)
    set_time(badge)
    install_checktest(badge)
    try:
        wc1_memo(Case(badge))
        wc2_reset(badge)
        wc3_daily_limit(Case(badge))
        wc4_auto_rows(badge)
    finally:
        try:
            stop_app(badge)
            to_launcher(badge)
            if badge.ok("VKGET day_limit"):
                set_secure(badge, "day_limit", "")
        finally:
            set_time(badge)
            to_launcher(badge)
