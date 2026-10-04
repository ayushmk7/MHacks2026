"""T-BOOT1, the serial-command smoke checks of WP01, and one navigation check (WP03).

Needs one badge with a dev build. No provisioning, no network, no hands.
"""

import re

from common import to_launcher


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

    # Navigation: an injected key press changes what is on the screen.
    to_launcher(badge)
    before = badge.shot()
    assert len(before) == 153600, "screenshot is %d bytes" % len(before)
    badge.btn("down", "tap")
    after = badge.shot()
    if after == before:
        # A launcher with no apps has one row, so DOWN has nowhere to go. CANCEL opens Settings.
        badge.btn("b", "tap")
        after = badge.shot()
        badge.btn("b", "tap")  # and back to the launcher
    assert after != before, "an injected key press did not change the screen"
