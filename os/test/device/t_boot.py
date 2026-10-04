"""T-BOOT1, the serial-command smoke checks of WP01, and one navigation check (WP03, WP37).

Needs one badge with a dev build. No provisioning, no network, no hands.
"""

import re

from common import assert_screen_lit, assert_screen_sent, to_launcher


def run(badge):
    # T-BOOT1: reset; "[os] ready" within 30 s; PING answers; the registries line is in the log.
    badge.clear_log()
    badge.reset()  # raises unless the badge is ready within 30 s
    boot_log = badge.log()
    assert any("[os] ready" in line for line in boot_log), (
        "the badge answers but logged no '[os] ready' after the reset pulse: it did not reboot")
    pong = badge.cmd("PING")[-1]
    assert pong == "OK pong", "PING -> %s" % pong

    registries = badge.wait_log(r"\[vk\] registries:", timeout=2)
    print(registries)  # the integrator copies this line into the tracking notes
    counts = {name: int(value) for name, value in re.findall(r"(\w+)=(\d+)", registries)}
    assert counts.get("commands", 0) >= 2, "expected commands>=2 in: %s" % registries
    # The status-item registry is gone; the settings pages are counted instead (13 shipped).
    assert "status" not in counts, "the registries line still has status=: %s" % registries
    assert counts.get("pages", 0) >= 1, "expected pages>=1 (settings pages) in: %s" % registries

    # The product is BadgeOS: no splash, and the names upstream printed are gone.
    assert not any("[boot] splash" in line for line in boot_log), "the boot log has a splash line"
    assert not any("solana os" in line.lower() or "skyrizz" in line.lower() for line in boot_log), (
        "the boot log names upstream: %s" % [l for l in boot_log if "solana os" in l.lower() or "skyrizz" in l.lower()])

    # VKHELP lists VKHELP and VKINFO.
    help_reply = badge.cmd("VKHELP")
    assert help_reply[-1].startswith("OK"), "VKHELP -> %s" % help_reply[-1]
    names = {line.split()[1] for line in help_reply[:-1] if len(line.split()) > 1}
    assert {"VKHELP", "VKINFO"} <= names, "VKHELP lists %s" % sorted(names)

    # VKINFO has profile=dev and api=2.
    info = badge.info()
    assert info.get("profile") == "dev", "VKINFO profile=%s" % info.get("profile")
    assert info.get("api") == "2", "VKINFO api=%s" % info.get("api")

    # VKSTATE parses as JSON (state() raises if it does not).
    state = badge.state()
    assert isinstance(state, dict) and "app" in state and "heap" in state, "VKSTATE: %s" % state
    assert "screen" in state, "VKSTATE has no screen field: %s" % state

    # Navigation: boot lands on the shell's launcher, and injected key presses move between the
    # shell's screens. The screen is read from VKSTATE, never guessed from a picture.
    state = to_launcher(badge)
    assert state["app"] == "" and state["screen"] == "launcher", "after boot: %s" % state
    # A screenshot is the canvas, not the glass: the backlight must be on and the canvas sent.
    sent = assert_screen_lit(state, "after boot")
    before = badge.shot()
    assert len(before) == 153600, "screenshot is %d bytes" % len(before)
    badge.btn("b", "tap")  # CANCEL on the launcher opens Settings
    badge.wait_state(lambda s: s["screen"] == "settings", timeout=5)
    assert_screen_sent(badge, sent, "the Settings list")
    after = badge.shot()
    assert after != before, "the Settings list looks the same as the launcher"
    badge.btn("b", "tap")  # and back
    badge.wait_state(lambda s: s["app"] == "" and s["screen"] == "launcher", timeout=5)
