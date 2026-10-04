-- Request: asks to be paid, waits, and checks the payment on chain (docs/os/apps/apps.md, "Request").
--
--   amount      UP/DOWN change the amount by config.step minor units, LEFT/RIGHT by ten steps.
--               SELECT opens the request. The amount is an integer count of minor units and is
--               turned into text ("10.00") by amount_text(); it is never a float.
--   waiting     wallet.request_open: the firmware signs and broadcasts the request and answers
--               presence checks. The screen shows the seconds left and how many badges are checking.
--               The screen is kept awake while a request is open.
--   checking    a RESULT frame for this request said "paid". A RESULT is not authenticated, so
--               vk.receive fetches the transaction it names and the firmware checks that it pays
--               this request (wallet.verify_payment); up to three reported transactions are
--               checked in turn, so a forged RESULT cannot hide the real one.
--   paid        only after that check passed: the label reads PAID, the LEDs go green, and the
--               payment is written to the history (wallet.record_received).
--
-- A RESULT that says cancelled or failed, or a transaction that is not this payment, puts a line
-- on the waiting screen and the request stays open: anyone in range can send such a frame, so it
-- must not be able to end a request.
--
-- CANCEL: on the amount screen it exits. While waiting it closes the request and goes back to the
-- amount. While checking it first asks (a real payment may be being checked): a second CANCEL
-- closes anyway, SELECT keeps checking.
--
-- Log lines (test/device/t_app_request.py, t_pay_2.py), each "[app] REQ ...":
--   amount <text>       the amount shown, on start and after every change
--   open <req_id>       a request was opened
--   err <reason>        request_open refused
--   result <status>     a RESULT frame for the open request: 0 paid, 1 rejected, 2 failed
--   confirm <state>     what the check says: pending, or failed <reason>
--   paid <signature>    the payment was verified (base58)
--   payer <address>     who paid, from the verified transaction (base58)
--   recorded / record failed <why>     the history row
--   expired             the request ran out
--   closed              CANCEL closed the request

local vk = require("vk")
local config = require("config")
local ui, wallet = vk.ui, badge.wallet

local LABELS = {amount = "PAY ME", waiting = "WAITING", confirming = "CHECKING", paid = "PAID"}

local view = "amount"      -- "amount", "waiting", "confirming" or "paid"
local asking = false       -- CANCEL was pressed while checking: the question is on screen
local symbol, decimals, unit = "", 2, 100
local minor = 0            -- the amount, in minor units
local want_open = false    -- SELECT was pressed: open from on_update (one signature)
local request = nil        -- {id =, expiry =, amount =}
local watch = nil          -- vk.receive for the open request
local watch_state = nil    -- the state last logged
local proofs = 0
local note = nil           -- a line under the rows
local note_bad = false
local status_at = 0
local shown = false        -- the current screen has been drawn at least once

local function amount_text(n)
  if decimals <= 0 then return string.format("%d", n) end
  return string.format("%d.%0" .. decimals .. "d", n // unit, n % unit)
end

local function set_note(text, bad)
  note, note_bad = text, bad or false
end

local function set_view(name)
  view, shown = name, false
  ui.dirty()
end

local function change(delta)
  local next_minor
  if delta > 0 then
    next_minor = minor > config.max - delta and config.max or minor + delta
  else
    next_minor = minor < config.min - delta and config.min or minor + delta
  end
  if next_minor == minor then return end
  minor = next_minor
  set_note(nil)
  badge.log("REQ amount " .. amount_text(minor))
end

local function open()
  if not badge.espnow.enabled() then badge.espnow.enable(true) end
  local text = amount_text(minor)
  local opened, why = wallet.request_open{
    amount = text, symbol = config.symbol, name = config.name, ttl_s = config.ttl_s,
  }
  if not opened then
    why = tostring(why)
    set_note(vk.reason_text(why, "request"), true)
    badge.log("REQ err " .. why)
    return
  end
  request = {id = opened.req_id, expiry = opened.expiry, amount = text}
  watch = vk.receive.start{
    req_id = opened.req_id, amount = text, symbol = config.symbol,
    every_ms = config.confirm_every_ms, for_ms = config.confirm_for_ms,
  }
  watch_state = nil
  proofs = 0
  set_note("waiting for payment")
  status_at = badge.millis()
  set_view("waiting")
  vk.keep_awake(true)
  badge.log("REQ open " .. request.id)
end

-- Back to the amount screen. The firmware keeps nothing of a closed request that this app needs.
local function to_amount(text, bad)
  request, watch, asking = nil, nil, false
  set_view("amount")
  set_note(text, bad)
  vk.keep_awake(false)
end

local function close_request()
  wallet.request_close(request.id)
  badge.log("REQ closed")
  to_amount(nil)
end

local function seconds_left()
  local now = wallet.time()
  if not now or not request or not request.expiry then return nil end
  return math.max(0, math.floor(request.expiry - now))
end

local function read_status(now)
  status_at = now
  local status = wallet.request_status(request.id)
  if not status then return end
  proofs = status.proofs or proofs
  if status.state ~= "open" and view == "waiting" then
    badge.log("REQ expired")
    to_amount("The request expired unpaid", true)
  end
end

local function paid()
  wallet.request_close(request.id)             -- paid: stop asking
  asking = false
  set_view("paid")
  set_note("payment verified on chain")
  vk.led_pulse("green", config.led_ms)
  vk.keep_awake(false)
  badge.log("REQ paid " .. tostring(watch.sig))
  badge.log("REQ payer " .. tostring(watch.payer))
  if watch.recorded then
    badge.log("REQ recorded")
  else
    badge.log("REQ record failed " .. tostring(watch.record_error))
  end
end

-- One step of the check (at most one blocking call), and what it means for the screen.
local function check_step()
  local state, detail = watch:update()
  local logged = state == "failed" and ("failed " .. tostring(detail)) or state
  if logged ~= watch_state then
    watch_state = logged
    if state ~= "paid" and state ~= "waiting" then badge.log("REQ confirm " .. logged) end
  end
  if state == "paid" then
    paid()
  elseif state == "pending" and view == "waiting" then
    set_view("confirming")
    set_note("checking the payment on chain")
  elseif state == "failed" and view == "confirming" then
    asking = false
    set_view("waiting")
    set_note(vk.reason_text(detail, "receive"), true)
  end
end

function on_start()
  for _, token in ipairs(wallet.tokens() or {}) do
    if not config.symbol or token.symbol == config.symbol then
      symbol, decimals = token.symbol, token.decimals
      break
    end
  end
  if symbol == "" then symbol, decimals = config.symbol or "", config.decimals end
  decimals = math.tointeger(decimals) or 0     -- an integer: it goes into a format string
  unit = 1
  for _ = 1, decimals do unit = unit * 10 end
  minor = math.max(config.min, math.min(config.max, config.start))
  badge.log("REQ amount " .. amount_text(minor))
end

function on_update(dt)
  if want_open then
    want_open = false
    if view == "amount" then open() end
    return
  end
  if not request then return end
  local now = badge.millis()
  if view == "confirming" then
    -- Not before the CHECKING screen has been drawn: a look-up blocks for up to vk.timeout_ms.
    if shown then check_step() end
  elseif view == "waiting" then
    if watch:pending() > 0 then
      check_step()
    elseif now - status_at >= config.status_ms then
      read_status(now)
    end
  end
end

-- The payer's RESULT (protocol/espnow.md). Unauthenticated: status 0 only queues the check.
function on_espnow(mac, data, rssi)
  if not request or not watch or (view ~= "waiting" and view ~= "confirming") then return end
  local result = vk.result_parse(data)
  if not result or result.req_id ~= request.id then return end
  badge.log("REQ result " .. result.status)
  if watch:result(result) then
    if view == "waiting" then
      set_view("confirming")
      set_note("checking the payment on chain")
    end
  elseif view == "waiting" and result.status == vk.RESULT_REJECTED then
    set_note("The payer says they cancelled. Still waiting.", true)
  elseif view == "waiting" and result.status == vk.RESULT_FAILED then
    set_note("The payer says the payment failed. Still waiting.", true)
  end
  ui.dirty()
end

function on_button(key, pressed)
  if not pressed then return end
  if view == "amount" then
    if key == "b" then
      badge.system.exit()
    elseif key == "a" then
      want_open = true
    elseif key == "up" then
      change(config.step)
    elseif key == "down" then
      change(-config.step)
    elseif key == "right" then
      change(10 * config.step)
    elseif key == "left" then
      change(-10 * config.step)
    end
  elseif view == "paid" then
    if key == "a" or key == "b" then to_amount(nil) end
  elseif view == "confirming" then
    if key == "b" then
      if asking then close_request() else asking = true end   -- a real payment may be being checked
    elseif key == "a" and asking then
      asking = false
    end
  elseif key == "b" then                       -- waiting: CANCEL closes the request
    close_request()
  elseif key == "a" and watch and watch:retryable() > 0 and watch:again() > 0 then
    set_view("confirming")                     -- the last check failed for want of network or time
    set_note("checking the payment on chain")
  end
end

local function wrap(text, width)
  local lines, line = {}, ""
  for word in tostring(text):gmatch("%S+") do
    if line == "" then
      line = word
    elseif #line + 1 + #word <= width then
      line = line .. " " .. word
    else
      lines[#lines + 1] = line
      line = word
    end
  end
  if line ~= "" then lines[#lines + 1] = line end
  return lines
end

-- The three rows of the body, for the current screen.
local function body_rows()
  if view == "amount" then
    return {
      {"UP / DOWN", amount_text(config.step)},
      {"LEFT / RIGHT", amount_text(10 * config.step)},
      {"SELECT", "open"},
    }
  end
  if view == "waiting" then
    local left = seconds_left()
    return {
      {"EXPIRES IN", left and (left .. " s") or "--"},
      {"BADGES CHECKING", tostring(proofs)},
      {"STATUS", "on air"},
    }
  end
  if view == "confirming" then
    return {{"REPORTED", "paid"}, {"REF", vk.short(watch:checking() or "")}, {"VERIFIED", "not yet"}}
  end
  return {
    {"VERIFIED", "on chain", ui.color("stamp_ok")},
    {"FROM", vk.short(watch.payer)},
    {"HISTORY", watch.recorded and "saved" or "not saved"},
  }
end

local function footer()
  if view == "amount" then return "SELECT open", "CANCEL back" end
  if view == "paid" then return "SELECT new request", "CANCEL back" end
  if view == "confirming" then
    if asking then return "SELECT keep checking", "CANCEL close anyway" end
    return "", "CANCEL close"
  end
  if watch and watch:retryable() > 0 then return "SELECT check again", "CANCEL close" end
  return "", "CANCEL close"
end

function on_draw()
  shown = true
  local x0, x1 = ui.BODY_X0, ui.BODY_X1
  ui.page()
  ui.header(config.header)
  ui.perforation(ui.SPLIT_X, ui.CONTENT_Y + 4, 208)

  -- Left stub: the amount under its label, then the badge's barcode.
  local text = request and request.amount or amount_text(minor)
  if view == "paid" then
    ui.amount(ui.STUB_CX, 40, "", text, symbol)      -- the kit's label is always ink
    ui.text_center(LABELS.paid, ui.STUB_CX, 40, ui.color("stamp_ok"))
  else
    ui.amount(ui.STUB_CX, 40, LABELS[view], text, symbol)
  end
  ui.barcode(20, 128, 106, 44)

  -- Body: the title, three rows, a rule, one note (or the question while checking).
  ui.title("REQUEST", 30, ui.BODY_CX)
  local rows = body_rows()
  for i = 1, #rows do
    ui.row(56 + (i - 1) * ui.ROW_PITCH, rows[i][1], rows[i][2], false, rows[i][3], x0, x1)
  end
  local rule_y = 56 + #rows * ui.ROW_PITCH
  ui.rule(rule_y, x0, x1)
  local line, bad = note, note_bad
  if asking then
    line, bad = "A payment is being checked. Close the request anyway? You may lose track of it.", true
  end
  if line then
    local color = ui.color(bad and "stamp_bad" or "sub")
    local lines = wrap(line, (x1 - x0) // 6)
    for i = 1, math.min(#lines, 6) do
      ui.text_center(lines[i], ui.BODY_CX, rule_y + 10 + (i - 1) * 13, color)
    end
  end
  ui.footer(footer())
end

-- Draw only when the screen changed (lib/vk.lua, "ui.frame"): a frame every pass would hold the
-- loop at 20 passes a second.
local draw_frame = on_draw
function on_draw() ui.frame(draw_frame) end
