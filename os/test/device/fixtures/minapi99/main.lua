-- Needs API 99 (dev only; the fixture of T-APP4 in test/device/t_app.py, never on a judge badge).
--
-- Its app.ini says min_api=99, so the firmware refuses to launch it and this file never runs.
-- If it does, the log says so.

badge.log("M99 start")

function on_draw()
  badge.gfx.clear(badge.gfx.BG)
  badge.gfx.text_center("minapi99 is running", badge.gfx.width() // 2, 110, badge.gfx.WHITE, 1)
end

function on_button(key, pressed)
  if pressed and key == "b" then badge.system.exit() end
end
