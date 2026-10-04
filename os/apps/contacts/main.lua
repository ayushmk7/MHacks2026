-- Contacts (docs/os/apps/apps.md, "Contacts")
--
--   List   wallet.contacts(), newest first: name, short address, the date it was added. The first
--          row is "Swap contacts"; with no contact yet its subline says what to do. RIGHT on a
--          contact asks on a confirm line; SELECT then removes, CANCEL keeps, other keys do nothing.
--   Swap   broadcasts wallet.contact_hello() once a second and listens.
--            a CONTACT_HELLO (vk.hello_parse): "Swap with <name>?"; SELECT sends
--              wallet.contact_card(hello) unicast to that badge.
--            The badges heard are a vk.peers list: a new badge never waits for room (the one
--            heard longest ago leaves, except the selected one and one a card was just sent to);
--            a HELLO from a listed MAC under another key is ignored; and once a card is sent,
--            that badge's HELLO frame is held for config.card_hold_ms, so a HELLO sent as its
--            MAC cannot change what the card answered.
--            a CONTACT_CARD: wallet.contact_accept(card); "Saved <name>" and a green flash, or the
--              reason in words.
--          Both badges do both halves, so each ends up with the other's card.
--
-- A name here is what its owner chose to call themselves, so it is labelled "self-named".
-- CANCEL goes back one step and exits from the list.
--
-- Log lines (each "[app] CON ..."), for test/device/t_app_contacts.py:
--   CON list <n>               the list was read: n contacts
--   CON removed <short>        a contact was removed
--   CON swap on / CON swap off
--   CON hello <short>          a badge in swap mode was heard for the first time
--   CON card sent <short>      a card went to that badge
--   CON saved <name>           a card was accepted
--   CON accept failed <reason>
--
-- The look and the frame helpers come from lib/vk.lua (pushed into this folder as vk.lua by
-- scripts/push-apps.sh). Every tunable and every word is in config.lua.

local cfg = require("config")
local vk = require("vk")
local ui, wallet, espnow = vk.ui, badge.wallet, badge.espnow
local text = cfg.text

local T_CONTACT_CARD = vk.T_CONTACT_CARD  -- 17 (docs/os/protocol/espnow.md)
local DOT = " \xC2\xB7 "                  -- the kit draws the middle dot
local ARROW = "\xE2\x96\xB8"              -- and the small triangle

local mode = "list"                       -- list | confirm | swap
local contacts = {}                       -- newest first
local rows = {}                           -- the list screen's rows; row 1 is "Swap contacts"
local sel = 1

local peers = vk.peers.new{max = cfg.max_peers, timeout_ms = cfg.peer_timeout_ms, hold_ms = cfg.card_hold_ms}
local chosen = nil                        -- swap: the MAC of the selected badge
local next_hello = 0                      -- badge.millis() of the next broadcast
local message, message_tone, message_until = nil, nil, 0

-- "Oct 3" for unix seconds; the configured blank when the clock had no source.
local function date_text(added)
  if type(added) ~= "number" or added <= 0 then return text.no_date end
  local ok, t = pcall(os.date, "!*t", vk.local_time(math.floor(added)))
  if not ok or type(t) ~= "table" then return text.no_date end
  return (cfg.months[t.month] or "?") .. " " .. t.day
end

local function build_rows()
  rows = {{l = text.swap_row, sub = #contacts == 0 and text.empty or text.swap_sub, r = ARROW}}
  for i = 1, #contacts do
    local c = contacts[i]
    local removing = mode == "confirm" and sel == i + 1
    rows[#rows + 1] = {
      l = c.name,
      sub = removing and text.confirm_sub or (vk.short(c.address) .. DOT .. text.self_named),
      r = date_text(c.added),
    }
  end
end

local function load_contacts()
  local list = wallet.contacts() or {}
  contacts = {}
  for i = #list, 1, -1 do contacts[#contacts + 1] = list[i] end      -- stored oldest first
  if sel > #contacts + 1 then sel = #contacts + 1 end
  build_rows()
  badge.log("CON list " .. #contacts)
end

local function set_mode(new)
  mode = new
  build_rows()
end

local function say(line, tone)
  message, message_tone = line, tone
  message_until = badge.millis() + cfg.message_ms
end

local function reason_text(reason)
  return vk.reason_text(reason, "contact")
end

-- The selected badge, and its place in the list (strongest signal first).
local function selected()
  local items = peers.items
  for i = 1, #items do
    if items[i].mac == chosen then return items[i], i end
  end
  chosen = items[1] and items[1].mac or nil
  return items[1], items[1] and 1 or nil
end

-- ---------------------------------------------------------------------------------------------
-- Swap
-- ---------------------------------------------------------------------------------------------

local function enter_swap()
  if not espnow.enabled() then espnow.enable(true) end
  peers.items, chosen = {}, nil
  message = nil
  next_hello = 0
  set_mode("swap")
  badge.log("CON swap on")
end

local function leave_swap()
  peers.items, chosen = {}, nil
  badge.log("CON swap off")
  mode = "list"
  load_contacts()
end

local function swap_update(now)
  if now >= next_hello then
    next_hello = now + cfg.hello_ms
    local frame, why = wallet.contact_hello()
    if frame then
      espnow.broadcast(frame)
    else
      say(reason_text(why), "bad")
    end
  end
  peers:expire(now)
  selected()
  if message and now >= message_until then message = nil end
end

-- A HELLO from a badge in swap mode: remember its newest frame (it carries the nonce a card for
-- it must be signed over).
local function heard_hello(mac, frame, hello, rssi)
  local info = {address = hello.address, name = hello.name, frame = frame, rssi = rssi}
  local _, how = peers:heard(mac, info, badge.millis(), chosen)
  if how == "new" then badge.log("CON hello " .. vk.short(hello.address)) end
  selected()                                             -- the cursor stays on the same badge
end

local function heard_card(frame)
  local contact, why = wallet.contact_accept(frame)
  if contact then
    badge.log("CON saved " .. tostring(contact.name))
    say(string.format(text.saved, tostring(contact.name)), "ok")
    vk.led_pulse(cfg.saved_led.color, cfg.saved_led.ms)
    next_hello = 0            -- the accept spent this badge's nonce: the next HELLO draws a new one
  else
    badge.log("CON accept failed " .. tostring(why))
    say(reason_text(why), "bad")
  end
end

local function send_card()
  local peer = selected()
  if not peer then return end
  local card, why = wallet.contact_card(peer.frame)
  if not card then return say(reason_text(why), "bad") end
  if espnow.send(peer.mac, card) then
    peers:sent(peer.mac, badge.millis())
    badge.log("CON card sent " .. vk.short(peer.address))
    say(string.format(text.sent_to, peer.name), "ok")
  else
    say(text.send_failed, "bad")
  end
end

local function draw_swap()
  ui.page()
  ui.header(cfg.header)
  ui.title(text.swap_title, ui.TITLE_Y)
  local items = peers.items
  local current = selected()
  if #items == 0 then
    ui.text_center(text.looking, ui.W // 2, 96, ui.color("ink"))
    ui.text_center(text.looking_sub, ui.W // 2, 114, ui.color("sub"))
  else
    ui.text_center(string.format(text.ask, current.name), ui.W // 2, 52, ui.color("ink"))
    for i = 1, #items do
      local peer = items[i]
      local y = 72 + (i - 1) * ui.SUB_PITCH
      local on = peer == current
      ui.row(y, peer.name:upper(), peer.sent and text.sent or ARROW, on)
      ui.subline(y + 13, vk.short(peer.address) .. DOT .. text.self_named, on)
    end
  end
  if message then
    ui.text_center(message, ui.W // 2, 200, ui.color(message_tone == "ok" and "stamp_ok" or "stamp_bad"))
  end
  ui.footer(#items > 0 and text.hint_send or "", text.back)
end

-- ---------------------------------------------------------------------------------------------
-- Callbacks
-- ---------------------------------------------------------------------------------------------

function on_start()
  load_contacts()
end

function on_update(dt)
  if mode == "swap" then swap_update(badge.millis()) end
end

function on_espnow(mac, data, rssi)
  if mode ~= "swap" then return end
  local hello = vk.hello_parse(data)
  if hello then return heard_hello(mac, data, hello, rssi) end
  if vk.frame_type(data) == T_CONTACT_CARD then heard_card(data) end
end

function on_draw()
  if mode == "swap" then return draw_swap() end
  local confirm = mode == "confirm"
  ui.list{
    header = cfg.header,
    title = text.title,
    rows = rows,
    sel = sel,
    hint = confirm and text.hint_confirm or (sel == 1 and text.hint_swap or text.hint_remove),
    back = confirm and text.keep or text.exit,
  }
end

function on_button(key, pressed)
  if not pressed then return end
  if mode == "swap" then
    local _, index = selected()
    if key == "b" then leave_swap()
    elseif key == "a" then send_card()
    elseif key == "up" and index and index > 1 then chosen = peers.items[index - 1].mac
    elseif key == "down" and index and index < #peers.items then chosen = peers.items[index + 1].mac
    end
  elseif mode == "confirm" then
    local contact = contacts[sel - 1]
    if key == "a" and contact then
      if wallet.contact_remove(contact.address) then
        badge.log("CON removed " .. vk.short(contact.address))
      end
      mode = "list"
      load_contacts()
    elseif key == "b" then
      set_mode("list")                    -- CANCEL keeps the contact; other keys do nothing
    end
  else
    if key == "b" then badge.system.exit()
    elseif key == "up" and sel > 1 then sel = sel - 1
    elseif key == "down" and sel <= #contacts then sel = sel + 1
    elseif key == "a" and sel == 1 then enter_swap()
    elseif key == "right" and sel > 1 then set_mode("confirm")
    end
  end
end

-- Draw only when the screen changed (lib/vk.lua, "ui.frame"): a frame every pass would hold the
-- loop at 20 passes a second.
local draw_frame = on_draw
function on_draw() ui.frame(draw_frame) end
