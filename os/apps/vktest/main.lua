-- Library test
--
-- Dev-only test fixture (docs/os/apps/apps.md). It runs lib/vk.lua on the badge: the JSON table
-- of config.lua, the frame helpers, badge.theme, then every network helper and the payer flow
-- with no network, where each must give nil and a message and the flow must end in "failed".
-- A script on the laptop reads the log (test/device/t_vk.py). It never ships on a judge badge.
--
-- One step per frame, so no callback runs long. Log lines (badge.log, so each is "[app] VT ..."):
--   VT json ok | VT json FAIL <what>
--   VT frames ok | VT frames FAIL <what>
--   VT theme ok <name> | VT theme FAIL <what>
--   VT net up                          the badge has a route: the steps that would post are skipped
--   VT rpc nil <message> | VT rpc ok   and the same for blockhash, confirm, record
--   VT send_tx nil <message>           and the same for report, feed (skipped when the network is up)
--   VT pay failed <reason> | VT pay done <signature> | VT pay stuck <state> | VT pay skipped
--   VT done
-- The screen lists the same results, drawn with vk.ui. CANCEL exits.

local cfg = require("config")
local vk = require("vk")
local wallet, codec = badge.wallet, badge.codec

local rows = {}              -- one per result, for the screen
local finished = false
local step = 1
local next_at = 0
local flow, flow_deadline = nil, 0
local net_up = false

local function one_line(text)
  text = tostring(text):gsub("%s+", " ")
  return text:sub(1, 100)
end

local function say(name, result, good)
  result = one_line(result)
  badge.log("VT " .. name .. " " .. result)
  rows[#rows + 1] = {l = name, r = result, tone = good and "ok" or "bad"}
end

-- The JSON table: every text decodes and encodes back as config.lua says; no bad text decodes.
local function check_json()
  for i, case in ipairs(cfg.json) do
    local value, why = vk.json.decode(case.text)
    if value == nil and why ~= nil then return "decode " .. i .. ": " .. why end
    local encoded, bad = vk.json.encode(value)
    if encoded ~= (case.encoded or case.text) then return "encode " .. i .. ": " .. tostring(encoded or bad) end
  end
  for i, text in ipairs(cfg.json_bad) do
    local value, why = vk.json.decode(text)
    if value ~= nil or type(why) ~= "string" then return "bad " .. i .. " was accepted" end
  end
  return nil
end

local function check_frames()
  local req_id = "0123456789abcdef"
  local sig = string.rep("\xA5", 64)

  if vk.frame_type("VK\1\4") ~= 4 or vk.frame_type("hello") ~= nil then return "frame_type" end

  local frame = vk.result_frame(req_id, vk.RESULT_OK, sig)
  if not frame or #frame ~= 77 then return "result_frame" end
  local result = vk.result_parse(frame)
  if not result or result.req_id ~= req_id or result.status ~= 0 or result.ref ~= sig then return "result_parse" end
  local rejected = vk.result_parse(vk.result_frame(req_id, vk.RESULT_REJECTED))
  if not rejected or rejected.status ~= 1 or rejected.ref ~= string.rep("\0", 64) then return "result rejected" end
  if vk.result_parse(frame:sub(1, 76)) ~= nil or vk.result_frame("xyz", 0, sig) ~= nil then return "result refusals" end

  -- A CONTACT_HELLO carrying this badge's own key must give this badge's own address.
  local key = wallet.pubkey()
  if #key == 32 then
    local hello = "VK\1\16" .. key .. string.rep("\x11", 16) .. string.char(7) .. "Judge B"
    local card = vk.hello_parse(hello)
    if not card or card.name ~= "Judge B" or card.address ~= wallet.address() then return "hello_parse" end
    if vk.hello_parse(hello .. "x") ~= nil then return "hello refusals" end
  end

  local invite = vk.app_frame(64, "5.00")
  if invite ~= "VK\1\64" .. "5.00" or vk.app_body(invite, 64) ~= "5.00" or vk.app_body(invite, 65) ~= nil then
    return "app_frame"
  end
  if vk.app_frame(4, "x") ~= nil or vk.app_frame(64, string.rep("x", 237)) ~= nil then return "app_frame refusals" end
  return nil
end

-- badge.theme: a name, an integer for every token, nil for a name that is not a token.
local function check_theme()
  local theme = badge.theme
  if not theme then return nil, "badge.theme is missing" end
  local name = theme.name()
  if type(name) ~= "string" or name == "" then return nil, "name" end
  for _, token in ipairs({"paper", "ink", "faint", "sub", "stamp_ok", "stamp_warn", "stamp_bad", "led",
                          "green", "amber", "red"}) do
    if math.type(theme.color(token)) ~= "integer" then return nil, token end
    if vk.ui.color(token) ~= theme.color(token) then return nil, "vk.ui.color " .. token end
  end
  if theme.color("nonsense") ~= nil then return nil, "an unknown token" end
  if theme.color("paper") == theme.color("ink") then return nil, "paper equals ink" end
  return name
end

-- A network helper: "nil <message>" is what no network must give.
local function helper(name, call)
  return function()
    local value, why = call()
    if value == nil then
      say(name, "nil " .. tostring(why), type(why) == "string")
    else
      say(name, "ok", true)
    end
  end
end

-- A helper that would post to the laptop or the chain: only run with no network.
local function posting(name, call)
  local run = helper(name, call)
  return function()
    if net_up then say(name, "skipped", true) else run() end
  end
end

local function own_address()
  return cfg.pay_to ~= "" and cfg.pay_to or wallet.address()
end

local steps = {
  function()
    local bad = check_json()
    say("json", bad and ("FAIL " .. bad) or "ok", not bad)
  end,
  function()
    local bad = check_frames()
    say("frames", bad and ("FAIL " .. bad) or "ok", not bad)
  end,
  function()
    local name, bad = check_theme()
    say("theme", name and ("ok " .. name) or ("FAIL " .. bad), name ~= nil)
  end,
  function()
    net_up = badge.wifi.connected()
    if net_up then say("net", "up", false) end
  end,
  helper("rpc", function() return vk.rpc(cfg.rpc_method) end),
  helper("blockhash", vk.blockhash),
  helper("confirm", function() return vk.confirm(cfg.signature) end),
  helper("record", function() return vk.record(own_address()) end),
  posting("send_tx", function() return vk.send_tx(cfg.wire) end),
  posting("report", function() return vk.report({payee = own_address(), reason = "unverified"}) end),
  posting("feed", function() return vk.feed(cfg.signature, nil) end),
  function()
    if net_up then
      say("pay", "skipped", true)     -- with a network this would open the approval
      return
    end
    flow = vk.pay.start({to = own_address(), amount = cfg.pay_amount})
    flow_deadline = badge.millis() + cfg.pay_timeout_ms
  end,
}

function on_update(dt)
  if finished then return end
  if flow then
    local state, detail = flow:update()
    if state == "failed" then
      say("pay", "failed " .. tostring(detail), true)
    elseif state == "done" then
      say("pay", "done " .. tostring(detail), false)
    elseif badge.millis() > flow_deadline then
      say("pay", "stuck " .. tostring(state), false)
    else
      return
    end
    flow = nil
    return
  end
  if badge.millis() < next_at then return end
  next_at = badge.millis() + cfg.step_ms
  local run = steps[step]
  if run then
    step = step + 1
    run()
  else
    finished = true
    badge.log("VT done")
  end
end

function on_draw()
  -- The newest nine results fit the list.
  local shown = {}
  for i = math.max(1, #rows - 8), #rows do shown[#shown + 1] = rows[i] end
  vk.ui.list({
    header = "LIBRARY TEST",
    title = "vk.lua",
    rows = shown,
    empty = "starting",
    hint = finished and "done" or "running",
    back = "CANCEL quit",
  })
end

function on_button(key, pressed)
  if pressed and key == "b" then badge.system.exit() end
end
