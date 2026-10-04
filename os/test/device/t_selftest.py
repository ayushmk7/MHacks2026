"""The TESTS app (src/native_apps/selftest, id `selftest`): every suite, on one badge.

In order:
  1. Launch `selftest` (a native app: nothing is pushed). It opens on the TESTS menu and runs the
     HARDWARE suite by itself. The log has "[selftest] start", one "[selftest] <name>=<OK|FAIL|-->
     <value>" line per hardware check, and "[selftest] done ok=.. fail=.. manual=..". The automatic
     checks take about three seconds (the microphone listens for two).
  2. No hardware check is FAIL. The checks that depend only on the badge (battery, mic, i2c,
     storage, nvs, key, crypto, memory) are OK. The ones that depend on where the badge is (clock,
     wifi, espnow) are OK or "--". The three manual checks (display, buttons, leds) are "--" until
     a person does them.
  3. The secure element is never addressed (upstream-hooks.md, H21): the log has
     "i2c 0x48 skipped (quarantined)" and no probe line for 0x48.
  4. The menu is drawn (shots/selftest_menu_<theme>.png); SELECT opens HARDWARE, whose checklist is
     drawn (shots/selftest_<theme>.png). Neither is blank.
  5. The summary line "[selftest] batt=..V mic=..dB btn=0x.. i2c=ok heap=.." appears within 5 s.
  6. The manual hardware tests can be entered and left. SELECT on DISPLAY (the cursor starts there)
     and seven more SELECTs step through the test screens and log "display=OK seen" (nobody looked:
     this checks the keys, not the panel). DOWN, SELECT enters the LED test; CANCEL stops it
     ("leds=-- stopped") and the app keeps running. The button test is not entered: it needs a
     finger on each key. CANCEL goes back to the menu.
  7. Every other suite, in menu order: SELECT runs it; its "suite <name> done ok=.. fail=..
     manual=.." line comes, one "<suite>.<name>=.." line per row, the counts add up, and no row is
     FAIL except the ones that depend on the network around the badge. Each checklist is drawn
     (shots/selftest_<suite>_<theme>.png).
       WALLET     key, address, limit, selfcheck, vectors are OK; provisioned, issuer, tokens OK or
                  "--". The signature test is entered: if its approval opens it is cancelled (this
                  test never approves a signature) and the row is "-- cancelled"; without the
                  signing domain it is "-- needs domain". Either way "--", never FAIL.
       CHECKS     every case of the check chain gives its expected verdict (all OK).
       APPROVAL SCREENS (dev builds only) green, amber and red: each demo approval opens with title
                  "Self test" and its severity (shots/selftest_approval_<look>_<theme>.png), is
                  closed with CANCEL, and SELECT ("looked right") makes the row OK.
       STORES     every row OK.
       RADIO      OK or "--", except broadcast, which must not FAIL when ESP-NOW is on; channel may
                  FAIL (a radio on another channel than the network is the surroundings).
       NETWORK    "--" when not joined; listener and rpc depend on the network and are not judged.
       APPS       no app is FAIL, and this app's own row is OK.
  8. RUN ALL runs every suite again and ends with "all done ok=.. fail=.. manual=..", whose counts
     are the sums of the suites' last done lines (shots/selftest_runall_<theme>.png).
  9. CANCEL back to the menu, CANCEL on the menu exits: VKSTATE app is "" and screen is "launcher".

Needs one badge with a dev build that contains the app. No hands, no network, no provisioning
(a provisioned badge works too: the signature test's approval, if any, is cancelled).
Written without a badge at hand: steps 4 and 6 to 9 have not been run yet.
"""

import os
import re
import time

from common import launch, on_launcher, to_launcher

_HERE = os.path.dirname(os.path.abspath(__file__))
SHOTS = os.path.join(_HERE, "shots")
APP = "selftest"

WIDTH, HEIGHT = 320, 240
ROW_BYTES = WIDTH * 2
SHOT_BYTES = ROW_BYTES * HEIGHT

MANUAL = ("display", "buttons", "leds")
# OK on any healthy badge, wherever it is.
ALWAYS_OK = ("battery", "mic", "i2c", "storage", "nvs", "key", "crypto", "memory")
# OK or "--", depending on the network and the settings.
ENVIRONMENT = ("clock", "wifi", "espnow")
CHECKS = MANUAL + ALWAYS_OK + ENVIRONMENT

# The suites after HARDWARE, in menu order, with what each row may be. "ok": OK only. "env": OK or
# "--". "any": not judged (depends on the network around the badge). "manual": "--" until done.
SUITES = (
    ("wallet", "WALLET", {"key": "ok", "address": "ok", "limit": "ok", "selfcheck": "ok", "vectors": "ok",
                          "provisioned": "env", "issuer": "env", "tokens": "env", "signature": "manual"}),
    ("checks", "CHECKS", {name: "ok" for name in (
        "green", "not_present", "unsynced", "cannot_read", "not_mine", "unknown_token", "over_limit",
        "no_record", "forged_record", "expired", "stale", "bad_proof")}),
    ("approvals", "APPROVAL SCREENS", {"green": "manual", "amber": "manual", "red": "manual"}),
    ("stores", "STORES", {"fs": "ok", "probe": "ok", "history": "ok", "contacts": "ok", "consent": "ok"}),
    ("radio", "RADIO", {"espnow": "env", "beacon": "env", "broadcast": "env", "peers": "env", "wifi": "env",
                        "channel": "any"}),
    ("network", "NETWORK", {"link": "env", "listener": "any", "rpc": "any", "time": "env"}),
    ("apps", "APPS", None),          # one row per installed app: none may FAIL
)
DEV_ONLY = ("approvals",)
WALLET_SIGNATURE_ROW = 8          # key, address, limit, selfcheck, vectors, provisioned, issuer, tokens, signature

_RESULT = re.compile(r"\[selftest\] ([a-z0-9]+)=(OK|FAIL|--)(?: (.*))?$")
_SUITE_RESULT = re.compile(r"\[selftest\] ([a-z]+)\.([a-z0-9._-]+)=(OK|FAIL|--)(?: (.*))?$")
_DONE = r"\[selftest\] done ok=(\d+) fail=(\d+) manual=(\d+)"
_SUITE_DONE = r"\[selftest\] suite %s done ok=(\d+) fail=(\d+) manual=(\d+)"
_ALL_DONE = r"\[selftest\] all done ok=(\d+) fail=(\d+) manual=(\d+)"
_BEAT = r"\[selftest\] batt=(\d+\.\d\d)V mic=(-?\d+dB|--) btn=0x([0-9A-F]{2}) i2c=(ok|LOW) heap=(\d+)"

DONE_TIMEOUT_S = 30        # the checks take about 3 s; a Wi-Fi scan on a joined badge up to 8 s more
BEAT_TIMEOUT_S = 8         # the summary line comes every 5 s
SUITE_TIMEOUT_S = 40       # RADIO listens 5 s; NETWORK makes two requests of up to 3 s each
RUN_ALL_TIMEOUT_S = 120
DISPLAY_SCREENS = 7        # colour bars, red, green, blue, white, black, checkerboard
KEY_GAP_S = 0.15


def theme_name(badge):
    """The active theme's name; an empty or missing config key means receipt-light."""
    reply = badge.cmd("VKGET theme")[-1]
    name = reply[3:].strip() if reply.startswith("OK ") else ""
    return name or "receipt-light"


def colours(pixels):
    return len({pixels[i:i + 2] for i in range(0, len(pixels), 2)})


def results(badge):
    """{name: (status, value)} of the hardware rows from the log, the last line of each winning."""
    found = {}
    for line in badge.log():
        match = _RESULT.search(line.rstrip())
        if match:
            found[match.group(1)] = (match.group(2), match.group(3) or "")
    return found


def suite_results(badge):
    """{suite: {name: (status, value)}} of the other suites' rows, the last line of each winning."""
    found = {}
    for line in badge.log():
        match = _SUITE_RESULT.search(line.rstrip())
        if match:
            found.setdefault(match.group(1), {})[match.group(2)] = (match.group(3), match.group(4) or "")
    return found


def suite_done(badge, suite):
    """(ok, fail, manual) of the last "suite <suite> done" line since clear_log, or None."""
    last = None
    for line in badge.log():
        match = re.search(_SUITE_DONE % re.escape(suite), line)
        if match:
            last = tuple(int(text) for text in match.groups())
    return last


def running(badge, what):
    state = badge.state()
    assert state["app"] == APP and not state["modal"], "%s: selftest is not running: %s" % (what, state)


def tap(badge, key, times=1):
    for _ in range(times):
        badge.btn(key, "tap")
        time.sleep(KEY_GAP_S)


def shoot(badge, name, theme, what, at_least=3):
    shot = badge.shot(os.path.join(SHOTS, "selftest_%s%s.png" % (name + "_" if name else "", theme)))
    assert len(shot) == SHOT_BYTES, "%s: the screenshot is %d bytes, expected %d" % (what, len(shot), SHOT_BYTES)
    assert colours(shot) >= at_least, "%s looks blank: %d colours" % (what, colours(shot))
    return shot


def judge(suite, rows, kinds):
    """Asserts each row against its kind; returns the rows that ended '--' or FAIL for the report."""
    failed = []
    for name, (status, value) in rows.items():
        kind = kinds.get(name, "ok") if kinds is not None else "ok"
        if kind == "ok":
            ok = status == "OK"
        elif kind == "env":
            ok = status in ("OK", "--")
        elif kind == "manual":
            ok = status in ("OK", "--")
        else:
            ok = True
        if not ok:
            failed.append("%s.%s=%s %s" % (suite, name, status, value))
    assert not failed, "rows that must not be like this: %s" % "; ".join(failed)
    if kinds is not None:
        missing = [name for name in kinds if name not in rows and kinds[name] != "manual"]   # a manual row has no line until it is done
        assert not missing, "%s: no result line for %s" % (suite, ", ".join(missing))


def wallet_signature(badge):
    """Enters the signature test and makes sure it ends '--' without anything being signed."""
    tap(badge, "down", WALLET_SIGNATURE_ROW)
    badge.clear_log()
    tap(badge, "a")
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        state = badge.state()
        if state["modal"]:
            assert state["title"] == "Self test", "the signature test opened another approval: %s" % state
            badge.wait_state(lambda s: s["phase"] == "ARMED" or not s["modal"], timeout=5)
            tap(badge, "b")                       # this test never approves a signature
            badge.wait_state(lambda s: not s["modal"], timeout=5)
            line = badge.wait_log(r"\[selftest\] wallet\.signature=", timeout=5)
            assert "wallet.signature=-- cancelled" in line, "a cancelled signature test: %s" % line
            return line
        if any("wallet.signature=" in line for line in badge.log()):
            break
        time.sleep(0.2)
    line = badge.wait_log(r"\[selftest\] wallet\.signature=", timeout=5)
    assert "wallet.signature=--" in line, "the signature test without an approval must be '--': %s" % line
    return line


def approval_looks(badge, theme):
    """Dev builds: each demo approval opens, is cancelled, and is confirmed as seen."""
    for look in ("green", "amber", "red"):    # the cursor moves to the next row after each OK
        badge.clear_log()
        tap(badge, "a")
        state = badge.wait_state(lambda s: s["modal"], timeout=5)
        assert state["title"] == "Self test" and state["severity"] == look, "demo %s: %s" % (look, state)
        badge.wait_state(lambda s: s["phase"] == "ARMED", timeout=5)
        shoot(badge, "approval_" + look, theme, "the %s demo approval" % look)
        tap(badge, "b")
        badge.wait_state(lambda s: not s["modal"], timeout=5)
        time.sleep(0.3)
        tap(badge, "a")                           # "looked right"
        line = badge.wait_log(r"\[selftest\] approvals\.%s=" % look, timeout=5)
        assert "approvals.%s=OK looked right" % look in line, line


def run(badge):
    os.makedirs(SHOTS, exist_ok=True)
    to_launcher(badge)
    theme = theme_name(badge)
    dev = badge.info().get("profile") == "dev"

    # 1. Launch; the hardware checks run by themselves.
    badge.clear_log()
    state = launch(badge, APP, timeout=20)
    assert state["app"] == APP and state.get("native") is True, "selftest is not running as a native app: %s" % state
    line = badge.wait_log(_DONE, timeout=DONE_TIMEOUT_S)
    ok, fail, manual = (int(text) for text in re.search(_DONE, line).groups())
    log = "\n".join(badge.log())
    assert "[selftest] start" in log, "no '[selftest] start' line"

    # 2. One line per check, none of them FAIL.
    found = results(badge)
    missing = [name for name in CHECKS if name not in found]
    assert not missing, "no result line for: %s" % ", ".join(missing)
    failed = ["%s (%s)" % (name, found[name][1]) for name in CHECKS if found[name][0] == "FAIL"]
    assert not failed, "self test FAIL: %s" % "; ".join(failed)
    not_ok = ["%s=%s %s" % (name, found[name][0], found[name][1]) for name in ALWAYS_OK if found[name][0] != "OK"]
    assert not not_ok, "checks that do not depend on the surroundings are not OK: %s" % "; ".join(not_ok)
    for name in ENVIRONMENT:
        assert found[name][0] in ("OK", "--"), "%s=%s %s" % (name, found[name][0], found[name][1])
    for name in MANUAL:
        assert found[name][0] == "--", "%s is %s before anyone did it" % (name, found[name][0])
    assert fail == 0, "the done line counts %d failures: %s" % (fail, line)
    assert ok + fail + manual == len(CHECKS), "the done line does not add up to %d checks: %s" % (len(CHECKS), line)
    untested = sum(1 for name in CHECKS if found[name][0] == "--")
    assert manual == untested, "the done line says manual=%d, the result lines have %d '--'" % (manual, untested)

    # 3. The quarantined address is skipped, never probed.
    assert "0x48 skipped" in log, "no '0x48 skipped' line: the I2C check must say it left the secure element alone"
    probed = [entry for entry in badge.log() if re.search(r"\[selftest\] i2c 0x48 (?:answers|no answer)", entry)]
    assert not probed, "the self test probed the quarantined address: %s" % probed[0]

    # 4. The menu, then the hardware checklist.
    time.sleep(0.5)
    running(badge, "after the automatic checks")
    menu = shoot(badge, "menu", theme, "the TESTS menu")
    tap(badge, "a")                                # HARDWARE is the first row
    time.sleep(0.3)
    shot = shoot(badge, "", theme, "the hardware checklist")
    assert shot != menu, "SELECT on HARDWARE did not open its checklist"

    # 5. The summary line.
    line = badge.wait_log(_BEAT, timeout=BEAT_TIMEOUT_S)
    beat = re.search(_BEAT, line)
    assert beat.group(4) == "ok", "the summary line says the I2C lines are low: %s" % line
    assert beat.group(3) == "00", "the summary line shows a key down with nobody pressing: %s" % line

    # 6. Manual tests: in with SELECT, through with SELECT, out with CANCEL.
    badge.clear_log()
    tap(badge, "a")                                # DISPLAY: the first test screen
    time.sleep(0.3)
    bars = badge.shot()
    assert bars != shot and colours(bars) >= 8, "SELECT on DISPLAY did not show the colour bars (%d colours)" % colours(bars)
    for _ in range(DISPLAY_SCREENS):               # six steps on, the seventh confirms
        badge.btn("a", "tap")
    badge.wait_log(r"\[selftest\] display=OK seen", timeout=5)
    line = badge.wait_log(_DONE, timeout=5)
    assert int(re.search(_DONE, line).group(1)) == ok + 1, "the done line after the display test: %s" % line
    running(badge, "after the display test")

    badge.btn("down", "tap")                       # the cursor moved to BUTTONS by itself; LEDS is next
    badge.btn("a", "tap")
    time.sleep(0.6)                                # the first LED is on
    badge.btn("b", "tap")
    badge.wait_log(r"\[selftest\] leds=-- stopped", timeout=5)
    running(badge, "after CANCEL in the LED test")
    tap(badge, "b")                                # back to the menu
    running(badge, "after CANCEL on the hardware checklist")

    # 7. Every other suite, in menu order. The menu's cursor is on HARDWARE (row 0).
    position = 0
    menu_row = 0
    done_counts = {}
    for name, title, kinds in SUITES:
        if name in DEV_ONLY and not dev:
            continue
        menu_row += 1
        tap(badge, "down", menu_row - position)
        position = menu_row
        badge.clear_log()
        tap(badge, "a")
        line = badge.wait_log(_SUITE_DONE % name, timeout=SUITE_TIMEOUT_S)
        time.sleep(0.3)
        running(badge, "after %s ran" % title)
        rows = suite_results(badge).get(name, {})
        counts = tuple(int(text) for text in re.search(_SUITE_DONE % name, line).groups())
        assert sum(counts) == len(rows), "%s: the done line counts %d rows, the log has %d: %s" % (
            title, sum(counts), len(rows), line)
        judge(name, rows, kinds)
        shoot(badge, name, theme, "the %s checklist" % title)

        if name == "wallet":
            wallet_signature(badge)
            running(badge, "after the signature test")
        elif name == "approvals":
            approval_looks(badge, theme)
            running(badge, "after the approval screens")
        elif name == "apps":
            assert rows.get(APP, ("", ""))[0] == "OK", "this app's own row: %s" % (rows.get(APP),)
        elif name == "radio":
            espnow_on = rows.get("espnow", ("--", ""))[0] == "OK"
            assert not espnow_on or rows.get("broadcast", ("", ""))[0] == "OK", "ESP-NOW is on but the broadcast: %s" % (
                rows.get("broadcast"),)
        done_counts[name] = suite_done(badge, name) or counts
        tap(badge, "b")                            # back to the menu
        running(badge, "after CANCEL on the %s checklist" % title)

    # 8. RUN ALL: the last row of the menu.
    tap(badge, "down", menu_row + 1 - position)
    position = menu_row + 1
    badge.clear_log()
    tap(badge, "a")
    line = badge.wait_log(_ALL_DONE, timeout=RUN_ALL_TIMEOUT_S)
    time.sleep(0.3)
    running(badge, "after RUN ALL")
    shoot(badge, "runall", theme, "the RUN ALL totals")
    total = [int(text) for text in re.search(_ALL_DONE, line).groups()]
    sums = [0, 0, 0]
    hardware = re.search(_DONE, badge.wait_log(_DONE, timeout=1))
    for i, text in enumerate(hardware.groups()):
        sums[i] += int(text)
    for name, _title, _kinds in SUITES:
        if name in DEV_ONLY and not dev:
            continue
        counts = suite_done(badge, name)
        if counts is None:                         # a suite with no automatic row logs no done line in RUN ALL
            counts = done_counts[name]
        for i in range(3):
            sums[i] += counts[i]
    assert total == sums, "RUN ALL counts %s, the suites' done lines add up to %s" % (total, sums)
    rows = suite_results(badge)
    for name, _title, kinds in SUITES:
        if name in rows:
            judge(name, rows[name], kinds)
    hw = results(badge)
    failed = ["%s (%s)" % (n, hw[n][1]) for n in CHECKS if n in hw and hw[n][0] == "FAIL"]
    assert not failed, "RUN ALL: hardware FAIL: %s" % "; ".join(failed)

    # 9. CANCEL back to the menu, CANCEL on the menu exits.
    tap(badge, "b")
    running(badge, "after CANCEL on RUN ALL")
    badge.clear_log()
    badge.btn("b", "tap")
    badge.wait_state(on_launcher, timeout=5)
    badge.wait_log(r"native app '%s' stopped" % APP, timeout=5)
    to_launcher(badge)
