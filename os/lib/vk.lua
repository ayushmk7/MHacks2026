-- lib/vk.lua: the shared Lua library of Badge OS (docs/os/platform/lua-api.md, "lib/vk.lua").
--
--   local vk = require("vk")
--
-- scripts/push-apps.sh copies this file into every app's folder as vk.lua, because an app can
-- only be pushed files under /apps/<id>/.
--
--   vk.json      decode, encode
--   vk.rpc       blockhash, send_tx, confirm       JSON-RPC to config rpc_url        (permission net)
--   vk.record    report, feed                      the laptop at config listener_url (permission net)
--   vk.frame_type, result_frame, result_parse, hello_parse, app_frame, app_body      ESP-NOW frames
--   vk.pay       the payer flow as a state machine (permissions sign, net, espnow)
--   vk.ui        the Receipt look, drawn with badge.gfx in the active theme's colours
--
-- Rules this file keeps:
--   - Every network helper returns nil, message on any failure. Nothing waits without a timeout
--     and nothing retries without a limit.
--   - Amounts are strings in display units ("12.50"). They are never converted to numbers: Lua
--     numbers here are 32-bit integers and 32-bit floats.
--   - Nothing here is trusted by the firmware. The approval decodes the message and verifies the
--     record, the request and the presence proof again itself.
--   - It runs on the badge's Lua 5.4 and on a laptop's Lua 5.3 or later (test/host/test_vk.lua).

local vk = {}

-- Timeout of one HTTP request, in ms (the firmware caps it at 10000).
vk.timeout_ms = 4000
-- Commitment used for the blockhash, the send preflight and the confirmation.
vk.commitment = "confirmed"

-- ---------------------------------------------------------------------------------------------
-- Small helpers
-- ---------------------------------------------------------------------------------------------

local function hex(bytes)
  return (bytes:gsub(".", function(c) return string.format("%02x", c:byte()) end))
end

local function unhex(text)
  if #text % 2 ~= 0 or text:find("%X") then return nil end
  return (text:gsub("%x%x", function(pair) return string.char(tonumber(pair, 16)) end))
end

-- "4vJD..BW97": the first four and last four characters of an address or a signature.
function vk.short(text)
  text = tostring(text or "")
  if #text <= 10 then return text end
  return text:sub(1, 4) .. ".." .. text:sub(-4)
end

-- ---------------------------------------------------------------------------------------------
-- vk.json
-- ---------------------------------------------------------------------------------------------
-- decode(text) -> value, or nil, message for text that is not JSON.
--   An object becomes a table with string keys, an array a table with keys 1..n.
--   null: an object field that is null is left out (so `reply.error` is nil); a null array
--   element is vk.json.null, so that the array has no hole.
--   Numbers become Lua numbers. Do not use them for amounts: RPC amounts are strings.
-- encode(value) -> text, or nil, message for a value JSON cannot hold.
--   A table with keys 1..n is an array, any other table an object (keys sorted). An empty table
--   is an object unless it came from decode as an array or was made with vk.json.array().

local json = {}
vk.json = json

json.null = setmetatable({}, {__tostring = function() return "null" end})

local ARRAY = {}                                   -- the metatable that marks an array
function json.array(t) return setmetatable(t or {}, ARRAY) end

local MAX_DEPTH = 32

local ESCAPES = {['"'] = '"', ["\\"] = "\\", ["/"] = "/", b = "\b", f = "\f", n = "\n", r = "\r", t = "\t"}

local function utf8_char(cp)
  if cp < 0x80 then return string.char(cp) end
  if cp < 0x800 then return string.char(0xC0 + cp // 64, 0x80 + cp % 64) end
  if cp < 0x10000 then
    return string.char(0xE0 + cp // 4096, 0x80 + (cp // 64) % 64, 0x80 + cp % 64)
  end
  return string.char(0xF0 + cp // 262144, 0x80 + (cp // 4096) % 64, 0x80 + (cp // 64) % 64, 0x80 + cp % 64)
end

local function fail(what, at)
  error(what .. " at byte " .. at, 0)
end

local function skip_space(text, at)
  local _, last = text:find("^[ \n\r\t]*", at)
  return last + 1
end

-- `at` is the opening quote. Returns the string and the position after the closing quote.
local function parse_string(text, at)
  local parts, n = nil, 0
  local from = at + 1
  while true do
    local stop = text:find('["\\]', from)
    if not stop then fail("unterminated string", at) end
    local piece = text:sub(from, stop - 1)
    if piece:find("[\0-\31]") then fail("control character in string", from) end
    if text:byte(stop) == 34 then                            -- the closing quote
      if not parts then return piece, stop + 1 end
      parts[n + 1] = piece
      return table.concat(parts), stop + 1
    end
    parts = parts or {}
    n = n + 1
    parts[n] = piece
    local escape = text:sub(stop + 1, stop + 1)
    if escape == "u" then
      local digits = text:match("^%x%x%x%x", stop + 2)
      if not digits then fail("bad \\u escape", stop) end
      local cp = tonumber(digits, 16)
      from = stop + 6
      if cp >= 0xD800 and cp <= 0xDBFF then                  -- a surrogate pair
        local low = text:match("^\\u(%x%x%x%x)", from)
        low = low and tonumber(low, 16)
        if low and low >= 0xDC00 and low <= 0xDFFF then
          cp = 0x10000 + (cp - 0xD800) * 0x400 + (low - 0xDC00)
          from = from + 6
        else
          cp = 0xFFFD
        end
      elseif cp >= 0xDC00 and cp <= 0xDFFF then
        cp = 0xFFFD
      end
      n = n + 1
      parts[n] = utf8_char(cp)
    else
      local char = ESCAPES[escape]
      if not char then fail("bad escape", stop) end
      n = n + 1
      parts[n] = char
      from = stop + 2
    end
  end
end

local function parse_number(text, at)
  local whole = text:match("^%-?%d+", at)
  if not whole then fail("bad number", at) end
  local digits = whole:gsub("^%-", "")
  if #digits > 1 and digits:sub(1, 1) == "0" then fail("bad number", at) end
  local stop = at + #whole
  local fraction = text:match("^%.%d+", stop)
  if fraction then
    stop = stop + #fraction
  elseif text:sub(stop, stop) == "." then
    fail("bad number", at)
  end
  local exponent = text:match("^[eE][+-]?%d+", stop)
  if exponent then
    stop = stop + #exponent
  elseif text:find("^[eE]", stop) then
    fail("bad number", at)
  end
  local value = tonumber(text:sub(at, stop - 1))
  if not value then fail("bad number", at) end
  return value, stop
end

local parse_value

local function parse_array(text, at, depth)
  local out, n = json.array(), 0
  at = skip_space(text, at + 1)
  if text:byte(at) == 93 then return out, at + 1 end          -- ]
  while true do
    local value
    value, at = parse_value(text, at, depth)
    n = n + 1
    if value == nil then out[n] = json.null else out[n] = value end
    at = skip_space(text, at)
    local c = text:byte(at)
    if c == 93 then return out, at + 1 end
    if c ~= 44 then fail("expected , or ]", at) end           -- ,
    at = skip_space(text, at + 1)
  end
end

local function parse_object(text, at, depth)
  local out = {}
  at = skip_space(text, at + 1)
  if text:byte(at) == 125 then return out, at + 1 end         -- }
  while true do
    if text:byte(at) ~= 34 then fail("expected a key", at) end
    local key
    key, at = parse_string(text, at)
    at = skip_space(text, at)
    if text:byte(at) ~= 58 then fail("expected :", at) end    -- :
    at = skip_space(text, at + 1)
    local value
    value, at = parse_value(text, at, depth)
    out[key] = value                                          -- null: the field is left out
    at = skip_space(text, at)
    local c = text:byte(at)
    if c == 125 then return out, at + 1 end
    if c ~= 44 then fail("expected , or }", at) end
    at = skip_space(text, at + 1)
  end
end

parse_value = function(text, at, depth)
  local c = text:byte(at)
  if c == 34 then return parse_string(text, at) end
  if c == 123 or c == 91 then                                 -- { [
    if depth >= MAX_DEPTH then fail("nested too deeply", at) end
    if c == 123 then return parse_object(text, at, depth + 1) end
    return parse_array(text, at, depth + 1)
  end
  if c == 45 or (c and c >= 48 and c <= 57) then return parse_number(text, at) end
  if text:sub(at, at + 3) == "true" then return true, at + 4 end
  if text:sub(at, at + 4) == "false" then return false, at + 5 end
  if text:sub(at, at + 3) == "null" then return nil, at + 4 end
  fail("unexpected character", at)
end

function json.decode(text)
  if type(text) ~= "string" then return nil, "not a string" end
  local ok, value, at = pcall(parse_value, text, skip_space(text, 1), 0)
  if not ok then return nil, tostring(value) end
  at = skip_space(text, at)
  if at <= #text then return nil, "trailing characters at byte " .. at end
  return value
end

local function quote(text)
  text = text:gsub('[%c"\\\127]', function(c)
    if c == '"' then return '\\"' end
    if c == "\\" then return "\\\\" end
    if c == "\n" then return "\\n" end
    if c == "\r" then return "\\r" end
    if c == "\t" then return "\\t" end
    return string.format("\\u%04x", c:byte())
  end)
  return '"' .. text .. '"'
end

local FLOAT_FORMATS = {"%.7g", "%.9g", "%.15g", "%.17g"}

-- The shortest text that reads back as the same number.
local function number_text(value)
  if math.type(value) == "integer" then return string.format("%d", value) end
  if value ~= value or value == math.huge or value == -math.huge then
    error("cannot encode a number that is not finite", 0)
  end
  for i = 1, #FLOAT_FORMATS do
    local text = string.format(FLOAT_FORMATS[i], value)
    if tonumber(text) == value then return text end
  end
  return string.format("%.17g", value)
end

local function is_array(value)
  local mt = getmetatable(value)
  if mt == ARRAY then return true end
  local count = 0
  for key in pairs(value) do
    if math.type(key) ~= "integer" or key < 1 then return false end
    count = count + 1
  end
  if count == 0 then return false end
  for i = 1, count do
    if value[i] == nil then return false end
  end
  return true
end

local function encode_value(value, out, depth)
  local kind = type(value)
  if value == nil or value == json.null then
    out[#out + 1] = "null"
  elseif kind == "boolean" then
    out[#out + 1] = value and "true" or "false"
  elseif kind == "number" then
    out[#out + 1] = number_text(value)
  elseif kind == "string" then
    out[#out + 1] = quote(value)
  elseif kind == "table" then
    if depth >= MAX_DEPTH then error("nested too deeply", 0) end
    if is_array(value) then
      out[#out + 1] = "["
      for i = 1, #value do
        if i > 1 then out[#out + 1] = "," end
        encode_value(value[i], out, depth + 1)
      end
      out[#out + 1] = "]"
    else
      local keys = {}
      for key in pairs(value) do
        if type(key) ~= "string" then error("an object key must be a string", 0) end
        keys[#keys + 1] = key
      end
      table.sort(keys)
      out[#out + 1] = "{"
      for i = 1, #keys do
        if i > 1 then out[#out + 1] = "," end
        out[#out + 1] = quote(keys[i])
        out[#out + 1] = ":"
        encode_value(value[keys[i]], out, depth + 1)
      end
      out[#out + 1] = "}"
    end
  else
    error("cannot encode a " .. kind, 0)
  end
end

function json.encode(value)
  local out = {}
  local ok, why = pcall(encode_value, value, out, 0)
  if not ok then return nil, tostring(why) end
  return table.concat(out)
end

-- ---------------------------------------------------------------------------------------------
-- Network helpers. Each returns nil, message on any failure.
-- ---------------------------------------------------------------------------------------------

-- The value of a config key that holds a URL, without a trailing slash.
local function config_url(name)
  local url = badge.wallet.config(name)
  if type(url) ~= "string" or url == "" then return nil, name .. " not set" end
  return (url:gsub("/+$", ""))
end

-- One request. Returns the status code and the body, or nil and a message (no route, timeout, ...).
local function request(method, url, body)
  local http = badge.http
  local code, reply
  if method == "GET" then
    code, reply = http.get(url, vk.timeout_ms)
  else
    code, reply = http.post(url, body, "application/json", vk.timeout_ms)
  end
  if not code then return nil, tostring(reply or "request failed") end
  return code, reply or ""
end

-- POSTs a JSON body to <listener_url><path>. Returns true, or nil and a message.
local function listener_post(path, body)
  local url, why = config_url("listener_url")
  if not url then return nil, why end
  local text, bad = json.encode(body)
  if not text then return nil, bad end
  local code, reply = request("POST", url .. path, text)
  if not code then return nil, reply end
  if code < 200 or code > 299 then return nil, "listener http " .. code end
  return true
end

-- vk.rpc(method, params) -> the reply's `result`, or nil, message.
function vk.rpc(method, params)
  local url, why = config_url("rpc_url")
  if not url then return nil, why end
  if params == nil or (type(params) == "table" and next(params) == nil) then params = json.array() end
  local body, bad = json.encode({jsonrpc = "2.0", id = 1, method = method, params = params})
  if not body then return nil, bad end
  local code, reply = request("POST", url, body)
  if not code then return nil, reply end
  local parsed = json.decode(reply)
  if type(parsed) ~= "table" then
    if code ~= 200 then return nil, "rpc http " .. code end
    return nil, "rpc reply not understood"
  end
  if parsed.error ~= nil then
    local message = type(parsed.error) == "table" and parsed.error.message or parsed.error
    return nil, type(message) == "string" and message or "rpc error"
  end
  if code ~= 200 then return nil, "rpc http " .. code end
  if parsed.result == nil then return nil, "rpc reply has no result" end
  return parsed.result
end

-- vk.blockhash() -> a recent blockhash, base58.
function vk.blockhash()
  local result, why = vk.rpc("getLatestBlockhash", {{commitment = vk.commitment}})
  if not result then return nil, why end
  local value = type(result) == "table" and result.value
  local blockhash = type(value) == "table" and value.blockhash
  if type(blockhash) ~= "string" or blockhash == "" then return nil, "rpc reply not understood" end
  return blockhash
end

-- vk.send_tx(wire_b64) -> the transaction signature, base58. `wire_b64` is wallet.wire_tx(sig, msg).
function vk.send_tx(wire_b64)
  if type(wire_b64) ~= "string" or wire_b64 == "" then return nil, "bad_arg" end
  local result, why = vk.rpc("sendTransaction", {wire_b64, {encoding = "base64", preflightCommitment = vk.commitment}})
  if not result then return nil, why end
  if type(result) ~= "string" then return nil, "rpc reply not understood" end
  return result
end

-- vk.confirm(sig) -> "confirmed", "pending" or "failed". `sig` is base58, or the 64 raw bytes
-- (the `ref` of a RESULT frame).
function vk.confirm(sig)
  if type(sig) ~= "string" or sig == "" then return nil, "bad_arg" end
  if #sig == 64 then sig = badge.codec.b58enc(sig) end
  local result, why = vk.rpc("getSignatureStatuses", {{sig}, {searchTransactionHistory = true}})
  if not result then return nil, why end
  local value = type(result) == "table" and result.value
  if type(value) ~= "table" then return nil, "rpc reply not understood" end
  local status = value[1]
  if type(status) ~= "table" or status == json.null then return "pending" end   -- the node has not seen it
  if status.err ~= nil then return "failed" end
  local level = status.confirmationStatus
  if level == "confirmed" or level == "finalized" then return "confirmed" end
  return "pending"
end

-- vk.record(address) -> record, sig (bytes), or nil, "unverified" when the registry has no record
-- for that key (404), or nil, message.
function vk.record(address)
  if type(address) ~= "string" or #address > 44 or not address:find("^[1-9A-HJ-NP-Za-km-z]+$") then
    return nil, "bad_arg"
  end
  local url, why = config_url("listener_url")
  if not url then return nil, why end
  local code, reply = request("GET", url .. "/registry/" .. address)
  if not code then return nil, reply end
  if code == 404 then return nil, "unverified" end
  if code ~= 200 then return nil, "registry http " .. code end
  local parsed = json.decode(reply)
  if type(parsed) ~= "table" or type(parsed.record) ~= "string" or type(parsed.sig) ~= "string" then
    return nil, "registry reply not understood"
  end
  local record = badge.codec.b64dec(parsed.record)
  local sig = badge.codec.b64dec(parsed.sig)
  if not record or record == "" or not sig or #sig ~= 64 then return nil, "registry reply not understood" end
  return record, sig
end

-- A REQ frame as the feed routes take it: base64, or null. Raw frame bytes are encoded here.
local function feed_req(req)
  if type(req) ~= "string" or req == "" then return json.null end
  if vk.frame_type(req) then return badge.codec.b64enc(req) end
  return req
end

-- The refusals the dashboard feed shows (reference/reasons.md).
local REPORTED = {unverified = true, revoked = true, expired = true, mismatch = true, bad_proof = true}

-- vk.report{payee = address, reason = reason, [req = frame], [payer = address]} -> true.
-- Posts a refusal seen on this badge to <listener_url>/feed/event. Only unverified, revoked,
-- expired, mismatch and bad_proof are forwarded; any other reason gives nil, "not reported"
-- without a request. `payer` defaults to this badge.
function vk.report(event)
  if type(event) ~= "table" or type(event.reason) ~= "string" then return nil, "bad_arg" end
  if not REPORTED[event.reason] then return nil, "not reported" end
  return listener_post("/feed/event", {
    payer = event.payer or badge.wallet.address(),
    payee = event.payee or "",
    reason = event.reason,
    req = feed_req(event.req),
  })
end

-- vk.feed(sig_b58, req) -> true. Tells the laptop about a confirmed payment
-- (<listener_url>/feed/solana). `req` is the REQ frame that was paid, or nil.
function vk.feed(sig_b58, req)
  if type(sig_b58) ~= "string" or sig_b58 == "" then return nil, "bad_arg" end
  return listener_post("/feed/solana", {tx_sig = sig_b58, req = feed_req(req)})
end

-- ---------------------------------------------------------------------------------------------
-- ESP-NOW frames (docs/os/protocol/espnow.md): 'V' 'K' version 1, a type byte, the body.
-- ---------------------------------------------------------------------------------------------

local FRAME_MAGIC = "VK\1"
local FRAME_MAX = 240
local T_RESULT, T_CONTACT_HELLO = 4, 16
local RESULT_LEN = 77

vk.RESULT_OK, vk.RESULT_REJECTED, vk.RESULT_FAILED = 0, 1, 2

-- vk.frame_type(data) -> the type of a VK frame, or nil for any other payload.
function vk.frame_type(data)
  if type(data) ~= "string" or #data < 4 or data:sub(1, 3) ~= FRAME_MAGIC then return nil end
  return data:byte(4)
end

-- vk.result_frame(req_id_hex, status, sig_bytes) -> RESULT frame bytes (77), or nil, "bad_arg".
-- status: 0 paid (with the 64-byte transaction signature), 1 rejected, 2 failed (no signature).
function vk.result_frame(req_id_hex, status, sig_bytes)
  local req_id = type(req_id_hex) == "string" and #req_id_hex == 16 and unhex(req_id_hex)
  if not req_id then return nil, "bad_arg" end
  if status ~= 0 and status ~= 1 and status ~= 2 then return nil, "bad_arg" end
  if sig_bytes == nil then sig_bytes = string.rep("\0", 64) end
  if type(sig_bytes) ~= "string" or #sig_bytes ~= 64 then return nil, "bad_arg" end
  return FRAME_MAGIC .. string.char(T_RESULT) .. req_id .. string.char(status) .. sig_bytes
end

-- vk.result_parse(data) -> {req_id = 16 hex characters, status = 0..2, ref = 64 bytes}, or nil.
-- A RESULT is unauthenticated: confirm `ref` on chain (vk.confirm) before showing "paid".
function vk.result_parse(data)
  if vk.frame_type(data) ~= T_RESULT or #data ~= RESULT_LEN then return nil end
  local status = data:byte(13)
  if status > 2 then return nil end
  return {req_id = hex(data:sub(5, 12)), status = status, ref = data:sub(14, 77)}
end

-- vk.hello_parse(data) -> {address = base58, name = text} from a CONTACT_HELLO frame, or nil.
-- The name is what the sender calls itself, not a verified identity.
function vk.hello_parse(data)
  if vk.frame_type(data) ~= T_CONTACT_HELLO or #data < 54 then return nil end
  local name_len = data:byte(53)
  if name_len < 1 or name_len > 32 or #data ~= 53 + name_len then return nil end
  local name = data:sub(54)
  if name:find("[^\32-\126]") then return nil end
  local address = badge.codec.b58enc(data:sub(5, 36))
  if not address then return nil end
  return {address = address, name = name}
end

-- vk.app_frame(type, body) -> frame bytes for an app-range type (64..255), or nil, "bad_arg".
function vk.app_frame(type_, body)
  body = body or ""
  if math.type(type_) ~= "integer" or type_ < 64 or type_ > 255 then return nil, "bad_arg" end
  if type(body) ~= "string" or #body > FRAME_MAX - 4 then return nil, "bad_arg" end
  return FRAME_MAGIC .. string.char(type_) .. body
end

-- vk.app_body(data, type) -> the body if `data` is a VK frame of that type, else nil.
function vk.app_body(data, type_)
  local found = vk.frame_type(data)
  if found == nil or found ~= type_ then return nil end
  return data:sub(5)
end

-- ---------------------------------------------------------------------------------------------
-- vk.pay: the payer flow (docs/os/platform/lua-api.md, "vk.pay")
-- ---------------------------------------------------------------------------------------------
--   local flow = vk.pay.start{ request = entry }                       an entry of wallet.requests()
--   local flow = vk.pay.start{ to = address, amount = "5.00", [symbol = "HACK"], [memo = "..."] }
--   local state, detail = flow:update()                                call every frame
--
-- States: "presence" (request only) -> "record" -> "blockhash" -> "approve" -> "submit" ->
-- "confirm" -> "done" (detail: the transaction signature, base58) or "failed" (detail: the
-- reason: a reason code of reference/reasons.md, or the message of the network call that failed).
-- flow.state, flow.detail and flow.signature (base58, once signed) can be read at any time.
--
-- update() makes at most one blocking call (an HTTP request, or opening the approval).
--   first       wallet.refresh_balance(), only when wallet.token_account() is not known yet
--   presence    wallet.challenge, then waits for the proof. No proof is not a failure: the
--               approval is then amber instead of green.
--   record      vk.record. No record (404) is not a failure either: the flow goes on with no
--               record and pays the address itself, so the firmware shows red UNVERIFIED
--               RECIPIENT and nothing can be signed.
--   blockhash   vk.blockhash
--   approve     wallet.build_transfer and wallet.begin_solana, then wallet.poll. The firmware
--               pauses the app while its approval is open.
--   submit      vk.send_tx
--   confirm     vk.confirm every 2 s for up to 30 s, then vk.feed (failures ignored)
-- When paying a request the payee is sent a RESULT frame: status 0 with the signature once the
-- payment is confirmed (and also when it was accepted by the node but not confirmed in time: the
-- payee confirms on chain itself), 1 when the approval did not sign, 2 when the transaction
-- could not be sent or failed on chain.
--
-- An extra option for demos: `destination` (a token account, base58) replaces the account named
-- by the recipient's record. The firmware then shows red WRONG RECIPIENT.

local pay = {}
vk.pay = pay

pay.presence_tries = 2          -- challenges sent before going on without a proof
pay.presence_margin_ms = 300    -- waited for a proof: config presence_ms plus this
pay.begin_tries = 10            -- while wallet.begin_solana answers "busy"
pay.begin_retry_ms = 300
pay.send_tries = 2
pay.send_retry_ms = 1000
pay.confirm_every_ms = 2000
pay.confirm_for_ms = 30000

local Flow = {}
Flow.__index = Flow

local function flow_fail(flow, reason)
  flow.failed_in = flow.state
  flow.state = "failed"
  flow.detail = tostring(reason or "failed")
end

-- Tells the payee how it ended. Unauthenticated and best effort: a lost frame is not an error.
local function send_result(flow, status)
  local request = flow.request
  if not request or flow.result_sent then return end
  local frame = vk.result_frame(request.req_id, status, status == vk.RESULT_OK and flow.sig or nil)
  if not frame then return end
  flow.result_sent = true
  badge.espnow.send(request.mac, frame)
end

-- The token account a registry record names, base58, or nil. The record is "key=value" lines.
local function record_account(record)
  local found = ("\n" .. record .. "\n"):match("\nsolana_ata=(%x+)\n")
  if not found or #found ~= 64 then return nil end
  return badge.codec.b58enc(unhex(found))
end

local STEPS = {}

function STEPS.presence(flow, now)
  local wallet, request = badge.wallet, flow.request
  if flow.challenge_until then
    if wallet.presence(request.req_id) ~= "pending" then
      flow.state = "record"                    -- present, late or bad_sig: the approval judges it
      return
    end
    if now < flow.challenge_until then return end
    if flow.challenges >= pay.presence_tries then
      flow.state = "record"                    -- no proof: the approval will be amber
      return
    end
  end
  if not badge.espnow.enabled() then badge.espnow.enable(true) end
  local ok, why = wallet.challenge(request.mac, request.req)
  if not ok then return flow_fail(flow, why) end
  flow.challenges = flow.challenges + 1
  local wait = tonumber(wallet.config("presence_ms") or "") or 1500
  flow.challenge_until = now + wait + pay.presence_margin_ms
end

function STEPS.record(flow)
  local record, sig = vk.record(flow.to)
  if record then
    flow.record, flow.record_sig = record, sig
    flow.destination = flow.destination or record_account(record) or flow.to
  elseif sig == "unverified" then
    flow.destination = flow.destination or flow.to
  else
    return flow_fail(flow, sig)
  end
  flow.state = "blockhash"
end

function STEPS.blockhash(flow)
  local blockhash, why = vk.blockhash()
  if not blockhash then return flow_fail(flow, why) end
  flow.blockhash = blockhash
  flow.state = "approve"
end

function STEPS.approve(flow, now)
  local wallet = badge.wallet
  if not flow.begun then
    if not flow.msg then
      local msg, why = wallet.build_transfer({
        destination = flow.destination, amount = flow.amount, blockhash = flow.blockhash,
        symbol = flow.symbol, memo = flow.memo,
      })
      if not msg then return flow_fail(flow, why) end
      flow.msg = msg
    end
    local ctx = {record = flow.record, record_sig = flow.record_sig, req = flow.request and flow.request.req}
    local ok, why = wallet.begin_solana(flow.msg, ctx)
    if ok then
      flow.begun = true
    elseif why == "busy" and flow.begins + 1 < pay.begin_tries then
      flow.begins = flow.begins + 1
      flow.wait_until = now + pay.begin_retry_ms
    else
      flow_fail(flow, why)
    end
    return
  end
  local result, why = wallet.poll()
  if result == "pending" then return end
  if not result then
    send_result(flow, vk.RESULT_REJECTED)
    return flow_fail(flow, why)
  end
  flow.sig = result
  flow.signature = badge.codec.b58enc(result)
  local wire, bad = wallet.wire_tx(result, flow.msg)
  if not wire or not flow.signature then
    send_result(flow, vk.RESULT_FAILED)
    return flow_fail(flow, bad or "bad_arg")
  end
  flow.wire = wire
  flow.state = "submit"
end

function STEPS.submit(flow, now)
  local sent, why = vk.send_tx(flow.wire)
  if sent then
    flow.state = "confirm"
    flow.confirm_until = now + pay.confirm_for_ms
    flow.wait_until = now + pay.confirm_every_ms
    return
  end
  flow.sends = flow.sends + 1
  if flow.sends >= pay.send_tries then
    send_result(flow, vk.RESULT_FAILED)
    return flow_fail(flow, why)
  end
  flow.wait_until = now + pay.send_retry_ms
end

function STEPS.confirm(flow, now)
  if flow.confirmed then
    vk.feed(flow.signature, flow.request and flow.request.req)       -- failures are ignored
    flow.state = "done"
    flow.detail = flow.signature
    return
  end
  local status = vk.confirm(flow.signature)
  if status == "confirmed" then
    flow.confirmed = true                    -- the feed call is the next update's blocking step
    send_result(flow, vk.RESULT_OK)
  elseif status == "failed" then
    send_result(flow, vk.RESULT_FAILED)
    flow_fail(flow, "transaction failed")
  elseif now >= flow.confirm_until then
    send_result(flow, vk.RESULT_OK)          -- sent but not seen confirmed: the payee checks the chain
    flow_fail(flow, "not confirmed")
  else
    flow.wait_until = now + pay.confirm_every_ms
  end
end

function Flow:update()
  local state = self.state
  if state == "done" or state == "failed" then return state, self.detail end
  local now = badge.millis()
  if self.wait_until and now < self.wait_until then return state end
  self.wait_until = nil
  if not self.ready then
    -- The transfer is built from this badge's token account, which the firmware learns from the node.
    self.ready = true
    local wallet = badge.wallet
    if not wallet.provisioned() then
      flow_fail(self, "not_provisioned")
    elseif wallet.token_account(self.symbol) == nil then
      local ok, why = wallet.refresh_balance()
      if not ok then flow_fail(self, why) end
    end
    return self.state, self.detail
  end
  STEPS[state](self, now)
  return self.state, self.detail
end

local function is_text(value)
  return type(value) == "string" and value ~= ""
end

function pay.start(opts)
  local flow = setmetatable({state = "failed", detail = "bad_arg", challenges = 0, begins = 0, sends = 0}, Flow)
  if type(opts) ~= "table" then return flow end
  local request = opts.request
  if request ~= nil then
    if type(request) ~= "table" or not is_text(request.req) or not is_text(request.mac)
        or not is_text(request.payee) or not is_text(request.amount) or not is_text(request.req_id) then
      return flow
    end
    if request.rail ~= nil and request.rail ~= "solana" then
      flow.detail = "unsupported"            -- the bank rail is not paid with a Solana transfer
      return flow
    end
    flow.request = request
    flow.to = request.payee
    flow.amount = request.amount
    flow.symbol = is_text(request.currency) and request.currency or nil
    flow.state = "presence"
  else
    -- An amount is a string in display units; a number is refused rather than formatted.
    if not is_text(opts.to) or not is_text(opts.amount) then return flow end
    if opts.symbol ~= nil and not is_text(opts.symbol) then return flow end
    if opts.memo ~= nil and type(opts.memo) ~= "string" then return flow end
    flow.to = opts.to
    flow.amount = opts.amount
    flow.symbol = opts.symbol
    flow.memo = opts.memo ~= "" and opts.memo or nil
    flow.state = "record"
  end
  if opts.destination ~= nil then
    if not is_text(opts.destination) then
      flow.state = "failed"
      return flow
    end
    flow.destination = opts.destination
  end
  flow.detail = nil
  return flow
end

-- ---------------------------------------------------------------------------------------------
-- vk.ui: the Receipt look for Lua apps (docs/os/ui/ui.md, "The receipt kit")
-- ---------------------------------------------------------------------------------------------
-- The same geometry as the firmware kit (src/vk/ui/receipt.cpp):
--   - A `y` is the top of the capital letters. Body text is the built-in font, 6 px per column.
--   - header: text at y 7, dashed rule at y 19. footer: dashed rule at y 216, text at y 224.
--   - A row at `y` owns y-5 .. y+12 (pitch 18); a selected row is filled from x0-10 to x1+10.
--     Its subline goes at y+13 and owns 13 px (a row with a subline has a pitch of 31).
--   - Two-column screens split at x 146: the stub is centred on 73, body rows run 156..310,
--     a body title is centred on 233.
--   - Text that does not fit is cut and ends in "..". The middle dot and the small triangles
--     (U+00B7, U+25C2, U+25B8) are drawn by hand; any other byte outside ASCII is drawn as "?".
-- Colours come from badge.theme.color; without that module, the receipt-light values.
-- The firmware kit has more fonts than badge.gfx: here a title and an amount are the built-in
-- font scaled up.

local ui = {}
vk.ui = ui

ui.W, ui.H = 320, 240
ui.MARGIN = 10
ui.CONTENT_Y = 24               -- content starts here, under the header
ui.ROW_PITCH = 18
ui.SUB_PITCH = 31               -- a row with a subline
ui.SPLIT_X = 146                -- the perforation of a two-column screen
ui.STUB_CX = 73
ui.BODY_X0, ui.BODY_X1, ui.BODY_CX = 156, 310, 233
ui.TITLE_Y = 26                 -- list screens: title, then rows from LIST_Y (as the native lists)
ui.LIST_Y = 46

local CHAR_W = 6
local DOT, TRI_LEFT, TRI_RIGHT = "\1", "\2", "\3"

local function rgb565(rgb)
  local r, g, b = rgb // 65536, (rgb // 256) % 256, rgb % 256
  return (r // 8) * 2048 + (g // 4) * 32 + b // 8
end

-- receipt-light (src/vk/ui/theme.cpp) and the approval's three fixed severity colours.
local FALLBACK = {
  paper = rgb565(0xF3EFE4), ink = rgb565(0x1B1A17), faint = rgb565(0x8A8474), sub = rgb565(0x6D6759),
  stamp_ok = rgb565(0x17804F), stamp_warn = rgb565(0xB56A00), stamp_bad = rgb565(0xC8321E),
  led = rgb565(0xFFE2AA),
  green = rgb565(0x1FBF75), amber = rgb565(0xFFB020), red = rgb565(0xFF4545),
}

-- ui.color(token) -> RGB565. Tokens: paper, ink, faint, sub, stamp_ok, stamp_warn, stamp_bad, led,
-- green, amber, red. An unknown token gives the ink colour.
function ui.color(token)
  local theme = badge.theme
  local value = theme and theme.color and theme.color(token)
  return value or FALLBACK[token] or FALLBACK.ink
end

-- Text as the kit draws it: one byte per column, the three marks as bytes 1..3.
local function columns(text)
  text = tostring(text == nil and "" or text)
  if not text:find("[^\32-\126]") then return text end
  text = text:gsub("[\0-\31\127]", "?")
  text = text:gsub("\xC2\xB7", DOT)
  text = text:gsub("\xE2\x97[\x82\x80\x84]", TRI_LEFT)
  text = text:gsub("\xE2\x96[\xB8\xB6\xBA]", TRI_RIGHT)
  return (text:gsub("[\128-\255]", "?"))
end

local function clip(cols, max)
  if #cols <= max then return cols end
  if max < 2 then return "" end
  return cols:sub(1, max - 2) .. ".."
end

local function mark(gfx, code, x, y, color)
  if code == DOT then
    gfx.fill_rect(x + 1, y + 2, 2, 2, color)
    return
  end
  for i = 0, 3 do                               -- 4 columns wide, 7 rows tall, the point on the centre row
    local col = code == TRI_LEFT and x + 1 + i or x + 4 - i
    gfx.fill_rect(col, y + 3 - i, 1, 2 * i + 1, color)
  end
end

-- Draws columns with their top-left at (x, y). `spacing` is extra pixels between columns;
-- `size` scales the built-in font (marks are drawn at size 1 only).
local function draw(cols, x, y, color, spacing, size)
  if cols == "" then return end
  local gfx = badge.gfx
  x, y = math.floor(x), math.floor(y)
  spacing, size = spacing or 0, size or 1
  if spacing == 0 and not cols:find("[\1-\3]") then
    gfx.text(cols, x, y, color, size)
    return
  end
  local step = CHAR_W * size + spacing
  if spacing ~= 0 then
    for i = 1, #cols do
      local c = cols:sub(i, i)
      if c <= TRI_RIGHT then mark(gfx, c, x, y, color) else gfx.text(c, x, y, color, size) end
      x = x + step
    end
    return
  end
  local at = 1
  while at <= #cols do
    local found = cols:find("[\1-\3]", at)
    if found == at then
      mark(gfx, cols:sub(at, at), x + (at - 1) * step, y, color)
      at = at + 1
    else
      local last = (found or #cols + 1) - 1
      gfx.text(cols:sub(at, last), x + (at - 1) * step, y, color, size)
      at = last + 1
    end
  end
end

-- Width in pixels of the ink: the last column's blank pixel is not counted.
local function ink_width(cols, spacing, size)
  size = size or 1
  return #cols * CHAR_W * size + (#cols - 1) * (spacing or 0) - size
end

local function draw_centered(cols, cx, y, color, spacing, size)
  if cols == "" then return end
  draw(cols, math.floor(cx) - ink_width(cols, spacing, size) // 2, y, color, spacing, size)
end

-- ui.text(text, x, y, [color]) and ui.text_center(text, cx, y, [color]): body text that may hold
-- a middle dot or a triangle. The colour defaults to ink.
function ui.text(text, x, y, color)
  draw(columns(text), x, y, color or ui.color("ink"))
end

function ui.text_center(text, cx, y, color)
  draw_centered(columns(text), cx, y, color or ui.color("ink"), 0)
end

function ui.page()
  badge.gfx.clear(ui.color("paper"))
end

-- ui.rule(y, [x0], [x1]): a dashed line, 3 px on and 2 px off, from x0 to x1 (default 10..310).
function ui.rule(y, x0, x1)
  local gfx, ink = badge.gfx, ui.color("ink")
  x0, x1 = x0 or ui.MARGIN, x1 or ui.W - ui.MARGIN
  for x = x0, x1, 5 do
    gfx.fill_rect(x, y, math.min(3, x1 - x + 1), 1, ink)
  end
end

-- ui.perforation(x, y0, y1): the dashed vertical line between a stub and its body.
function ui.perforation(x, y0, y1)
  local gfx, ink = badge.gfx, ui.color("ink")
  for y = y0, y1, 5 do
    gfx.fill_rect(x, y, 1, math.min(3, y1 - y + 1), ink)
  end
end

-- The time, a middle dot and the battery ("14:32 . 87%"), or "USB" for the battery on external
-- power: what the right side of a header shows.
-- The time (UTC) is the firmware clock's (wallet.time()), so it is left out while the clock has
-- no trusted source, as in the firmware's own header.
function ui.status()
  local parts = {}
  local wallet, battery = badge.wallet, badge.battery
  local seconds = wallet and wallet.time and wallet.time()
  if seconds then
    seconds = math.floor(seconds)      -- a float past 2038 (32-bit Lua integers)
    parts[#parts + 1] = string.format("%02d:%02d", (seconds // 3600) % 24, (seconds // 60) % 60)
  end
  if battery then
    if battery.charging() then
      parts[#parts + 1] = "USB"
    else
      local percent = math.floor((battery.percent() or 0) + 0.5)
      parts[#parts + 1] = string.format("%d%%", math.max(0, math.min(100, percent)))
    end
  end
  return table.concat(parts, " \xC2\xB7 ")
end

-- Left text at x 10 and right text ending at x 310, at `y`. The right text is kept whole.
local function edge_texts(left, right, y)
  local ink = ui.color("ink")
  local r = clip(columns(right), (ui.W - 2 * ui.MARGIN) // CHAR_W)
  local rw = #r * CHAR_W
  local room = ui.W - 2 * ui.MARGIN - rw - (rw > 0 and 2 * CHAR_W or 0)
  draw(clip(columns(left), room // CHAR_W), ui.MARGIN, y, ink)
  draw(r, ui.W - ui.MARGIN - rw, y, ink)
end

-- ui.header(left, [right]): `right` defaults to ui.status(), the time and the battery.
function ui.header(left, right)
  edge_texts(left, right == nil and ui.status() or right, 7)
  ui.rule(19)
end

-- ui.footer(left, right): clears the strip under y 216 first, like the firmware kit.
function ui.footer(left, right)
  badge.gfx.fill_rect(0, 216, ui.W, ui.H - 216, ui.color("paper"))
  ui.rule(216, 0, ui.W - 1)
  edge_texts(left, right, 224)
end

-- ui.title(text, y, [cx]): a section title, centred on cx (default: the screen). The built-in
-- font at twice its size, 13 px per character like the firmware's title font.
function ui.title(text, y, cx)
  local cols = columns(text)
  if cols == "" then return end
  draw_centered(cols:gsub("[\1-\3]", "?"), cx or ui.W // 2, y - 1, ui.color("ink"), 1, 2)
end

-- ui.row(y, label, value, [selected], [value_color], [x0], [x1]): label left, value right, a
-- dotted leader between. x0 and x1 default to the page margins (10 and 310).
function ui.row(y, label, value, selected, value_color, x0, x1)
  local gfx = badge.gfx
  x0, x1 = x0 or ui.MARGIN, x1 or ui.W - ui.MARGIN
  local text = ui.color(selected and "paper" or "ink")
  local leader = selected and text or ui.color("faint")
  if selected then gfx.fill_rect(x0 - 10, y - 5, (x1 - x0) + 20, ui.ROW_PITCH, ui.color("ink")) end

  -- The label is kept; the value gets what is left, less two columns for the leader.
  local cols = (x1 - x0) // CHAR_W
  local l = clip(columns(label), cols)
  local v = clip(columns(value), cols - #l - (#l > 0 and 2 or 0))
  local lw, vw = #l * CHAR_W, #v * CHAR_W
  draw(l, x0, y, text)
  draw(v, x1 - vw, y, value_color or text)

  -- One dot every 3 px, on a 3 px grid so that the leaders of stacked rows line up.
  local from = x0 + (lw > 0 and lw - 1 + 4 or 0)
  local to = x1 - (vw > 0 and vw + 4 or 0)
  from = from + (3 - from % 3) % 3
  for x = from, to - 1, 3 do gfx.pixel(x, y + 4, leader) end
end

-- ui.subline(y, text, [selected], [x0], [x1]): the second line of a row, at the row's y + 13.
function ui.subline(y, text, selected, x0, x1)
  x0, x1 = x0 or ui.MARGIN, x1 or ui.W - ui.MARGIN
  if selected then badge.gfx.fill_rect(x0 - 10, y, (x1 - x0) + 20, 13, ui.color("ink")) end
  draw(clip(columns(text), (x1 - x0 - 12) // CHAR_W), x0 + 12, y, ui.color(selected and "paper" or "sub"))
end

-- ui.amount(cx, y, label, value, unit): the label at y, the value as large as fits 136 px in the
-- 34 rows from y+15, the unit at y+56; all centred on cx. `value` is a string and is never cut.
function ui.amount(cx, y, label, value, unit)
  local ink = ui.color("ink")
  draw_centered(columns(label), cx, y, ink, 2)
  local cols = columns(value):gsub("[\1-\3]", "?")
  if cols ~= "" then
    local size = 1
    for candidate = 4, 2, -1 do
      if ink_width(cols, 0, candidate) <= 136 then size = candidate break end
    end
    draw_centered(cols, cx, y + 15 + (34 - 7 * size) // 2, ink, 0, size)
  end
  draw_centered(columns(unit), cx, y + 56, ink, 1)
end

-- ui.barcode(x, y, w, h, [seed]): bars derived from the bytes of `seed`, by default this badge's
-- public key, so each badge prints its own. The same bars as the firmware kit draws.
function ui.barcode(x, y, w, h, seed)
  if w <= 0 or h <= 0 then return end
  local gfx, ink = badge.gfx, ui.color("ink")
  if type(seed) ~= "string" or seed == "" then seed = badge.wallet and badge.wallet.pubkey() or "" end
  if seed == "" then seed = "\x15\x92\x6B\x04\xD3\x28\x7C\xA1" end
  local nibbles, stop = #seed * 2, x + w
  local px, i = x, 0
  while px < stop do                            -- each half-byte is one bar and the gap after it
    local byte = seed:byte((i % nibbles) // 2 + 1)
    local nibble = i % 2 == 0 and byte // 16 or byte % 16
    local bar = math.min(1 + (nibble % 4) % 3, stop - px)
    gfx.fill_rect(px, y, bar, h, ink)
    px = px + bar + 1 + (nibble // 4) % 3
    i = i + 1
  end
end

local TONES = {ok = "stamp_ok", warn = "stamp_warn", bad = "stamp_bad", mut = "faint"}

-- ui.list{title=, rows={{l=, r=, sub=, tone=}}, sel=, hint=} draws a whole list screen: header,
-- title, rows, footer. `sel` is the index of the selected row (1-based; nil selects none); the
-- window scrolls to keep it in view: 5 rows when any row has a subline, else 9. `tone` colours a
-- row's value: "ok", "warn", "bad" or "mut". The title and the labels are drawn in capitals.
-- Also read: `header` (left header text, default "BADGE OS"), `back` (right footer text, default
-- "CANCEL back"), `empty` (a line shown when there are no rows).
function ui.list(model)
  local rows = model.rows or {}
  ui.page()
  ui.header(model.header or "BADGE OS")
  ui.title(tostring(model.title or ""):upper(), ui.TITLE_Y)

  local with_sub = false
  for i = 1, #rows do
    if rows[i].sub then with_sub = true break end
  end
  local visible = with_sub and 5 or 9
  local pitch = with_sub and ui.SUB_PITCH or ui.ROW_PITCH
  local sel = model.sel
  local first = math.max(0, math.min((sel or 1) - 1 - visible // 2, #rows - visible))

  if #rows == 0 and model.empty then
    ui.text_center(model.empty, ui.W // 2, 112, ui.color("sub"))
  end
  for slot = 0, visible - 1 do
    local index = first + slot + 1
    local row = rows[index]
    if not row then break end
    local y = ui.LIST_Y + slot * pitch
    local selected = index == sel
    ui.row(y, tostring(row.l or ""):upper(), row.r, selected, row.tone and ui.color(TONES[row.tone] or "ink") or nil)
    if row.sub then ui.subline(y + 13, row.sub, selected) end
  end
  ui.footer(model.hint or "", model.back or "CANCEL back")
end

return vk
