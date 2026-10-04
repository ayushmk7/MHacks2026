-- VU Meter
--
-- Shows what the two PDM microphones are hearing. Also a worked example of
-- badge.storage.kv: the loudest reading ever seen is kept across reboots.

local g = badge.gfx
local peak_l, peak_r = 0, 0
local history = {}          -- rolling max, oldest first
local record = 0

local HISTORY_LEN = 60

function on_start()
  -- The microphones are off unless an app asks for them.
  if not badge.mic.enable() then
    badge.log("microphones failed to start")
  end

  record = tonumber(badge.storage.kv.get("record", "0")) or 0
  badge.led.take()

  for i = 1, HISTORY_LEN do
    history[i] = 0
  end
end

function on_update(dt)
  local l, r = badge.mic.level()

  -- Peaks fall back slowly so a transient stays readable.
  peak_l = math.max(l, peak_l - dt * 40)
  peak_r = math.max(r, peak_r - dt * 40)

  table.remove(history, 1)
  history[HISTORY_LEN] = math.max(l, r)

  local loudest = math.max(l, r)
  if loudest > record then
    record = loudest
    badge.storage.kv.set("record", string.format("%.1f", record))
  end

  local t = loudest / 100
  badge.led.gradient(0, t, t)
  badge.led.gradient(1, t, t)
  badge.led.show()
end

local function bar(label, x, level, peak)
  local top, height, width = 40, 130, 40

  g.text(label, x + 12, top - 14, g.MUTED, 1)
  g.rect(x, top, width, height, g.BORDER)

  -- Fill from the bottom, coloured along the gradient by height so a loud
  -- signal reads green and a quiet one purple.
  local fill = math.floor(height * level / 100)
  for i = 0, fill - 1 do
    g.line(x + 1, top + height - 1 - i, x + width - 2, top + height - 1 - i,
           g.gradient(i / height))
  end

  local py = top + height - 1 - math.floor(height * peak / 100)
  g.line(x, py, x + width, py, g.WHITE)

  -- "%.0f", not "%d": mic.level() is a float and Lua 5.4 refuses to format a
  -- fractional number with an integer specifier rather than rounding it for you.
  g.text_center(string.format("%.0f", level), x + width // 2, top + height + 6, g.MUTED, 1)
end

function on_draw()
  g.clear(g.BG)
  g.text("VU METER", 12, 12, g.WHITE, 1)
  g.text_right(string.format("peak %.0f", record), g.width() - 12, 12, g.SOLANA_GREEN, 1)

  local l, r = badge.mic.level()
  bar("L", 30, l, peak_l)
  bar("R", 86, r, peak_r)

  -- Rolling history to the right of the bars.
  local hx, hy, hh = 150, 40, 130
  g.rect(hx, hy, HISTORY_LEN * 2 + 2, hh, g.BORDER)
  for i, value in ipairs(history) do
    local h = math.floor(hh * value / 100)
    if h > 0 then
      g.line(hx + 1 + (i - 1) * 2, hy + hh - 1, hx + 1 + (i - 1) * 2, hy + hh - h,
             g.gradient(value / 100))
    end
  end

  local dl, dr = badge.mic.db()
  g.text(string.format("%.0f dBFS / %.0f dBFS", dl, dr), 12, 186, g.MUTED, 1)
  g.text("SELECT reset peak   CANCEL quit", 12, g.height() - 16, g.MUTED, 1)
end

function on_button(key, pressed)
  if not pressed then return end
  if key == "a" then
    record = 0
    badge.storage.kv.set("record", "0")
    peak_l, peak_r = 0, 0
  elseif key == "b" then
    badge.system.exit()
  end
end

function on_stop()
  badge.mic.enable(false)
  badge.led.off()
end
