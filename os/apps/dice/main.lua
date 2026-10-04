-- Dice & Deck
--
-- A dice roller and a shuffled 52-card deck, both driven by real entropy.
--
-- The point of this app is the randomness, not the pictures. Two things it is
-- careful about, and both are easy to get wrong:
--
--   1. Where the bytes come from. The SE050 secure element has a certified
--      hardware RNG; the ESP32-S3 has its own hardware TRNG. Either is fine,
--      but which one is actually live is never guessed at -- it is probed at
--      start, re-checked on every failure, and printed on screen in its own
--      colour. If both were to fail we drop to math.random, which is a
--      deterministic PRNG with a fixed seed, and the banner says so in red.
--      Predictable dice that *look* random are worse than no dice.
--
--   2. How bytes become numbers. See rand_below(): `byte % n` is biased and is
--      not used anywhere here.

local g = badge.gfx

-- ===========================================================================
-- Entropy
-- ===========================================================================

local SRC_SE050, SRC_TRNG, SRC_PRNG = 1, 2, 3

local SOURCE_NAME = {[SRC_SE050] = "SE050",
                     [SRC_TRNG]  = "ESP32 TRNG",
                     [SRC_PRNG]  = "PRNG (unseeded)"}

local source = SRC_PRNG

-- Bytes are pulled in blocks: an SE050 exchange is a multi-frame I2C
-- conversation and doing one per die would be slow enough to feel like a bug.
-- 64 covers a whole 52-card shuffle in two refills.
local CHUNK = 64

local pool = ""
local pool_pos = 1

local function source_name() return SOURCE_NAME[source] end

local function source_color()
  if source == SRC_SE050 then return g.SOLANA_GREEN end
  if source == SRC_TRNG then return g.CYAN end
  return g.RED  -- error colour: these numbers are predictable
end

-- A demotion is permanent for the session. Retrying the secure element on every
-- refill would cost a failing I2C round trip per handful of bytes, and the
-- banner would flicker between sources while the app tried to make up its mind.
local function demote(reason)
  if source == SRC_SE050 then
    badge.log("se050 random: " .. reason .. "; using ESP32 TRNG")
    source = SRC_TRNG
  elseif source == SRC_TRNG then
    badge.log("ESP32 TRNG: " .. reason .. "; falling back to math.random")
    source = SRC_PRNG
  end
end

-- A block of identical bytes is not entropy, whatever the source claims. It has
-- to be caught here rather than shrugged off downstream, because rand_below()
-- answers an out-of-range byte by drawing another one -- and a pool of, say, all
-- 0xFF puts every draw out of range on a d20. That loop never ends, every lap
-- refills from the same dead source, and because badge.se050.random() pushes the
-- callback deadline out on each call the runtime's own watchdog never fires
-- either. The badge stops responding until it is reset.
--
-- 0xFF specifically is what the SE050 sends while it has nothing to say, so a
-- part that answers the GetRandom framing without actually producing bytes is
-- the likely way to arrive here rather than a hypothetical one.
--
-- With a live source the odds of a false positive are 256^-63.
local function is_flat(bytes)
  local first = string.byte(bytes, 1)
  for i = 2, #bytes do
    if string.byte(bytes, i) ~= first then return false end
  end
  return true
end

local function why_bad(bytes, reason)
  if not bytes then return "failed (" .. tostring(reason) .. ")" end
  if #bytes ~= CHUNK then return "returned " .. #bytes .. " of " .. CHUNK .. " bytes" end
  return "returned a constant block"
end

-- Refills the byte pool, demoting the source if the current one refuses or is
-- plainly not producing random bytes.
local function refill()
  if source == SRC_SE050 then
    local bytes, reason = badge.se050.random(CHUNK)
    if bytes and #bytes == CHUNK and not is_flat(bytes) then
      pool, pool_pos = bytes, 1
      return
    end
    demote(why_bad(bytes, reason))
  end

  if source == SRC_TRNG then
    local bytes = badge.system.random_bytes(CHUNK)
    if bytes and #bytes == CHUNK and not is_flat(bytes) then
      pool, pool_pos = bytes, 1
      return
    end
    demote(why_bad(bytes, "no bytes"))
  end

  -- Last resort. Deterministic, unseeded, and labelled as such on screen.
  local parts = {}
  for i = 1, CHUNK do parts[i] = string.char(math.random(0, 255)) end
  pool, pool_pos = table.concat(parts), 1
end

local function next_byte()
  if pool_pos > #pool then refill() end
  local b = string.byte(pool, pool_pos)
  pool_pos = pool_pos + 1
  return b
end

-- Uniform integer in [0, n-1], for n <= 256.
--
-- The obvious `next_byte() % n` is modulo-biased whenever n does not divide
-- 256. With n = 100 the 256 possible bytes split into two full runs of 100 and
-- a ragged tail of 56, so results 0..55 occur three times per 256 draws and
-- 56..99 only twice -- the low faces come up 50% more often than the high ones.
-- On a d20 the effect is smaller but still there.
--
-- Rejection sampling removes it exactly: keep only the bytes below the largest
-- multiple of n that fits in a byte, and re-draw the rest. The cost is a few
-- wasted bytes (never more than 22% of them, at n = 129..255) and no bias at
-- all. It is not an approximation.
--
-- The re-draw is bounded rather than a `while true`. Rejection sampling only
-- terminates because the source keeps offering different bytes, so an unbounded
-- loop here quietly makes "the entropy source is stuck" mean "the badge is
-- stuck" - see the note on is_flat(). At the worst n, 40 consecutive rejections
-- has probability (127/256)^40, about 1e-12, so tripping this on a live source
-- is not something that happens.
local MAX_REDRAWS = 40

local function rand_below(n)
  if n <= 1 then return 0 end
  local limit = 256 - (256 % n)
  for _ = 1, MAX_REDRAWS do
    local b = next_byte()
    if b < limit then return b % n end
  end

  -- The source is stuck in the rejection window. Drop to the one generator that
  -- cannot do this, and let the banner turn red and say so.
  demote("every draw landed in the rejection window")
  pool, pool_pos = "", 1
  return math.random(0, n - 1)
end

-- Fisher-Yates, the modern in-place form. Walks from the top down, swapping
-- each element with one drawn uniformly from the part not yet visited -- which
-- is what makes every one of the 52! orderings equally likely. Drawing j from
-- the *whole* array instead (the classic off-by-one bug) does not.
local function shuffle(list)
  for i = #list, 2, -1 do
    local j = rand_below(i) + 1
    list[i], list[j] = list[j], list[i]
  end
  return list
end

-- ===========================================================================
-- Dice
-- ===========================================================================

local DICE = {4, 6, 8, 10, 12, 20, 100}
local die_index = 6      -- d20
local die_count = 1
local rolls = {}
local roll_total = 0

local function roll_dice()
  local faces = DICE[die_index]
  rolls = {}
  roll_total = 0
  for i = 1, die_count do
    local value = rand_below(faces) + 1
    rolls[i] = value
    roll_total = roll_total + value
  end
end

-- ===========================================================================
-- Cards
-- ===========================================================================

local RANKS = {"A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"}
local SUITS = {"spade", "heart", "diamond", "club"}

local deck = {}
local drawn = nil

-- One shuffle, then deal off the top. Re-shuffling before each draw would let
-- the same card come out twice and would make "37 remaining" a lie.
local function new_deck()
  deck = {}
  for suit = 1, 4 do
    for rank = 1, 13 do
      deck[#deck + 1] = {rank = rank, suit = suit}
    end
  end
  shuffle(deck)
  drawn = nil
end

local function draw_card()
  if #deck == 0 then return false end
  drawn = table.remove(deck)
  return true
end

local function card_is_red(card)
  return card.suit == 2 or card.suit == 3  -- hearts, diamonds
end

-- ===========================================================================
-- Drawing
-- ===========================================================================

local MODE_DICE, MODE_CARDS = 1, 2
local mode = MODE_DICE
local t = 0

-- The built-in font is ASCII, so the four suits are drawn out of primitives.
local function draw_suit(suit, cx, cy, r, color)
  if suit == 1 or suit == 4 then
    -- Spade and club share a stem.
    g.fill_triangle(cx - r // 2, cy + r, cx + r // 2, cy + r, cx, cy + r // 3, color)
    g.fill_rect(cx - r // 6, cy + r // 3, r // 3 + 1, r // 2, color)
  end

  if suit == 1 then           -- spade: an inverted heart over the stem
    g.fill_triangle(cx - r, cy + r // 4, cx + r, cy + r // 4, cx, cy - r, color)
    g.fill_circle(cx - r // 2, cy + r // 5, r // 2, color)
    g.fill_circle(cx + r // 2, cy + r // 5, r // 2, color)
  elseif suit == 2 then       -- heart
    g.fill_triangle(cx - r, cy - r // 4, cx + r, cy - r // 4, cx, cy + r, color)
    g.fill_circle(cx - r // 2, cy - r // 4, r // 2, color)
    g.fill_circle(cx + r // 2, cy - r // 4, r // 2, color)
  elseif suit == 3 then       -- diamond
    local w = (r * 7) // 10
    g.fill_triangle(cx, cy - r, cx - w, cy, cx + w, cy, color)
    g.fill_triangle(cx, cy + r, cx - w, cy, cx + w, cy, color)
  else                        -- club
    g.fill_circle(cx, cy - r // 2, r // 2, color)
    g.fill_circle(cx - r // 2, cy + r // 6, r // 2, color)
    g.fill_circle(cx + r // 2, cy + r // 6, r // 2, color)
  end
end

-- Pip positions on a 3x3 grid, as {column, row} pairs with 0,0 top-left.
local PIPS = {
  {{1, 1}},
  {{0, 0}, {2, 2}},
  {{0, 0}, {1, 1}, {2, 2}},
  {{0, 0}, {2, 0}, {0, 2}, {2, 2}},
  {{0, 0}, {2, 0}, {1, 1}, {0, 2}, {2, 2}},
  {{0, 0}, {2, 0}, {0, 1}, {2, 1}, {0, 2}, {2, 2}},
}

local function draw_die(x, y, size, value, faces)
  -- A natural 1 or a natural max is worth seeing at a glance.
  local edge = g.BORDER
  if value == faces then edge = g.SOLANA_GREEN
  elseif value == 1 then edge = g.RED end

  g.fill_round_rect(x, y, size, size, 7, g.WHITE)
  g.round_rect(x, y, size, size, 7, edge)
  g.round_rect(x + 1, y + 1, size - 2, size - 2, 6, edge)

  if faces == 6 then
    -- Real pips, laid out on the 3x3 grid the face of a die actually uses.
    local radius = size // 10
    local step = (size - 2 * (radius + 5)) // 2
    local origin = x + radius + 5
    for _, pip in ipairs(PIPS[value]) do
      g.fill_circle(origin + pip[1] * step, y + radius + 5 + pip[2] * step, radius, g.BLACK)
    end
  else
    local label = tostring(value)
    local scale = (#label > 2) and 2 or 3
    g.text_center(label, x + size // 2, y + (size - g.text_height(scale)) // 2, g.BLACK, scale)
  end
end

local function draw_header()
  for x = 0, g.width() - 1 do
    g.line(x, 0, x, 2, g.gradient(x / g.width()))
  end

  -- The entropy banner is permanent, in both modes. It is the only way to tell
  -- from the outside which RNG produced the numbers on screen.
  local color = source_color()
  local label = source_name()
  local w = g.text_width(label) + 12
  local x = g.width() - w - 6
  g.fill_round_rect(x, 8, w, 16, 4, g.PANEL)
  g.round_rect(x, 8, w, 16, 4, color)
  g.text_center(label, x + w // 2, 12, color)
  g.text("ENTROPY", x - g.text_width("ENTROPY") - 8, 12, g.MUTED)

  g.text("DICE & DECK", 8, 12, g.WHITE)
end

local function draw_tabs()
  local names = {"DICE", "CARDS"}
  for i = 1, 2 do
    local w, x = 74, 8 + (i - 1) * 80
    if mode == i then
      g.fill_round_rect(x, 30, w, 18, 4, g.SOLANA_PURPLE)
      g.text_center(names[i], x + w // 2, 35, g.WHITE)
    else
      g.round_rect(x, 30, w, 18, 4, g.BORDER)
      g.text_center(names[i], x + w // 2, 35, g.MUTED)
    end
  end
  g.text_right("LEFT/RIGHT mode", g.width() - 8, 35, g.MUTED)
end

local function draw_dice_mode()
  local faces = DICE[die_index]
  g.text(die_count .. " x d" .. faces, 10, 56, g.SOLANA_TEAL, 2)

  if #rolls == 0 then
    g.text_center("press SELECT to roll", g.width() // 2, 120, g.MUTED, 2)
    return
  end

  -- Up to six dice, three to a row, each row centred on its own.
  local size, gap = 56, 14
  local rows = (#rolls + 2) // 3
  local top = 78 + (2 - rows) * 14
  for i = 1, #rolls do
    local row = (i - 1) // 3
    local in_row = math.min(#rolls - row * 3, 3)
    local col = (i - 1) % 3
    local span = in_row * size + (in_row - 1) * gap
    local x = (g.width() - span) // 2 + col * (size + gap)
    draw_die(x, top + row * (size + gap), size, rolls[i], faces)
  end

  if #rolls > 1 then
    g.text_right("TOTAL " .. roll_total, g.width() - 10, 56, g.SOLANA_GREEN, 2)
  end
end

local function draw_card_face(x, y, w, h, card)
  local ink = card_is_red(card) and g.RED or g.BLACK
  g.fill_round_rect(x, y, w, h, 8, g.WHITE)
  g.round_rect(x, y, w, h, 8, g.BORDER)

  local rank = RANKS[card.rank]
  g.text(rank, x + 7, y + 7, ink, 2)
  draw_suit(card.suit, x + 13, y + 32, 6, ink)

  draw_suit(card.suit, x + w // 2, y + h // 2, 22, ink)

  g.text_right(rank, x + w - 7, y + h - 23, ink, 2)
  draw_suit(card.suit, x + w - 13, y + h - 32, 6, ink)
end

local function draw_card_back(x, y, w, h)
  g.fill_round_rect(x, y, w, h, 8, g.SOLANA_PURPLE)
  g.round_rect(x, y, w, h, 8, g.SOLANA_TEAL)
  for i = 0, 5 do
    local phase = (t * 0.6 + i * 0.16) % 1
    g.line(x + 6, y + 12 + i * 20, x + w - 6, y + 12 + i * 20, g.gradient(phase))
  end
end

local function draw_cards_mode()
  local w, h = 100, 138
  local x, y = (g.width() - w) // 2, 58

  if drawn then
    draw_card_face(x, y, w, h, drawn)
  elseif #deck == 0 then
    draw_card_back(x, y, w, h)
  else
    draw_card_back(x, y, w, h)
    g.text_center("SELECT", g.width() // 2, y + h // 2 - 8, g.WHITE, 2)
  end

  -- Remaining, as a number and as a bar. The bar only moves because the deck
  -- was shuffled once and is being dealt from -- that is the whole point.
  local left = #deck
  g.text(left .. " / 52 left", 10, 206, left == 0 and g.ORANGE or g.WHITE)
  local bar = (300 * left) // 52
  g.fill_rect(10, 220, 300, 4, g.PANEL)
  if bar > 0 then g.fill_rect(10, 220, bar, 4, g.SOLANA_GREEN) end

  if left == 0 then
    g.text_right("UP to reshuffle", g.width() - 10, 206, g.ORANGE)
  end
end

local function draw_hints()
  if mode == MODE_DICE then
    g.text_center("SELECT roll   UP die   DOWN count   CANCEL exit", g.width() // 2, 228, g.MUTED)
  else
    g.text_center("SELECT draw   UP reshuffle   CANCEL exit", g.width() // 2, 228, g.MUTED)
  end
end

-- ===========================================================================
-- Lifecycle
-- ===========================================================================

function on_start()
  badge.led.take()

  -- Probe once, up front, so the banner is honest before the first roll rather
  -- than after it. on_start gets a 5 s budget, which is plenty for one refill.
  if badge.se050 and badge.se050.random_available and badge.se050.random_available() then
    source = SRC_SE050
  else
    source = SRC_TRNG
  end
  refill()
  badge.log("entropy source: " .. source_name())

  new_deck()
end

function on_update(dt)
  t = t + dt
end

function on_draw()
  g.clear(g.BG)
  draw_header()
  draw_tabs()
  if mode == MODE_DICE then draw_dice_mode() else draw_cards_mode() end
  draw_hints()
end

function on_button(key, pressed)
  if not pressed then return end

  if key == "b" then
    badge.system.exit()
  elseif key == "left" then
    mode = MODE_DICE
  elseif key == "right" then
    mode = MODE_CARDS
  elseif key == "a" then
    if mode == MODE_DICE then
      roll_dice()
      badge.led.pulse(153, 69, 255, 220)      -- Solana purple
    elseif draw_card() then
      local red = card_is_red(drawn)
      badge.led.pulse(red and 255 or 20, red and 40 or 241, red and 40 or 149, 220)
    end
  elseif key == "up" then
    if mode == MODE_DICE then
      die_index = die_index % #DICE + 1
      rolls = {}
    else
      new_deck()
      badge.led.pulse(20, 241, 149, 220)
    end
  elseif key == "down" then
    if mode == MODE_DICE then
      die_count = die_count % 6 + 1
      rolls = {}
    end
  end
end

function on_stop()
  badge.led.off()
end
