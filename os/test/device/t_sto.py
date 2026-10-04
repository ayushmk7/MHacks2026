"""T-STO1 and T-STO2: the history survives a reboot, and an app cannot open the history file
(testing.md, Acceptance tests; wallet/stores.md; implementation-plan.md, WP24).

Needs one badge with a dev build. No network, no hands. Uses the checktest app (apps/checktest)
with cases built by test/device/fixtures.py; the badge is provisioned with the test values.

  T-STO1  1. An approval is refused, so the newest history entry is not a signature. The app's
             wallet.history(1) shows it: the history is being written.
          2. A 10.00 HACK transfer is signed (the dev override hold, as in T-APR1).
          3. The badge is reset.
          4. wallet.history(1) gives "signed 10.00 HACK": the entry was written after step 1 and
             is still there after the reboot.
  T-STO2  badge.storage.read("../../vk/history.bin") and ("/vk/history.bin") both fail.

It leaves the badge provisioned with the test values, the clock set, and the launcher on the
screen.
"""

from common import HOLD_MARGIN_MS, hold_ms, provision_test, to_launcher
from fixtures import (Keys, badge_pubkey, cancel, ct_wait, install_checktest, open_case, poll_result, set_time,
                      start_case, stop_app, verify)


def newest_history(badge):
    """Runs a history case and returns what the app logged: "<outcome> <amount> <symbol>" or "none"."""
    start_case(badge, {"history": True})
    return ct_wait(badge, r"hist \S.*", timeout=10)[len("hist "):]


def sto1(badge, keys, own, hold):
    msg = keys.transfer(own)

    # 1. A refusal first: whatever earlier tests signed, the newest entry is now not "signed".
    open_case(badge, {"msg_hex": msg})
    answer = cancel(badge)
    assert answer == ("err", "unverified"), "T-STO1: poll() gave %r after CANCEL" % (answer,)
    entry = newest_history(badge)
    assert entry in ("blocked 10.00 HACK", "cancelled 10.00 HACK"), (
        "T-STO1: after a refused 10.00 HACK payment the newest history entry is %r" % entry)

    # 2. Sign once.
    state = open_case(badge, {"msg_hex": msg})
    assert state["dev_override"] is True and state["select"] == "hold", (
        "T-STO1 signs through the dev override, which this approval does not offer: %s" % state)
    badge.btn("a", "hold", hold)
    kind, sig = poll_result(badge)
    assert kind == "sig" and verify(own, msg, sig), "T-STO1: the payment was not signed: %s %r" % (kind, sig)
    entry = newest_history(badge)
    assert entry == "signed 10.00 HACK", "T-STO1: right after signing the newest history entry is %r" % entry

    # 3. Reboot.
    stop_app(badge)
    to_launcher(badge)
    badge.reset()

    # 4. The entry is still the newest one.
    entry = newest_history(badge)
    assert entry == "signed 10.00 HACK", "T-STO1: after the reboot the newest history entry is %r" % entry


def sto2(badge):
    start_case(badge, {"storage_probe": True})
    result = ct_wait(badge, r"storage \S.*", timeout=10)
    assert result == "storage fail fail", (
        "T-STO2: badge.storage reached the history file: %r (for ../../vk/history.bin and /vk/history.bin)" % result)


def run(badge):
    provision_test(badge)
    set_time(badge)
    keys = Keys()
    own = badge_pubkey(badge)
    hold = hold_ms(badge) + HOLD_MARGIN_MS

    to_launcher(badge)
    install_checktest(badge)
    try:
        sto1(badge, keys, own, hold)
        sto2(badge)
    finally:
        try:
            set_time(badge)   # the reset in T-STO1 cleared the clock
        finally:
            to_launcher(badge)
