"""T-BOOT2, T-CFG3, T-CFG1, T-CFG2: the config store and provisioning (testing.md, Acceptance tests;
platform/config.md).

Needs one badge with a dev build that has the approval engine (WP12): the secure-change and reset
confirmations are approved with injected holds. No network, no hands.

It leaves the badge provisioned with the test values of common.test_config() (approval_tmo_s 10,
hold_ms 1000), as common.provision_test() does.

T-CFG4 (send VKINFO over BLE or the HTTP push API: it must not be recognised, because the VK
commands exist on the USB console only) needs hands: a phone or a laptop on the badge's network.
It is not scripted here.
"""

import os

from common import HOLD_MARGIN_MS, hold_ms, provision_test, to_launcher

_HERE = os.path.dirname(os.path.abspath(__file__))
SHOTS = os.path.join(_HERE, "shots")

SHOT_BYTES = 320 * 240 * 2
# The launcher's balance row (ui/shell.md, "Launcher": receipt::row at y = 166, which owns the
# rows y - 5 .. y + 12): SETUP NEEDED while unprovisioned, BALANCE afterwards. As a byte range of
# the 320-pixel-wide RGB565 screenshot.
ROW_BAND = slice(320 * 2 * 158, 320 * 2 * 182)


def _keys(badge):
    """VKKEYS: the names of the registered config keys. Checks the reply's shape on the way."""
    reply = badge.cmd("VKKEYS")
    final, lines = reply[-1], reply[:-1]
    assert final == "OK %d" % len(lines), "VKKEYS ended with %r after %d lines" % (final, len(lines))
    names = []
    for line in lines:
        parts = line.split()
        # "+ <name> <type> <flags> <help>"
        assert len(parts) >= 4 and parts[0] == "+", "VKKEYS line: %r" % line
        assert parts[2] in ("STR", "U32", "KEY32", "TOKENS"), "VKKEYS type in: %r" % line
        assert len(parts[1]) <= 15, "config key name longer than an NVS key: %r" % line
        names.append(parts[1])
    for core_key in ("rpc_url", "listener_url", "display_name"):
        assert core_key in names, "VKKEYS does not list %s" % core_key
    return names


def _approve(badge):
    """Holds SELECT on the confirmation that is on the screen and waits for it to close."""
    badge.wait_state(lambda s: s["modal"] and s["phase"] == "ARMED", timeout=5)
    badge.btn("a", "hold", hold_ms(badge) + HOLD_MARGIN_MS)
    badge.wait_state(lambda s: not s["modal"], timeout=5)


def _ensure_unprovisioned(badge):
    """A provisioned badge is reset: VKRESET, then an injected hold on its confirmation."""
    if badge.info().get("provisioned") != "1":
        return
    reply = badge.cmd("VKRESET")[-1]
    assert reply == "OK pending", "VKRESET -> %s" % reply
    state = badge.wait_state(lambda s: s["modal"] and s["phase"] == "ARMED", timeout=5)
    assert state["title"] == "Change setting", "VKRESET confirmation title: %r" % state["title"]
    assert state["headline"] == "ERASE WALLET CONFIG", "VKRESET confirmation headline: %r" % state["headline"]
    assert state["severity"] == "amber" and state["select"] == "hold", (
        "VKRESET confirmation is %s / %s, expected amber / hold" % (state["severity"], state["select"]))
    _approve(badge)
    assert badge.info().get("provisioned") == "0", "VKRESET was approved but provisioned is not 0"


def _invalid_value_is_refused(badge, names):
    """T-CFG3: an invalid value answers ERR invalid and changes nothing. `tokens` is registered by
    solana_pay (WP13); until it exists the check runs against rpc_url, whose minimum length is 8."""
    key, bad = ("tokens", "garbage") if "tokens" in names else ("rpc_url", "x")
    before = badge.ok("VKGET " + key)
    reply = badge.cmd("VKSET %s %s" % (key, bad))[-1]
    assert reply == "ERR invalid", "VKSET %s %s -> %s" % (key, bad, reply)
    after = badge.ok("VKGET " + key)
    assert after == before, "%s changed from %r to %r after a refused VKSET" % (key, before, after)


def run(badge):
    os.makedirs(SHOTS, exist_ok=True)
    to_launcher(badge)  # also cancels an approval left open by an earlier test
    _ensure_unprovisioned(badge)
    names = _keys(badge)

    # ---- T-BOOT2: unprovisioned badge in the launcher --------------------------------------------
    state = to_launcher(badge)
    assert state["app"] == "" and state["screen"] == "launcher", "not on the launcher: %s" % state
    assert state["provisioned"] is False, "VKSTATE provisioned is %r on an unprovisioned badge" % state["provisioned"]
    assert badge.info().get("provisioned") == "0", "VKINFO provisioned is not 0"
    # The picture is for a person: the launcher's balance row reads SETUP NEEDED.
    unprovisioned_shot = badge.shot(os.path.join(SHOTS, "cfg_boot2_setup.png"))
    assert len(unprovisioned_shot) == SHOT_BYTES, "screenshot is %d bytes" % len(unprovisioned_shot)

    # ---- T-CFG3 (unprovisioned): invalid value refused, value unchanged ---------------------------
    _invalid_value_is_refused(badge, names)
    reply = badge.cmd("VKSET no_such_key 1")[-1]
    assert reply == "ERR unknown_key", "VKSET no_such_key 1 -> %s" % reply
    reply = badge.cmd("VKGET no_such_key")[-1]
    assert reply == "ERR unknown_key", "VKGET no_such_key -> %s" % reply

    # VKCOMMIT while a required key is missing names it. rpc_url is required; it is empty on a badge
    # that was just reset or never provisioned, but an earlier run that stopped half-way may have
    # left one behind, and then this check is skipped.
    if badge.ok("VKGET rpc_url") == "":
        reply = badge.cmd("VKCOMMIT")[-1]
        assert reply.startswith("ERR missing "), "VKCOMMIT with rpc_url unset -> %s" % reply
        assert reply.split()[-1] in names, "VKCOMMIT names a key VKKEYS does not list: %s" % reply
        assert badge.info().get("provisioned") == "0", "a refused VKCOMMIT provisioned the badge"

    # ---- T-CFG1: provision, reboot, every value survives ------------------------------------------
    values = provision_test(badge)  # VKSET of every test value whose key is listed, then VKCOMMIT
    set_keys = [key for key in values if key in names]
    assert "rpc_url" in set_keys, "the test provisioning did not set rpc_url"
    assert badge.state()["provisioned"] is True, "VKSTATE provisioned is not true after VKCOMMIT"

    # SETUP NEEDED gives way to the balance row when the badge becomes provisioned (the config store
    # asks the shell to repaint). Compared with the unprovisioned picture, that row must differ.
    state = to_launcher(badge)
    assert state["screen"] == "launcher" and state["provisioned"] is True, "after VKCOMMIT: %s" % state
    provisioned_shot = badge.shot(os.path.join(SHOTS, "cfg_boot2_provisioned.png"))
    assert provisioned_shot[ROW_BAND] != unprovisioned_shot[ROW_BAND], (
        "the launcher's balance row looks the same unprovisioned and provisioned: "
        "SETUP NEEDED was not drawn, or the shell did not repaint after VKCOMMIT")

    badge.reset()
    for key in set_keys:
        got = badge.ok("VKGET " + key)
        assert got == values[key], "after a reboot %s is %r, expected %r" % (key, got, values[key])
    assert badge.info().get("provisioned") == "1", "after a reboot VKINFO provisioned is not 1"
    assert badge.state()["provisioned"] is True, "after a reboot VKSTATE provisioned is not true"

    # ---- T-CFG3 again, now against a stored value --------------------------------------------------
    _invalid_value_is_refused(badge, names)

    # A non-secure key is written at once on a provisioned badge, and an empty value is a value.
    reply = badge.cmd("VKSET display_name T-CFG badge")[-1]
    assert reply == "OK", "VKSET display_name on a provisioned badge -> %s" % reply
    assert badge.ok("VKGET display_name") == "T-CFG badge", "display_name did not keep a value with a space"
    reply = badge.cmd("VKSET display_name")[-1]
    assert reply == "OK", "VKSET display_name (empty) -> %s" % reply
    assert badge.ok("VKGET display_name") == "", "display_name was not cleared"

    # ---- T-CFG2: a secure key on a provisioned badge needs a hold on the badge's own screen --------
    assert "approval_tmo_s" in names, "approval_tmo_s is not registered (the approval engine, WP12, is missing)"
    to_launcher(badge)
    start = badge.ok("VKGET approval_tmo_s")  # 10 under the test provisioning
    assert start == values["approval_tmo_s"], "approval_tmo_s is %r before T-CFG2" % start

    reply = badge.cmd("VKSET approval_tmo_s 12")[-1]
    assert reply == "OK pending", "VKSET approval_tmo_s 12 on a provisioned badge -> %s" % reply
    state = badge.wait_state(lambda s: s["modal"] and s["phase"] == "ARMED", timeout=5)
    assert state["title"] == "Change setting", "title: %r" % state["title"]
    assert state["severity"] == "amber", "severity: %r" % state["severity"]
    assert state["select"] == "hold", "select: %r" % state["select"]
    assert state["headline"] == "SECURITY SETTING", "headline: %r" % state["headline"]
    assert state["big"] == "approval_tmo_s", "big: %r" % state["big"]
    assert state["lines"] == [["Old", start], ["New", "12"]], "lines: %r" % state["lines"]
    assert badge.ok("VKGET approval_tmo_s") == start, "the value changed before the confirmation was approved"
    badge.shot(os.path.join(SHOTS, "cfg_change_setting.png"))
    badge.btn("a", "hold", hold_ms(badge) + HOLD_MARGIN_MS)  # 1300 ms under the test provisioning
    badge.wait_state(lambda s: not s["modal"], timeout=5)
    assert badge.ok("VKGET approval_tmo_s") == "12", "an approved change was not written"

    reply = badge.cmd("VKSET approval_tmo_s 14")[-1]
    assert reply == "OK pending", "VKSET approval_tmo_s 14 -> %s" % reply
    state = badge.wait_state(lambda s: s["modal"] and s["phase"] == "ARMED", timeout=5)
    assert state["lines"] == [["Old", "12"], ["New", "14"]], "lines: %r" % state["lines"]
    badge.btn("b", "tap")
    badge.wait_state(lambda s: not s["modal"], timeout=5)
    assert badge.ok("VKGET approval_tmo_s") == "12", "a cancelled change was written"

    # Put the test value back: later tests count on the 10 s approval timeout.
    reply = badge.cmd("VKSET approval_tmo_s %s" % start)[-1]
    assert reply == "OK pending", "VKSET approval_tmo_s %s -> %s" % (start, reply)
    _approve(badge)
    assert badge.ok("VKGET approval_tmo_s") == start, "approval_tmo_s was not restored to %s" % start
    to_launcher(badge)
