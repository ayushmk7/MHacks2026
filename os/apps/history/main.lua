-- History
--
-- The signature log (docs/os/apps/apps.md, "History"; layout: docs/os/ui/ui.md, "Any list").
--
--   list     wallet.history(max_entries), newest first. Three kinds of row, each drawn its own way:
--              approval  the recipient's name (or short address) and the amount with its symbol,
--                        coloured by the outcome; under it the time, the app and the outcome.
--              auto      what the badge signed with no screen (payment requests, presence proofs,
--                        contact cards, store registration): the domain's label and "<n> signed";
--                        under it the time, the app and "automatic".
--              received  a payment this badge checked and was paid: "From <short payer>" and the
--                        amount with a "+"; under it the time, the app and "received".
--   detail   SELECT on a row: the whole record. UP/DOWN step to the next or previous record.
--
-- The list is read again every config.refresh_ms while it is showing, when the detail is closed,
-- and on SELECT when it is empty, so a payment made or received meanwhile appears.
-- CANCEL goes back from the detail to the list, and from the list out to the launcher.
-- Amounts are the strings the firmware returns; nothing here does arithmetic on them. Times are
-- shown in UTC, like the header's clock. Reasons are in the words every app shares
-- (vk.reason_text).
--
-- Log line ("[app] HIST ..."), for test/device/t_app_history.py:
--   HIST n <count>                   on start and whenever a reload finds another count
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
local logged = nil                     -- the count last logged
local loaded_at = 0

-- "14:20", or nil for a record made while the clock had no source.
local function clock_of(t)
  if type(t) ~= "number" or t <= 0 then return nil end
  t = math.floor(t)
  return string.format("%02d:%02d", (t // 3600) % 24, (t // 60) % 60)
end

-- "14:20:05", or the no-time mark.
local function seconds_of(t)
  if type(t) ~= "number" or t <= 0 then return text.no_time end
  t = math.floor(t)
  return string.format("%02d:%02d:%02d", (t // 3600) % 24, (t // 60) % 60, t % 60)
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

local function kind_of(entry)
  if entry.kind == "auto" or entry.kind == "received" then return entry.kind end
  return "approval"
end

local function outcome_word(entry)
  return cfg.outcome_text[entry.outcome] or tostring(entry.outcome)
end

local function domain_word(entry)
  return cfg.domain_label[entry.domain] or tostring(entry.domain or "")
end

-- Who the record is about: the verified name, else the short address, else the domain.
local function recipient(entry)
  if filled(entry.name) then return entry.name end
  if filled(entry.address) then return vk.short(entry.address) end
  return domain_word(entry)
end

-- "10.00 HACK"; nil for a record with no amount (a confirmation, an automatic signature).
local function amount_of(entry)
  if not filled(entry.amount) or entry.amount == "0" then return nil end
  if filled(entry.symbol) then return entry.amount .. " " .. entry.symbol end
  return entry.amount
end

local function app_of(entry)
  return filled(entry.app) and entry.app or text.no_app
end

local function list_row(entry)
  local time = clock_of(entry.time) or text.no_time
  local kind = kind_of(entry)
  if kind == "auto" then
    local count = math.tointeger(entry.count) or 1
    return {
      l = domain_word(entry),
      r = string.format(text.auto_count, count),
      tone = cfg.tone.auto,
      sub = time .. DOT .. app_of(entry) .. DOT .. text.kind_auto,
    }
  end
  if kind == "received" then
    local amount = amount_of(entry)
    return {
      l = string.format(text.from, filled(entry.address) and vk.short(entry.address) or text.none),
      r = amount and (text.plus .. amount) or outcome_word(entry),
      tone = cfg.tone.received,
      sub = time .. DOT .. app_of(entry) .. DOT .. text.kind_received,
    }
  end
  local word = outcome_word(entry)
  return {
    l = recipient(entry),
    r = amount_of(entry) or word,
    tone = cfg.tone[entry.outcome],
    sub = time .. DOT .. app_of(entry) .. DOT .. word,
  }
end

local function when_of(entry)
  local date = date_of(entry.time)
  return date and (date .. " " .. clock_of(entry.time) .. text.utc) or text.no_date
end

local function reason_of(entry)
  if not filled(entry.reason) then return text.none end
  return vk.reason_text(entry.reason, "short")
end

local function detail_rows(entry)
  local kind = kind_of(entry)
  local short_sig = filled(entry.sig) and vk.short(entry.sig) or text.none
  if kind == "auto" then
    local list = {
      {l = text.time, r = when_of(entry)},
      {l = text.kind, r = text.kind_auto, tone = cfg.tone.auto},
      {l = text.domain, r = filled(entry.domain) and entry.domain or text.none},
      {l = text.what, r = domain_word(entry)},
      {l = text.count, r = tostring(math.tointeger(entry.count) or 1)},
      {l = text.outcome, r = outcome_word(entry), tone = cfg.tone[entry.outcome]},
      {l = text.app, r = app_of(entry)},
    }
    local items = type(entry.items) == "table" and entry.items or {}
    for i = #items, 1, -1 do                   -- newest first, as many as fit after the rows
      if #list >= cfg.detail_rows then break end
      local item = items[i]
      local digest = type(item.digest) == "string" and item.digest:sub(1, 16) or text.none
      list[#list + 1] = {l = seconds_of(item.time), r = digest}
    end
    return list
  end
  if kind == "received" then
    return {
      {l = text.time, r = when_of(entry)},
      {l = text.kind, r = text.kind_received, tone = cfg.tone.received},
      {l = text.amount, r = amount_of(entry) or text.none},
      {l = text.payer, r = filled(entry.address) and vk.short(entry.address) or text.none},
      {l = text.request, r = filled(entry.req_id) and entry.req_id or text.none},
      {l = text.app, r = app_of(entry)},
      {l = text.signature, r = short_sig},
    }
  end
  local outcome = outcome_word(entry) .. (entry.dev and text.dev_mark or "")
  local list = {
    {l = text.time, r = when_of(entry)},
    {l = text.outcome, r = outcome, tone = cfg.tone[entry.outcome]},
    {l = text.reason, r = reason_of(entry)},
    {l = text.amount, r = amount_of(entry) or text.none},
    {l = text.to, r = filled(entry.name) and entry.name or text.none},
    {l = text.address, r = filled(entry.address) and vk.short(entry.address) or text.none},
    {l = text.app, r = app_of(entry)},
    {l = text.domain, r = filled(entry.domain) and entry.domain or text.none},
    {l = text.signature, r = short_sig},
  }
  -- A payment that answered a request: its id in place of the domain (nine rows fit the screen).
  if filled(entry.req_id) then list[8] = {l = text.request, r = entry.req_id} end
  return list
end

local function load()
  local keep = entries[sel]
  local ok, found = pcall(badge.wallet.history, cfg.max_entries)
  entries = ok and type(found) == "table" and found or {}
  rows = {}
  for i = 1, #entries do
    rows[i] = list_row(entries[i])
    -- The cursor follows its record: a new record on top moves it down.
    if keep and entries[i].time == keep.time and entries[i].sig == keep.sig
        and entries[i].kind == keep.kind and entries[i].domain == keep.domain then
      sel = i
      keep = nil
    end
  end
  sel = math.max(1, math.min(sel, #entries))
  loaded_at = badge.millis()
  if #entries ~= logged then
    logged = #entries
    badge.log("HIST n " .. #entries)
  end
  ui.dirty()
end

local function move(step)
  local to = sel + step
  if to < 1 or to > #entries then return end
  sel = to
  if detail then detail = detail_rows(entries[sel]) end
end

function on_start()
  load()
end

function on_update(dt)
  local input = badge.input
  if input.repeated("up") then move(-1) end
  if input.repeated("down") then move(1) end
  if not detail and badge.millis() - loaded_at >= cfg.refresh_ms then load() end
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
      hint = #rows > 0 and text.details or text.reload,
      back = text.back,
    })
  end
end

function on_button(key, pressed)
  if not pressed then return end
  if key == "b" then
    if detail then
      detail = nil
      load()                               -- back on the list: show what came in meanwhile
    else
      badge.system.exit()
    end
  elseif key == "a" then
    if detail then return end
    if entries[sel] then detail = detail_rows(entries[sel]) else load() end
  end
end

-- Draw only when the screen changed (lib/vk.lua, "ui.frame"): a frame every pass would hold the
-- loop at 20 passes a second.
local draw_frame = on_draw
function on_draw() ui.frame(draw_frame) end
