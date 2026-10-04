-- Gallery
--
-- Shows every image in the app's own directory, one at a time. Nothing is
-- hardcoded: the list is storage.list() filtered by extension and rescanned a
-- couple of times a second, so a picture pushed while the app is running just
-- turns up.
--
--   tools/badge-push.py --host <ip> --token <code> push apps/gallery
--
-- Two things here are worth stealing for other apps.
--
-- It only draws when something changed. The framebuffer survives between frames
-- and the runtime's flush is a no-op unless an app touched it, so returning
-- early from on_draw() leaves the previous frame on the panel for free. That
-- matters here because gfx.image() decodes the file every time it is called;
-- doing that sixty times a second for a picture that is not moving would spend
-- the whole frame budget redrawing the same pixels.
--
-- And gfx.image() draws but does not measure, so centring and fitting get the
-- pixel size from gfx.image_size(), which reads the header only. Cached per
-- file, since the answer cannot change while the app is running.

local g = badge.gfx

local W, H = g.width(), g.height()
local BAR_H = 36                 -- the caption strip along the bottom
local AREA_H = H - BAR_H         -- everything above it belongs to the picture

local SCAN_INTERVAL = 2.0        -- seconds between storage.list() sweeps

-- A "fit" step needs the image size, so it falls back to 1x when the header
-- could not be read.
local ZOOMS = {
  { label = "1x",  scale = 1 },
  { label = "2x",  scale = 2 },
  { label = "fit", scale = nil },
}

local files = {}        -- sorted image names in the app directory
local index = 1         -- 1-based selection into files
local zoom = 1          -- index into ZOOMS
local sizes = {}        -- name -> {w=, h=}, or false once a read has failed
local dirty = true      -- redraw this frame?
local measure = true    -- read the current file's header this frame?
local scan_timer = 0

-- ---------------------------------------------------------------------------
-- Discovery

local function is_image(name)
  if name:sub(-1) == "/" then return false end     -- storage.list marks dirs
  local lower = name:lower()
  return lower:sub(-4) == ".png"
      or lower:sub(-4) == ".jpg"
      or lower:sub(-5) == ".jpeg"
end

local function rescan()
  local names = badge.storage.list()
  if not names then return end

  local found = {}
  for _, name in ipairs(names) do
    if is_image(name) then found[#found + 1] = name end
  end
  table.sort(found)

  -- Bail out unless the set actually changed, so the common case - nothing
  -- pushed since the last sweep - costs one comparison and no redraw.
  local same = #found == #files
  if same then
    for i = 1, #found do
      if found[i] ~= files[i] then
        same = false
        break
      end
    end
  end
  if same then return end

  -- Stay on whatever was on screen if it is still there.
  local showing = files[index]
  files = found
  index = 1
  for i, name in ipairs(files) do
    if name == showing then index = i end
  end

  badge.log("gallery: " .. #files .. " image(s)")
  measure, dirty = true, true
end

-- ---------------------------------------------------------------------------
-- Measuring
--
-- gfx.image_size reads only the file header, so this costs a few dozen bytes
-- rather than a full decode - which is why it is safe to call on the largest
-- image the badge will accept. Result is cached per file: `false` records a
-- miss so a file that cannot be measured is not retried on every page turn.
local function measure_current()
  local name = files[index]
  if name == nil or sizes[name] ~= nil then return end

  local w, h = badge.gfx.image_size(name)
  if w and h and w > 0 and h > 0 then
    sizes[name] = { w = w, h = h }
  else
    sizes[name] = false
  end
end

-- ---------------------------------------------------------------------------
-- Drawing

local function scale_for(size)
  local step = ZOOMS[zoom]
  if step.scale then return step.scale end
  if not size then return 1 end                        -- "fit" needs the size
  local fit = math.min(W / size.w, AREA_H / size.h)
  return math.max(0.05, math.min(8, fit))
end

-- Cuts a string down until it fits, with an ellipsis to say it was cut.
local function ellipsise(text, limit)
  if g.text_width(text) <= limit then return text end
  while #text > 1 and g.text_width(text .. "..") > limit do
    text = text:sub(1, #text - 1)
  end
  return text .. ".."
end

local function draw_picture(name)
  local size = sizes[name] or nil        -- false (unmeasured) becomes nil
  local scale = scale_for(size)

  -- Unmeasured images go in the top-left corner: guessing at a centre would
  -- move the picture the moment the guess was wrong.
  local x, y = 0, 0
  if size then
    x = math.floor((W - size.w * scale) / 2)
    y = math.floor((AREA_H - size.h * scale) / 2)
  end

  -- Every failure - missing, empty, oversized, undecodable - comes back as
  -- false plus a reason, so no pcall is needed. The reason is logged rather
  -- than drawn: the error card has room for the filename and not much else, and
  -- "file is empty (interrupted push?)" is exactly the kind of thing worth
  -- having in the console when a push went wrong.
  local drew, reason = g.image(name, x, y, scale)
  if not drew then
    badge.log("gallery: " .. name .. ": " .. tostring(reason))
  end
  return drew
end

local function draw_error(name)
  local w, h = 268, 92
  local x, y = (W - w) // 2, (AREA_H - h) // 2

  -- A decoder that returns false may already have painted part of a frame, so
  -- start over rather than leaving half an image behind the card.
  g.clear(g.BG)
  g.fill_round_rect(x, y, w, h, 6, g.PANEL)
  g.round_rect(x, y, w, h, 6, g.RED)
  g.text_center("CANNOT DECODE", W // 2, y + 16, g.RED, 1)
  g.text_center(ellipsise(name, w - 24), W // 2, y + 40, g.WHITE, 1)
  g.text_center("PNG or baseline JPEG only,", W // 2, y + 60, g.MUTED, 1)
  g.text_center("up to 256 KB", W // 2, y + 72, g.MUTED, 1)
end

local function draw_empty()
  g.text_center("NO IMAGES", W // 2, 44, g.WHITE, 2)
  g.text_center("this app shows whatever is next to it", W // 2, 74, g.MUTED, 1)

  local x, y, w, h = 20, 96, W - 40, 76
  g.fill_round_rect(x, y, w, h, 6, g.PANEL)
  g.round_rect(x, y, w, h, 6, g.BORDER)
  g.text_center("push a .png or .jpg into apps/gallery", W // 2, y + 10, g.MUTED, 1)
  g.text_center("tools/badge-push.py \\", W // 2, y + 30, g.SOLANA_GREEN, 1)
  g.text_center("--host <ip> --token <code> \\", W // 2, y + 42, g.SOLANA_GREEN, 1)
  g.text_center("push apps/gallery", W // 2, y + 54, g.SOLANA_GREEN, 1)
end

-- The caption strip. Drawn last and opaque, so an image scaled past the bottom
-- of the screen is cropped by it rather than fighting with it.
local function draw_caption()
  g.fill_rect(0, AREA_H, W, BAR_H, g.PANEL)
  g.line(0, AREA_H, W - 1, AREA_H, g.BORDER)

  local counter = (#files == 0 and 0 or index) .. " / " .. #files
  g.text_right(counter, W - 10, AREA_H + 8, g.SOLANA_GREEN, 1)

  local name = files[index]
  if name then
    -- Whatever is left after the counter and a gap belongs to the filename.
    local room = W - 20 - g.text_width(counter) - 12
    local size = sizes[name] or nil
    if size then
      name = name .. string.format("  %dx%d", size.w, size.h)
    end
    g.text(ellipsise(name, room), 10, AREA_H + 8, g.WHITE, 1)
  end

  -- Not "hold CANCEL": on_button exits on the press. Holding is the OS-level
  -- force-quit, which is for apps that trap the key, and this one does not.
  g.text(string.format("LEFT/RIGHT page   SELECT zoom (%s)   CANCEL quit", ZOOMS[zoom].label),
         10, AREA_H + 22, g.MUTED, 1)
end

-- ---------------------------------------------------------------------------
-- Lifecycle

function on_start()
  badge.led.take()          -- otherwise the launcher keeps breathing over us
  rescan()
  measure_current()
  measure = false
end

function on_update(dt)
  -- Reading a file and decoding one are the two expensive things this app
  -- does, and each callback has its own 250 ms budget - so the read happens
  -- here and the decode happens in on_draw, never in the same callback.
  if measure then
    measure = false
    measure_current()
    dirty = true
    return
  end

  scan_timer = scan_timer + dt
  if scan_timer >= SCAN_INTERVAL then
    scan_timer = 0
    rescan()
  end
end

function on_draw()
  if not dirty then return end     -- the panel still holds the last frame
  dirty = false

  g.clear(g.BG)

  local name = files[index]
  if name == nil then
    draw_empty()
  elseif not draw_picture(name) then
    draw_error(name)
  end
  draw_caption()

  -- The LEDs walk the brand ramp as you page through, so you can feel where
  -- you are in the set without reading the counter.
  local t = 0
  if #files > 1 then t = (index - 1) / (#files - 1) end
  badge.led.gradient(0, t, 0.5)
  badge.led.gradient(1, t, 0.5)
  badge.led.show()
end

local function step(delta)
  if #files == 0 then return end
  index = (index - 1 + delta) % #files + 1
  measure, dirty = true, true
end

function on_button(key, pressed)
  if not pressed then return end

  if key == "left" or key == "up" then
    step(-1)
  elseif key == "right" or key == "down" then
    step(1)
  elseif key == "a" then
    zoom = zoom % #ZOOMS + 1
    dirty = true
  elseif key == "b" then
    badge.system.exit()
  end
end

function on_stop()
  badge.led.off()
end
