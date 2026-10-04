-- R6 fuzz
--
-- Dev-only (docs/specs/P1-r-test-harness.md R6): the badge half of the fuzz set. SELECT starts an
-- unattended loop over the cases harness/r6_fuzz.mjs serves through the R3 path:
--
--   GET  <listener>/badge/pending?badge=<pubkey>&up=<ms>  -> {id, fn, n, t1, a1, ...} | 204 when done
--   call wallet[fn](args...) under pcall
--   POST <listener>/badge/outcome {id, outcome, reason, sig, up, ms}
--
-- outcome: refused (nil, reason) | raised (error) | screen (begin_* returned true) | signed (64 B)
--        | returned (anything else) | missing (no such function) | crash (see below)
--
-- Crash detection: the case id goes to storage.kv (config.marker) right before the call and is
-- removed right after. If the call kills the app or resets the badge, relaunching R6 finds it and
-- reports `crash` for that case before anything else. `up` (ms since boot) lets the laptop tell a
-- badge reset from an app crash.
--
-- A control (or a decoder bug) can open the firmware's approval screen. Press CANCEL there, never
-- SELECT. The app reports `screen` at once and the final poll() result afterwards.
--
-- Runs on the temp_firmware fork with the wallet stand-in, and on BadgeOS (which has no
-- sign_request, sign_proof or begin_bank: those cases report `missing`). harness/r6_app.py writes
-- the listener and the badge's key into config.lua and adds lib/vk.lua as vk.lua.
--
-- Log lines (each "[app] R6 ..."): R6 <state>: <detail> whenever the state changes, R6 reported
-- <id> <outcome> [reason] after each case, and R6 found in-flight marker <id> after a crash.
--
-- CANCEL pauses the loop, or exits when idle.

local cfg = require("config")
local vk = require("vk")
local http, wifi, sys, storage = badge.http, badge.wifi, badge.system, badge.storage
local wallet, codec = badge.wallet, badge.codec
local kv = storage and storage.kv
local null = vk.json.null

local FUNCTIONS = { { "begin_solana", "sol" }, { "begin_bank", "bank" }, { "sign_request", "req" }, { "sign_proof", "proof" } }

-- ── state ───────────────────────────────────────────────────────────────────
local state = "idle"        -- idle | fetch | call | approving | report | done | paused | error
local detail, tone = "SELECT: start the fuzz run", "mut"
local case = nil            -- {id, fn, expect, name, index, total, args, n}
local result = nil          -- outcome table waiting to be POSTed
local after_report = "fetch"
local retries = 0
local t_call = 0
local tally = { refused = 0, fail = 0, crash = 0, missing = 0, controls_ok = 0, controls_bad = 0 }
local last_line = ""

local function set(s, d, t)
  state, detail, tone = s, d, t or tone
  badge.log(string.format("R6 %s: %s", s, d))
  vk.ui.dirty()
end

local function own_address()
  local address = wallet and wallet.address and wallet.address()
  if address == nil or address == "" then return nil end
  return address
end

local function pubkey()
  local me = own_address()
  if me and cfg.badge ~= "" and me ~= cfg.badge then
    return nil, "pushed for " .. cfg.badge:sub(1, 8) .. " but this is " .. me:sub(1, 8)
  end
  local pk = me or (cfg.badge ~= "" and cfg.badge) or nil
  if not pk then return nil, "no pubkey: push with --badge or --pubkey" end
  return pk
end

-- A leftover marker means the last call never returned: report it before anything else.
local leftover = kv and kv.get(cfg.marker) or nil
if leftover and leftover ~= "" then
  result = { id = leftover, outcome = "crash", reason = "relaunched with case in flight" }
  after_report = "idle"
  state, detail, tone = "report", "previous run died in " .. leftover .. ": reporting", "bad"
  badge.log("R6 found in-flight marker " .. leftover .. ": the call crashed the app or reset the badge")
end

-- ── one case ────────────────────────────────────────────────────────────────
local function fetch()
  local pk, err = pubkey()
  if not pk then return set("error", err, "bad") end
  if not wifi.connected() then return set("error", "Wi-Fi not connected", "bad") end
  local status, body = http.get(cfg.listener .. "/badge/pending?badge=" .. pk .. "&up=" .. sys.millis(),
                                "application/json", cfg.get_timeout_ms)
  if not status then
    retries = retries + 1
    if retries > cfg.retries then return set("error", "listener: " .. tostring(body), "bad") end
    return
  end
  retries = 0
  if status == 204 then return set("done", "all cases run: see the laptop report", "ok") end
  if status ~= 200 then return set("error", "listener HTTP " .. status, "bad") end

  local reply = vk.json.decode(body)
  if type(reply) ~= "table" or reply.id == nil or reply.fn == nil then
    return set("error", "bad reply: " .. body:sub(1, 40), "bad")
  end
  local c = { id = tostring(reply.id), fn = tostring(reply.fn), expect = reply.expect and tostring(reply.expect),
              name = tostring(reply.name or ""), index = tonumber(reply.index) or 0,
              total = tonumber(reply.total) or 0, n = tonumber(reply.n) or 0, reply = reply }
  case = c
  set("call", string.format("%d/%d %s %s", c.index, c.total, c.id, c.fn), "warn")
end

-- The case's arguments: t<i> is the type (b bytes as base64, n number, T empty table, B true,
-- anything else nil) and a<i> the value.
local function arguments(c)
  local args = {}
  for i = 1, c.n do
    local t, a = c.reply["t" .. i], c.reply["a" .. i]
    a = a ~= nil and tostring(a) or ""
    if t == "b" then args[i] = codec.b64dec(a) or ""
    elseif t == "n" then args[i] = tonumber(a)
    elseif t == "T" then args[i] = {}
    elseif t == "B" then args[i] = true
    else args[i] = nil end
  end
  return args
end

local function call()
  local c = case
  local f = wallet and wallet[c.fn]
  if type(f) ~= "function" then
    result = { id = c.id, outcome = "missing", reason = "no wallet." .. c.fn }
    return set("report", c.id .. ": firmware has no wallet." .. c.fn, "mut")
  end
  if not codec then return set("error", "firmware has no badge.codec: flash the harness build", "bad") end
  c.args = arguments(c)
  if kv then kv.set(cfg.marker, c.id) end
  t_call = sys.millis()
  local ok, r1, r2 = pcall(f, table.unpack(c.args, 1, c.n))
  local ms = sys.millis() - t_call
  if kv then kv.remove(cfg.marker) end

  local begin = c.fn:sub(1, 6) == "begin_"
  if not ok then
    result = { id = c.id, outcome = "raised", reason = tostring(r1), ms = ms }
  elseif begin and r1 == true then
    result = { id = c.id, outcome = "screen", ms = ms }
    after_report = "approving"
    badge.log(string.format("R6 %s opened the approval screen%s", c.id,
              c.expect == "screen" and " (control): press CANCEL" or ": DECODER ACCEPTED A FUZZ INPUT, press CANCEL"))
    return set("report", c.id .. ": approval screen up, press CANCEL", c.expect == "screen" and "warn" or "bad")
  elseif r1 == nil then
    result = { id = c.id, outcome = "refused", reason = r2 ~= nil and tostring(r2) or "nil", ms = ms }
  elseif not begin and type(r1) == "string" and #r1 == 64 then
    result = { id = c.id, outcome = "signed", sig = codec.b64enc(r1), ms = ms }
  else
    result = { id = c.id, outcome = "returned",
               reason = type(r1) .. (type(r1) == "string" and (" " .. #r1 .. " B") or (" " .. tostring(r1))), ms = ms }
  end
  set("report", string.format("%s -> %s %s", c.id, result.outcome, result.reason or ""), "mut")
end

local function poll()
  local r, reason = wallet.poll()
  if r == "pending" then return end
  if type(r) == "string" and #r == 64 then
    result = { id = case.id, outcome = "signed", sig = codec.b64enc(r), ms = sys.millis() - t_call }
  else
    result = { id = case.id, outcome = "refused", reason = tostring(reason or r), ms = sys.millis() - t_call }
  end
  set("report", string.format("%s approval ended: %s", case.id, result.outcome), "mut")
end

local function count(r)
  if not case or r.id ~= case.id then
    if r.outcome == "crash" then tally.crash = tally.crash + 1 end
    return
  end
  if after_report == "approving" then return end   -- the interim "screen" report
  if case.expect == "refuse" then
    if case.saw_screen then tally.fail = tally.fail + 1   -- the decoder accepted it, whatever happened next
    elseif r.outcome == "refused" or r.outcome == "raised" then tally.refused = tally.refused + 1
    elseif r.outcome == "missing" then tally.missing = tally.missing + 1
    else tally.fail = tally.fail + 1 end
  elseif r.outcome == "missing" then tally.missing = tally.missing + 1
  elseif (case.expect == "screen" and r.outcome ~= "missing" and case.saw_screen) or (case.expect == "signed" and r.outcome == "signed") then
    tally.controls_ok = tally.controls_ok + 1
  else tally.controls_bad = tally.controls_bad + 1 end
end

local function report()
  local r = result
  if r.outcome == "screen" and case then case.saw_screen = true end
  local body = vk.json.encode({ id = r.id, outcome = r.outcome, reason = r.reason == nil and null or r.reason,
                                sig = r.sig == nil and null or r.sig, ms = r.ms == nil and null or r.ms, up = sys.millis() })
  local status, err = http.post(cfg.listener .. "/badge/outcome", body, "application/json", cfg.post_timeout_ms)
  if status ~= 200 then
    retries = retries + 1
    if retries > cfg.retries then return set("error", "could not report " .. r.id .. ": " .. tostring(status or err), "bad") end
    return
  end
  retries = 0
  if r.outcome == "crash" and kv then kv.remove(cfg.marker) end   -- reported: do not report it again
  count(r)
  last_line = r.id .. " " .. r.outcome .. (r.reason and (" " .. r.reason) or "")
  badge.log("R6 reported " .. last_line)
  vk.ui.dirty()
  result = nil
  local nxt = after_report
  after_report = "fetch"
  if nxt == "approving" then return set("approving", "press CANCEL on the approval screen", "warn") end
  if nxt == "idle" then return set("idle", "crash reported. SELECT: continue", "warn") end
  state = "fetch"
end

function on_update(dt)
  if state == "fetch" then fetch()
  elseif state == "call" then call()
  elseif state == "approving" then poll()
  elseif state == "report" then report() end
end

function on_button(key, pressed)
  if not pressed then return end
  if key == "a" and (state == "idle" or state == "paused" or state == "error" or state == "done") then
    retries = 0
    if result then state = "report" else set("fetch", "running", "warn") end
  elseif key == "b" then
    if state == "fetch" or state == "call" then set("paused", "paused. SELECT: resume  CANCEL: exit", "warn")
    elseif state ~= "approving" and state ~= "report" then sys.exit() end
  end
end

local function draw()
  local ui = vk.ui
  local tones = { ok = "stamp_ok", warn = "stamp_warn", bad = "stamp_bad", mut = "faint" }
  local connected = wifi.connected()
  local pk = pubkey()
  local fns = {}
  for _, f in ipairs(FUNCTIONS) do
    fns[#fns + 1] = f[2] .. ((wallet and type(wallet[f[1]]) == "function") and "+" or "-")
  end
  ui.page()
  ui.header(cfg.header)
  ui.title("R6 FUZZ", ui.TITLE_Y)
  local y, p = ui.LIST_Y, ui.ROW_PITCH
  ui.row(y, "WI-FI", connected and wifi.ssid() or "not connected", false, not connected and ui.color("stamp_bad") or nil)
  ui.row(y + p, "BADGE", pk and vk.short(pk) or "?")
  ui.row(y + 2 * p, "WALLET", table.concat(fns, " "))
  ui.row(y + 3 * p, "CASE", case and string.format("%d/%d %s %s", case.index, case.total, case.id, case.fn) or "-")
  ui.row(y + 4 * p, "REFUSED / FAIL", tally.refused .. " / " .. tally.fail, false,
         (tally.fail + tally.crash) > 0 and ui.color("stamp_bad") or nil)
  ui.row(y + 5 * p, "CRASH / MISSING", tally.crash .. " / " .. tally.missing, false,
         tally.crash > 0 and ui.color("stamp_bad") or nil)
  ui.row(y + 6 * p, "CONTROLS OK / BAD", tally.controls_ok .. " / " .. tally.controls_bad, false,
         tally.controls_bad > 0 and ui.color("stamp_warn") or nil)
  ui.rule(y + 7 * p - 4)
  ui.row(y + 7 * p + 6, "STATUS", state, false, ui.color(tones[tone]))
  ui.subline(y + 7 * p + 19, detail)
  local hint = (state == "fetch" or state == "call") and "CANCEL pause"
            or (state == "approving" or state == "report") and "waiting..."
            or "SELECT " .. (state == "idle" and "start" or "continue")
  ui.footer(hint, (state == "fetch" or state == "call" or state == "approving" or state == "report") and "" or "CANCEL quit")
end

function on_draw() vk.ui.frame(draw) end
