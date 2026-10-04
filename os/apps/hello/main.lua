-- Hello Solana
--
-- The shape every badge app has: on_start to set things up, on_update for
-- state, on_draw for pixels, on_button for input.

local g = badge.gfx
local t = 0
local last_key = "none"

-- on_button hands over the logical key name, which is what code should switch
-- on. What goes on screen is the silkscreen label instead: four of the six
-- happen to match, but "a" and "b" name nothing the badge is marked with - the
-- board says SELECT and CANCEL.
local KEY_LABEL = {up = "UP", down = "DOWN", left = "LEFT", right = "RIGHT",
                   a = "SELECT", b = "CANCEL"}

function on_start()
  badge.log("hello from " .. badge.device_name)
  badge.led.take()          -- stop the launcher's idle animation
end

function on_update(dt)
  t = t + dt

  -- The two LEDs walk the Solana gradient in opposite directions.
  local phase = (math.sin(t) + 1) / 2
  badge.led.gradient(0, phase, 0.6)
  badge.led.gradient(1, 1 - phase, 0.6)
  badge.led.show()
end

function on_draw()
  g.clear(g.BG)

  -- A gradient rule across the top, which is the cheapest way to make
  -- something look like it belongs on this badge.
  for x = 0, g.width() - 1 do
    g.line(x, 0, x, 3, g.gradient(x / g.width()))
  end

  g.text_center("Hello, Solana", g.width() // 2, 60, g.WHITE, 2)
  g.text_center(badge.device_name, g.width() // 2, 92, g.SOLANA_GREEN, 1)

  -- A pulsing ring, so it is obvious the frame loop is live.
  local r = 18 + math.floor(6 * math.sin(t * 3))
  g.circle(g.width() // 2, 145, r, g.gradient((math.sin(t) + 1) / 2))

  g.text_center("last button: " .. last_key, g.width() // 2, 185, g.MUTED, 1)
  g.text_center("CANCEL to quit", g.width() // 2, 205, g.MUTED, 1)
end

function on_button(key, pressed)
  if not pressed then return end
  last_key = KEY_LABEL[key] or key
  badge.led.pulse(20, 241, 149, 250)
  if key == "b" then
    badge.system.exit()
  end
end

function on_stop()
  badge.led.off()
end
