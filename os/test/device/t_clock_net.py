"""WP20 clock, the half that needs a network: time=sntp within 10 s of Wi-Fi connecting.

The badge must be able to join a Wi-Fi network that reaches its SNTP host (config key ntp_server).
Either it already has the network saved (VKWIFI, or `vkdev.py provision --wifi`), or this test
joins it: set the environment variables VK_WIFI_SSID and VK_WIFI_PASSWORD before running.

After a reset the clock must start at "none" or reach "sntp" only after Wi-Fi is up; the test
measures from the first VKINFO that shows wifi=1 to the first that shows time=sntp. It also prints
which path reported the sync (the SNTP callback or the once-a-second status poll), which settles
README open item U5.
"""

import os
import re
import time

NEEDS = "network"

WIFI_TIMEOUT_S = 40   # upstream gives a join 20 s, and starts it a few seconds into the boot
SNTP_LIMIT_S = 10     # implementation-plan.md, WP20
POLL_S = 0.25

_SYNC_LINE = re.compile(r"\[vk\] clock: sntp \d+ \((sync callback|status poll)\)")


def run(badge):
    ssid = os.environ.get("VK_WIFI_SSID")
    if ssid:
        reply = badge.cmd("VKWIFI %s|%s" % (ssid, os.environ.get("VK_WIFI_PASSWORD", "")))[-1]
        assert reply.startswith("OK"), "VKWIFI -> %s" % reply

    badge.clear_log()
    badge.reset()  # the saved network is joined again during this boot

    # Wait for Wi-Fi. The clock may not claim SNTP before the network exists.
    deadline = time.monotonic() + WIFI_TIMEOUT_S
    while True:
        info = badge.info()
        if info.get("wifi") == "1":
            break
        assert info.get("time") == "none", "time=%s before Wi-Fi connected" % info.get("time")
        assert time.monotonic() < deadline, (
            "Wi-Fi did not connect within %d s (no saved network? set VK_WIFI_SSID and "
            "VK_WIFI_PASSWORD, or run VKWIFI once)" % WIFI_TIMEOUT_S)
        time.sleep(POLL_S)
    connected_at = time.monotonic()

    # time=sntp within 10 s of that.
    while info.get("time") != "sntp":
        assert time.monotonic() - connected_at < SNTP_LIMIT_S + POLL_S, (
            "time=%s %d s after Wi-Fi connected (is the badge in hotspot mode, or is SNTP to "
            "'%s' blocked on this network?)" % (info.get("time"), SNTP_LIMIT_S, _ntp_server(badge)))
        time.sleep(POLL_S)
        info = badge.info()
    print("time=sntp %.1f s after wifi=1" % (time.monotonic() - connected_at))

    state = badge.state()
    assert state["time"] == "sntp", "VKINFO says sntp but VKSTATE time is %r" % state["time"]

    # Which path saw the sync. The poll logs at most a second after the callback.
    time.sleep(1.5)
    assert any("[vk] clock: sntp started" in line for line in badge.log()), (
        "time=sntp but the log has no '[vk] clock: sntp started' line")
    paths = sorted({match.group(1) for match in map(_SYNC_LINE.search, badge.log()) if match})
    assert paths, "time=sntp but neither the sync callback nor the status poll logged it"
    print("sntp sync reported by: %s" % ", ".join(paths))  # U5: "sync callback" means the callback exists


def _ntp_server(badge):
    reply = badge.cmd("VKGET ntp_server")[-1]
    return reply[3:] if reply.startswith("OK ") else "?"
