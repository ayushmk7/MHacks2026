-- __NAME__ (__ID__): made from os/templates/lua_app by scripts/new-app.sh.
--
-- A two-row list in the Receipt look: "Count" goes up on SELECT, "Reset" puts it back to zero.
-- Replace the rows and the key handling with your own app; keep the shape:
--   - every knob and every word comes from config.lua;
--   - all drawing is in draw(), and on_draw() hands it to vk.ui.frame, which draws only when
--     something changed (a key, vk.ui.dirty(), a paused frame), so the badge stays fast;
--   - CANCEL ("b") always exits; no app traps it.
-- The API: docs/os/platform/lua-api.md. The rules: docs/os/apps/apps.md.

local cfg = require("config")
local vk = require("vk")          -- lib/vk.lua: push-apps.sh puts it in this folder as vk.lua
local ui = vk.ui

local count = 0
local sel = 1                     -- the selected row, 1-based

-- An LED flash in a colour of the active theme (RGB565 -> 8-bit components).
local function flash(spec)
  local c = ui.color(spec.color)
  badge.led.pulse((c // 2048) * 255 // 31, ((c // 32) % 64) * 255 // 63, (c % 32) * 255 // 31, spec.ms)
end

-- The whole screen. vk.ui.list draws the header, the title, the rows and the footer with the
-- firmware's own receipt kit, in the active theme's colours.
local function draw()
  ui.list{
    header = cfg.header,
    title = cfg.text.title,
    rows = {
      {l = cfg.text.count, r = tostring(count)},
      {l = cfg.text.reset, sub = cfg.text.reset_sub},
    },
    sel = sel,
    hint = cfg.text.hint,
    back = cfg.text.back,
  }
end

function on_start()
  badge.log("__ID__ start")
end

function on_draw()
  ui.frame(draw)
end

function on_button(key, pressed)
  if not pressed then return end
  if key == "b" then
    badge.system.exit()
  elseif key == "up" then
    sel = math.max(1, sel - 1)
  elseif key == "down" then
    sel = math.min(2, sel + 1)
  elseif key == "a" then
    if sel == 1 then
      count = count + cfg.step
      flash(cfg.led)
    else
      count = 0
    end
    badge.log("__ID__ count " .. count)
  end
end
