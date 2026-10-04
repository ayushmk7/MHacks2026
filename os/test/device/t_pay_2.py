"""One whole payment between two badges with the shipped apps (WP41): Request on B, Pay on A.

`badge` is the payer (A), `badge2` the payee (B).

NEEDS two badges, and ALSO THE NETWORK: the payer fetches the payee's registry record and a
blockhash, sends the transaction, and both badges confirm it on chain. So, unlike the other device
tests, this one does NOT provision the badges with the test values (those point rpc_url at
127.0.0.1 and use the test issuer key). It needs this setup, which it checks but does not make:

  - both badges provisioned for the demo (scripts/vkdev.py provision --env) and joined to the
    hotspot, so that listener_url and rpc_url answer and both are on one ESP-NOW channel;
  - the backend running, with a registry record for B's key;
  - A holding at least 10.00 of the default token (and SOL for the fee).

In order:
  1. B starts Request and opens a request for its start amount, 10.00: "REQ open <req_id>".
  2. A starts Pay and lists exactly that request: "PAY list 1". SELECT: "PAY start <req_id>".
  3. The firmware approval opens on A. Green (VERIFIED - PRESENT) is approved with a press, amber
     with a hold; red fails the test, because the setup above is then not in place.
  4. A logs "PAY done <signature>" and shows PAID (shots/pay_2_paid.png).
  5. B, after confirming the same signature on chain itself, logs "REQ paid <signature>" and
     shows PAID (shots/request_2_paid.png).
  6. CANCEL twice on each badge returns to the launcher.
"""

import os
import re
import time

from common import HOLD_MARGIN_MS, hold_ms, launch, to_launcher
from fixtures import set_time
from t_app_pay import APP_DIR as PAY_DIR
from t_app_pay import at_launcher, colours, lua_errors, push_app
from t_app_request import APP_DIR as REQUEST_DIR
from t_app_request import OPEN_TIMEOUT_S, req_wait

NEEDS = "two-badges"   # and the network: see the text above

SHOTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "shots")
AMOUNT = "10.00"

_PAY = r"\[app\] PAY "
_LUA_ERROR = r"\[lua\] .*(?:error|stopping)"

# Presence (two challenges at most), the record, the blockhash: each HTTP call may take 4 s.
APPROVAL_TIMEOUT_S = 40
# Send (two tries) and confirm (every 2 s for 30 s), then the feed call.
DONE_TIMEOUT_S = 60
# The payee confirms on chain itself, every 2 s for up to 30 s.
PAID_TIMEOUT_S = 45


def pay_wait(badge, pattern, timeout=10):
    """The text after 'PAY ' of the first log line since the last clear_log() that matches."""
    line = badge.wait_log("%s(?:%s)|%s" % (_PAY, pattern, _LUA_ERROR), timeout=timeout)
    found = re.search(_PAY + r"(.*)", line)
    assert found, "pay failed: %s" % line
    return found.group(1).strip()


def check_setup(each, who):
    info = each.info()
    assert info.get("provisioned") == "1", (
        "%s is not provisioned: run scripts/vkdev.py provision --env on it first" % who)
    for key in ("rpc_url", "listener_url"):
        assert each.ok("VKGET %s" % key).strip(), "%s has no %s" % (who, key)


def run(badge, badge2):
    payer, payee = badge, badge2
    for each, who in ((payer, "the payer"), (payee, "the payee")):
        to_launcher(each)
        check_setup(each, who)
        set_time(each)
    assert payer.info().get("pubkey") != payee.info().get("pubkey"), "both ports are the same badge"
    push_app(payee, REQUEST_DIR, "request")
    push_app(payer, PAY_DIR, "pay")
    os.makedirs(SHOTS, exist_ok=True)

    try:
        # ---- 1. B asks for 10.00 ---------------------------------------------------------------
        payee.clear_log()
        launch(payee, "request", timeout=20)
        shown = req_wait(payee, r"amount \S+").split()[1]
        assert shown == AMOUNT, "request starts at %s, not %s: check apps/request/config.lua" % (shown, AMOUNT)
        payee.clear_log()
        payee.btn("a", "tap")
        opened = req_wait(payee, r"open \S+|err \S+", timeout=OPEN_TIMEOUT_S)
        assert opened.startswith("open "), "the payee could not open a request: REQ %s" % opened
        req_id = opened.split()[1]

        # ---- 2. A lists it and starts paying ---------------------------------------------------
        payer.clear_log()
        launch(payer, "pay", timeout=20)
        listed = pay_wait(payer, r"list [1-9]\d*", timeout=10)
        assert listed == "list 1", (
            "pay shows %s requests: another badge in range has one open, so the selected row may "
            "not be the payee's. Close it and run again" % listed.split()[1])
        payer.btn("a", "tap")
        started = pay_wait(payer, r"start \S+")
        assert started == "start %s" % req_id, "pay started %r, the payee opened %s" % (started, req_id)

        # ---- 3. The firmware approval ----------------------------------------------------------
        deadline = time.monotonic() + APPROVAL_TIMEOUT_S
        while True:
            state = payer.state()
            if state["modal"]:
                break
            early = [line for line in payer.log() if re.search(_PAY + "failed", line)]
            assert not early, "the payment ended before the approval (network or setup): %s" % early[0]
            assert time.monotonic() < deadline, "no approval within %d s; state: %s" % (APPROVAL_TIMEOUT_S, state)
            time.sleep(0.2)
        state = payer.wait_state(lambda s: s["modal"] and s["phase"] == "ARMED", timeout=5)
        print("approval: %s / %s / %s" % (state["severity"], state["headline"], state["select"]))
        if state["select"] == "press":
            payer.btn("a", "tap")
        elif state["select"] == "hold":
            payer.btn("a", "hold", hold_ms(payer) + HOLD_MARGIN_MS)
        else:
            payer.btn("b", "tap")
            raise AssertionError("the approval is blocked (%s): is the payee registered at the backend, "
                                 "and are both clocks set?" % state["headline"])
        payer.wait_state(lambda s: not s["modal"], timeout=10)

        # ---- 4. A: paid ------------------------------------------------------------------------
        ended = pay_wait(payer, r"done \S+|failed .*", timeout=DONE_TIMEOUT_S)
        assert ended.startswith("done "), "the payment was not made: PAY %s" % ended
        signature = ended.split()[1]
        time.sleep(0.5)
        paid_a = payer.shot(os.path.join(SHOTS, "pay_2_paid.png"))
        assert colours(paid_a) >= 3, "the payer's result screen is blank"

        # ---- 5. B: paid, after its own confirmation --------------------------------------------
        result = req_wait(payee, r"paid \S+", timeout=PAID_TIMEOUT_S)
        assert result == "paid %s" % signature, "the payee confirmed %r, the payer paid %s" % (result, signature)
        time.sleep(0.5)
        paid_b = payee.shot(os.path.join(SHOTS, "request_2_paid.png"))
        assert colours(paid_b) >= 3, "the payee's result screen is blank"

        # ---- 6. Back to the launcher -----------------------------------------------------------
        for each, who in ((payer, "pay"), (payee, "request")):
            each.btn("b", "tap")
            time.sleep(0.3)
            each.btn("b", "tap")
            each.wait_state(at_launcher, timeout=10)
            failed = lua_errors(each)
            assert not failed, "%s logged a Lua error: %s" % (who, failed[0])
    finally:
        for each in (payer, payee):
            to_launcher(each)
