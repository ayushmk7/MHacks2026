-- Home
--
-- The landing app (docs/os/apps/apps.md, "Home"; layout: docs/os/ui/ui.md, "Screens").
--
--   left stub   BALANCE, the default token's balance and its symbol (SETUP NEEDED on a badge
--               that is not provisioned), then a QR code of the config key repo_url (the
--               project's repository) or, when that key is empty, this badge's barcode
--   body        rows NAME, ADDRESS, KEY, CLOCK; a rule; a menu of the other installed apps;
--               a rule; THANK YOU FOR HACKING
--
-- The firmware does not poll the balance while an app runs, so Home fetches it itself: once
-- after its first frame is on screen, then every balance_poll_s seconds. A fetch that fails
-- changes one line under the code and is tried again at the next period, never sooner; with
-- no network no call is made at all.
--
-- UP/DOWN move in the menu, SELECT launches the selected app, CANCEL exits to the launcher.
--
-- Log lines (each "[app] HOME ..."), for test/device/t_app_home.py:
--   HOME addr <short address>        once, on start
--   HOME balance <ok|reason>         after each fetch that was attempted
--
-- The look comes from lib/vk.lua (pushed into this folder as vk.lua by scripts/push-apps.sh).

local cfg = require("config")
local vk = require("vk")
local ui = vk.ui
local wallet, system = badge.wallet, badge.system
local text = cfg.text

-- Layout (pixels; a y is the top of the capital letters, as in vk.ui).
local AMOUNT_Y = 40                    -- the stub's label; the value and the unit follow it
local BARCODE_X, BARCODE_Y, BARCODE_W, BARCODE_H = 16, 122, 114, 40
local NOTE_Y = 176                     -- the line under the barcode
-- With a link to show, the stub is the amount (moved up), the QR code and the line under it.
-- 102 px: 3 px modules for a link of up to 53 characters, with the quiet zone inside the square.
local QR_AMOUNT_Y = 28
local QR_SIZE, QR_Y = 102, 98
local QR_NOTE_Y = 204
local INFO_Y = 32                      -- first body row
local MENU_RULE_Y = INFO_Y + 4 * ui.ROW_PITCH - 3
local MENU_Y = MENU_RULE_Y + 11        -- first menu row
local TRIANGLE = "\xE2\x97\x82"        -- the mark on the selected row, as on the launcher

local menu = {}                        -- {id = , name = } of each app offered
local sel = 1
local drawn = false                    -- a frame has been shown: a blocking fetch may now run
local next_fetch = 0                   -- badge.millis() of the next fetch; nil: no more fetches
local note = nil                       -- the line under the barcode

-- The apps of cfg.menu that are installed, in cfg.menu's order.
local function load_menu()
  local installed = {}
  local ok, apps = pcall(system.apps)
  if ok and type(apps) == "table" then
    for i = 1, #apps do
      local app = apps[i]
      if type(app) == "table" and type(app.id) == "string" then installed[app.id] = app end
    end
  end
  local me = system.current_app and system.current_app() or "home"
  menu = {}
  for i = 1, #cfg.menu do
    local id = cfg.menu[i]
    local app = installed[id]
    if app and id ~= me then
      local name = type(app.name) == "string" and app.name ~= "" and app.name or id
      menu[#menu + 1] = {id = id, name = name:upper()}
    end
  end
  sel = math.max(1, math.min(sel, #menu))
end

-- The poll period in ms, or nil for "never again" (balance_poll_s 0).
local function poll_ms()
  local seconds = math.tointeger(tonumber(wallet.config("balance_poll_s") or "") or cfg.default_poll_s)
  if not seconds or seconds < 0 then seconds = cfg.default_poll_s end
  if seconds == 0 then return nil end
  return math.min(seconds, cfg.max_poll_s) * 1000
end

-- One fetch at most, and only when it can work. Whatever happens, the next one is a period away.
local function fetch(now)
  local period = poll_ms()
  next_fetch = period and now + period or nil
  if not wallet.provisioned() then
    note = nil                         -- the stub already says SETUP NEEDED
    return
  end
  if not (badge.wifi and badge.wifi.connected()) then
    note = cfg.fetch_text.no_network
    return
  end
  local called, ok, why = pcall(wallet.refresh_balance)
  if called and ok then
    note = nil
    badge.log("HOME balance ok")
  else
    local reason = tostring(called and why or "error")
    note = cfg.fetch_text[reason]
    badge.log("HOME balance " .. reason)
  end
end

function on_start()
  load_menu()
  badge.log("HOME addr " .. vk.short(wallet.address()))
end

function on_update(dt)
  if not drawn or not next_fetch then return end
  local now = badge.millis()
  if now >= next_fetch then fetch(now) end
end

local function draw_stub()
  local cx = ui.STUB_CX
  -- The link is a provisioned setting, read on every frame: a VKSET shows without a restart.
  -- ui.qr draws nothing on a firmware without the kit's QR code: the barcode stays then.
  local link = wallet.config("repo_url")
  local qr = type(link) == "string" and link ~= "" and ui.qr(cx - QR_SIZE // 2, QR_Y, QR_SIZE, link)
  local amount_y = qr and QR_AMOUNT_Y or AMOUNT_Y
  if wallet.provisioned() then
    local tokens = wallet.tokens()
    local symbol = type(tokens) == "table" and tokens[1] and tokens[1].symbol or ""
    ui.amount(cx, amount_y, text.balance, wallet.balance() or text.unknown_amount, symbol)
  else
    ui.amount(cx, amount_y, text.setup_needed, text.unknown_amount, "")
  end
  if not qr then ui.barcode(BARCODE_X, BARCODE_Y, BARCODE_W, BARCODE_H) end
  if note then ui.text_center(note, cx, qr and QR_NOTE_Y or NOTE_Y, ui.color("sub")) end
end

local function draw_body()
  local x0, x1 = ui.BODY_X0, ui.BODY_X1
  local name = wallet.config("display_name")
  if type(name) ~= "string" or name == "" then name = system.name() end
  local address = wallet.address()
  local clock_ok = wallet.time_ok()
  local location = wallet.key_location()

  local y = INFO_Y
  ui.row(y, text.name, name, false, nil, x0, x1)
  y = y + ui.ROW_PITCH
  ui.row(y, text.address, address ~= "" and vk.short(address) or text.no_identity, false, nil, x0, x1)
  y = y + ui.ROW_PITCH
  ui.row(y, text.key, cfg.key_text[location] or location, false, nil, x0, x1)
  y = y + ui.ROW_PITCH
  ui.row(y, text.clock, clock_ok and text.clock_ok or text.clock_unset, false,
    not clock_ok and ui.color("stamp_warn") or nil, x0, x1)
  ui.rule(MENU_RULE_Y, x0, x1)

  local visible = cfg.menu_rows
  if #menu == 0 then
    ui.text_center(text.no_apps, ui.BODY_CX, MENU_Y + ui.ROW_PITCH, ui.color("sub"))
  else
    local first = math.max(0, math.min(sel - 1 - visible // 2, #menu - visible))
    for slot = 0, visible - 1 do
      local index = first + slot + 1
      local app = menu[index]
      if not app then break end
      local selected = index == sel
      ui.row(MENU_Y + slot * ui.ROW_PITCH, app.name, selected and TRIANGLE or "", selected, nil, x0, x1)
    end
  end

  local end_y = MENU_Y + visible * ui.ROW_PITCH - 3
  ui.rule(end_y, x0, x1)
  ui.text_center(text.thanks, ui.BODY_CX, end_y + 11)
end

function on_draw()
  ui.page()
  ui.header(cfg.header)
  draw_stub()
  draw_body()
  ui.perforation(ui.SPLIT_X, ui.CONTENT_Y, 212)
  ui.footer(#menu > 0 and text.open or "", text.back)
  drawn = true
end

function on_button(key, pressed)
  if not pressed then return end
  if key == "b" then
    system.exit()
  elseif key == "up" then
    if sel > 1 then sel = sel - 1 end
  elseif key == "down" then
    if sel < #menu then sel = sel + 1 end
  elseif key == "a" then
    local app = menu[sel]
    -- launch raises for an app that was deleted since the menu was built: rebuild it instead.
    if app and not pcall(system.launch, app.id) then load_menu() end
  end
end

-- Draw only when the screen changed (lib/vk.lua, "ui.frame"): a frame every pass would hold the
-- loop at 20 passes a second.
local draw_frame = on_draw
function on_draw() ui.frame(draw_frame) end
