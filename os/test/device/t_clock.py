"""WP20 clock, the half that needs no network, and the WP22 regression for the ESP-NOW handler.

1. Review focus 4: after a reset the badge logs "[os] ready" within 30 s whether or not Wi-Fi is
   there (boot never waits for SNTP), and with Wi-Fi absent VKINFO reports time=none.
2. VKTIME <now> marks the clock synced: VKINFO time=sntp and VKSTATE "time":"sntp".
3. Upstream's sample app "hello" still launches and stops now that the router owns the one
   ESP-NOW receive handler (hooks H3 and H9).

Needs one badge with a dev build. No provisioning, no network, no hands. The SNTP check is in
t_clock_net.py (NEEDS network); T-HOOK1 and the whosnear check are in t_hook.py (NEEDS two badges).
The test leaves the clock set to the laptop's time, source sntp.
"""

import os
import time

from common import launch, to_launcher

_APPS = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "apps"))


def run(badge):
    # 1. Boot does not wait for the network.
    badge.clear_log()
    waited = badge.reset()  # raises unless the badge is ready within 30 s
    assert any("[os] ready" in line for line in badge.log()), (
        "the badge answers but logged no '[os] ready' after the reset pulse: it did not reboot")
    print("ready %.1f s after the reset pulse" % waited)

    info = badge.info()
    assert "time" in info, "VKINFO has no time field (clock.cpp not linked?): %s" % info
    state = badge.state()
    if info.get("wifi") == "1":
        # A saved network was in range, so SNTP may already have answered. The boot-time half of
        # the check still holds (ready within 30 s); time=none cannot be asserted on this badge.
        print("note: Wi-Fi is connected, so time=none is not asserted (VKINFO time=%s)" % info["time"])
        assert info["time"] in ("none", "sntp"), "VKINFO time=%s right after boot" % info["time"]
    else:
        assert info["time"] == "none", "Wi-Fi absent, yet VKINFO time=%s right after boot" % info["time"]
        assert state["time"] == "none", "Wi-Fi absent, yet VKSTATE time is %r right after boot" % state["time"]

    # 2. The dev command sets the time and marks the source SNTP.
    badge.ok("VKTIME %d" % int(time.time()))
    info = badge.info()
    assert info.get("time") == "sntp", "after VKTIME, VKINFO time=%s" % info.get("time")
    state = badge.state()
    assert state["time"] == "sntp", "after VKTIME, VKSTATE time is %r" % state["time"]

    # 3. An upstream sample app still launches, keeps running, and stops.
    badge.push(os.path.join(_APPS, "hello"), "hello")
    badge.clear_log()
    launch(badge, "hello")
    time.sleep(1.0)  # a few dozen frames: an app that errors is stopped by the runtime
    state = badge.state()
    assert state["app"] == "hello", "hello stopped by itself; state: %s; log: %s" % (state, badge.log()[-5:])
    badge.stop()
    badge.wait_state(lambda s: s["app"] != "hello", timeout=5)
    to_launcher(badge)

    # The clock kept its source across the app's life.
    assert badge.info().get("time") == "sntp", "the clock lost its source while an app ran"
