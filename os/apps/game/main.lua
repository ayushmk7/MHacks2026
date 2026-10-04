-- Game (docs/os/apps/apps.md, "Game" and "Evil game")
--
-- A small arcade game with a shop, to show that an ordinary app can take payments safely.
--
--   Title   a list: Play, then one "Buy <item>" row for each item of config.shop.items.
--   Play    a dodge game: LEFT/RIGHT move the player along the bottom, blocks fall, the score is
--           the seconds survived, the speed rises. The high score is kept in badge.storage.
--   Shop    SELECT on an item pays the shop with vk.pay; the firmware's approval shows what is
--           really being signed. On "done" the item is unlocked and stored.
--
-- This file is also the evil game: scripts/push-apps.sh copies it into apps/evilgame, whose
-- config.lua adds `evil`. Everything here reads its behaviour from config.lua.
--   evil = "amount"      this screen shows item.price; the transfer is for config.evil_amount
--   evil = "recipient"   the transfer goes to config.evil_recipient, with the real shop's record
-- The honest and the dishonest purchase share one code path (start_purchase): the lie is two
-- options of vk.pay, and the firmware's approval shows the truth either way.
--
-- CANCEL goes back one step and exits from the title.
--
-- Log lines (each "[app] GAME ..."), for test/device/t_app_game.py:
--   GAME title                 the title screen is showing
--   GAME play                  a run started
--   GAME score <n>             once a second while playing: seconds survived
--   GAME over <n>              the run ended with that score
--   GAME shop <id> <price>     a purchase started (the price is the one this screen shows)
--   GAME buy done <id>         paid and unlocked
--   GAME buy failed <reason>
--   GAME buy dropped           CANCEL before the approval opened
--
-- The look and the payer flow come from lib/vk.lua (pushed into this folder as vk.lua by
-- scripts/push-apps.sh). The playfield is drawn with badge.gfx in the theme's colours.

local cfg = require("config")
local vk = require("vk")
local ui, gfx, input, wallet = vk.ui, badge.gfx, badge.input, badge.wallet
local text = cfg.text
local items = cfg.shop.items

local ARROW = "\xE2\x96\xB8"              -- the kit draws the small triangle
local DOT = " \xC2\xB7 "
local FIELD_TOP = ui.CONTENT_Y            -- the playfield: under the header's rule ...
local FIELD_BOTTOM = 212                  -- ... and above the footer's (y 216)
local LINE_COLUMNS = 50                   -- a centred line of body text that fits the screen

local mode = "title"                      -- title | play | over | buy
local sel = 1                             -- title: row 1 is Play, row i + 1 is items[i]
local rows = {}

local best = 0                            -- the high score, seconds
local owned = {}                          -- item id -> true
local unit = ""                           -- the token's symbol, for the price on the shop screen

local px, blocks, elapsed, score, lives, spawn_in = 0, {}, 0, 0, 0, 0
local over_until, new_best = 0, false

local flow = nil                          -- the running purchase (vk.pay)
local buying = nil                        -- its item
local result = nil                        -- {ok =, line =}: how the last purchase ended, until seen

-- ---------------------------------------------------------------------------------------------
-- Storage: badge.storage.kv, strings only. A failure never stops the game.
-- ---------------------------------------------------------------------------------------------

local kv = badge.storage and badge.storage.kv

local function load(key)
  if not kv then return nil end
  local ok, value = pcall(kv.get, key)
  if ok then return value end
  return nil
end

local function save(key, value)
  if kv then pcall(kv.set, key, value) end
end

local function owned_key(item)
  return "o" .. tostring(item.id)
end

-- ---------------------------------------------------------------------------------------------
-- Small helpers
-- ---------------------------------------------------------------------------------------------

local function fit(line)
  line = tostring(line)
  if #line <= LINE_COLUMNS then return line end
  return line:sub(1, LINE_COLUMNS - 2) .. ".."
end

local function reason_text(reason)
  reason = tostring(reason or "failed")
  return cfg.reasons[reason] or reason
end

-- An LED flash in a colour of the active theme (RGB565 -> 8-bit components).
local function flash(spec)
  local c = ui.color(spec.color)
  badge.led.pulse((c // 2048) * 255 // 31, ((c // 32) % 64) * 255 // 63, (c % 32) * 255 // 31, spec.ms)
end

local function price_text(item)
  if unit == "" then return item.price end
  return item.price .. " " .. unit
end

-- What the unlocked items give: the extra hits a run survives, and the player's colour.
local function extra_lives()
  local n = 0
  for i = 1, #items do
    if items[i].effect == "life" and owned[items[i].id] then n = n + 1 end
  end
  return n
end

local function player_color()
  local token = cfg.colors.player
  for i = 1, #items do
    if items[i].effect == "color" and owned[items[i].id] then token = items[i].color end
  end
  return ui.color(token)
end

-- ---------------------------------------------------------------------------------------------
-- Title
-- ---------------------------------------------------------------------------------------------

local function build_rows()
  rows = {{l = text.play, sub = string.format(text.best, best), r = ARROW}}
  for i = 1, #items do
    local item = items[i]
    local has = owned[item.id]
    rows[#rows + 1] = {
      l = string.format(text.buy, item.name),
      sub = (has and text.owned or text.shop) .. DOT .. tostring(item.note or ""),
      r = item.price,
      tone = has and "mut" or nil,
    }
  end
end

-- The title, or first the result of a purchase that ended while another screen was showing.
local function enter_title()
  if result then
    mode = "buy"
    return
  end
  mode = "title"
  build_rows()
  badge.log("GAME title")
end

-- ---------------------------------------------------------------------------------------------
-- Shop
-- ---------------------------------------------------------------------------------------------

-- The one place a payment is started. The screen always shows item.price; an evil config makes
-- the transfer differ from it, and the firmware's approval shows the difference.
local function start_purchase(item)
  local opts = {to = cfg.shop.recipient, amount = item.price, symbol = cfg.shop.symbol, memo = item.name}
  if cfg.evil == "amount" then
    opts.amount = cfg.evil_amount               -- signed for this, whatever the screen says
  elseif cfg.evil == "recipient" then
    opts.destination = cfg.evil_recipient       -- paid here, under the real shop's record
  end
  return vk.pay.start(opts)
end

local function open_shop(item)
  if flow then                                  -- one purchase at a time: show the running one
    mode = "buy"
    return
  end
  if owned[item.id] then return end
  buying, result = item, nil
  flow = start_purchase(item)
  mode = "buy"
  badge.log("GAME shop " .. tostring(item.id) .. " " .. item.price)
end

local function unlock(item)
  owned[item.id] = true
  save(owned_key(item), "1")
end

local function shop_update()
  local state, detail = flow:update()
  if state == "done" then
    unlock(buying)
    badge.log("GAME buy done " .. tostring(buying.id))
    result = {ok = true, line = string.format(text.bought, buying.name)}
    flash(cfg.bought_led)
    flow = nil
  elseif state == "failed" then
    badge.log("GAME buy failed " .. tostring(detail))
    result = {ok = false, line = string.format(text.not_bought, reason_text(detail))}
    flow = nil
  end
  if result and mode == "title" then mode = "buy" end
end

-- CANCEL on the shop screen. Before the approval has opened the purchase is dropped; after it
-- (signed, being sent or confirmed) it goes on behind the title and its result is shown later.
local function leave_shop()
  if result then
    result, buying = nil, nil
  elseif flow and (flow.state == "record" or flow.state == "blockhash") then
    flow, buying = nil, nil
    badge.log("GAME buy dropped")
  end
  enter_title()
end

local function draw_shop()
  ui.page()
  ui.header(cfg.header)
  ui.title(text.shop_title, ui.TITLE_Y)
  if buying then
    ui.text_center(fit(string.format(text.offer, buying.name, price_text(buying))), ui.W // 2, 64)
  end
  local state
  if result then
    state = result.ok and "done" or "failed"
  else
    state = flow and (flow.ready and flow.state or "start") or "failed"
  end
  ui.row(100, text.status, cfg.states[state] or state)
  if result then
    ui.text_center(fit(result.line), ui.W // 2, 132, ui.color(result.ok and "stamp_ok" or "stamp_bad"))
  end
  ui.footer(result and text.ok or "", text.back)
end

-- ---------------------------------------------------------------------------------------------
-- Play
-- ---------------------------------------------------------------------------------------------

local function start_play()
  mode = "play"
  px = (ui.W - cfg.player.w) / 2
  blocks = {}
  elapsed, score, spawn_in = 0, 0, 0
  lives = extra_lives()
  badge.log("GAME play")
end

local function end_run()
  new_best = score > best
  if new_best then
    best = score
    save("best", tostring(best))
  end
  badge.log("GAME over " .. score)
end

local function spawn()
  local w = cfg.block.w
  local x
  if math.random(100) <= cfg.spawn.aim_percent then
    x = px + (cfg.player.w - w) / 2             -- straight at the player
  else
    x = math.random(0, ui.W - w)
  end
  blocks[#blocks + 1] = {x = math.max(0, math.min(ui.W - w, x)), y = FIELD_TOP}
end

local function play_update(dt)
  if dt > 0.1 then dt = 0.1 end                 -- after a pause (the approval), do not jump
  elapsed = elapsed + dt

  local move = 0
  if input.down("left") then move = move - 1 end
  if input.down("right") then move = move + 1 end
  px = math.max(0, math.min(ui.W - cfg.player.w, px + move * cfg.player.speed * dt))

  spawn_in = spawn_in - dt * 1000
  if spawn_in <= 0 then
    spawn()
    spawn_in = math.max(cfg.spawn.min_ms, cfg.spawn.start_ms - cfg.spawn.gain_ms * elapsed)
  end

  local fall = math.min(cfg.speed.max, cfg.speed.start + cfg.speed.gain * elapsed) * dt
  local top = FIELD_BOTTOM - cfg.player.h
  local bw, bh = cfg.block.w, cfg.block.h
  for i = #blocks, 1, -1 do
    local b = blocks[i]
    b.y = b.y + fall
    if b.y >= FIELD_BOTTOM then
      table.remove(blocks, i)
    elseif b.y + bh >= top and b.x < px + cfg.player.w and b.x + bw > px then
      table.remove(blocks, i)
      flash(cfg.hit_led)
      lives = lives - 1
      if lives < 0 then
        end_run()
        mode = "over"
        over_until = badge.millis() + cfg.over_ms
        return
      end
    end
  end

  local seconds = math.floor(elapsed)
  if seconds > score then
    score = seconds
    badge.log("GAME score " .. score)
  end
end

local function draw_play()
  ui.page()
  ui.header(string.format(text.seconds, cfg.title:upper(), score))
  local color, bw, bh = ui.color(cfg.colors.block), cfg.block.w, cfg.block.h
  for i = 1, #blocks do
    local b = blocks[i]
    local y = math.floor(b.y)
    gfx.fill_rect(math.floor(b.x), y, bw, math.min(bh, FIELD_BOTTOM - y), color)
  end
  gfx.fill_rect(math.floor(px), FIELD_BOTTOM - cfg.player.h, cfg.player.w, cfg.player.h, player_color())
  ui.footer(string.format(text.lives, math.max(0, lives)), text.quit)
end

local function draw_over()
  ui.page()
  ui.header(cfg.header)
  ui.title(text.over, ui.TITLE_Y)
  ui.amount(ui.W // 2, 70, text.survived, tostring(score), text.seconds_unit)
  ui.row(160, text.best_row, new_best and (best .. DOT .. text.new_best) or tostring(best))
  ui.footer(text.again, text.back)
end

-- ---------------------------------------------------------------------------------------------
-- Callbacks
-- ---------------------------------------------------------------------------------------------

function on_start()
  math.randomseed(badge.millis())
  best = math.tointeger(tonumber(load("best") or "") or 0) or 0
  for i = 1, #items do
    owned[items[i].id] = load(owned_key(items[i])) == "1" or nil
  end
  local tokens = wallet.tokens() or {}
  unit = cfg.shop.symbol or (tokens[1] and tokens[1].symbol) or ""
  enter_title()
end

function on_update(dt)
  if flow then shop_update() end
  if mode == "play" then
    play_update(dt)
  elseif mode == "over" and badge.millis() >= over_until then
    enter_title()
  end
end

function on_draw()
  if mode == "play" then return draw_play() end
  if mode == "over" then return draw_over() end
  if mode == "buy" then return draw_shop() end
  ui.list{header = cfg.header, title = cfg.title, rows = rows, sel = sel, hint = text.hint, back = text.exit}
end

function on_button(key, pressed)
  if not pressed then return end
  if mode == "play" then
    if key == "b" then                          -- quit the run; the score still counts
      end_run()
      enter_title()
    end
  elseif mode == "over" then
    if key == "a" or key == "b" then enter_title() end
  elseif mode == "buy" then
    if key == "b" or (key == "a" and result) then leave_shop() end
  else
    if key == "b" then badge.system.exit()
    elseif key == "up" and sel > 1 then sel = sel - 1
    elseif key == "down" and sel <= #items then sel = sel + 1
    elseif key == "a" then
      if sel == 1 then start_play() else open_shop(items[sel - 1]) end
    end
  end
end

-- Draw only when the screen changed (lib/vk.lua, "ui.frame"): a frame every pass would hold the
-- loop at 20 passes a second.
local draw_frame = on_draw
function on_draw() ui.frame(draw_frame, mode == "play" and 0 or nil) end   -- the playfield animates
