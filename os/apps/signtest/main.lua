-- Sign test
--
-- Dev-only (docs/os/apps/apps.md, "Sign test"). The first app that signs: it asks the laptop for
-- a transfer, hands the bytes to the firmware approval, and submits what the badge signed.
--
--   1. GET <listener_url>/badge/pending?badge=<address> every poll_ms. 204: nothing waiting.
--      200: {id, messageBase64, ...} (docs/os/integration/backend.md).
--   2. wallet.begin_solana(message) with no ctx. The firmware decides what the screen says; in the
--      dev profile that is red "UNVERIFIED RECIPIENT" with the hold-to-sign override.
--   3. Signed: sendTransaction with wallet.wire_tx(sig, message). The laptop sees the transfer on
--      chain; nothing is posted to it. Refused: POST <listener_url>/badge/outcome
--      {id, outcome = "rejected"}.
--   4. One status line: waiting, approving, sent <short sig>, refused: <reason>.
--
-- A network call that fails shows a status line and is tried again later. At most one blocking
-- call is made per frame, and each has a timeout, so nothing hangs.
--
-- Log lines (each "[app] ST ..."), for test/device/t_sign_net.py:
--   ST status <the status line>      whenever it changes
--   ST begin <id>                    the approval was opened for that attempt
--   ST sig <base58>                  the signature the badge made
--   ST sent <base58>                 the RPC node accepted the transaction
--   ST refused <reason>
--
-- The JSON field extraction and the RPC call below are temporary: lib/vk.lua replaces them
-- (vk.json, vk.send_tx) when it exists.

local cfg = require("config")
local gfx, wallet, codec, http = badge.gfx, badge.wallet, badge.codec, badge.http

local status = ""
local state = "waiting"      -- waiting -> approving -> sending | reporting -> waiting
local next_at = 0            -- badge.millis() before which no network call is made
local handled = {}           -- ids of the attempts already taken: the laptop serves each for 90 s
local attempt = nil          -- {id = , msg = }: the attempt being approved, sent or reported
local wire = nil             -- the signed transaction, base64
local tries = 0
local keep_until = 0         -- badge.millis() until which "sent ..." or "refused: ..." stays on screen

local function set_status(text)
  if text ~= status then
    status = text
    badge.log("ST status " .. text)
  end
end

local function short(text)
  if #text <= 10 then return text end
  return text:sub(1, 4) .. ".." .. text:sub(-4)
end

-- The value of the first "key": in a JSON text. A string value is returned unescaped; a number,
-- true, false or null is returned as its text. Enough for the flat replies read here.
local function json_field(text, key)
  local _, last = text:find('"' .. key .. '"%s*:%s*')
  if not last then return nil end
  local at = last + 1
  if text:sub(at, at) ~= '"' then
    return text:match("^[^,}%]%s]+", at)
  end
  local out = {}
  at = at + 1
  while at <= #text do
    local stop = text:find('["\\]', at)
    if not stop then return nil end
    out[#out + 1] = text:sub(at, stop - 1)
    if text:sub(stop, stop) == '"' then return table.concat(out) end
    local escaped = text:sub(stop + 1, stop + 1)
    if escaped == "n" then
      out[#out + 1] = "\n"
    elseif escaped == "t" then
      out[#out + 1] = "\t"
    elseif escaped == "u" then
      out[#out + 1] = "?"
      stop = stop + 4
    else
      out[#out + 1] = escaped
    end
    at = stop + 2
  end
  return nil
end

local function json_string(text)
  return '"' .. text:gsub('[\\"]', "\\%0") .. '"'
end

local function base_url(name)
  local url = wallet.config(name)
  if not url or url == "" then return nil end
  return (url:gsub("/+$", ""))
end

-- One JSON-RPC call. Returns the "result" string, or nil and a message.
local function rpc_string(method, params_json)
  local url = base_url("rpc_url")
  if not url then return nil, "rpc_url not set" end
  local body = '{"jsonrpc":"2.0","id":1,"method":' .. json_string(method) .. ',"params":' .. params_json .. "}"
  local code, reply = http.post(url, body, "application/json", cfg.http_timeout_ms)
  if not code then return nil, tostring(reply) end
  if code ~= 200 then return nil, "rpc http " .. tostring(code) end
  if reply:find('"error"%s*:') then
    return nil, json_field(reply, "message") or "rpc error"
  end
  local result = json_field(reply, "result")
  if not result then return nil, "rpc reply not understood" end
  return result
end

local function later(ms)
  next_at = badge.millis() + ms
end

-- "waiting", unless the result of the last attempt is still being shown.
local function show_waiting()
  if badge.millis() >= keep_until then set_status("waiting") end
end

local function back_to_waiting()
  state = "waiting"
  attempt, wire = nil, nil
  tries = 0
  keep_until = badge.millis() + cfg.result_ms
  later(cfg.poll_ms)
end

local function refuse(why)
  tries = 0
  state = "reporting"
  next_at = 0
  badge.log("ST refused " .. why)
  set_status("refused: " .. why)
end

-- Step 1 and 2: ask for a transfer; open the approval for a new one.
local function ask()
  local listener = base_url("listener_url")
  if not listener then
    set_status("listener_url not set")
    return later(cfg.retry_ms)
  end
  if not badge.wifi.connected() then
    set_status("no network")
    return later(cfg.retry_ms)
  end
  local code, body = http.get(listener .. "/badge/pending?badge=" .. wallet.address(), cfg.http_timeout_ms)
  if not code then
    set_status("laptop: " .. tostring(body))
    return later(cfg.retry_ms)
  end
  if code == 204 then
    show_waiting()
    return later(cfg.poll_ms)
  end
  if code ~= 200 then
    set_status("laptop: http " .. tostring(code))
    return later(cfg.poll_ms)
  end

  local id = json_field(body, "id")
  local encoded = json_field(body, "messageBase64")
  if not id or handled[id] then
    show_waiting()
    return later(cfg.poll_ms)
  end
  local msg = encoded and codec.b64dec(encoded)
  if not msg or #msg == 0 then
    handled[id] = true
    set_status("laptop sent no message")
    return later(cfg.poll_ms)
  end

  local ok, why = wallet.begin_solana(msg)
  if ok then
    handled[id] = true
    attempt = {id = id, msg = msg}
    state = "approving"
    badge.log("ST begin " .. id)
    set_status("approving")
  elseif why == "busy" then
    set_status("wallet busy")
    later(cfg.retry_ms)
  else
    -- Refused before any screen (not provisioned, too long for this key, ...).
    handled[id] = true
    attempt = {id = id, msg = msg}
    refuse(tostring(why))
  end
end

-- The app is paused while the approval is open; this runs again once the user has decided.
local function approving()
  local result, why = wallet.poll()
  if result == "pending" then return end
  if result then
    badge.log("ST sig " .. codec.b58enc(result))
    wire = wallet.wire_tx(result, attempt.msg)
    tries = 0
    state = "sending"
    next_at = 0
    set_status("sending")
  else
    refuse(tostring(why))
  end
end

-- Step 3, signed: submit the transaction.
local function send()
  tries = tries + 1
  local sig, why = rpc_string("sendTransaction", "[" .. json_string(wire) .. ',{"encoding":"base64"}]')
  if sig then
    badge.log("ST sent " .. sig)
    set_status("sent " .. short(sig))
    return back_to_waiting()
  end
  if tries >= cfg.send_tries then
    set_status("not sent: " .. tostring(why))
    return back_to_waiting()
  end
  set_status("send failed, retrying: " .. tostring(why))
  later(cfg.retry_ms)
end

-- Step 3, refused: tell the laptop. The status line keeps saying why.
local function report()
  tries = tries + 1
  local listener = base_url("listener_url")
  local code = nil
  if listener and badge.wifi.connected() then
    local body = '{"id":' .. json_string(attempt.id) .. ',"outcome":"rejected"}'
    code = http.post(listener .. "/badge/outcome", body, "application/json", cfg.http_timeout_ms)
  end
  -- 200: recorded. 4xx: the laptop will never take it (already resolved, or expired).
  if (code and code >= 200 and code < 500) or tries >= cfg.report_tries then
    return back_to_waiting()
  end
  later(cfg.retry_ms)
end

function on_start()
  set_status("waiting")
end

function on_update(dt)
  if state == "approving" then return approving() end
  if badge.millis() < next_at then return end
  if state == "waiting" then
    ask()
  elseif state == "sending" then
    send()
  elseif state == "reporting" then
    report()
  end
end

function on_draw()
  local cx = gfx.width() // 2
  gfx.clear()
  gfx.text_center("Sign test", cx, 50, gfx.WHITE, 2)
  gfx.text_center(status, cx, 110, gfx.SOLANA_GREEN, 1)
  gfx.text_center("CANCEL to quit", cx, 205, gfx.MUTED, 1)
end

function on_button(key, pressed)
  if pressed and key == "b" then badge.system.exit() end
end
