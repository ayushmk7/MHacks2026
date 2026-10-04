-- Contacts (docs/os/apps/apps.md, "Contacts")
--
--   List   wallet.contacts(), newest first: name, short address, the date it was added. The first
--          row is "Swap contacts". RIGHT on a contact asks on a confirm line; SELECT then removes.
--   Swap   broadcasts wallet.contact_hello() once a second and listens.
--            a CONTACT_HELLO (vk.hello_parse): "Swap with <name>?"; SELECT sends
--              wallet.contact_card(hello) unicast to that badge.
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

local peers = {}                          -- swap: {mac, frame, name, address, rssi, heard, sent}
local peer_sel = 1
local next_hello = 0                      -- badge.millis() of the next broadcast
local message, message_tone, message_until = nil, nil, 0

-- "Oct 3" for unix seconds; the configured blank when the clock had no source.
local function date_text(added)
  if type(added) ~= "number" or added <= 0 then return text.no_date end
  local ok, t = pcall(os.date, "!*t", math.floor(added))
  if not ok or type(t) ~= "table" then return text.no_date end
  return (cfg.months[t.month] or "?") .. " " .. t.day
end

local function build_rows()
  rows = {{l = text.swap_row, sub = text.swap_sub, r = ARROW}}
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
  reason = tostring(reason or "failed")
  return cfg.reasons[reason] or reason
end

-- An LED flash in a colour of the active theme (RGB565 -> 8-bit components).
local function flash(spec)
  local c = ui.color(spec.color)
  badge.led.pulse((c // 2048) * 255 // 31, ((c // 32) % 64) * 255 // 63, (c % 32) * 255 // 31, spec.ms)
end

-- ---------------------------------------------------------------------------------------------
-- Swap
-- ---------------------------------------------------------------------------------------------

local function enter_swap()
  if not espnow.enabled() then espnow.enable(true) end
  peers, peer_sel = {}, 1
  message = nil
  next_hello = 0
  set_mode("swap")
  badge.log("CON swap on")
end

local function leave_swap()
  peers = {}
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
  for i = #peers, 1, -1 do
    if now - peers[i].heard > cfg.peer_timeout_ms then table.remove(peers, i) end
  end
  if peer_sel > #peers then peer_sel = math.max(1, #peers) end
  if message and now >= message_until then message = nil end
end

-- A HELLO from a badge in swap mode: remember its newest frame (it carries the nonce a card for
-- it must be signed over).
local function heard_hello(mac, frame, hello, rssi)
  local now = badge.millis()
  local chosen = peers[peer_sel] and peers[peer_sel].mac
  local peer
  for i = 1, #peers do
    if peers[i].mac == mac then peer = peers[i] break end
  end
  if not peer then
    if #peers >= cfg.max_peers then return end
    peer = {mac = mac}
    peers[#peers + 1] = peer
    badge.log("CON hello " .. vk.short(hello.address))
  end
  if peer.frame ~= frame then peer.sent = false end      -- a new nonce: the old card is spent
  peer.frame, peer.name, peer.address = frame, hello.name, hello.address
  peer.rssi, peer.heard = rssi or -127, now
  table.sort(peers, function(a, b) return a.rssi > b.rssi end)
  for i = 1, #peers do                                   -- keep the cursor on the same badge
    if peers[i].mac == chosen then peer_sel = i break end
  end
end

local function heard_card(frame)
  local contact, why = wallet.contact_accept(frame)
  if contact then
    badge.log("CON saved " .. tostring(contact.name))
    say(string.format(text.saved, tostring(contact.name)), "ok")
    flash(cfg.saved_led)
    next_hello = 0            -- the accept spent this badge's nonce: the next HELLO draws a new one
  else
    badge.log("CON accept failed " .. tostring(why))
    say(reason_text(why), "bad")
  end
end

local function send_card()
  local peer = peers[peer_sel]
  if not peer then return end
  local card, why = wallet.contact_card(peer.frame)
  if not card then return say(reason_text(why), "bad") end
  if espnow.send(peer.mac, card) then
    peer.sent = true
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
  if #peers == 0 then
    ui.text_center(text.looking, ui.W // 2, 96, ui.color("ink"))
    ui.text_center(text.looking_sub, ui.W // 2, 114, ui.color("sub"))
  else
    ui.text_center(string.format(text.ask, peers[peer_sel].name), ui.W // 2, 52, ui.color("ink"))
    for i = 1, #peers do
      local peer = peers[i]
      local y = 72 + (i - 1) * ui.SUB_PITCH
      local selected = i == peer_sel
      ui.row(y, peer.name:upper(), peer.sent and text.sent or ARROW, selected)
      ui.subline(y + 13, vk.short(peer.address) .. DOT .. text.self_named, selected)
    end
  end
  if message then
    ui.text_center(message, ui.W // 2, 200, ui.color(message_tone == "ok" and "stamp_ok" or "stamp_bad"))
  end
  ui.footer(#peers > 0 and text.hint_send or "", text.back)
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
    if key == "b" then leave_swap()
    elseif key == "a" then send_card()
    elseif key == "up" and peer_sel > 1 then peer_sel = peer_sel - 1
    elseif key == "down" and peer_sel < #peers then peer_sel = peer_sel + 1
    end
  elseif mode == "confirm" then
    local contact = contacts[sel - 1]
    if key == "a" and contact then
      if wallet.contact_remove(contact.address) then
        badge.log("CON removed " .. vk.short(contact.address))
      end
      mode = "list"
      load_contacts()
    else
      set_mode("list")                    -- CANCEL, or any other key: keep the contact
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
