-- Dice (docs/os/apps/apps.md, "Rules for every Lua app")
--
-- A small game: throw one to five dice and read the total.
--
--   SELECT     rolls config.count dice. They tumble for config.roll_ms, then settle; the LEDs
--              flash once in the theme's LED colour.
--   UP, DOWN   one die more or fewer, between 1 and config.max.
--   CANCEL     exits to the launcher.
--
-- Screen, top to bottom: header, title, the dice as square faces with pips (ink on paper), a
-- rule, then the total as an amount on the left and the last rolls as rows on the right.
--
-- The numbers come from the chip's hardware random source (badge.system.random_bytes) when the
-- firmware has it, else from math.random seeded with the uptime. The faces shown while the dice
-- tumble are decoration; the result is drawn once, when the roll starts.
--
-- The app needs no permission: it uses the screen, the keys and the LEDs only.
--
-- Log lines (each "[app] DICE ..."), for test/device/t_app_dice.py:
--   DICE count <n>                        on start, and when UP or DOWN changed the number of dice
--   DICE roll <d1> <d2> ... total <n>     when a roll has settled: each die, then the sum
--
-- The look comes from lib/vk.lua (pushed into this folder as vk.lua by scripts/push-apps.sh).

local cfg = require("config")
local vk = require("vk")
local ui, gfx = vk.ui, badge.gfx
local text = cfg.text
local die = cfg.die

local SIDES = 6                           -- a face with pips has six sides; not a knob

-- Where each face puts its pips, on a 3 x 3 grid: {column, row}, 1..3.
local PIPS = {
  {{2, 2}},
  {{1, 1}, {3, 3}},
  {{1, 1}, {2, 2}, {3, 3}},
  {{1, 1}, {3, 1}, {1, 3}, {3, 3}},
  {{1, 1}, {3, 1}, {2, 2}, {1, 3}, {3, 3}},
  {{1, 1}, {3, 1}, {1, 2}, {3, 2}, {1, 3}, {3, 3}},
}

-- Layout (docs/os/ui/ui.md, "The receipt kit"): the dice start where a list's rows start; the
-- area under the rule is a two-column receipt, split at the perforation.
local W, MARGIN = ui.W, ui.MARGIN
local BODY_X0, BODY_X1, BODY_CX = ui.BODY_X0 or 156, ui.BODY_X1 or 310, ui.BODY_CX or 233
local DICE_Y = ui.LIST_Y
local CONTENT_BOTTOM = 212                -- the footer's rule is at y 216
local AMOUNT_H = 64                       -- an amount: its label, the value, the unit

local function clamp(value, low, high)
  value = math.floor(tonumber(value) or low)
  if value < low then return low end
  if value > high then return high end
  return value
end

local max_count = clamp(cfg.max, 1, 9)
local count = clamp(cfg.count, 1, max_count)

local faces = {}                          -- what each die shows: 1..6
local settled = false                     -- the faces are a roll's result (not a placeholder)
local total = nil                         -- the sum of the last roll; nil before one
local pending = nil                       -- the result of the roll that is tumbling, else nil
local roll_until, next_tick = 0, 0
local rolls = 0                           -- how many rolls so far: the next history number
local history = {}                        -- newest first: {label =, value =}

-- ---------------------------------------------------------------------------------------------
-- Random numbers
-- ---------------------------------------------------------------------------------------------

local seeded = false

-- 1..SIDES from Lua's own generator: the tumbling faces, and the result on a firmware without
-- the hardware source.
local function soft_face()
  if not seeded then
    math.randomseed(badge.millis())
    seeded = true
  end
  return math.random(SIDES)
end

-- n faces. A hardware byte is used only when it is below the largest multiple of SIDES (252), so
-- that every face is equally likely; twice the bytes needed are asked for to cover the rest.
local function roll_faces(n)
  local out = {}
  local system = badge.system
  local random_bytes = system and system.random_bytes
  local limit = 256 - 256 % SIDES
  local asked = 0
  while random_bytes and #out < n and asked < 4 do
    asked = asked + 1
    local ok, bytes = pcall(random_bytes, 2 * n)
    if not ok or type(bytes) ~= "string" or #bytes == 0 then break end
    for i = 1, #bytes do
      local byte = bytes:byte(i)
      if byte < limit and #out < n then out[#out + 1] = byte % SIDES + 1 end
    end
  end
  while #out < n do out[#out + 1] = soft_face() end
  return out
end

-- ---------------------------------------------------------------------------------------------
-- State changes
-- ---------------------------------------------------------------------------------------------

-- Before the first roll, and after the number of dice changed: faces 1, 2, 3, ... drawn faint, so
-- that nobody reads them as a result.
local function placeholder()
  faces = {}
  for i = 1, count do faces[i] = (i - 1) % SIDES + 1 end
  settled, total = false, nil
end

-- An LED flash in a colour of the active theme (RGB565 -> 8-bit components).
local function flash()
  local led = badge.led
  if not (led and led.pulse) then return end
  local c = ui.color(cfg.led.color)
  led.pulse((c // 2048) * 255 // 31, ((c // 32) % 64) * 255 // 63, (c % 32) * 255 // 31, cfg.led.ms)
end

local function settle()
  faces, pending = pending, nil
  settled = true
  total = 0
  for i = 1, #faces do total = total + faces[i] end

  rolls = rolls + 1
  local value = tostring(total)
  if #faces > 1 then value = table.concat(faces, "+") .. " = " .. value end
  table.insert(history, 1, {label = string.format(text.roll, rolls), value = value})
  history[clamp(cfg.history, 1, 20) + 1] = nil    -- one was added: at most one is over

  badge.log("DICE roll " .. table.concat(faces, " ") .. " total " .. total)
  ui.dirty()
end

local function start_roll()
  if pending then return end              -- one roll at a time
  pending = roll_faces(count)
  local now = badge.millis()
  roll_until = now + clamp(cfg.roll_ms, 0, 10000)
  next_tick = now
  flash()
  ui.dirty()
  if roll_until <= now then settle() end
end

local function change_count(step)
  if pending then return end
  local to = clamp(count + step, 1, max_count)
  if to == count then return end
  count = to
  placeholder()
  badge.log("DICE count " .. count)
  ui.dirty()
end

-- ---------------------------------------------------------------------------------------------
-- Drawing
-- ---------------------------------------------------------------------------------------------

-- The side of a die: the configured size, or less when `count` dice would not fit the width.
local function die_size()
  local room = W - 2 * MARGIN - (count - 1) * die.gap
  return math.max(12, math.min(die.size, room // count))
end

local function draw_die(x, y, size, face, pip_color)
  local edge = math.max(1, die.edge)
  gfx.fill_round_rect(x, y, size, size, die.radius, ui.color(cfg.colors.edge))
  gfx.fill_round_rect(x + edge, y + edge, size - 2 * edge, size - 2 * edge,
    math.max(0, die.radius - edge), ui.color(cfg.colors.face))
  local pips = PIPS[face]
  if not pips then return end
  local radius = math.max(1, size // die.pip_div)
  for i = 1, #pips do
    -- Grid lines at a quarter, a half and three quarters of the side.
    gfx.fill_circle(x + size * pips[i][1] // 4, y + size * pips[i][2] // 4, radius, pip_color)
  end
end

local function draw()
  ui.page()
  ui.header(cfg.header)
  ui.title(text.title, ui.TITLE_Y)

  -- The dice, centred.
  local size = die_size()
  local width = count * size + (count - 1) * die.gap
  local x = (W - width) // 2
  local pip_color = ui.color((settled or pending) and cfg.colors.pip or cfg.colors.idle_pip)
  for i = 1, count do
    draw_die(x + (i - 1) * (size + die.gap), DICE_Y, size, faces[i], pip_color)
  end

  local rule_y = DICE_Y + size + 8
  ui.rule(rule_y)
  ui.perforation(ui.SPLIT_X, rule_y + 4, CONTENT_BOTTOM)

  -- Left stub: the total, in the middle of the space under the rule.
  local shown = pending and text.rolling or (total and tostring(total) or text.no_total)
  local unit = count == 1 and text.unit_one or string.format(text.unit_many, count)
  local amount_y = rule_y + math.max(8, (CONTENT_BOTTOM - rule_y - AMOUNT_H) // 2)
  ui.amount(ui.STUB_CX, amount_y, text.total, shown, unit)

  -- Body: the last rolls, newest first. A row at y owns y-5 .. y+12.
  local first_y = rule_y + 14
  local fit = (CONTENT_BOTTOM - 12 - first_y) // ui.ROW_PITCH + 1
  if #history == 0 then
    ui.text_center(text.empty, BODY_CX, first_y, ui.color("sub"))
  end
  for i = 1, math.min(#history, fit) do
    ui.row(first_y + (i - 1) * ui.ROW_PITCH, history[i].label, history[i].value, false, nil, BODY_X0, BODY_X1)
  end

  ui.footer(text.hint, text.exit)
end

-- ---------------------------------------------------------------------------------------------
-- Callbacks
-- ---------------------------------------------------------------------------------------------

function on_start()
  placeholder()
  badge.log("DICE count " .. count)
end

function on_update(dt)
  if not pending then return end
  local now = badge.millis()
  if now >= roll_until then
    settle()
  elseif now >= next_tick then
    for i = 1, count do faces[i] = soft_face() end
    next_tick = now + clamp(cfg.tick_ms, 10, 1000)
  end
end

-- Draw only when the screen can have changed (lib/vk.lua, "ui.frame"). While the dice tumble a
-- frame is due every tick; otherwise only the header's clock and battery move.
function on_draw()
  if pending then
    ui.frame(draw, clamp(cfg.tick_ms, 10, 1000))
  else
    ui.frame(draw, clamp(cfg.idle_ms, 100, 60000))
  end
end

function on_button(key, pressed)
  if not pressed then return end
  if key == "b" then
    badge.system.exit()
  elseif key == "a" then
    start_roll()
  elseif key == "up" then
    change_count(1)
  elseif key == "down" then
    change_count(-1)
  end
end
