"""The Self test app (src/native_apps/selftest), on one badge.

In order:
  1. Launch `selftest` (a native app: nothing is pushed). The log has "[selftest] start", one
     "[selftest] <name>=<OK|FAIL|--> <value>" line per check, and "[selftest] done ok=.. fail=..
     manual=..". The automatic checks take about three seconds (the microphone listens for two).
  2. No check is FAIL. The checks that depend only on the badge (battery, mic, i2c, storage, nvs,
     key, crypto, memory) are OK. The ones that depend on where the badge is (clock, wifi, espnow)
     are OK or "--": a clock with no source, Wi-Fi off or not joined, and ESP-NOW off are "--".
     The three manual checks (display, buttons, leds) are "--" until a person does them.
  3. The secure element is never addressed (upstream-hooks.md, H21): the log has
     "i2c 0x48 skipped (quarantined)" and no probe line for 0x48.
  4. The checklist is drawn: shots/selftest_<theme>.png, not blank.
  5. The summary line "[selftest] batt=..V mic=..dB btn=0x.. i2c=ok heap=.." appears within the
     5 s period.
  6. The manual tests can be entered and left. SELECT on DISPLAY (the cursor starts there) and
     seven more SELECTs step through the test screens and log "display=OK seen" (nobody looked:
     this checks the keys, not the panel). DOWN, SELECT enters the LED test; CANCEL stops it
     ("leds=-- stopped") and the app keeps running. The button test is not entered: it needs
     a finger on each key.
  7. CANCEL on the checklist exits: VKSTATE app is "" and screen is "launcher".

Needs one badge with a dev build that contains the app. No hands, no network, no provisioning.
Written without a badge at hand: it has not been run yet.
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

_RESULT = re.compile(r"\[selftest\] ([a-z0-9]+)=(OK|FAIL|--)(?: (.*))?$")
_DONE = r"\[selftest\] done ok=(\d+) fail=(\d+) manual=(\d+)"
_BEAT = r"\[selftest\] batt=(\d+\.\d\d)V mic=(-?\d+dB|--) btn=0x([0-9A-F]{2}) i2c=(ok|LOW) heap=(\d+)"

DONE_TIMEOUT_S = 30        # the checks take about 3 s; a Wi-Fi scan on a joined badge up to 8 s more
BEAT_TIMEOUT_S = 8         # the summary line comes every 5 s
DISPLAY_SCREENS = 7        # colour bars, red, green, blue, white, black, checkerboard


def theme_name(badge):
    """The active theme's name; an empty or missing config key means receipt-light."""
    reply = badge.cmd("VKGET theme")[-1]
    name = reply[3:].strip() if reply.startswith("OK ") else ""
    return name or "receipt-light"


def colours(pixels):
    return len({pixels[i:i + 2] for i in range(0, len(pixels), 2)})


def results(badge):
    """{name: (status, value)} from the log, the last line of each check winning."""
    found = {}
    for line in badge.log():
        match = _RESULT.search(line.rstrip())
        if match:
            found[match.group(1)] = (match.group(2), match.group(3) or "")
    return found


def running(badge, what):
    state = badge.state()
    assert state["app"] == APP and not state["modal"], "%s: selftest is not running: %s" % (what, state)


def run(badge):
    os.makedirs(SHOTS, exist_ok=True)
    to_launcher(badge)
    theme = theme_name(badge)

    # 1. Launch; the automatic checks run by themselves.
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

    # 4. The checklist.
    time.sleep(0.5)
    running(badge, "after the automatic checks")
    shot = badge.shot(os.path.join(SHOTS, "selftest_%s.png" % theme))
    assert len(shot) == SHOT_BYTES, "the screenshot is %d bytes, expected %d" % (len(shot), SHOT_BYTES)
    assert colours(shot) >= 3, "the checklist looks blank: %d colours" % colours(shot)

    # 5. The summary line.
    line = badge.wait_log(_BEAT, timeout=BEAT_TIMEOUT_S)
    beat = re.search(_BEAT, line)
    assert beat.group(4) == "ok", "the summary line says the I2C lines are low: %s" % line
    assert beat.group(3) == "00", "the summary line shows a key down with nobody pressing: %s" % line

    # 6. Manual tests: in with SELECT, through with SELECT, out with CANCEL.
    badge.clear_log()
    badge.btn("a", "tap")                          # DISPLAY: the first test screen
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

    # 7. CANCEL on the checklist exits.
    badge.btn("b", "tap")
    badge.wait_state(on_launcher, timeout=5)
    badge.wait_log(r"native app '%s' stopped" % APP, timeout=5)
    to_launcher(badge)
