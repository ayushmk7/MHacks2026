-- No network (dev only; the fixture of T-APP7 in test/device/t_app.py, never on a judge badge).
--
-- Its app.ini has an empty permissions= line, so it is granted nothing. It reports over the log:
--
--   NN pcall <ok> <message>   what a caught badge.http.get gave
--   NN wifi <ok>              whether badge.wifi, the other table behind `net`, could be used
--   NN free <ok>              whether badge.system, which needs no permission, could be used
--
-- and then calls badge.http.get from on_start without catching the error, so the runtime stops
-- the app with it. No request is ever sent: the address is this badge's own, port 9.

local URL = "http://127.0.0.1:9/"

local ok, message = pcall(function() return badge.http.get(URL) end)
badge.log("NN pcall " .. tostring(ok) .. " " .. tostring(message))
badge.log("NN wifi " .. tostring((pcall(function() return badge.wifi.connected() end))))
badge.log("NN free " .. tostring((pcall(function() return badge.system.millis() end))))

function on_start()
  badge.http.get(URL)
  badge.log("NN reached")   -- only if the call above did not raise
end

function on_draw()
  badge.gfx.clear(badge.gfx.BG)
  badge.gfx.text_center("nonet", badge.gfx.width() // 2, 110, badge.gfx.WHITE, 2)
end

function on_button(key, pressed)
  if pressed and key == "b" then badge.system.exit() end
end
