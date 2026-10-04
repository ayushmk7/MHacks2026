-- Plain (dev only; a fixture of test/device/t_clock.py and t_hook.py, never on a judge badge).
--
-- The smallest Lua app: it asks for no permission, draws one line and exits on CANCEL. Tests use
-- it where they need "some Lua app" to launch and stop. It logs:
--
--   PLAIN start           the app was launched

local gfx = badge.gfx

badge.log("PLAIN start")

function on_draw()
  gfx.clear(gfx.BG)
  gfx.text_center("plain", gfx.width() // 2, 100, gfx.TEXT, 2)
  gfx.text_center("CANCEL to quit", gfx.width() // 2, 140, gfx.MUTED, 1)
end

function on_button(key, pressed)
  if pressed and key == "b" then badge.system.exit() end
end
