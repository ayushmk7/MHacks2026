-- Request test (dev only; a test fixture for WP23, never pushed to a judge badge).
--
-- Opens config.count payment requests for config.amount when it starts (one per frame, from
-- on_update) and reports over the log, one line each, so a script on the laptop can follow along
-- (test/device/t_req_single.py, t_req.py):
--
--   RT up ch <channel>            once, on start
--   RT open <req_id>              a request was opened (req_id is 16 hex characters)
--   RT err <reason>               request_open refused
--   RT status <state> <proofs>    once a second, for the first request opened
--   RT result <status>            a RESULT frame for one of this app's requests: ok, rejected, failed
--
-- The firmware does the rest: it signs the request, broadcasts it and answers presence checks.
-- CANCEL exits; the firmware closes the requests when the app stops.

local config = require("config")
local gfx, wallet = badge.gfx, badge.wallet

local opened = {}          -- req_ids, in the order they were opened
local lines = {}           -- what the screen shows
local to_open = 0          -- requests still to open
local since_status = 0
local last_status = "-"
local last_result = "-"

local RESULT_LEN = 77      -- header 4, req_id 8, status 1, ref 64 (protocol/espnow.md, RESULT)
local RESULT_STATUS = { [0] = "ok", [1] = "rejected", [2] = "failed" }

local function hex(bytes)
  local out = {}
  for i = 1, #bytes do out[i] = string.format("%02x", bytes:byte(i)) end
  return table.concat(out)
end

local function say(line)
  badge.log(line)
  lines[#lines + 1] = line
  if #lines > 6 then table.remove(lines, 1) end
end

local function open_one()
  local req, reason = wallet.request_open{
    amount = config.amount, symbol = config.symbol, name = config.name, ttl_s = config.ttl_s,
  }
  if req then
    opened[#opened + 1] = req.req_id
    say("RT open " .. req.req_id)
  else
    say("RT err " .. tostring(reason))
  end
end

function on_start()
  badge.espnow.enable(true)
  say("RT up ch " .. badge.espnow.channel())
  to_open = config.count or 1
end

function on_update(dt)
  -- One request per frame: each open is one signature, which blocks the badge for about a second
  -- with a software key. Spread out, the badge keeps answering the test script in between.
  if to_open > 0 then
    to_open = to_open - 1
    open_one()
    return
  end
  since_status = since_status + dt
  if since_status < 1 then return end
  since_status = 0
  if not opened[1] then return end
  local status = wallet.request_status(opened[1])
  if status then
    last_status = status.state .. " " .. status.proofs
  else
    last_status = "unknown 0"
  end
  badge.log("RT status " .. last_status)
end

-- RESULT is unauthenticated (protocol/espnow.md): this fixture only logs it.
function on_espnow(mac, data, rssi)
  if #data ~= RESULT_LEN or data:sub(1, 4) ~= "VK\1\4" then return end
  local req_id = hex(data:sub(5, 12))
  for _, mine in ipairs(opened) do
    if mine == req_id then
      last_result = RESULT_STATUS[data:byte(13)] or "invalid"
      say("RT result " .. last_result)
      return
    end
  end
end

function on_draw()
  gfx.clear(gfx.BG)
  gfx.text("Request test", 12, 10, gfx.WHITE, 2)
  gfx.text(tostring(config.amount) .. "  x" .. tostring(config.count or 1), 12, 36, gfx.SOLANA_GREEN, 1)
  gfx.text("status: " .. last_status, 12, 52, gfx.WHITE, 1)
  gfx.text("result: " .. last_result, 12, 66, gfx.WHITE, 1)
  for i, line in ipairs(lines) do
    gfx.text(line, 12, 84 + i * 14, gfx.MUTED, 1)
  end
  gfx.text("CANCEL to quit", 12, 222, gfx.MUTED, 1)
end

function on_button(key, pressed)
  if pressed and key == "b" then badge.system.exit() end
end
