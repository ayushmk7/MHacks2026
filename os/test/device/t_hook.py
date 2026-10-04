"""T-HOOK1 (WP22, ESP-NOW router). Needs two badges with a dev build.

Both badges must be on the same ESP-NOW channel: joined to the same hotspot, or neither joined to
any network. The test reads each badge's channel and says so if they differ.

T-HOOK1 (upstream finding F1 fixed by hooks H3 and H9): on badge 1 launch an app and exit it, then
launch a second app that listens for ESP-NOW; badge 2 broadcasts; the second app receives the
frames. The same run checks the router's forwarding rules (protocol/espnow.md, "Router"):
  - a payload that is not a VK frame reaches the app (rule 3);
  - a VK frame of an app-range type (64) reaches the app (rule 3);
  - CHAL and PROOF (types 2 and 3) never reach the app (rule 2).

The first app is the pushed fixture "plain" (fixtures/plain). The two helper apps (hookrx, hooktx)
are written to a temporary folder and pushed by this test. The check that used upstream's
whosnear sample is gone with the sample (upstream-hooks.md, "Replaced upstream files"); the same
path, two badges exchanging app frames, is what T-HOOK1 itself exercises.

Both badges must run BadgeOS: its ESP-NOW magic (BDOS, hook H23) differs from upstream's.
"""

import os
import re
import tempfile
import time

from common import launch, to_launcher

NEEDS = "two-badges"

_PLAIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures", "plain")

_INI = "name=%s\nversion=1.0.0\nauthor=BadgeOS tests\ndescription=T-HOOK1 helper\npermissions=espnow\n"

# Logs every payload it receives: "hookrx <kind> <mac> <text>". kind is "plain", or "vk<type>" for
# a VK v1 frame (then <text> is the body after the 4-byte header).
_RX_LUA = r"""
local g = badge.gfx
local count = 0
function on_start()
  badge.espnow.enable(true)
  badge.log("hookrx up ch " .. badge.espnow.channel())
end
function on_espnow(mac, data, rssi)
  local kind = "plain"
  if #data >= 4 and data:sub(1, 3) == "VK\1" then
    kind = "vk" .. data:byte(4)
    data = data:sub(5)
  end
  count = count + 1
  badge.log("hookrx " .. kind .. " " .. mac .. " " .. data)
end
function on_draw()
  g.clear(g.BG)
  g.text("hookrx " .. count, 12, 12, g.WHITE, 2)
end
"""

# Broadcasts four payloads in turn, one every 250 ms: plain, VK type 64, VK type 2, VK type 3.
_TX_LUA = r"""
local g = badge.gfx
local TOKEN = "@TOKEN@"
local frames = { "HK1|" .. TOKEN, "VK\1\64" .. TOKEN, "VK\1\2" .. TOKEN, "VK\1\3" .. TOKEN }
local wait, turn, sent = 0, 0, 0
function on_start()
  badge.espnow.enable(true)
  badge.log("hooktx up ch " .. badge.espnow.channel())
end
function on_update(dt)
  wait = wait - dt
  if wait > 0 then return end
  wait = 0.25
  turn = turn % #frames + 1
  badge.espnow.broadcast(frames[turn])
  sent = sent + 1
end
function on_draw()
  g.clear(g.BG)
  g.text("hooktx " .. sent, 12, 12, g.WHITE, 2)
end
"""


def _push_helper(badge, app_id, lua):
    with tempfile.TemporaryDirectory() as folder:
        with open(os.path.join(folder, "app.ini"), "w", encoding="utf-8") as handle:
            handle.write(_INI % app_id)
        with open(os.path.join(folder, "main.lua"), "w", encoding="utf-8") as handle:
            handle.write(lua)
        badge.push(folder, app_id)


def _channel(badge, app):
    line = badge.wait_log(r"%s up (?:on channel|ch) (\d+)" % app, timeout=10)
    return int(re.search(r"up (?:on channel|ch) (\d+)", line).group(1))


def run(badge, badge2):
    to_launcher(badge)
    to_launcher(badge2)
    token = "%08x" % (int(time.time() * 1000) & 0xFFFFFFFF)

    # ---- T-HOOK1 ----------------------------------------------------------------------------
    badge.push(_PLAIN, "plain")
    _push_helper(badge, "hookrx", _RX_LUA)
    _push_helper(badge2, "hooktx", _TX_LUA.replace("@TOKEN@", token))

    # The first app. Upstream cleared the receive handler when it stopped (finding F1).
    launch(badge, "plain")
    badge.stop()
    badge.wait_state(lambda s: s["app"] != "plain", timeout=5)

    # The second app, the one that listens.
    badge.clear_log()
    launch(badge, "hookrx")
    channel_rx = _channel(badge, "hookrx")
    badge2.clear_log()
    launch(badge2, "hooktx")
    channel_tx = _channel(badge2, "hooktx")
    assert channel_rx == channel_tx, (
        "the badges are on different ESP-NOW channels (%d and %d): join both to the same hotspot, "
        "or neither to any network" % (channel_rx, channel_tx))

    plain = badge.wait_log(r"hookrx plain [0-9a-fA-F:]{17} HK1\|%s$" % token, timeout=10)
    print(plain)  # T-HOOK1: the second app received a frame
    badge.wait_log(r"hookrx vk64 [0-9a-fA-F:]{17} %s$" % token, timeout=10)

    # Let several rounds of all four payloads go by, then check which kinds arrived.
    time.sleep(4.0)
    lines = [line for line in badge.log() if "hookrx " in line and line.endswith(token)]
    kinds = [line.split("hookrx ", 1)[1].split(" ", 1)[0] for line in lines]
    assert kinds.count("plain") >= 2 and kinds.count("vk64") >= 2, "too few frames arrived: %s" % kinds
    assert "vk2" not in kinds, "a CHAL (type 2) frame was forwarded to the app"
    assert "vk3" not in kinds, "a PROOF (type 3) frame was forwarded to the app"

    badge2.stop()
    badge.stop()
    badge.wait_state(lambda s: s["app"] != "hookrx", timeout=5)
    badge2.wait_state(lambda s: s["app"] != "hooktx", timeout=5)
    to_launcher(badge)
    to_launcher(badge2)
