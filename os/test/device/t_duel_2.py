"""The Duel app on two badges (WP45): invite, accept, play, settle, and "unpaid" when the loser
cancels.

`badge` is the inviter (A) and is made the winner; `badge2` is the guest (B) and the loser. Both
must be on the same ESP-NOW channel: joined to the same hotspot, or neither joined to any network.
The test reads both channels ("DUEL up ch <n>") and says so if they differ.

In order, twice (one duel to settle, one for the loser to cancel):
  1. A: SELECT on the title invites ("DUEL invite"). B hears it ("DUEL invited <stake> <mac>");
     SELECT accepts ("DUEL accept"); A logs "DUEL accepted".
  2. Rounds: both log "DUEL flash <round>". The test presses SELECT on A at once and on B half a
     second later, so A wins every round: both log "DUEL round <n> <mine> <theirs> <verdict>",
     A "won" and B "lost", until A logs "DUEL over won ..." and B "DUEL over lost ...".
  3. Settle: A opens a request ("DUEL request <req_id>"); B finds it by A's key ("DUEL paying
     <the same req_id>") and runs vk.pay.
       First duel: if the firmware approval opens on B it is approved with an injected hold; A
       must log "DUEL paid" (after its own look-up on chain) and B "DUEL pay done". Without a
       network the payment cannot be built: B logs "DUEL pay failed <reason>" and A "DUEL unpaid".
       Second duel, the loser cancels: CANCEL on B's approval if it opens (B "DUEL pay failed
       cancelled", A "DUEL unpaid rejected"), else CANCEL in the app (A "DUEL unpaid left", or
       "unpaid failed" if the payment had already failed by itself). A never logs "DUEL paid".

The paid path needs more than two badges: a network both badges reach, the RPC node and the
registry (listener_url) with a record for A's key, and tokens on B's account. With the test
provisioning (rpc_url 127.0.0.1) only the unpaid paths run, and the test prints a note saying so.

Needs two badges with dev builds, both running this firmware. No hands. The test provisions both
badges with the test values, sets both clocks, and leaves duel installed on both.
"""

import os
import re
import time

from common import HOLD_MARGIN_MS, hold_ms, launch, provision_test, to_launcher

NEEDS = "two-badges"

_HERE = os.path.dirname(os.path.abspath(__file__))
DUEL_DIR = os.path.normpath(os.path.join(_HERE, "..", "..", "apps", "duel"))
VK_LUA = os.path.normpath(os.path.join(_HERE, "..", "..", "lib", "vk.lua"))
DUEL = "duel"

_D = r"\[app\] DUEL "
_LUA_ERROR = r"\[lua\] .*(?:error|stopping)"

# How long after A's press B is pressed: B's reaction time is at least this, A's a frame or two.
LOSER_DELAY_S = 0.5


def config_value(name, default):
    """An integer tunable of apps/duel/config.lua."""
    with open(os.path.join(DUEL_DIR, "config.lua"), "r", encoding="utf-8") as handle:
        match = re.search(r"^\s*%s\s*=\s*(\d+)" % re.escape(name), handle.read(), re.MULTILINE)
    return int(match.group(1)) if match else default


def d_lines(badge, pattern):
    """The text after 'DUEL ' of every log line since the last clear_log() that matches."""
    found = (re.search(_D + "(" + pattern + ")", line) for line in badge.log())
    return [match.group(1) for match in found if match]


def d_wait(badge, pattern, timeout=10):
    """The text after 'DUEL ' of the first log line matching `pattern`."""
    line = badge.wait_log(_D + pattern, timeout=timeout)
    return re.search(_D + "(" + pattern + ")", line).group(1)


def no_lua_error(*badges):
    for one in badges:
        failed = [line for line in one.log() if re.search(_LUA_ERROR, line)]
        assert not failed, "duel stopped with a Lua error: %s" % failed[0]


def start_duel(badge):
    """Stops whatever runs, starts duel with a clean log and returns its ESP-NOW channel."""
    to_launcher(badge)
    badge.clear_log()
    launch(badge, DUEL)
    channel = int(d_wait(badge, r"up ch \d+").split()[-1])
    d_wait(badge, r"title")
    return channel


def play(a, b):
    """Invite from A, accept on B, play until the duel is decided with A winning every round.
    Returns the req_id of the request A opened and B found."""
    channel_a, channel_b = start_duel(a), start_duel(b)
    assert channel_a == channel_b, (
        "the badges are on different ESP-NOW channels (%d and %d): join both to the same hotspot, "
        "or neither to any network" % (channel_a, channel_b))

    a.btn("a")
    time.sleep(0.5)
    refused = d_lines(a, r"not ready \S+")
    assert not refused, "A refused to invite: %s" % refused[0]
    d_wait(a, r"invite [0-9a-f]{16}", timeout=5)
    d_wait(b, r"invited \S+ \S+", timeout=config_value("invite_timeout_s", 15))
    b.btn("a")
    d_wait(b, r"accept [0-9a-f]{16}", timeout=5)
    d_wait(a, r"accepted \S+", timeout=5)

    flash_s = config_value("delay_max_ms", 5000) / 1000.0 + 5
    pause_s = config_value("round_pause_ms", 2500) / 1000.0 + 5
    over = None
    for number in range(1, config_value("max_rounds", 9) + 1):
        d_wait(a, r"flash %d\b" % number, timeout=flash_s)
        a.btn("a")
        time.sleep(LOSER_DELAY_S)
        b.btn("a")
        verdict_a = d_wait(a, r"round %d \d+ \d+ \w+" % number, timeout=10).split()
        verdict_b = d_wait(b, r"round %d \d+ \d+ \w+" % number, timeout=10).split()
        assert verdict_a[-1] == "won" and verdict_b[-1] == "lost", (
            "round %d: A logged %s and B %s; A was pressed first" % (number, verdict_a, verdict_b))
        assert verdict_a[2:4] == verdict_b[3:1:-1], (
            "round %d: the badges disagree about the times: A %s, B %s" % (number, verdict_a, verdict_b))
        # A either starts the next round or the duel is over.
        step = d_wait(a, r"(?:go %d \d+|over \w+ \S+)" % (number + 1), timeout=pause_s)
        if step.startswith("over"):
            over = step
            break
    assert over and over.split()[1] == "won", "the duel did not end with A winning: %r" % over
    lost = d_wait(b, r"over \w+ \S+", timeout=pause_s)
    assert lost.split()[1] == "lost", "B logged %r, expected 'over lost'" % lost
    need = config_value("rounds", 3) // 2 + 1
    assert over.split()[2] == "%d-0" % need, "A's score is %s, expected %d-0" % (over.split()[2], need)

    opened = d_wait(a, r"request (?:err \S+|[0-9a-f]{16})", timeout=10)
    assert " err " not in opened, "A could not open its request: %s" % opened
    req_id = opened.split()[1]
    paying = d_wait(b, r"paying [0-9a-f]{16}", timeout=15)
    assert paying.split()[1] == req_id, "B is paying request %s, A opened %s" % (paying.split()[1], req_id)
    no_lua_error(a, b)
    return req_id


def approval_or_end(b, timeout=20):
    """Waits until B's firmware approval is open (returns True) or its payment has ended without
    one (returns False)."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if b.state()["modal"]:
            return True
        if d_lines(b, r"pay (?:done|failed) .*"):
            return False
        time.sleep(0.2)
    return False


def run(badge, badge2):
    a, b = badge, badge2
    with open(VK_LUA, "rb") as handle:
        vk_lua = handle.read()
    for one in (a, b):
        to_launcher(one)
        provision_test(one)
        one.ok("VKTIME %d" % int(time.time()))
        one.push(DUEL_DIR, DUEL, extra={"vk.lua": vk_lua})
    settle_s = config_value("settle_timeout_s", 90)

    # ---- First duel: settle ----------------------------------------------------------------------
    play(a, b)
    if approval_or_end(b):
        b.wait_state(lambda s: s["phase"] == "ARMED" or not s["modal"], timeout=5)
        b.btn("a", "hold", hold_ms(b) + HOLD_MARGIN_MS)
        b.wait_state(lambda s: not s["modal"], timeout=5)
        ended = d_wait(b, r"pay (?:done|failed) .*", timeout=60)
        assert ended.startswith("pay done"), "B approved but its payment ended as: %s" % ended
        d_wait(a, r"paid \S+", timeout=settle_s + 10)
    else:
        ended = d_wait(b, r"pay (?:done|failed) .*", timeout=60)
        assert ended.startswith("pay failed"), "B's payment ended as %r without an approval" % ended
        print("note: no approval opened on B (%s): the paid path needs a network, the registry and "
              "tokens, so only 'unpaid' is checked" % ended)
        d_wait(a, r"unpaid \S+", timeout=settle_s + 10)
        assert not d_lines(a, r"paid \S+"), "A shows PAID although B's payment failed"
    no_lua_error(a, b)

    # ---- Second duel: the loser cancels ----------------------------------------------------------
    play(a, b)
    if approval_or_end(b):
        b.btn("b")                                   # CANCEL on the firmware approval
        b.wait_state(lambda s: not s["modal"], timeout=5)
        d_wait(b, r"pay failed \S+", timeout=10)
        why = d_wait(a, r"unpaid \S+", timeout=10)
        assert why == "unpaid rejected", "A logged %r after B cancelled the approval" % why
    else:
        if not d_lines(b, r"pay failed .*"):
            b.btn("b")                               # CANCEL in the app: B leaves the duel
            d_wait(b, r"left", timeout=5)
        d_wait(a, r"unpaid \S+", timeout=settle_s + 10)
    assert not d_lines(a, r"paid \S+"), "A shows PAID although B cancelled"
    no_lua_error(a, b)

    # ---- Both leave ------------------------------------------------------------------------------
    for one in (a, b):
        to_launcher(one)
        one.wait_state(lambda s: s["app"] == "" and s["screen"] == "launcher", timeout=5)
