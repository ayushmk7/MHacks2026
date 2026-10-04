-- R3 sign
--
-- Dev-only (docs/specs/P1-r-test-harness.md R3, §4.1): the badge half of the signing harness.
-- SELECT fetches the unsigned legacy HACK transfer that `npm run r3` queued for this badge, hands
-- the raw message bytes to wallet.begin_solana (the firmware takes the screen for the approval),
-- polls wallet.poll() from on_update, and POSTs the 64-byte signature back as base64:
--
--   GET  <listener>/badge/pending?badge=<pubkey>  -> {id, messageBase64, claimed, ...} | 204
--   POST <listener>/badge/signature  {id, sig}      -> {ok, sent, link} | 409 stale_id | 400 reason
--   POST <listener>/badge/outcome    {id, outcome}  when the wallet refuses or the user cancels
--
-- The laptop verifies, attaches, sends to devnet and records the explorer link in
-- harness/RESULTS.md. R3 passes no record or REQ in ctx, so the firmware must be a dev build
-- (VK_DEV_ALLOW_UNVERIFIED on the temp_firmware fork; the dev profile's hold-to-sign override on
-- BadgeOS), otherwise poll() returns "unverified".
--
-- Runs on BadgeOS and on the temp_firmware fork with the wallet stand-in. harness/r3_app.py writes
-- the listener and the badge's key into config.lua and adds lib/vk.lua as vk.lua.
--
-- Log lines (each "[app] R3 ..."):
--   R3 <state>: <detail>          whenever the state changes
--   R3 run <n> ssid=... ip=... rssi=... badge=<pubkey> listener=<url>
--   R3 job <id> <bytes> B legacy message claimed <amount> <symbol>
--   R3 signed <id> in <ms> ms (incl. approval)
--   R3 CONFIRMED <explorer link>
--
-- CANCEL exits (not while the approval or the POST is running).

local cfg = require("config")
local vk = require("vk")
local http, wifi, sys = badge.http, badge.wifi, badge.system
local wallet, codec = badge.wallet, badge.codec

local job = nil          -- {id, message, amount, symbol}
local state, detail, tone = "idle", "SELECT: fetch and sign", "mut"
local t_begin = 0
local runs = 0

local function set(s, d, t)
  state, detail, tone = s, d, t
  badge.log(string.format("R3 %s: %s", s, d))
  vk.ui.dirty()
end

local function snippet(body) return (body or ""):gsub("%s+", " "):sub(1, 40) end

-- This badge's own address, or nil on a firmware without the wallet (or with no identity: BadgeOS
-- returns "").
local function own_address()
  local address = wallet and wallet.address and wallet.address()
  if address == nil or address == "" then return nil end
  return address
end

-- Which pubkey to poll for. A pushed `badge` that disagrees with the firmware's own key means the
-- laptop queued for a different badge: the signature would never verify.
local function pubkey()
  local me = own_address()
  if me and cfg.badge ~= "" and me ~= cfg.badge then
    return nil, "pushed for " .. cfg.badge:sub(1, 8) .. " but this is " .. me:sub(1, 8)
  end
  local pk = me or (cfg.badge ~= "" and cfg.badge) or nil
  if not pk then return nil, "no pubkey: push with --badge or --pubkey" end
  return pk
end

local function post(path, body)
  return http.post(cfg.listener .. path, vk.json.encode(body), "application/json", cfg.post_timeout_ms)
end

local function report_outcome(outcome)
  if not job then return end
  post("/badge/outcome", { id = job.id, outcome = outcome })
end

-- ── 1. fetch and begin ──────────────────────────────────────────────────────
local function fetch_and_begin()
  runs = runs + 1
  if not (wallet and wallet.begin_solana and wallet.poll) then
    return set("error", "firmware has no wallet.begin_solana (needs A6)", "bad")
  end
  if not codec then return set("error", "firmware has no badge.codec: flash the harness build", "bad") end
  local pk, err = pubkey()
  if not pk then return set("error", err, "bad") end
  if not wifi.connected() then return set("error", "Wi-Fi not connected", "bad") end

  badge.log(string.format("R3 run %d ssid=%s ip=%s rssi=%d badge=%s listener=%s",
            runs, wifi.ssid(), wifi.ip(), wifi.rssi(), pk, cfg.listener))
  local status, body = http.get(cfg.listener .. "/badge/pending?badge=" .. pk, "application/json", cfg.get_timeout_ms)
  if not status then return set("error", "listener: " .. tostring(body), "bad") end
  if status == 204 then return set("error", "nothing queued for " .. pk:sub(1, 8), "warn") end
  if status ~= 200 then return set("error", "listener HTTP " .. status, "bad") end

  local reply = vk.json.decode(body)
  local id = type(reply) == "table" and reply.id or nil
  local message = id ~= nil and type(reply.messageBase64) == "string" and codec.b64dec(reply.messageBase64) or nil
  if not message then return set("error", "bad reply: " .. snippet(body), "bad") end
  local claimed = type(reply.claimed) == "table" and reply.claimed or {}
  job = { id = id, message = message, amount = tostring(reply.amount or claimed.amount or "?"),
          symbol = tostring(reply.symbol or claimed.symbol or "") }
  badge.log(string.format("R3 job %s %d B legacy message claimed %s %s", tostring(id), #message, job.amount, job.symbol))

  local ok, reason = wallet.begin_solana(message, {})
  if not ok then
    report_outcome(reason or "refused")
    return set("error", "begin_solana: " .. tostring(reason), "bad")
  end
  t_begin = sys.millis()
  set("approving", "approve on the wallet screen", "warn")
end

-- ── 3. return the signature ─────────────────────────────────────────────────
local function send_signature(sig)
  badge.log(string.format("R3 signed %s in %d ms (incl. approval)", tostring(job.id), sys.millis() - t_begin))
  set("posting", "verifying + sending on devnet...", "warn")
  local status, body = post("/badge/signature", { id = job.id, sig = codec.b64enc(sig) })
  if not status then
    -- The laptop only answers after devnet confirms; a timeout here is not a failure by itself.
    return set("error", "no reply (" .. tostring(body) .. "): check laptop", "warn")
  end
  local reply = vk.json.decode(body)
  reply = type(reply) == "table" and reply or {}
  if status == 200 then
    local link = type(reply.link) == "string" and reply.link or nil
    if link then badge.log("R3 CONFIRMED " .. link) end
    local txsig = link and link:match("/tx/([%w]+)")
    return set("done", txsig and ("confirmed " .. txsig:sub(1, 20) .. "...") or "verified (not sent)", "ok")
  end
  if status == 409 then return set("error", "rebuilt while approving: SELECT again", "warn") end
  set("error", "rejected: " .. (type(reply.reason) == "string" and reply.reason or ("HTTP " .. status)), "bad")
end

function on_update(dt)
  if state ~= "approving" then return end
  local r, reason = wallet.poll()
  if r == "pending" then return end
  if type(r) == "string" and #r == 64 then return send_signature(r) end
  reason = reason or (r == nil and "unknown" or ("unexpected poll result, " .. #tostring(r) .. " B"))
  report_outcome(reason)
  set("error", "not signed: " .. reason, reason == "cancelled" and "warn" or "bad")
end

function on_button(key, pressed)
  if not pressed then return end
  if key == "a" and state ~= "approving" and state ~= "posting" then fetch_and_begin() end
  if key == "b" and state ~= "approving" then sys.exit() end
end

local function draw()
  local ui = vk.ui
  local connected = wifi.connected()
  local pk = pubkey()
  local api = wallet and wallet.begin_solana
  ui.page()
  ui.header(cfg.header)
  ui.title("R3 SIGNING", ui.TITLE_Y)
  local y = ui.LIST_Y
  ui.row(y, "WI-FI", connected and (wifi.ssid() .. "  " .. wifi.rssi() .. " dBm") or "not connected", false,
         not connected and ui.color("stamp_bad") or nil)
  ui.row(y + ui.ROW_PITCH, "LISTENER", cfg.listener)
  ui.row(y + 2 * ui.ROW_PITCH, "BADGE", pk and vk.short(pk) or "?")
  ui.row(y + 3 * ui.ROW_PITCH, "WALLET API", api and "present" or "MISSING (needs A6)", false,
         not api and ui.color("stamp_bad") or nil)
  if job then
    ui.row(y + 4 * ui.ROW_PITCH, "JOB", tostring(job.id))
    ui.row(y + 5 * ui.ROW_PITCH, "CLAIMED", job.amount .. " " .. job.symbol .. "  " .. #job.message .. " B")
  end
  local tones = { ok = "stamp_ok", warn = "stamp_warn", bad = "stamp_bad", mut = "faint" }
  ui.rule(y + 6 * ui.ROW_PITCH - 4)
  ui.row(y + 6 * ui.ROW_PITCH + 6, "STATUS", state, false, ui.color(tones[tone]))
  ui.subline(y + 6 * ui.ROW_PITCH + 19, detail)
  ui.footer((state == "approving" or state == "posting") and "waiting..."
            or ("SELECT " .. (runs > 0 and "fetch again" or "fetch and sign")), "CANCEL quit")
end

function on_draw() vk.ui.frame(draw) end
