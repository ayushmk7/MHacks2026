-- R2 network probe
--
-- Dev-only (docs/specs/P1-r-test-harness.md, R2). SELECT runs three checks from the badge and
-- shows the result and latency of each:
--
--   1. the laptop's badge listener   GET  <listener>/health   -> {"ok":true}
--   2. devnet RPC                    POST getHealth           -> {"result":"ok"}
--   3. the clock                     wallet.time_ok(), else badge.system.time_ok() (SNTP synced);
--                                    amber on a firmware with neither
--
-- Runs on BadgeOS and on the temp_firmware fork. harness/r2_probe.py writes the laptop's address
-- into config.lua and adds lib/vk.lua to the folder as vk.lua when it pushes the app.
--
-- Log lines (each "[app] R2 ..."):
--   R2 run <n> wifi=<status> ssid=<ssid> ip=<ip> rssi=<dBm> listener=<url>
--   R2 <listener|rpc|clock> <ok|warn|fail> <ms> ms  <detail>
--
-- CANCEL exits.

local cfg = require("config")
local vk = require("vk")
local http, wifi, sys = badge.http, badge.wifi, badge.system

local CHECKS = {
  { name = "listener", label = "Badge listener" },
  { name = "rpc", label = "Devnet RPC" },
  { name = "clock", label = "Clock / SNTP" },
}
local TONE = { ok = "ok", warn = "warn", fail = "bad" }
local ran = 0

-- First bytes of an unexpected reply, so a captive-portal page is recognisable on screen.
local function snippet(body)
  return "got: " .. (body or ""):gsub("%s+", " "):sub(1, 40)
end

local RUN = {}

function RUN.listener()
  local status, body = http.get(cfg.listener .. "/health", "text/plain", cfg.timeout_ms)
  if not status then return "fail", body end
  if status ~= 200 then return "fail", "HTTP " .. status end
  local reply = vk.json.decode(body)
  if type(reply) == "table" and reply.ok == true then return "ok", "ok" end
  return "fail", snippet(body)
end

function RUN.rpc()
  local request = vk.json.encode({ jsonrpc = "2.0", id = 1, method = "getHealth" })
  local status, body = http.post(cfg.rpc_url, request, "application/json", cfg.timeout_ms)
  if not status then return "fail", body end
  if status ~= 200 then return "fail", "HTTP " .. status end
  local reply = vk.json.decode(body)
  if type(reply) == "table" and reply.result == "ok" then return "ok", "healthy" end
  local err = type(reply) == "table" and type(reply.error) == "table" and reply.error.message
  return "fail", type(err) == "string" and err or snippet(body)
end

function RUN.clock()
  local wallet = badge.wallet
  local now = os.date("!%Y-%m-%d %H:%M", os.time())
  local time_ok, source = nil, nil
  if wallet and wallet.time_ok then
    time_ok, source = wallet.time_ok, "wallet"
  elseif sys.time_ok then
    time_ok, source = sys.time_ok, "SNTP"
  end
  if time_ok then
    if time_ok() then return "ok", source .. ", " .. now end
    return "fail", "not synced (" .. now .. ")"
  end
  -- Firmware without SNTP: the clock is seconds since boot, or the build date once Wi-Fi
  -- connects. Not trustworthy for expiry checks.
  return "warn", "no SNTP in firmware (" .. now .. ")"
end

local function run_all()
  ran = ran + 1
  badge.log(string.format("R2 run %d wifi=%s ssid=%s ip=%s rssi=%d listener=%s",
            ran, wifi.status(), wifi.ssid(), wifi.ip(), wifi.rssi(), cfg.listener))
  for _, check in ipairs(CHECKS) do
    local t0 = sys.millis()
    local ok, state, detail = pcall(RUN[check.name])
    if not ok then state, detail = "fail", tostring(state) end
    check.state, check.detail, check.ms = state, detail or "", sys.millis() - t0
    badge.log(string.format("R2 %-8s %-4s %5d ms  %s", check.name, check.state, check.ms, check.detail))
  end
  vk.ui.dirty()
end

local function draw()
  local connected = wifi.connected()
  local rows = {{
    l = "Wi-Fi", r = connected and wifi.ssid() or "not connected",
    sub = connected and (wifi.ip() .. "  " .. wifi.rssi() .. " dBm") or "Settings > Wi-Fi",
    tone = not connected and "bad" or nil,
  }}
  for _, check in ipairs(CHECKS) do
    rows[#rows + 1] = check.state
      and { l = check.label, r = check.state .. "  " .. check.ms .. " ms", sub = check.detail, tone = TONE[check.state] }
      or { l = check.label, r = "not run", sub = check.name == "listener" and cfg.listener or "", tone = "mut" }
  end
  vk.ui.list({
    header = cfg.header, title = "Network probe", rows = rows,
    hint = ran > 0 and ("run " .. ran .. "  SELECT again") or "SELECT run checks", back = "CANCEL quit",
  })
end

function on_draw() vk.ui.frame(draw) end

function on_button(key, pressed)
  if not pressed then return end
  if key == "a" then run_all() end
  if key == "b" then sys.exit() end
end
