-- Check test
--
-- Dev-only test fixture (docs/os/apps/apps.md). It runs one case and logs every step, so a
-- script on the laptop can drive the firmware approval and read what happened
-- (test/device/t_sign.py, t_chk.py, t_sto.py). It never ships on a judge badge.
--
-- The case is the table returned by case.lua, a file the test pushes into this app's folder.
-- Fields, all optional:
--   msg_hex        the message to sign, as hex
--   transfer       or: a table for wallet.build_transfer{...}, built on the badge
--   record_hex     ctx.record      (registry record)
--   sig_hex        ctx.record_sig  (issuer signature)
--   req_hex        ctx.req         (REQ frame)
--   delay_ms       how long after the start the case runs
--   flush_draw     paint the whole screen and call gfx.flush() on every frame
--   history        log the newest wallet.history() entry
--   storage_probe  try to read the history file through badge.storage
--
-- Log lines (badge.log, so each is "[app] CT ..."):
--   CT nocase                          there is no case.lua (CT case error: ... if it does not load)
--   CT tick <n>                        once a second, from on_update
--   CT hist <outcome> <amount> <symbol>   (CT hist none: the history is empty)
--   CT storage <a> <b>                 "ok" or "fail" for each probed path
--   CT call <ms>                       badge.millis() just before begin_solana (measurement M3)
--   CT begin ok | CT begin err <reason>
--   CT poll sig <hex> | CT poll err <reason>
--
-- CANCEL exits.

local cfg = require("config")
local gfx, wallet, codec = badge.gfx, badge.wallet, badge.codec

local found, case = pcall(require, "case")
if not found or type(case) ~= "table" then
  badge.log("CT nocase")
  -- A case.lua that is there but does not load is a mistake in the test: say why.
  if not found and not tostring(case):find("module 'case' not found", 1, true) then
    badge.log("CT case error: " .. tostring(case):gsub("%s+", " "):sub(1, 150))
  end
  case = nil
end

local started = badge.millis()
local last_tick = started
local ticks = 0
local frame = 0
local stage = case and "wait" or "idle"   -- wait -> poll -> done; idle with no case
local status = case and "waiting" or "no case"

local loud = gfx.color(cfg.flush_color[1], cfg.flush_color[2], cfg.flush_color[3])

local function log_history()
  local list = wallet.history and wallet.history(1)
  local entry = list and list[1]
  if entry then
    badge.log("CT hist " .. tostring(entry.outcome) .. " " .. tostring(entry.amount) .. " " .. tostring(entry.symbol))
  else
    badge.log("CT hist none")
  end
end

-- badge.storage raises for a path that leaves the app's directory and returns nil for a file it
-- cannot open. Either way the read failed.
local function probe(path)
  local ok, data = pcall(badge.storage.read, path)
  if ok and data ~= nil then return "ok" end
  return "fail"
end

local function log_storage()
  local results = {}
  for i, path in ipairs(cfg.probe_paths) do results[i] = probe(path) end
  badge.log("CT storage " .. table.concat(results, " "))
end

local function unhex(text)
  if text == nil then return nil end
  return codec.unhex(text)
end

-- Runs the case once. Returns the next stage.
local function act()
  if case.history then log_history() end
  if case.storage_probe then log_storage() end

  local msg, why
  if case.msg_hex then
    msg = unhex(case.msg_hex)
    if not msg then why = "bad_arg" end
  elseif case.transfer then
    msg, why = wallet.build_transfer(case.transfer)
  else
    status = "done"
    return "done"
  end
  if not msg then
    status = "begin err " .. tostring(why)
    badge.log("CT begin err " .. tostring(why))
    return "done"
  end

  local ctx
  if case.record_hex or case.sig_hex or case.req_hex then
    ctx = {record = unhex(case.record_hex), record_sig = unhex(case.sig_hex), req = unhex(case.req_hex)}
  end

  badge.log("CT call " .. badge.millis())
  local ok
  if ctx then ok, why = wallet.begin_solana(msg, ctx) else ok, why = wallet.begin_solana(msg) end
  if ok then
    status = "begin ok"
    badge.log("CT begin ok")
    return "poll"
  end
  status = "begin err " .. tostring(why)
  badge.log("CT begin err " .. tostring(why))
  return "done"
end

local function poll()
  local result, why = wallet.poll()
  if result == "pending" then return "poll" end
  if result then
    status = "signed"
    badge.log("CT poll sig " .. codec.hex(result))
  else
    status = "poll err " .. tostring(why)
    badge.log("CT poll err " .. tostring(why))
  end
  return "done"
end

-- A flush_draw case: the whole screen in the loud colour with a bar that moves every frame,
-- pushed to the panel at once. If this app ran while the approval was open, a screenshot would
-- show it.
local function paint_loud()
  frame = frame + 1
  gfx.clear(loud)
  gfx.fill_rect((frame * 7) % gfx.width(), 0, 12, gfx.height(), gfx.BLACK)
  gfx.text("checktest frame " .. frame, 8, 8, gfx.BLACK, 2)
  gfx.flush()
end

function on_update(dt)
  local now = badge.millis()
  if now - last_tick >= cfg.tick_ms then
    last_tick = now
    ticks = ticks + 1
    badge.log("CT tick " .. ticks)
  end

  if stage == "wait" then
    if now - started >= (case.delay_ms or 0) then stage = act() end
  elseif stage == "poll" then
    stage = poll()
  end

  if case and case.flush_draw then paint_loud() end
end

function on_draw()
  if case and case.flush_draw then
    paint_loud()
    return
  end
  local cx = gfx.width() // 2
  gfx.clear()
  gfx.text_center("Check test", cx, 50, gfx.WHITE, 2)
  gfx.text_center(status, cx, 100, gfx.SOLANA_GREEN, 1)
  gfx.text_center("tick " .. ticks, cx, 124, gfx.MUTED, 1)
  gfx.text_center("CANCEL to quit", cx, 205, gfx.MUTED, 1)
end

function on_button(key, pressed)
  if pressed and key == "b" then badge.system.exit() end
end
