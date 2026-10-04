-- Who's Near
--
-- A roster of the badges around you that are running *this* app, right now.
--
-- Not to be confused with the radar, which plots every badge in range by
-- signal strength. This one is about identity and presence: who else opened
-- the same script, and are they still there.
--
--
-- WHY THERE ARE TWO NOTIONS OF "PEER"
--
-- `badge.espnow.peers()` is the OS peer table. Every badge broadcasts a
-- presence beacon from the firmware no matter which app is on screen, so that
-- table lists the whole room -- launcher, radar, vumeter, everyone. It cannot
-- tell you what anyone is running, because the beacon does not carry that.
--
-- So this app runs its own presence protocol on top of the OS one, and keeps
-- its own roster. The OS table is used for exactly one thing: the "N badges in
-- range" figure in the header, which is what makes an empty roster readable --
-- "6 badges near, 0 running this" is a completely different problem from
-- "0 badges near".
--
--
-- THE PROTOCOL  (WN1)
--
--   message   "WN1|" .. device_name          -- e.g. "WN1|badge-4F2A"
--             ASCII, one line, no framing. 4 bytes of tag plus a name we cap
--             at 20 chars, so ~24 bytes against the 240-byte payload limit.
--
--   transport ESP-NOW broadcast. There is no unicast reply and no handshake:
--             everyone shouts on the same interval and everyone listens, so
--             the cost of the protocol is O(n) frames per interval rather
--             than O(n^2), and a badge that joins late is in everyone's list
--             within one interval without anyone having to notice it.
--
--   interval  1500 ms, plus up to 200 ms of jitter. The jitter keeps a room
--             full of badges from lockstepping onto the same air slot after
--             they have been running a while.
--
--   timeout   6000 ms. Four missed hellos before we drop someone, which
--             tolerates a couple of collisions without a peer flickering out
--             of the list. It is deliberately well under the OS peer table's
--             own 12 s expiry, so leaving this app removes you from other
--             people's rosters faster than it removes you from their radar.
--
--   ignoring  anything that does not start with "WN1|" is dropped without a
--             glance. Other apps share this callback and this channel.
--
-- There is no goodbye message. A badge that exits, sleeps or walks away all
-- look identical from here, and inventing a difference we cannot observe
-- would only be a lie with better latency.
--
--
-- CHANNEL
--
-- ESP-NOW and Wi-Fi share one radio and therefore one channel. If the badge is
-- joined to an access point it sits on that AP's channel and simply cannot
-- hear badges anywhere else -- no error, no packets, just an empty list. That
-- is the single most common reason this screen looks broken, so the channel
-- and the Wi-Fi network responsible for it are both in the header.

local g   = badge.gfx
local net = badge.espnow

-- Protocol constants. Kept together and named, because these three numbers
-- are the whole contract with the other badges.
local TAG        = "WN1|"
local HELLO_MS   = 1500
local HELLO_JIT  = 200
local TIMEOUT_MS = 6000
local NAME_MAX   = 20

-- Layout.
local ROW_H      = 21
local LIST_Y     = 82
local ROWS       = 6

local roster  = {}    -- mac -> {mac, name, rssi, last_ms}
local sorted  = {}    -- roster as an array, strongest first; rebuilt per frame
local scroll  = 0     -- index of the first visible row
local hello_in = 0    -- ms until the next hello
local anim    = 0     -- seconds, free-running, for the idle LED breath
local near    = 0     -- OS peer count: every badge in range, app or not
local own_mac = "??:??:??:??:??:??"

-- Announce ourselves. `immediate` skips the jitter for the very first hello so
-- a badge appears on everyone else's screen the moment it launches.
local function hello(immediate)
  net.broadcast(TAG .. badge.device_name:sub(1, NAME_MAX))
  hello_in = HELLO_MS + (immediate and 0 or math.random(0, HELLO_JIT))
end

function on_start()
  -- May already be on -- the launcher leaves it enabled -- but asking again is
  -- harmless and means the app works from a cold radio too.
  net.enable(true)
  badge.led.take()

  -- The station MAC is what our frames carry, so it is the address other
  -- badges will have us under. Wrapped because a headless radio can return nil,
  -- and lowercased because `wifi.mac()` hands back the Arduino uppercase form
  -- while ESP-NOW formats its addresses lowercase -- the same badge would
  -- otherwise print two different ways on the same screen.
  local ok, mac = pcall(badge.wifi.mac)
  if ok and mac then own_mac = mac:lower() end

  hello(true)
  badge.log("whosnear up on channel " .. net.channel() .. " as " .. badge.device_name)
end

function on_espnow(mac, data, rssi)
  -- Every app on the badge shares this callback. Anything without our tag is
  -- someone else's traffic and none of our business.
  if data:sub(1, #TAG) ~= TAG then return end

  -- A radio does not hear its own transmissions, so this should never fire.
  -- It is here because if it ever did, the symptom would be a badge listed
  -- twice with itself at the top of the roster, which reads as a bug in the
  -- protocol rather than in the radio.
  if mac:lower() == own_mac then return end

  local name = data:sub(#TAG + 1, #TAG + NAME_MAX)
  if name == "" then name = "?" end

  local e = roster[mac]
  if e then
    -- Smooth the signal rather than taking the last reading. RSSI on a single
    -- frame swings 10 dB while nobody moves, and since the list is sorted by
    -- signal an unsmoothed value would make rows swap places constantly.
    e.rssi = e.rssi + (rssi - e.rssi) * 0.3
    e.name = name
  else
    e = { mac = mac, name = name, rssi = rssi }
    roster[mac] = e
  end
  e.last_ms = badge.millis()
end

function on_update(dt)
  anim = anim + dt

  -- Broadcasting on a timer, not in a loop: callbacks have 250 ms to return.
  hello_in = hello_in - dt * 1000
  if hello_in <= 0 then hello(false) end

  -- Age out anyone who has gone quiet, then rebuild the sorted view once here
  -- so on_draw allocates nothing.
  local now = badge.millis()
  sorted = {}
  for mac, e in pairs(roster) do
    if now - e.last_ms > TIMEOUT_MS then
      roster[mac] = nil
    else
      sorted[#sorted + 1] = e
    end
  end

  -- Sorted by signal strength, strongest first, which matches what
  -- espnow.peers() does and answers the question people actually have in a
  -- crowded room: who of these is standing next to me. Most-recently-seen was
  -- the alternative and is nearly useless here -- everyone refreshes on the
  -- same 1.5 s interval, so that ordering is close to random. MAC breaks ties
  -- so equal signals do not shuffle between frames.
  table.sort(sorted, function(a, b)
    if a.rssi == b.rssi then return a.mac < b.mac end
    return a.rssi > b.rssi
  end)

  local max_scroll = math.max(0, #sorted - ROWS)
  if scroll > max_scroll then scroll = max_scroll end

  -- The OS peer table, sampled once a frame. This is the only place the app
  -- looks at it, and only for the "of N near" figure -- see the header comment.
  near = #net.peers()

  -- LEDs: greener and brighter the more company you have. Four peers saturates
  -- the ramp. Alone, a slow purple breath -- dim enough to read as "listening"
  -- rather than "found something", but not off, because off looks crashed.
  local n = #sorted
  if n > 0 then
    local t = math.min(n / 4, 1)
    badge.led.gradient(0, t, 0.25 + t * 0.75)
    badge.led.gradient(1, t, 0.25 + t * 0.75)
  else
    local breath = 0.10 + 0.06 * (math.sin(anim * 2) + 1)
    badge.led.gradient(0, 0, breath)
    badge.led.gradient(1, 0, breath)
  end
  badge.led.show()
end

-- Four bars from -95 dBm (one bar) to -45 dBm (four).
local function bars(x, y, rssi)
  local lit = math.floor((rssi + 95) / 12.5) + 1
  if lit < 1 then lit = 1 end
  if lit > 4 then lit = 4 end
  for i = 1, 4 do
    local h = 3 + i * 2
    local c = (i <= lit) and g.gradient((i - 1) / 3) or g.BORDER
    g.fill_rect(x + (i - 1) * 5, y + 11 - h, 3, h, c)
  end
end

local function ago(ms)
  if ms < 1200 then return "now" end
  return math.floor(ms / 1000) .. "s"
end

-- MACs are all badges, so the vendor half is noise. The last three octets are
-- what tells two of them apart.
local function short_mac(mac)
  return mac:sub(10)
end

local function draw_header()
  g.fill_rect(0, 0, g.width(), 30, g.PANEL)
  g.line(0, 30, g.width(), 30, g.BORDER)
  g.text("WHO'S NEAR", 12, 8, g.WHITE, 2)

  -- The count that matters, and next to it the count that explains it.
  local n = #sorted
  g.text_right(tostring(n), 250, 8, n > 0 and g.SOLANA_GREEN or g.MUTED, 2)
  g.text("running", 256, 4, g.MUTED, 1)
  g.text("of " .. near .. " near", 256, 16, g.MUTED, 1)
end

local function draw_you()
  g.fill_round_rect(10, 36, 300, 24, 4, g.PANEL)
  g.fill_round_rect(16, 41, 26, 14, 3, g.SOLANA_PURPLE)
  g.text("YOU", 20, 45, g.WHITE, 1)
  g.text(badge.device_name:sub(1, NAME_MAX), 50, 45, g.WHITE, 1)
  g.text(short_mac(own_mac), 172, 45, g.MUTED, 1)

  -- Channel lives on the "you" card because it is a property of this badge,
  -- not of the network: peers on another channel are not far away, they are
  -- invisible. Naming the AP when we are joined to one points at the cause.
  local ch = "CH " .. net.channel()
  local ok, joined = pcall(badge.wifi.connected)
  if ok and joined then
    local ok2, ssid = pcall(badge.wifi.ssid)
    if ok2 and ssid and ssid ~= "" then ch = ch .. " wifi:" .. ssid:sub(1, 8) end
  end
  g.text_right(ch, 304, 45, g.SOLANA_TEAL, 1)
end

local function draw_empty()
  local cx = g.width() // 2
  g.text_center("No badges are running this app.", cx, LIST_Y + 16, g.MUTED, 1)
  g.text_center("Nothing is broken -- someone else has to", cx, LIST_Y + 38, g.MUTED, 1)
  g.text_center("open Who's Near too, on channel " .. net.channel() .. ".", cx, LIST_Y + 50, g.MUTED, 1)
  if near > 0 then
    g.text_center("Badges are in range, just not in this app.", cx, LIST_Y + 72, g.BORDER, 1)
  else
    g.text_center("No badges in range at all on this channel.", cx, LIST_Y + 72, g.BORDER, 1)
  end
end

local function draw_rows()
  local now = badge.millis()
  for i = 1, ROWS do
    local e = sorted[scroll + i]
    if not e then break end

    local y = LIST_Y + (i - 1) * ROW_H
    if (scroll + i) % 2 == 1 then
      g.fill_rect(10, y - 3, 300, ROW_H, g.PANEL)
    end

    -- Fade a row towards MUTED as it goes stale, so a peer about to time out
    -- looks like one rather than vanishing without warning.
    local age = now - e.last_ms
    local stale = age > HELLO_MS * 2

    g.text(e.name:sub(1, 13), 16, y + 2, stale and g.MUTED or g.WHITE, 1)
    g.text(short_mac(e.mac), 116, y + 2, g.MUTED, 1)
    g.text_right(math.floor(e.rssi) .. "dBm", 232, y + 2, g.MUTED, 1)
    bars(240, y - 2, e.rssi)
    g.text_right(ago(age), 304, y + 2, stale and g.ORANGE or g.MUTED, 1)
  end
end

function on_draw()
  g.clear(g.BG)
  draw_header()
  draw_you()

  -- Column headings, so the two anonymous numeric columns are readable.
  g.text("PEER", 16, LIST_Y - 12, g.MUTED, 1)
  g.text("MAC", 116, LIST_Y - 12, g.MUTED, 1)
  g.text("SIGNAL", 196, LIST_Y - 12, g.MUTED, 1)
  g.text_right("SEEN", 304, LIST_Y - 12, g.MUTED, 1)
  g.line(10, LIST_Y - 2, 310, LIST_Y - 2, g.BORDER)

  if #sorted == 0 then
    draw_empty()
  else
    draw_rows()
  end

  local hint = "SELECT say hi   CANCEL quit"
  if #sorted > ROWS then
    hint = "UP/DOWN scroll   " .. hint .. "   " ..
           (scroll + 1) .. "-" .. math.min(scroll + ROWS, #sorted) .. "/" .. #sorted
  end
  g.text_center(hint, g.width() // 2, g.height() - 12, g.MUTED, 1)
end

function on_button(key, pressed)
  if not pressed then return end

  if key == "b" then
    badge.system.exit()
  elseif key == "a" then
    -- SELECT: an out-of-band hello. Useful when you have just walked up to someone
    -- and would rather not wait out the interval on both badges.
    hello(true)
    badge.led.pulse(153, 69, 255, 250)
  elseif key == "up" then
    if scroll > 0 then scroll = scroll - 1 end
  elseif key == "down" then
    if scroll < #sorted - ROWS then scroll = scroll + 1 end
  end
end

function on_stop()
  badge.led.off()
end
