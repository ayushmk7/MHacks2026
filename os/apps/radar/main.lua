-- Badge Radar
--
-- Plots every badge the ESP-NOW beacon has heard, placed by signal strength:
-- close to the centre means a strong signal, which is near enough to "close"
-- to be useful for finding someone in a room.
--
-- Press SELECT to ping everyone; a badge running this app answers, and the reply
-- shows up as a flash on that peer's dot.
--
-- Every coordinate handed to gfx goes through math.floor first. The bindings
-- take integers - a screen has no half pixels - and Lua 5.4 will not silently
-- truncate a float that has a fractional part, it raises "number has no integer
-- representation". Trigonometry produces almost nothing but such floats, so an
-- unfloored sin/cos here is not a rounding wart, it is a crash on frame one.

local g = badge.gfx
local sweep = 0
local pinged = {}      -- mac -> millis of the last reply
local messages = {}    -- recent inbound lines, newest first

local CX, CY, R = 108, 128, 90

function on_start()
  badge.espnow.enable(true)
  badge.led.take()
  badge.log("radar up on channel " .. badge.espnow.channel())
end

-- RSSI to a radius. -45 dBm and better sits at the centre, -95 at the rim.
local function radius_for(rssi)
  local t = (rssi + 45) / -50      -- 0.0 at -45, 1.0 at -95
  if t < 0 then t = 0 end
  if t > 1 then t = 1 end
  return 12 + t * (R - 16)
end

function on_update(dt)
  sweep = (sweep + dt * 90) % 360

  local peers = badge.espnow.peers()
  if #peers > 0 then
    -- Brighten with the strongest signal in range, so the LEDs alone tell you
    -- whether you are getting warmer.
    local best = (peers[1].rssi + 95) / 50
    if best < 0 then best = 0 end
    if best > 1 then best = 1 end
    badge.led.gradient(0, best, 0.2 + best * 0.8)
    badge.led.gradient(1, best, 0.2 + best * 0.8)
  else
    badge.led.all(0, 0, 0)
  end
  badge.led.show()
end

function on_draw()
  g.clear(g.BG)

  -- Range rings.
  for i = 1, 3 do
    g.circle(CX, CY, R * i // 3, g.BORDER)
  end
  g.line(CX - R, CY, CX + R, CY, g.BORDER)
  g.line(CX, CY - R, CX, CY + R, g.BORDER)

  -- Sweep line.
  local rad = math.rad(sweep)
  g.line(CX, CY, math.floor(CX + math.cos(rad) * R), math.floor(CY + math.sin(rad) * R),
         g.SOLANA_GREEN)

  g.fill_circle(CX, CY, 4, g.SOLANA_PURPLE)

  local peers = badge.espnow.peers()
  local now = badge.millis()

  for i, peer in ipairs(peers) do
    -- Fan the peers around the dial by index. There is no direction
    -- information in an ESP-NOW frame, so the angle is arbitrary and only the
    -- distance from the centre carries meaning.
    local angle = (i - 1) * (2 * math.pi / math.max(#peers, 1))
    local dist = radius_for(peer.rssi)
    local x = math.floor(CX + math.cos(angle) * dist)
    local y = math.floor(CY + math.sin(angle) * dist)

    local recent = pinged[peer.mac] and (now - pinged[peer.mac]) < 1500
    local color = recent and g.SOLANA_GREEN or g.gradient(1 - dist / R)

    g.fill_circle(x, y, recent and 6 or 4, color)
    g.text(peer.name, x + 8, y - 3, g.MUTED, 1)
  end

  -- Side panel.
  local px = 216
  g.line(px - 8, 26, px - 8, g.height() - 20, g.BORDER)
  g.text("PEERS", px, 30, g.MUTED, 1)
  g.text(tostring(#peers), px, 44, g.WHITE, 2)
  g.text("CH " .. badge.espnow.channel(), px, 72, g.MUTED, 1)

  g.text("LOG", px, 96, g.MUTED, 1)
  for i = 1, math.min(#messages, 6) do
    g.text(messages[i], px, 108 + (i - 1) * 12, g.MUTED, 1)
  end

  g.text_center("SELECT ping   CANCEL quit", g.width() // 2, g.height() - 14, g.MUTED, 1)
end

function on_button(key, pressed)
  if not pressed then return end

  if key == "a" then
    badge.espnow.broadcast("PING " .. badge.device_name)
    badge.led.pulse(153, 69, 255, 250)
  elseif key == "b" then
    badge.system.exit()
  end
end

function on_espnow(mac, data, rssi)
  if data:sub(1, 5) == "PING " then
    -- Someone is looking for badges; answer so their radar lights us up.
    badge.espnow.send(mac, "PONG " .. badge.device_name)
    table.insert(messages, 1, "ping " .. rssi .. "dBm")
  elseif data:sub(1, 5) == "PONG " then
    pinged[mac] = badge.millis()
    table.insert(messages, 1, data:sub(6, 16))
  else
    table.insert(messages, 1, data:sub(1, 12))
  end

  while #messages > 6 do
    table.remove(messages)
  end
end

function on_stop()
  badge.led.off()
end
