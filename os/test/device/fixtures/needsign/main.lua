-- Needs sign (dev only; the fixture of T-APP2 and T-APP3 in test/device/t_app.py, never on a
-- judge badge).
--
-- Its app.ini asks for `sign`, a permission the user must approve on the first launch. T-APP3
-- pushes it again with permissions=sign,net. It signs nothing; it reports over the log:
--
--   NS start              the app was launched
--   NS poll <ok> <why>    wallet.poll(), which needs `sign`: "true idle" when granted
--   NS net <ok>           whether badge.wifi, which needs `net`, could be used
--
-- CANCEL exits.

local gfx = badge.gfx

badge.log("NS start")
local ok, _, why = pcall(badge.wallet.poll)
badge.log("NS poll " .. tostring(ok) .. " " .. tostring(why))
badge.log("NS net " .. tostring((pcall(function() return badge.wifi.connected() end))))

function on_draw()
  gfx.clear(gfx.BG)
  gfx.text_center("needsign", gfx.width() // 2, 100, gfx.WHITE, 2)
  gfx.text_center("CANCEL to quit", gfx.width() // 2, 140, gfx.MUTED, 1)
end

function on_button(key, pressed)
  if pressed and key == "b" then badge.system.exit() end
end
