-- History
--
-- The signature log (docs/os/apps/apps.md, "History"; layout: docs/os/ui/ui.md, "Any list").
--
--   list     wallet.history(max_entries), newest first. One row per record: the recipient's
--            name (or short address) and the amount with its symbol, coloured by the outcome;
--            under it the time, the app that asked and the outcome.
--   detail   SELECT on a row: the whole record, with the reason and the short signature.
--            UP/DOWN step to the next or previous record.
--
-- CANCEL goes back from the detail to the list, and from the list out to the launcher.
-- Amounts are the strings the firmware returns; nothing here does arithmetic on them. Times are
-- shown in UTC, like the header's clock.
--
-- Log line ("[app] HIST ..."), for test/device/t_app_history.py:
--   HIST n <count>                   once, on start: how many records were read
--
-- The look comes from lib/vk.lua (pushed into this folder as vk.lua by scripts/push-apps.sh).

local cfg = require("config")
local vk = require("vk")
local ui = vk.ui
local text = cfg.text

local DOT = " \xC2\xB7 "

local entries = {}                     -- wallet.history(), newest first
local rows = {}                        -- the list screen's rows, one per entry
local sel = 1
local detail = nil                     -- the detail screen's rows while it is open, else nil

-- "14:20", or nil for a record made while the clock had no source.
local function clock_of(t)
  if type(t) ~= "number" or t <= 0 then return nil end
  t = math.floor(t)
  return string.format("%02d:%02d", (t // 3600) % 24, (t // 60) % 60)
end

-- "2026-10-03" (the civil date of a unix time, UTC), or nil.
local function date_of(t)
  if type(t) ~= "number" or t <= 0 then return nil end
  local z = math.floor(t) // 86400 + 719468
  local era = z // 146097
  local doe = z - era * 146097
  local yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
  local doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
  local mp = (5 * doy + 2) // 153
  local day = doy - (153 * mp + 2) // 5 + 1
  local month = mp < 10 and mp + 3 or mp - 9
  local year = yoe + era * 400 + (month <= 2 and 1 or 0)
  return string.format("%04d-%02d-%02d", year, month, day)
end

local function filled(value)
  return type(value) == "string" and value ~= ""
end

local function outcome_word(entry)
  return cfg.outcome_text[entry.outcome] or tostring(entry.outcome)
end

-- Who the record is about: the verified name, else the short address, else the domain.
local function recipient(entry)
  if filled(entry.name) then return entry.name end
  if filled(entry.address) then return vk.short(entry.address) end
  return cfg.domain_label[entry.domain] or tostring(entry.domain or "")
end

-- "10.00 HACK"; nil for a record with no amount (a confirmation).
local function amount_of(entry)
  if not filled(entry.amount) or entry.amount == "0" then return nil end
  if filled(entry.symbol) then return entry.amount .. " " .. entry.symbol end
  return entry.amount
end

local function list_row(entry)
  local word = outcome_word(entry)
  return {
    l = recipient(entry),
    r = amount_of(entry) or word,
    tone = cfg.tone[entry.outcome],
    sub = (clock_of(entry.time) or text.no_time) .. DOT
      .. (filled(entry.app) and entry.app or text.no_app) .. DOT .. word,
  }
end

local function detail_rows(entry)
  local date = date_of(entry.time)
  local when = date and (date .. " " .. clock_of(entry.time) .. text.utc) or text.no_date
  local outcome = outcome_word(entry) .. (entry.dev and text.dev_mark or "")
  return {
    {l = text.time, r = when},
    {l = text.outcome, r = outcome, tone = cfg.tone[entry.outcome]},
    {l = text.reason, r = cfg.reason_text[entry.reason] or tostring(entry.reason or text.none)},
    {l = text.amount, r = amount_of(entry) or text.none},
    {l = text.to, r = filled(entry.name) and entry.name or text.none},
    {l = text.address, r = filled(entry.address) and vk.short(entry.address) or text.none},
    {l = text.app, r = filled(entry.app) and entry.app or text.no_app},
    {l = text.domain, r = filled(entry.domain) and entry.domain or text.none},
    {l = text.signature, r = filled(entry.sig) and vk.short(entry.sig) or text.none},
  }
end

local function load()
  local ok, found = pcall(badge.wallet.history, cfg.max_entries)
  entries = ok and type(found) == "table" and found or {}
  rows = {}
  for i = 1, #entries do rows[i] = list_row(entries[i]) end
  sel = math.max(1, math.min(sel, #entries))
end

local function move(step)
  local to = sel + step
  if to < 1 or to > #entries then return end
  sel = to
  if detail then detail = detail_rows(entries[sel]) end
end

function on_start()
  load()
  badge.log("HIST n " .. #entries)
end

function on_update(dt)
  local input = badge.input
  if input.repeated("up") then move(-1) end
  if input.repeated("down") then move(1) end
end

function on_draw()
  if detail then
    ui.list({
      header = cfg.header,
      title = string.format("%s %d/%d", text.detail_title, sel, #entries),
      rows = detail,
      hint = #entries > 1 and text.next or "",
      back = text.back,
    })
  else
    ui.list({
      header = cfg.header,
      title = text.title,
      rows = rows,
      sel = #rows > 0 and sel or nil,
      empty = text.empty,
      hint = #rows > 0 and text.details or "",
      back = text.back,
    })
  end
end

function on_button(key, pressed)
  if not pressed then return end
  if key == "b" then
    if detail then detail = nil else badge.system.exit() end
  elseif key == "a" then
    if not detail and entries[sel] then detail = detail_rows(entries[sel]) end
  end
end

-- Draw only when the screen changed (lib/vk.lua, "ui.frame"): a frame every pass would hold the
-- loop at 20 passes a second.
local draw_frame = on_draw
function on_draw() ui.frame(draw_frame) end
