-- Request: asks to be paid, waits, and confirms the payment on chain (docs/os/apps/apps.md, "Request").
--
--   amount      UP/DOWN change the amount by config.step minor units, LEFT/RIGHT by ten steps.
--               SELECT opens the request. The amount is an integer count of minor units and is
--               turned into text ("10.00") by amount_text(); it is never a float.
--   waiting     wallet.request_open: the firmware signs and broadcasts the request and answers
--               presence checks. The screen shows the seconds left and how many badges are checking.
--   confirming  a RESULT frame for this request said "paid". A RESULT is not authenticated, so the
--               transaction it names is looked up on chain (vk.confirm) every
--               config.confirm_every_ms for up to config.confirm_for_ms.
--   paid        only after the chain said "confirmed": the label reads PAID and the LEDs go green.
--
-- A RESULT that says cancelled or failed, or a payment that does not confirm, puts a line on the
-- waiting screen and the request stays open: anyone in range can send such a frame, so it must not
-- be able to end a request.
--
-- CANCEL: on the amount screen it exits; on every other screen it closes the request and goes back
-- to the amount.
--
-- Log lines (test/device/t_app_request.py, t_pay_2.py), each "[app] REQ ...":
--   amount <text>       the amount shown, on start and after every change
--   open <req_id>       a request was opened
--   err <reason>        request_open refused
--   result <status>     a RESULT frame for the open request: 0 paid, 1 rejected, 2 failed
--   confirm <status>    what the chain said: confirmed, pending, failed, or the network error
--   paid <signature>    confirmed on chain (base58)
--   expired             the request ran out
--   closed              CANCEL closed the request

local vk = require("vk")
local config = require("config")
local ui, wallet = vk.ui, badge.wallet

local REASONS = {          -- why request_open refused, in words (reference/reasons.md)
  not_provisioned = "This badge is not set up",
  no_time = "The clock is not set (no_time). Join Wi-Fi to set it",
  busy = "Two requests are already open",
  bad_arg = "This amount cannot be requested",
  sign_failed = "The key did not sign",
}

local LABELS = {amount = "PAY ME", waiting = "WAITING", confirming = "RECEIVED", paid = "PAID"}

local view = "amount"      -- "amount", "waiting", "confirming" or "paid"
local symbol, decimals, unit = "", 2, 100
local minor = 0            -- the amount, in minor units
local want_open = false    -- SELECT was pressed: open from on_update (one signature)
local request = nil        -- {id =, expiry =, amount =}
local proofs = 0
local note = nil           -- a line under the rows
local note_bad = false
local status_at = 0
local ref = nil            -- the 64-byte transaction signature a RESULT named
local confirm_at, confirm_until = 0, 0
local shown = false        -- the current screen has been drawn at least once

local function amount_text(n)
  if decimals <= 0 then return string.format("%d", n) end
  return string.format("%d.%0" .. decimals .. "d", n // unit, n % unit)
end

local function set_note(text, bad)
  note, note_bad = text, bad or false
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

-- The LEDs in a theme colour: badge.theme gives RGB565, the LEDs take 0..255 each.
local function pulse(token)
  local c = math.floor(ui.color(token))
  local r, g, b = c // 2048, (c // 32) % 64, c % 32
  badge.led.pulse(r * 255 // 31, g * 255 // 63, b * 255 // 31, config.led_ms)
end

local function open()
  if not badge.espnow.enabled() then badge.espnow.enable(true) end
  local text = amount_text(minor)
  local opened, why = wallet.request_open{
    amount = text, symbol = config.symbol, name = config.name, ttl_s = config.ttl_s,
  }
  if not opened then
    why = tostring(why)
    set_note(REASONS[why] or why, true)
    badge.log("REQ err " .. why)
    return
  end
  request = {id = opened.req_id, expiry = opened.expiry, amount = text}
  proofs, ref = 0, nil
  set_note("waiting for payment")
  status_at = badge.millis()
  view = "waiting"
  badge.log("REQ open " .. request.id)
end

-- Back to the amount screen. The firmware keeps nothing of a closed request that this app needs.
local function to_amount(text, bad)
  request, ref = nil, nil
  view = "amount"
  set_note(text, bad)
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

local function confirm_step()
  local status, why = vk.confirm(ref)          -- one blocking request
  badge.log("REQ confirm " .. tostring(status or why))
  local now = badge.millis()
  if status == "confirmed" then
    wallet.request_close(request.id)           -- paid: stop asking
    view = "paid"
    set_note("payment confirmed on chain")
    pulse("green")
    badge.log("REQ paid " .. tostring(badge.codec.b58enc(ref)))
  elseif status == "failed" then
    view = "waiting"
    set_note("Payment failed", true)
  elseif now >= confirm_until then
    view = "waiting"
    set_note("Payment not confirmed", true)
  else
    confirm_at = now + config.confirm_every_ms
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
    -- Not before the RECEIVED screen has been drawn: the lookup blocks for up to vk.timeout_ms.
    if shown and now >= confirm_at then confirm_step() end
  elseif view == "waiting" and now - status_at >= config.status_ms then
    read_status(now)
  end
end

-- The payer's RESULT (protocol/espnow.md). Unauthenticated: status 0 only starts the lookup.
function on_espnow(mac, data, rssi)
  if view ~= "waiting" or not request then return end
  local result = vk.result_parse(data)
  if not result or result.req_id ~= request.id then return end
  badge.log("REQ result " .. result.status)
  if result.status == vk.RESULT_OK then
    ref = result.ref
    view = "confirming"
    shown = false
    set_note("confirming on chain")
    confirm_at = badge.millis()
    confirm_until = confirm_at + config.confirm_for_ms
  elseif result.status == vk.RESULT_REJECTED then
    set_note("Payer cancelled", true)
  else
    set_note("Payment failed", true)
  end
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
  elseif key == "b" then                       -- waiting or confirming: CANCEL closes the request
    wallet.request_close(request.id)
    badge.log("REQ closed")
    to_amount(nil)
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
  local short = vk.short(badge.codec.b58enc(ref) or "")
  if view == "confirming" then
    return {{"REPORTED", "paid"}, {"REF", short}, {"CONFIRMED", "not yet"}}
  end
  return {{"CONFIRMED", "on chain", ui.color("stamp_ok")}, {"REF", short}, {"STATUS", "closed"}}
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

  -- Body: the title, three rows, a rule, one note.
  ui.title("REQUEST", 30, ui.BODY_CX)
  local rows = body_rows()
  for i = 1, #rows do
    ui.row(56 + (i - 1) * ui.ROW_PITCH, rows[i][1], rows[i][2], false, rows[i][3], x0, x1)
  end
  local rule_y = 56 + #rows * ui.ROW_PITCH
  ui.rule(rule_y, x0, x1)
  if note then
    local color = ui.color(note_bad and "stamp_bad" or "sub")
    local lines = wrap(note, (x1 - x0) // 6)
    for i = 1, math.min(#lines, 4) do
      ui.text_center(lines[i], ui.BODY_CX, rule_y + 10 + (i - 1) * 13, color)
    end
  end

  if view == "amount" then
    ui.footer("SELECT open", "CANCEL back")
  elseif view == "paid" then
    ui.footer("SELECT new request", "CANCEL back")
  else
    ui.footer("", "CANCEL close")
  end
end

-- Draw only when the screen changed (lib/vk.lua, "ui.frame"): a frame every pass would hold the
-- loop at 20 passes a second.
local draw_frame = on_draw
function on_draw() ui.frame(draw_frame) end
