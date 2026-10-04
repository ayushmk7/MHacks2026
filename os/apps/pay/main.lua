-- Pay: lists the payment requests heard from badges nearby and pays one (docs/os/apps/apps.md, "Pay").
--
--   list     wallet.requests(), strongest signal first, read again every config.refresh_ms.
--            The name in a row is what the sender calls itself: a claim. The verified name is
--            shown only by the firmware's approval.
--   paying   SELECT starts vk.pay.start{request = entry}; the screen says in words what the flow
--            is doing. The approval itself is the firmware's screen, never this app's.
--   result   PAID with the short signature and green LEDs, or the reason in words
--            (vk.reason_text, the one table every app shares). A refusal by the firmware's checks
--            is reported to the dashboard feed (vk.report). With no network the text says what to
--            do (join Wi-Fi in Settings), and the list says so before a payment is tried.
--
-- A request payment carries the request's id as its memo: vk.pay passes req_id to
-- wallet.build_transfer, and the payee's badge looks for it before it shows PAID.
--
-- CANCEL: on the list it exits. While paying, before anything is signed, it abandons the payment
-- and goes back to the list; once the approval has opened the payment is finished in the
-- background (the payee must still be told how it ended) and its result is shown when it ends.
--
-- Log lines (test/device/t_app_pay.py, t_pay_2.py), each "[app] PAY ...":
--   list <n>            the number of requests listed, whenever it changes
--   start <req_id>      a payment was started
--   state <state>       the flow moved to another state
--   done <signature>    paid and confirmed (base58)
--   failed <reason>     not paid
--   abandoned           CANCEL before the approval

local vk = require("vk")
local config = require("config")
local ui, wallet = vk.ui, badge.wallet

local WORDS = {            -- the flow's states, in words
  presence = "checking presence",
  record = "fetching record",
  blockhash = "building",
  approve = "approve on the firmware screen",
  submit = "sending",
  confirm = "confirming",
}

-- States in which nothing has been signed or shown for approval yet.
local EARLY = {presence = true, record = true, blockhash = true}

local view = "list"        -- "list", "paying" or "result"
local requests = {}        -- wallet.requests(), sorted
local selected_id = nil    -- req_id of the selected row: the cursor follows the request, not the slot
local heard_at = {}        -- req_id -> "14:20", wall-clock time the request was first heard (clock synced only)
local listed = nil         -- the count last logged
local refreshed_at = nil
local flow = nil           -- the running payment
local target = nil         -- the request being paid, or last paid
local last_state = nil
local result = nil         -- {paid =, signature =, reason =}
local report = nil         -- a refusal still to be sent to the feed
local offline = false      -- the list's last look: Wi-Fi is not joined

local function selected_index()
  for i = 1, #requests do
    if requests[i].req_id == selected_id then return i end
  end
  return nil
end

local function refresh()
  local heard = wallet.requests() or {}
  table.sort(heard, function(a, b)
    if a.rssi ~= b.rssi then return a.rssi > b.rssi end
    return a.req_id < b.req_id
  end)
  requests = heard
  if wallet.time_ok() then
    for i = 1, #heard do
      local id = heard[i].req_id
      if not heard_at[id] then heard_at[id] = os.date("!%H:%M", math.floor(vk.local_time(wallet.time()))) end
    end
  end
  if #heard ~= listed then
    listed = #heard
    badge.log("PAY list " .. listed)
  end
  if not selected_index() then selected_id = heard[1] and heard[1].req_id or nil end
end

-- "10.00 HACK": the amount is a string from the firmware and is never turned into a number.
local function money(entry)
  return tostring(entry.amount) .. " " .. tostring(entry.currency or "")
end

local function bars(rssi)
  local levels = config.signal_dbm
  local n = 1
  for i = 1, #levels do
    if rssi >= levels[i] then n = #levels + 2 - i break end
  end
  return string.rep("|", n) .. string.rep(".", #levels + 1 - n)
end

local function start(entry)
  target = entry
  result = nil
  last_state = nil
  flow = vk.pay.start{request = entry}
  view = "paying"
  badge.log("PAY start " .. tostring(entry.req_id))
end

local function finish(state, detail)
  local paid = state == "done"
  detail = tostring(detail or "failed")
  local shown = flow:reason()                -- registry_missing is not reported as an impostor
  result = {paid = paid, signature = flow.signature, reason = not paid and shown or nil}
  if paid then
    vk.led_pulse("green", config.led_ms)
    badge.log("PAY done " .. detail)
  else
    if shown == detail then
      -- Sent from the next update, so that the result is on screen before the request blocks.
      -- vk.report forwards only the firmware blocks the feed shows.
      report = {payee = target.payee, reason = detail, req = target.req}
    end
    badge.log("PAY failed " .. detail)
  end
  flow = nil
  view = "result"
end

function on_start()
  if not badge.espnow.enabled() then badge.espnow.enable(true) end
  refresh()
  refreshed_at = badge.millis()
end

function on_update(dt)
  if report then
    local event = report
    report = nil
    vk.report(event)                     -- best effort: a failure changes nothing here
  end
  if flow then
    local state, detail = flow:update()  -- at most one blocking step
    if state ~= last_state then
      last_state = state
      badge.log("PAY state " .. tostring(state))
    end
    if state == "done" or state == "failed" then finish(state, detail) end
  end
  local now = badge.millis()
  if view == "list" and now - refreshed_at >= config.refresh_ms then
    refreshed_at = now
    refresh()
    local wifi = badge.wifi
    offline = wifi ~= nil and not wifi.connected()
  end
end

function on_button(key, pressed)
  if not pressed then return end
  if view == "list" then
    local index = selected_index()
    if key == "b" then
      badge.system.exit()
    elseif key == "up" and index and index > 1 then
      selected_id = requests[index - 1].req_id
    elseif key == "down" and index and index < #requests then
      selected_id = requests[index + 1].req_id
    elseif key == "a" and index and not flow then
      start(requests[index])
    end
  elseif view == "paying" then
    if key == "b" then
      if flow and EARLY[flow.state] then
        flow = nil
        badge.log("PAY abandoned")
      end
      view = "list"
    end
  elseif key == "a" or key == "b" then   -- result
    result = nil
    view = "list"
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

local function draw_list()
  local rows = {}
  for i = 1, #requests do
    local r = requests[i]
    rows[i] = {
      l = r.name,
      r = money(r),
      sub = (heard_at[r.req_id] and heard_at[r.req_id] .. " \xC2\xB7 " or "") .. vk.short(r.payee) .. " \xC2\xB7 name is a claim \xC2\xB7 signal " .. bars(r.rssi),
    }
  end
  local hint = #rows > 0 and "SELECT pay" or ""
  if offline then hint = config.offline_hint end      -- nothing can be paid: say what to do
  if flow then hint = WORDS[flow.state] or "" end     -- a payment finishing in the background
  ui.list{
    header = config.header, title = "PAY", rows = rows, sel = selected_index(),
    empty = config.empty, hint = hint, back = "CANCEL back",
  }
end

-- One column, no coloured band and no amount stub: nothing here looks like the firmware approval.
local function draw_payment()
  ui.page()
  ui.header(config.header)
  ui.title("PAY", ui.TITLE_Y)
  local y = ui.LIST_Y
  ui.row(y, "CLAIMED NAME", target.name)
  ui.row(y + ui.ROW_PITCH, "KEY", vk.short(target.payee))
  ui.row(y + 2 * ui.ROW_PITCH, "AMOUNT", money(target))
  local signature = result and result.signature or flow and flow.signature
  ui.row(y + 3 * ui.ROW_PITCH, "SIGNATURE", signature and vk.short(signature) or "--")
  ui.rule(y + 4 * ui.ROW_PITCH)

  local cx, text_y = ui.W // 2, y + 4 * ui.ROW_PITCH + 14
  if view == "paying" then
    ui.text_center(WORDS[flow and flow.state] or "", cx, text_y, ui.color("ink"))
    ui.footer("", "CANCEL back")
    return
  end
  if result.paid then
    ui.text_center("PAID", cx, text_y, ui.color("stamp_ok"))
    ui.text_center("Paid " .. money(target), cx, text_y + 16, ui.color("sub"))
  else
    ui.text_center("NOT PAID", cx, text_y, ui.color("stamp_bad"))
    local lines = wrap(vk.reason_text(result.reason), 48)
    for i = 1, math.min(#lines, 3) do
      ui.text_center(lines[i], cx, text_y + 3 + i * 13, ui.color("sub"))
    end
  end
  ui.footer("SELECT ok", "CANCEL back")
end

function on_draw()
  if view == "list" or not target then
    draw_list()
  else
    draw_payment()
  end
end

-- Draw only when the screen changed (lib/vk.lua, "ui.frame"): a frame every pass would hold the
-- loop at 20 passes a second.
local draw_frame = on_draw
function on_draw() ui.frame(draw_frame) end
