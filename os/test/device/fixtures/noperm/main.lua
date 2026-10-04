-- No permissions (dev only; the fixture of T-APP1 in test/device/t_app.py, never on a judge badge).
--
-- Its app.ini has no permissions= line, so it is granted nothing. It reports over the log:
--
--   NP pcall <ok> <message>   what a caught call of wallet.begin_solana gave
--   NP free <ok>              whether wallet.address, which needs no permission, could be called
--
-- and then calls wallet.begin_solana from on_start without catching the error, so the runtime
-- stops the app with it.

local wallet = badge.wallet

local ok, message = pcall(wallet.begin_solana, "", {})
badge.log("NP pcall " .. tostring(ok) .. " " .. tostring(message))
badge.log("NP free " .. tostring((pcall(wallet.address))))

function on_start()
  wallet.begin_solana("", {})
  badge.log("NP reached")   -- only if the call above did not raise
end

function on_draw()
  badge.gfx.clear(badge.gfx.BG)
  badge.gfx.text_center("noperm", badge.gfx.width() // 2, 110, badge.gfx.WHITE, 2)
end

function on_button(key, pressed)
  if pressed and key == "b" then badge.system.exit() end
end
