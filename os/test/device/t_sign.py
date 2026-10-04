"""T-APR1 (the offline half), T-APR4, T-APR5 and review focus 2: a payment is shown by the
firmware, signed only after the button, and never signed when it cannot be read (testing.md,
Acceptance tests; implementation-plan.md, WP13 and Review focus).

Needs one badge with a dev build. No network, no hands. The test provisions the badge with the test
values (common.provision_test), sets its clock, and pushes the checktest app, which runs one case
per launch (apps/checktest, test/device/fixtures.py). Every message is built on the laptop with the
badge's own key as account 0.

In order:
  1. T-APR1, offline half: a valid transfer with no record. The approval is red "UNVERIFIED
     RECIPIENT" with the dev override; a tap of SELECT does nothing; the full hold signs; the
     signature verifies on the laptop against the badge's VKINFO key. Prints the "[vk] sign" line
     (measurement M2) and the time from begin_solana to the first frame of the approval (M3).
  2. The same transfer built on the badge by wallet.build_transfer: the signature verifies over
     the bytes fixtures.build_transfer_msg makes (with a Memo on a software key).
  3. T-APR4: an app that paints the whole screen and calls gfx.flush() on every frame. While the
     approval is open two screenshots 500 ms apart are identical, none of the app's colour is on
     the screen, and "CT tick" does not advance.
  4. T-APR5: second instruction, wrong mint, two signers, v0 message, trailing bytes. Each is red
     with its headline, SELECT is disabled even in the dev profile, a hold does nothing, and
     poll() gives "undecodable".
  5. Review focus 2: a STOP sent during the approval is applied before the app can poll, and the
     un-polled signature does not make the next begin "busy"; DEL of the app while its approval is
     open closes the approval and leaves no result.

It leaves the badge provisioned with the test values, the clock set, checktest installed and the
launcher on the screen. The half of T-APR1 that needs the laptop's listener and devnet is in
t_sign_net.py.
"""

import os
import re
import time

from common import HOLD_MARGIN_MS, b58enc, hold_ms, provision_test, to_launcher
from fixtures import (BEGIN_TIMEOUT_S, CHECKTEST, REFUSAL_VECTORS, Keys, badge_pubkey, cancel, ct_ticks, ct_wait,
                      install_checktest, lines_of, log_ms, open_case, poll_result, set_time, short_address,
                      start_case, verify)

_HERE = os.path.dirname(os.path.abspath(__file__))
SHOTS = os.path.join(_HERE, "shots")

# apps/checktest/config.lua flush_color (255, 0, 255) as an RGB565 pixel, little-endian.
LOUD = b"\x1f\xf8"

SIGN_LINE = r"\[vk\] sign solana (\d+) bytes \d+ ms"
# The decoder error each refusal vector must be logged with ("[pay] undecodable: <name>",
# reasons.md). The wrong mint decodes; the check chain refuses it.
DECODER_ERROR = {"second instruction": "program", "wrong mint": None, "two signers": "header",
                 "v0 message": "version", "trailing bytes": "trailing"}


def loud_share(pixels):
    """The share of a screenshot's pixels that have the checktest app's flush colour."""
    count = sum(1 for i in range(0, len(pixels), 2) if pixels[i:i + 2] == LOUD)
    return count / (len(pixels) // 2)


def check_unverified(state, big, dev):
    """The approval of a readable transfer with no record."""
    assert state["title"] == "Pay", "title %r" % state["title"]
    assert state["severity"] == "red", "severity %s" % state["severity"]
    assert state["headline"] == "UNVERIFIED RECIPIENT", "headline %r" % state["headline"]
    assert state["red_reason"] == "unverified", "red_reason %s" % state["red_reason"]
    assert state["big"] == big, "big %r, expected %r" % (state["big"], big)
    assert state["sub"] == "to unverified recipient", "sub %r" % state["sub"]
    assert state["dev_override"] is dev, "dev_override %r on a %s build" % (state["dev_override"], "dev" if dev else "release")
    assert state["select"] == ("hold" if dev else "disabled"), "select %s" % state["select"]


def sign_with_hold(badge, hold, own, msg, what):
    """Taps SELECT (which must not sign), then holds it, and checks the signature the app polled."""
    badge.btn("a", "tap")
    state = badge.state()
    assert state["modal"] and state["phase"] == "ARMED", "%s: a tap of SELECT gave phase %s" % (what, state["phase"])
    assert not log_ms(badge, SIGN_LINE), "%s: the badge signed before SELECT was held" % what

    badge.btn("a", "hold", hold)
    kind, sig = poll_result(badge)
    assert kind == "sig", "%s: after the hold poll() gave %s" % (what, sig)
    assert verify(own, msg, sig), "%s: the signature does not verify over the message with the badge's key" % what
    signed = log_ms(badge, SIGN_LINE)
    assert signed == [len(msg)], "%s: '[vk] sign solana' lines for %r bytes, expected one of %d" % (what, signed, len(msg))
    return sig


def apr1(badge, keys, own, hold, dev):
    """Step 1, T-APR1 offline half."""
    assert dev, "T-APR1 signs through the dev override; this build's profile is not dev"
    msg = keys.transfer(own)
    state = open_case(badge, {"msg_hex": msg})
    check_unverified(state, "10.00 HACK", dev)
    assert lines_of(state).get("Account") == short_address(keys.merchant_ata), "lines %r" % state["lines"]
    os.makedirs(SHOTS, exist_ok=True)
    badge.shot(os.path.join(SHOTS, "pay_unverified_dev.png"))

    sign_with_hold(badge, hold, own, msg, "T-APR1")
    print("M2 %s" % badge.wait_log(SIGN_LINE, timeout=3).strip())
    called = log_ms(badge, r"\[app\] CT call (\d+)")
    drawn = log_ms(badge, r"\[vk\] approval first draw at (\d+) ms")
    if called and drawn:
        print("M3 begin_solana to first draw, no record: %d ms" % (drawn[0] - called[0]))
    badge.wait_state(lambda s: not s["modal"] and s["poll"] == "idle", timeout=5)


def built_on_badge(badge, keys, own, hold, dev):
    """Step 2: wallet.build_transfer makes the bytes fixtures.build_transfer_msg makes."""
    # A Memo makes the message 259 bytes: more than an SE050-held key signs (T-SE2).
    memo = "coffee #42" if badge.info().get("key") == "software" else None
    transfer = {
        "destination": b58enc(keys.merchant_ata),
        "source": b58enc(keys.source_ata),
        "blockhash": b58enc(keys.blockhash),
        "amount": "10.00",
        "symbol": keys.symbol,
        "memo": memo,
    }
    msg = keys.transfer(own, memo=memo)
    state = open_case(badge, {"transfer": transfer})
    check_unverified(state, "10.00 HACK", dev)
    if memo:
        assert lines_of(state).get("Memo") == memo, "lines %r" % state["lines"]
    sign_with_hold(badge, hold, own, msg, "build_transfer")
    badge.wait_state(lambda s: not s["modal"], timeout=5)


def apr4(badge, keys, own):
    """Step 3, T-APR4."""
    start_case(badge, {"msg_hex": keys.transfer(own), "flush_draw": True, "delay_ms": 5000})
    ct_wait(badge, r"tick 1\b", timeout=5)
    painted = loud_share(badge.shot())
    assert painted > 0.5, "the app's own screen is only %.0f %% its flush colour" % (painted * 100)

    result = ct_wait(badge, r"begin (?:ok|err \S+)", timeout=BEGIN_TIMEOUT_S)
    assert result == "begin ok", "T-APR4: begin_solana was refused: %s" % result
    badge.wait_state(lambda s: s["modal"] and s["phase"] == "ARMED", timeout=5)
    before = ct_ticks(badge)

    first = badge.shot(os.path.join(SHOTS, "apr4_approval.png"))
    time.sleep(0.5)
    second = badge.shot()
    assert first == second, "T-APR4: the screen changed while the approval was open"
    assert loud_share(first) == 0, "T-APR4: the app's colour is on the screen during the approval"
    time.sleep(1.5)
    state = badge.state()
    assert state["modal"] and state["phase"] == "ARMED", "T-APR4: the approval closed by itself (%s)" % state["phase"]
    after = ct_ticks(badge)
    assert after == before, "T-APR4: on_update ran during the approval (ticks %r, then %r)" % (before, after)

    answer = cancel(badge)
    assert answer == ("err", "unverified"), "T-APR4: poll() gave %r after CANCEL" % (answer,)
    ct_wait(badge, r"tick %d\b" % (before[-1] + 1), timeout=5)  # the app runs again


def apr5(badge, keys, own, hold):
    """Step 4, T-APR5."""
    valid = keys.transfer(own)
    for name, mutate, headline in REFUSAL_VECTORS:
        state = open_case(badge, {"msg_hex": mutate(valid)})
        assert state["severity"] == "red", "T-APR5 %s: severity %s" % (name, state["severity"])
        assert state["headline"] == headline, "T-APR5 %s: headline %r" % (name, state["headline"])
        assert state["select"] == "disabled", "T-APR5 %s: select %s" % (name, state["select"])
        assert state["dev_override"] is False, "T-APR5 %s: the dev override is offered" % name
        assert state["red_reason"] == "undecodable", "T-APR5 %s: red_reason %s" % (name, state["red_reason"])
        assert state["big"] == "" and state["sub"] == "", "T-APR5 %s: an amount is shown: %r %r" % (
            name, state["big"], state["sub"])
        expected = DECODER_ERROR[name]
        if expected:
            logged = [line for line in badge.log() if "[pay] undecodable:" in line]
            assert any(re.search(r"\[pay\] undecodable: %s\b" % expected, line) for line in logged), (
                "T-APR5 %s: no '[pay] undecodable: %s' in the log (%r)" % (name, expected, logged))

        badge.btn("a", "hold", hold)
        state = badge.state()
        assert state["modal"] and state["phase"] == "ARMED", "T-APR5 %s: a hold of SELECT gave phase %s" % (name, state["phase"])
        answer = cancel(badge)
        assert answer == ("err", "undecodable"), "T-APR5 %s: poll() gave %r" % (name, answer)
        assert not log_ms(badge, SIGN_LINE), "T-APR5 %s: the badge signed" % name


def stop_before_poll(badge, keys, own, hold):
    """Step 5a. A pushed STOP waits for the approval to close (hook H4) and is applied on that same
    pass, so the app is stopped holding a signature it never polled."""
    msg = keys.transfer(own)
    open_case(badge, {"msg_hex": msg})
    badge.stop()
    state = badge.state()
    assert state["modal"] and state["app"] == CHECKTEST, "STOP was applied while the approval was open: %s" % state

    badge.btn("a", "hold", hold)
    badge.wait_state(lambda s: not s["modal"] and s["app"] != CHECKTEST, timeout=10)
    assert log_ms(badge, SIGN_LINE) == [len(msg)], "the approval was not signed before the app stopped"
    polled = [line for line in badge.log() if re.search(r"\[app\] CT poll", line)]
    assert not polled, "the app polled before the STOP was applied, so this step proves nothing: %r" % polled
    state = badge.state()
    assert state["poll"] == "idle", "the result outlived its app: VKSTATE poll is %s" % state["poll"]

    # The next begin, from a fresh launch, is not refused as busy (open_case asserts "CT begin ok").
    open_case(badge, {"msg_hex": msg})
    answer = cancel(badge)
    assert answer == ("err", "unverified"), "after the relaunch poll() gave %r" % (answer,)


def delete_while_modal(badge, keys, own):
    """Step 5b. DEL of the running app is the one stop that happens during an approval: upstream
    stops the app at once, and the approval must close with it."""
    open_case(badge, {"msg_hex": keys.transfer(own)})
    reply = badge.cmd("AUTH " + badge.ok("VKPAIR"))[-1]
    assert reply.startswith("OK"), "AUTH -> %s" % reply
    reply = badge.cmd("DEL " + CHECKTEST, timeout=10)[-1]
    assert reply == "OK deleted", "DEL %s -> %s" % (CHECKTEST, reply)

    state = badge.wait_state(lambda s: not s["modal"], timeout=3)
    assert state["phase"] == "IDLE" and state["app"] != CHECKTEST, "after DEL: %s" % state
    assert state["poll"] == "idle", "a result was left behind: VKSTATE poll is %s" % state["poll"]
    badge.wait_log(r"\[vk\] approval closed \(app stopped\)", timeout=3)
    assert not log_ms(badge, SIGN_LINE), "the badge signed although the app was deleted"
    install_checktest(badge)


def run(badge):
    provision_test(badge)
    set_time(badge)
    keys = Keys()
    own = badge_pubkey(badge)
    hold = hold_ms(badge) + HOLD_MARGIN_MS
    dev = badge.info().get("profile") == "dev"

    to_launcher(badge)
    install_checktest(badge)
    try:
        apr1(badge, keys, own, hold, dev)
        built_on_badge(badge, keys, own, hold, dev)
        apr4(badge, keys, own)
        apr5(badge, keys, own, hold)
        stop_before_poll(badge, keys, own, hold)
        delete_while_modal(badge, keys, own)
    finally:
        to_launcher(badge)
