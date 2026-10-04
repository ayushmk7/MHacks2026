-- lib/vk.lua: the shared Lua library of BadgeOS (docs/os/platform/lua-api.md, "lib/vk.lua").
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
--   vk.receive   the payee's check of a RESULT before PAID (permission net; history to record it)
--   vk.reason_text, vk.reasons, vk.offline      one text for every reason, for every app
--   vk.peers     badges heard nearby by MAC (Contacts)
--   vk.keep_awake, vk.led_pulse
--   vk.ui        the Receipt look: the firmware's kit (badge.receipt), or badge.gfx without it
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

-- A transport failure (no answer at all) is returned as one of these codes, so that every app can
-- say what to do next (vk.reason_text). The transport's own message is kept in vk.last_error.
--   no_wifi                not joined: badge.http has no route ("wifi not connected", "no network")
--   no_route               the phone bridge, the only route, failed
--   rpc_unreachable        a route, but the Solana node (config rpc_url) did not answer
--   listener_unreachable   a route, but the laptop (config listener_url) did not answer
--   bad_url                rpc_url or listener_url is not a URL the badge can use
local NOT_JOINED = {["wifi not connected"] = true, ["no network"] = true}
local BRIDGE_DOWN = {["bridge unavailable"] = true, ["bridge failed"] = true}
local OFFLINE = {no_wifi = true, no_route = true, rpc_unreachable = true, listener_unreachable = true}

vk.last_error = nil

-- vk.offline(reason) -> true for the four codes above that mean "no network": the app can then
-- offer the next step (the texts of vk.reason_text name it).
function vk.offline(reason) return OFFLINE[reason] == true end

-- One request to `server` ("rpc" or "listener"). Returns the status code and the body, or nil and
-- a transport code (above).
local function request(method, url, body, server)
  local http = badge.http
  local code, reply
  if method == "GET" then
    code, reply = http.get(url, vk.timeout_ms)
  else
    code, reply = http.post(url, body, "application/json", vk.timeout_ms)
  end
  if not code then
    local message = tostring(reply or "request failed")
    vk.last_error = message
    if NOT_JOINED[message] then return nil, "no_wifi" end
    if BRIDGE_DOWN[message] then return nil, "no_route" end
    if message == "bad url" then return nil, "bad_url" end
    return nil, server .. "_unreachable"
  end
  return code, reply or ""
end

-- When the firmware's balance fetch answers "timeout" (no route, or no answer), which of the
-- no-network codes it was. badge.wifi needs the permission net; without it, rpc_unreachable.
local function balance_reason(why)
  if why ~= "timeout" then return why end
  local wifi = badge.wifi
  local ok, joined = pcall(function() return wifi and wifi.connected() end)
  if ok and joined == false then return "no_wifi" end
  return "rpc_unreachable"
end

-- POSTs a JSON body to <listener_url><path>. Returns true, or nil and a message. A 2xx reply
-- whose JSON body says {"ok": false} is a refusal, not a success: nil and its `reason` (the
-- backend answers 200 {"ok":false,"reason":"bad_request"} to a body it will not take).
local function listener_post(path, body)
  local url, why = config_url("listener_url")
  if not url then return nil, why end
  local text, bad = json.encode(body)
  if not text then return nil, bad end
  local code, reply = request("POST", url .. path, text, "listener")
  if not code then return nil, reply end
  if code < 200 or code > 299 then return nil, "listener http " .. code end
  local parsed = json.decode(reply)
  if type(parsed) == "table" and parsed.ok == false then
    return nil, type(parsed.reason) == "string" and parsed.reason ~= "" and parsed.reason or "listener refused"
  end
  return true
end

-- vk.rpc(method, params) -> the reply's `result`, or nil, message.
function vk.rpc(method, params)
  local url, why = config_url("rpc_url")
  if not url then return nil, why end
  if params == nil or (type(params) == "table" and next(params) == nil) then params = json.array() end
  local body, bad = json.encode({jsonrpc = "2.0", id = 1, method = method, params = params})
  if not body then return nil, bad end
  local code, reply = request("POST", url, body, "rpc")
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
-- (the `ref` of a RESULT frame). It says only that *some* transaction with that signature landed,
-- not that it paid anyone: a payee checks a RESULT with vk.receive, never with this.
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

-- True when a 404 reply says the registry has no record for the key (integration/backend.md: the
-- listener answers {"error":{"code":"not_found","message":"no registry record for this key"}}),
-- rather than that the route does not exist ({"error":{..., "message":"no such route"}}).
local function names_missing_record(reply)
  local parsed = json.decode(reply)
  if type(parsed) ~= "table" then return false end
  local err = parsed.error
  local text = type(err) == "table" and err.message or err
  if type(text) ~= "string" then text = parsed.message end
  if type(text) ~= "string" then return false end
  text = text:lower()
  return text:find("record", 1, true) ~= nil or text:find("attestation", 1, true) ~= nil
end

-- vk.record(address) -> record, sig (bytes), or nil, reason:
--   "unverified"        the registry answered 404 and says it has no record for that key;
--   "registry_missing"  a 404 that does not say so: the listener has no registry route (or is not
--                       the dashboard). Not an impostor warning, but no record either;
--   a transport code (listener_unreachable, no_wifi, ...) or another message.
-- Neither 404 is ever a record: a payment goes on to the firmware's red UNVERIFIED RECIPIENT.
function vk.record(address)
  if type(address) ~= "string" or #address > 44 or not address:find("^[1-9A-HJ-NP-Za-km-z]+$") then
    return nil, "bad_arg"
  end
  local url, why = config_url("listener_url")
  if not url then return nil, why end
  local code, reply = request("GET", url .. "/registry/" .. address, nil, "listener")
  if not code then return nil, reply end
  if code == 404 then return nil, names_missing_record(reply) and "unverified" or "registry_missing" end
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
-- (<listener_url>/feed/solana). `req` is the REQ frame that was paid. A payment that answers no
-- request (a shop) is not posted: the backend refuses req = null (200 {"ok":false}), so it gives
-- nil, "no request" with no HTTP request, and logs why.
function vk.feed(sig_b58, req)
  if type(sig_b58) ~= "string" or sig_b58 == "" then return nil, "bad_arg" end
  if type(req) ~= "string" or req == "" then
    badge.log("vk feed skipped: no request (the backend refuses req null)")
    return nil, "no request"
  end
  return listener_post("/feed/solana", {tx_sig = sig_b58, req = feed_req(req)})
end

-- ---------------------------------------------------------------------------------------------
-- ESP-NOW frames (docs/os/protocol/espnow.md): 'V' 'K' version 1, a type byte, the body.
-- ---------------------------------------------------------------------------------------------

local FRAME_MAGIC = "VK\1"
local FRAME_MAX = 240
local T_RESULT, T_CONTACT_HELLO, T_CONTACT_CARD = 4, 16, 17
local RESULT_LEN = 77

-- Frame types an app compares vk.frame_type(data) with (the type registry of espnow.md).
vk.T_RESULT, vk.T_CONTACT_HELLO, vk.T_CONTACT_CARD = T_RESULT, T_CONTACT_HELLO, T_CONTACT_CARD

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
-- A RESULT is unauthenticated: check `ref` with vk.receive (getTransaction + wallet.verify_payment)
-- before showing "paid".
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
-- Fields an app may read at any time (lua-api.md, "vk.pay"): flow.state, flow.detail, flow.ready
-- (true once the first update() has finished its balance step), flow.sig (the 64 signature bytes,
-- once signed), flow.signature (the same in base58), flow.result_sent (true once a RESULT frame
-- went to the payee), flow.request and flow.failed_in.
--
-- update() makes at most one blocking call (an HTTP request, or opening the approval).
--   first       wallet.refresh_balance(), only when wallet.token_account() is not known yet
--   presence    wallet.challenge, then waits for the proof. No proof is not a failure: the
--               approval is then amber instead of green.
--   record      vk.record. No record (404) is not a failure either: the flow goes on with no
--               record and pays the address itself, so the firmware shows red UNVERIFIED
--               RECIPIENT and nothing can be signed. flow.record_miss says which 404 it was
--               ("unverified" or "registry_missing"), and flow:reason() reads it.
--   blockhash   vk.blockhash
--   approve     wallet.build_transfer and wallet.begin_solana, then wallet.poll. The firmware
--               pauses the app while its approval is open. A request payment passes
--               req_id = request.req_id, so the firmware writes the request memo (without it the
--               approval is red WRONG MEMO); a free memo is used only for a payment with no request.
--   submit      vk.send_tx
--   confirm     vk.confirm every 2 s for up to 30 s, then vk.feed (failures ignored; a payment
--               with no request is not fed: the backend refuses req null)
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
  elseif sig == "unverified" or sig == "registry_missing" then
    flow.record_miss = sig
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
      local request = flow.request
      local msg, why = wallet.build_transfer({
        destination = flow.destination, amount = flow.amount, blockhash = flow.blockhash,
        symbol = flow.symbol,
        memo = not request and flow.memo or nil,            -- a request payment's memo is its id,
        req_id = request and request.req_id or nil,         -- written by the firmware
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
      if not ok then flow_fail(self, balance_reason(why)) end
    end
    return self.state, self.detail
  end
  STEPS[state](self, now)
  return self.state, self.detail
end

-- flow:reason() -> the reason to show the user once the flow failed (nil otherwise): the detail,
-- except that a red UNVERIFIED RECIPIENT that came from a missing registry route is
-- "registry_missing", so it does not read as an impostor (the approval stayed red either way).
function Flow:reason()
  if self.state ~= "failed" then return nil end
  if self.detail == "unverified" and self.record_miss == "registry_missing" then return "registry_missing" end
  return self.detail
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
-- vk.receive: the payee's check of a payment (docs/os/platform/lua-api.md, "vk.receive")
-- ---------------------------------------------------------------------------------------------
--   local watch = vk.receive.start{ req_id = req.req_id, amount = "10.00",
--                                   [symbol =], [to =], [payer =], [record = true],
--                                   [every_ms =], [for_ms =] }
--   function on_espnow(mac, data) watch:result(data) end         -- every frame; it filters
--   local state, detail = watch:update()                         -- every frame
--
-- A RESULT frame is unauthenticated: anyone in range can send one naming any transaction. So a
-- status-0 RESULT only queues its `ref` (the transaction signature) to be checked; the payment is
-- "paid" only once the transaction itself has been fetched (getTransaction), did not fail on
-- chain (meta.err), and the firmware's wallet.verify_payment says those bytes pay this request:
-- this badge's token account, the amount, the token, the request memo, the payer's signature.
--
-- States:
--   "waiting"  no status-0 RESULT to check (status 1 and 2 are only noted in watch.reported)
--   "pending"  checking one or more refs
--   "paid"     detail = {payer =, sig =} (base58, from the verified transaction). Final
--   "failed"   detail = the reason the last ref was refused: verify_payment's reason (mismatch,
--              bad_proof, undecodable), "transaction failed", "not confirmed" (never visible in
--              for_ms), or a no-network code. Not final: another RESULT, or watch:again(), goes
--              back to "pending". Final only for a watch that cannot work (bad_arg, unsupported:
--              watch.fatal).
--
-- Several RESULTs: up to receive.slots (3) distinct refs are held and checked in turn (each
-- update looks up the next one that is due), so a forged RESULT with a junk ref cannot make the
-- watch ignore the real one; the first ref that verifies wins. A fourth ref is still taken: the
-- held ref tried most often (the oldest on a tie) makes room. A refused ref is remembered and not
-- tried again. A sender that keeps sending fresh junk refs faster than they are checked can delay
-- PAID (denial of service); nothing it sends can make PAID appear.
--
-- update() makes at most one blocking call: one getTransaction (vk.timeout_ms at most), or one
-- wallet.refresh_balance (when verify_payment needs this badge's token account), or the check
-- (verify_payment, about 18 ms, then record_received). Each ref is looked up every every_ms for
-- up to for_ms from its first look-up.
--
-- record (default true): after a verified payment, wallet.record_received writes the received row
-- to the history (permission history). A failure there (no permission, file full) does not undo
-- the payment: watch.recorded is false and watch.record_error says why.
--
-- Fields an app may read: state, detail, payer, sig, recorded, record_error, reported (the last
-- status 1 or 2 heard), last_reason and last_detail (of the last refused ref), fatal.

local receive = {}
vk.receive = receive

receive.slots = 3               -- refs held at once
receive.every_ms = 2000         -- between two look-ups of one ref
receive.for_ms = 30000          -- how long one ref is looked up before it counts as not confirmed
receive.remember = 8            -- refused refs remembered, so they are not tried again

local Watch = {}
Watch.__index = Watch

local ZERO_REF = string.rep("\0", 64)
-- Refusals that may pass on a later try (the transaction appears, the network comes back).
local FINAL = {mismatch = true, bad_proof = true, undecodable = true, ["transaction failed"] = true}

local function positive(value, default)
  if value == nil then return default end
  if math.type(value) ~= "integer" or value <= 0 then return nil end
  return value
end

function receive.start(opts)
  local watch = setmetatable({state = "failed", detail = "bad_arg", fatal = true, queue = {},
                              refused = {}, refused_order = {}, again_list = {}}, Watch)
  if type(opts) ~= "table" or not is_text(opts.req_id) or #opts.req_id ~= 16 or opts.req_id:find("%X")
      or not is_text(opts.amount) then
    return watch
  end
  for _, key in ipairs({"symbol", "to", "payer"}) do
    if opts[key] ~= nil and not is_text(opts[key]) then return watch end
  end
  local every, window = positive(opts.every_ms, receive.every_ms), positive(opts.for_ms, receive.for_ms)
  if not every or not window then return watch end
  if not badge.wallet.verify_payment then
    watch.detail = "unsupported"                -- a firmware with no payment check: never paid
    return watch
  end
  watch.req_id, watch.amount = opts.req_id:lower(), opts.amount
  watch.symbol, watch.to, watch.expect_payer = opts.symbol, opts.to, opts.payer
  watch.record = opts.record ~= false
  watch.every_ms, watch.for_ms = every, window
  watch.state, watch.detail, watch.fatal = "waiting", nil, false
  return watch
end

local function queue_ref(watch, ref)
  local queue = watch.queue
  if #queue >= receive.slots then
    local victim = 1
    for i = 2, #queue do
      if queue[i].tries > queue[victim].tries then victim = i end
    end
    table.remove(queue, victim)
  end
  queue[#queue + 1] = {ref = ref, sig58 = badge.codec.b58enc(ref), tries = 0, next_at = 0}
  watch.state, watch.detail = "pending", nil
end

-- watch:result(data) -> true when this RESULT queued a new ref to check. `data` is the frame (any
-- ESP-NOW payload: others are ignored) or a table from vk.result_parse.
function Watch:result(data)
  local result = type(data) == "table" and data or vk.result_parse(data)
  if type(result) ~= "table" or type(result.req_id) ~= "string" or result.req_id:lower() ~= self.req_id then
    return false
  end
  if result.status ~= vk.RESULT_OK then
    self.reported = result.status               -- never decisive: anyone can send it
    return false
  end
  if self.fatal or self.state == "paid" then return false end
  local ref = result.ref
  if type(ref) ~= "string" or #ref ~= 64 or ref == ZERO_REF or self.refused[ref] then return false end
  for i = 1, #self.queue do
    if self.queue[i].ref == ref then return false end
  end
  for i = #self.again_list, 1, -1 do
    if self.again_list[i] == ref then table.remove(self.again_list, i) end
  end
  queue_ref(self, ref)
  return true
end

-- watch:pending() -> the number of refs being checked; watch:checking() -> the base58 signature
-- of the next one, or nil.
function Watch:pending() return #self.queue end
function Watch:checking() return self.queue[1] and self.queue[1].sig58 end

-- watch:retryable() -> the number of refs that were given up for a reason that may pass (not
-- found in time, no network); watch:again() queues them again (at most receive.slots), each with
-- a new window, and returns how many.
function Watch:retryable() return #self.again_list end

function Watch:again()
  if self.fatal or self.state == "paid" then return 0 end
  local n = 0
  while #self.again_list > 0 and #self.queue < receive.slots do
    queue_ref(self, table.remove(self.again_list, 1))
    n = n + 1
  end
  return n
end

local function fail_watch(watch, reason)
  watch.fatal, watch.queue = true, {}
  watch.state, watch.detail = "failed", reason
end

local function refuse(watch, cand, reason, detail)
  for i = #watch.queue, 1, -1 do
    if watch.queue[i] == cand then table.remove(watch.queue, i) end
  end
  if FINAL[reason] then
    watch.refused[cand.ref] = true
    local order = watch.refused_order
    order[#order + 1] = cand.ref
    if #order > receive.remember then watch.refused[table.remove(order, 1)] = nil end
  else
    local list = watch.again_list
    list[#list + 1] = cand.ref
    if #list > receive.slots then table.remove(list, 1) end
  end
  watch.last_reason, watch.last_detail = reason, detail
  if #watch.queue == 0 then watch.state, watch.detail = "failed", reason end
end

-- Not this time: look again after every_ms, until the ref's window is over.
local function later(watch, cand, now, reason)
  if now >= cand.until_ms then return refuse(watch, cand, reason) end
  cand.next_at = now + watch.every_ms
end

local function expected_for(watch, cand)
  return {amount = watch.amount, req_id = watch.req_id, symbol = watch.symbol, to = watch.to,
          payer = watch.expect_payer, sig = cand.ref}
end

local function fetch(watch, cand, now)
  cand.tries = cand.tries + 1
  local tx, why = vk.rpc("getTransaction", {cand.sig58,
    {encoding = "base64", commitment = vk.commitment, maxSupportedTransactionVersion = 0}})
  if not tx then
    -- A null result: the node does not have it (yet) at this commitment.
    return later(watch, cand, now, why == "rpc reply has no result" and "not confirmed" or why)
  end
  local meta = type(tx) == "table" and tx.meta
  local body = type(tx) == "table" and tx.transaction
  if type(meta) ~= "table" or type(body) ~= "table" or type(body[1]) ~= "string" then
    return later(watch, cand, now, "rpc reply not understood")
  end
  if meta.err ~= nil then return refuse(watch, cand, "transaction failed") end
  local raw = badge.codec.b64dec(body[1])
  if not raw or raw == "" then return later(watch, cand, now, "rpc reply not understood") end
  cand.raw = raw                                -- checked by the next update
end

local function check_payment(watch, cand)
  local wallet = badge.wallet
  local expected = expected_for(watch, cand)
  local ok, info, detail = wallet.verify_payment(cand.raw, expected)
  if ok then
    if watch.record then
      local called, done, why = pcall(wallet.record_received, cand.raw, expected)
      watch.recorded = called and done == true
      if not watch.recorded then watch.record_error = tostring(called and why or done) end
    end
    watch.payer, watch.sig = info.payer, info.sig
    watch.queue, watch.again_list = {}, {}
    watch.state, watch.detail = "paid", {payer = info.payer, sig = info.sig}
    return
  end
  if info == "unsupported" and detail == "to" and not cand.refreshed then
    cand.need_refresh = true                    -- learn this badge's token account, then again
    return
  end
  if info == "bad_arg" or info == "unsupported" then return fail_watch(watch, info) end
  cand.raw = nil
  refuse(watch, cand, info, detail)
end

function Watch:update()
  if self.fatal or self.state == "paid" then return self.state, self.detail end
  local now = badge.millis()
  local cand
  for i = 1, #self.queue do
    if now >= self.queue[i].next_at then cand = self.queue[i] break end
  end
  if not cand then return self.state, self.detail end
  cand.until_ms = cand.until_ms or now + self.for_ms
  if cand.need_refresh then
    local ok, why = badge.wallet.refresh_balance()
    if ok then
      cand.need_refresh, cand.refreshed = nil, true
    else
      later(self, cand, now, balance_reason(why))
    end
  elseif cand.raw then
    check_payment(self, cand)
  else
    fetch(self, cand, now)
  end
  return self.state, self.detail
end

-- ---------------------------------------------------------------------------------------------
-- Reason texts: one table for every app (docs/os/reference/reasons.md)
-- ---------------------------------------------------------------------------------------------
-- vk.reason_text(reason, [context]) -> a sentence for the user, with the next step when there is
-- one. `reason` is a reason code of reasons.md, a code of this library (no_wifi, no_route,
-- rpc_unreachable, listener_unreachable, bad_url, registry_missing) or a message of a network
-- helper ("rpc http 503", "rpc_url not set", ...); anything else is shown as it is.
-- `context` picks other words for codes whose meaning depends on who asks:
--   "receive"  the payee checking a transaction (vk.receive)
--   "request"  wallet.request_open refusing
--   "contact"  wallet.contact_accept / contact_card refusing
--   "short"    a few words, for a row's value (History)
-- or is a table of the app's own overrides. A context falls back to the common table.

vk.REASON_MAX = 96              -- the longest text, so that it fits a screen's note lines

local REASONS = {
  ok = "Done.",
  cancelled = "You cancelled.",
  timeout = "Not answered in time.",
  undecodable = "The payment could not be read. Nothing was signed.",
  unverified = "The recipient has no verified record. It may be an impostor: do not pay.",
  revoked = "The recipient's record was revoked. Do not pay.",
  expired = "The record or the request has expired. Ask for a new request.",
  mismatch = "The payment does not match the record or the request. Nothing was signed.",
  bad_proof = "The badge that answered is not the recipient on record. Do not pay.",
  over_cap = "The amount is over this badge's limit.",
  no_time = "The clock is not set. Join Wi-Fi in Settings > Wi-Fi to set it.",
  busy = "The wallet is busy. Try again in a moment.",
  denied = "This app is not allowed to do that.",
  not_provisioned = "This badge is not set up yet. Provision it over USB.",
  too_long = "The payment is too long for this badge's key to sign.",
  sign_failed = "The key did not sign.",
  bad_arg = "That could not be used.",
  unsupported = "This badge or firmware cannot do that.",
  idle = "Nothing was waiting.",
  over_daily = "Over today's spending limit. Try tomorrow, or raise day_limit over USB.",
  low_battery = "Battery too low to approve a payment. Plug in USB power.",
  -- This library's codes.
  no_wifi = "No network: Wi-Fi is not joined. Join Wi-Fi in Settings > Wi-Fi.",
  no_route = "No network: the phone bridge failed. Join Wi-Fi in Settings > Wi-Fi.",
  rpc_unreachable = "The RPC node did not answer. Check the hotspot's internet, then try again.",
  listener_unreachable = "The laptop listener did not answer. Start the dashboard, then try again.",
  bad_url = "rpc_url or listener_url is not a usable address. Fix it over USB.",
  registry_missing = "The listener has no registry, so the recipient could not be checked. Fix the dashboard.",
  ["transaction failed"] = "The transaction failed on chain. Nothing was paid.",
  ["not confirmed"] = "Sent, but not confirmed in time. Check History later.",
  ["rpc reply not understood"] = "The RPC node's answer was not understood. Check rpc_url.",
  ["rpc reply has no result"] = "The RPC node did not have it. Try again.",
  ["registry reply not understood"] = "The registry's answer was not understood. Check the dashboard.",
}
vk.reasons = REASONS

local CONTEXTS = {
  receive = {
    undecodable = "That transaction is not a payment this badge can read.",
    mismatch = "That transaction is not this payment.",
    bad_proof = "That transaction's signature is not valid.",
    unsupported = "This badge's token account is not known yet. Join Wi-Fi, then try again.",
    bad_arg = "This request cannot be checked.",
    ["transaction failed"] = "That transaction failed on chain: nothing was paid.",
    ["not confirmed"] = "No such payment was found on chain in time.",
  },
  request = {
    busy = "Two requests are already open.",
    bad_arg = "This amount cannot be requested.",
  },
  contact = {
    bad_arg = "That was not a valid card.",
    mismatch = "The card was made for another badge.",
    expired = "The swap expired. Try again.",
    bad_proof = "The card's signature is wrong.",
    unsupported = "The contact could not be saved.",
    sign_failed = "This badge has no key.",
  },
  short = {
    ok = "ok", cancelled = "cancelled by you", timeout = "not answered in time",
    undecodable = "could not be read", unverified = "recipient not verified",
    revoked = "recipient revoked", expired = "expired", mismatch = "does not match",
    bad_proof = "wrong badge answered", over_cap = "over the limit", no_time = "clock not set",
    busy = "wallet busy", denied = "not allowed", not_provisioned = "not set up",
    too_long = "too long to sign", sign_failed = "key did not sign", bad_arg = "bad argument",
    unsupported = "not supported", idle = "nothing waiting", over_daily = "over daily limit",
    low_battery = "battery too low",
  },
}
vk.reason_contexts = CONTEXTS

local PATTERNS = {
  {"^(.+) not set$", "This badge is not set up: %s is not set. Provision it over USB."},
  {"^rpc http (%d+)$", "The RPC node answered HTTP %s. Check rpc_url."},
  {"^registry http (%d+)$", "The registry answered HTTP %s. Check the dashboard."},
  {"^listener http (%d+)$", "The listener answered HTTP %s. Check the dashboard."},
}

function vk.reason_text(reason, context)
  reason = tostring(reason == nil and "failed" or reason)
  local words = type(context) == "table" and context or CONTEXTS[context]
  local text = words and words[reason] or REASONS[reason]
  if text then return text end
  for i = 1, #PATTERNS do
    local value = reason:match(PATTERNS[i][1])
    if value then return string.format(PATTERNS[i][2], value) end
  end
  return reason                                 -- a node's own message ("insufficient funds")
end

-- ---------------------------------------------------------------------------------------------
-- vk.peers: badges heard nearby, by MAC (Contacts' swap list)
-- ---------------------------------------------------------------------------------------------
--   local list = vk.peers.new{ max = 4, timeout_ms = 4000, hold_ms = 10000 }
--   list:heard(mac, {address =, name =, frame =, rssi =}, now, [keep_mac]) -> peer, how
--   list:sent(mac, now)      a card went to that badge: its frame is held for hold_ms
--   list:expire(now)         drops badges not heard for timeout_ms
--   list:find(mac), list.items (strongest signal first)
--
-- `how`: "new"; "updated"; "held" (the same key with a new frame while a card sent to it is
-- pending: the frame is kept, so a HELLO sent as that MAC cannot change what was answered);
-- "ignored" (a known MAC with another key: a made-up HELLO cannot rename or re-key a listed
-- badge); "full" (every listed badge is kept: the selected one, `keep_mac`, and those with a card
-- pending). A new badge is never refused otherwise: the one heard longest ago makes room, so
-- made-up HELLOs from many MACs cannot lock an honest badge out.

local peers = {}
vk.peers = peers

local Peers = {}
Peers.__index = Peers

function peers.new(opts)
  opts = opts or {}
  return setmetatable({items = {}, max = opts.max or 4, timeout_ms = opts.timeout_ms or 4000,
                       hold_ms = opts.hold_ms or 10000}, Peers)
end

function Peers:find(mac)
  for i = 1, #self.items do
    if self.items[i].mac == mac then return self.items[i], i end
  end
  return nil
end

local function held(peer, now) return peer.sent_until ~= nil and now < peer.sent_until end

local function by_signal(a, b)
  if a.rssi ~= b.rssi then return a.rssi > b.rssi end
  return a.mac < b.mac
end

function Peers:heard(mac, info, now, keep)
  local peer = self:find(mac)
  local rssi = info.rssi or -127
  if peer then
    if peer.address ~= info.address then return peer, "ignored" end
    peer.name, peer.rssi, peer.heard = info.name, rssi, now
    local how = "updated"
    if info.frame ~= peer.frame then
      if held(peer, now) then
        how = "held"
      else
        peer.frame, peer.sent, peer.sent_until = info.frame, false, nil
      end
    end
    table.sort(self.items, by_signal)
    return peer, how
  end
  if #self.items >= self.max then
    local victim
    for i = 1, #self.items do
      local p = self.items[i]
      if p.mac ~= keep and not held(p, now) and (not victim or p.heard < self.items[victim].heard) then
        victim = i
      end
    end
    if not victim then return nil, "full" end
    table.remove(self.items, victim)
  end
  peer = {mac = mac, address = info.address, name = info.name, frame = info.frame, rssi = rssi,
          heard = now, sent = false}
  self.items[#self.items + 1] = peer
  table.sort(self.items, by_signal)
  return peer, "new"
end

function Peers:sent(mac, now)
  local peer = self:find(mac)
  if peer then peer.sent, peer.sent_until = true, now + self.hold_ms end
end

function Peers:expire(now)
  for i = #self.items, 1, -1 do
    if now - self.items[i].heard > self.timeout_ms then table.remove(self.items, i) end
  end
end

-- ---------------------------------------------------------------------------------------------
-- Small helpers
-- ---------------------------------------------------------------------------------------------

-- vk.keep_awake(on): badge.screen.keep_awake, or nothing on a firmware without it. A request
-- being waited on, a game being played: the screen would otherwise dim after 30 s.
function vk.keep_awake(on)
  local screen = badge.screen
  if screen and screen.keep_awake then screen.keep_awake(on and true or false) end
end

-- vk.led_pulse(token, ms): the LEDs in a colour of the active theme (RGB565 to 0..255 each).
function vk.led_pulse(token, ms)
  local c = math.floor(vk.ui.color(token))
  local r, g, b = c // 2048, (c // 32) % 64, c % 32
  badge.led.pulse(r * 255 // 31, g * 255 // 63, b * 255 // 31, ms)
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
--
-- Each function hands its work to the firmware's kit, badge.receipt, when the firmware has it:
-- one call into C per element, and the shell's own fonts (the serif title and amount). Without
-- that module (older firmware, the host tests) the same picture is drawn here with badge.gfx,
-- where a title and an amount are the built-in font scaled up. The arguments and the geometry are
-- the same either way.

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

-- badge.receipt.<name>, or nil when the firmware has no such function.
local function kit(name)
  local receipt = badge.receipt
  return receipt and receipt[name]
end

-- What the pure-Lua drawing accepts as a text, for the kit: nil is the empty text.
local function str(value)
  if value == nil then return "" end
  return tostring(value)
end
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

-- ---------------------------------------------------------------------------------------------
-- Drawing only when the screen changed (docs/os/platform/lua-api.md, "vk.ui.frame")
-- ---------------------------------------------------------------------------------------------
-- A full frame costs about 15 ms of drawing and a 34 ms transfer to the panel, and the runtime
-- calls on_draw() on every loop pass: an app that draws each time runs its loop at 20 passes a
-- second and feels slow. An app therefore keeps its drawing in one function and writes
--
--   local function draw() ... end
--   function on_draw() vk.ui.frame(draw) end
--
-- ui.frame(draw, [period_ms]) calls draw() only when the picture can have changed:
--   * ui.dirty() was called since the last frame (the app changed something it shows);
--   * a key is down, or was down on the last pass (key handlers change state; repeats scroll);
--   * on_draw was not called for ui.pause_ms: an approval was up and the canvas holds its picture;
--   * period_ms (default ui.frame_ms, 250) passed since the last frame: clocks, countdowns, and a
--     state change whose code did not call ui.dirty(). Pass 0 for an animation (every pass).
-- It returns true when it drew. Between frames a pass costs about a millisecond.
ui.frame_ms = 250
ui.pause_ms = 150

local frame_dirty, frame_at, frame_seen, frame_keys = true, nil, nil, false

function ui.dirty() frame_dirty = true end

function ui.frame(draw, period_ms)
  local now = badge.millis()
  local keys = badge.input.any()
  local due = frame_dirty or keys or frame_keys or frame_at == nil
    or now - frame_at >= (period_ms or ui.frame_ms)
    or now - frame_seen >= ui.pause_ms
  frame_seen, frame_keys = now, keys
  if not due then return false end
  frame_dirty, frame_at = false, now
  draw()
  return true
end

function ui.page()
  local native = kit("page")
  if native then return native() end
  badge.gfx.clear(ui.color("paper"))
end

-- ui.rule(y, [x0], [x1]): a dashed line, 3 px on and 2 px off, from x0 to x1 (default 10..310).
function ui.rule(y, x0, x1)
  x0, x1 = x0 or ui.MARGIN, x1 or ui.W - ui.MARGIN
  local native = kit("rule")
  if native then return native(y, x0, x1) end
  local gfx, ink = badge.gfx, ui.color("ink")
  for x = x0, x1, 5 do
    gfx.fill_rect(x, y, math.min(3, x1 - x + 1), 1, ink)
  end
end

-- ui.perforation(x, y0, y1): the dashed vertical line between a stub and its body.
function ui.perforation(x, y0, y1)
  local native = kit("perforation")
  if native then return native(x, y0, y1) end
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
  local native = kit("header")
  if native then return native(str(left), right ~= nil and str(right) or nil) end
  edge_texts(left, right == nil and ui.status() or right, 7)
  ui.rule(19)
end

-- ui.footer(left, right): clears the strip under y 216 first, like the firmware kit.
function ui.footer(left, right)
  local native = kit("footer")
  if native then return native(str(left), str(right)) end
  badge.gfx.fill_rect(0, 216, ui.W, ui.H - 216, ui.color("paper"))
  ui.rule(216, 0, ui.W - 1)
  edge_texts(left, right, 224)
end

-- ui.title(text, y, [cx]): a section title, centred on cx (default: the screen). The kit's serif
-- title font; without the kit, the built-in font at twice its size, 13 px per character like it.
function ui.title(text, y, cx)
  local native = kit("title")
  if native then return native(str(text), y, cx or ui.W // 2) end
  local cols = columns(text)
  if cols == "" then return end
  draw_centered(cols:gsub("[\1-\3]", "?"), cx or ui.W // 2, y - 1, ui.color("ink"), 1, 2)
end

-- ui.row(y, label, value, [selected], [value_color], [x0], [x1]): label left, value right, a
-- dotted leader between. x0 and x1 default to the page margins (10 and 310).
function ui.row(y, label, value, selected, value_color, x0, x1)
  x0, x1 = x0 or ui.MARGIN, x1 or ui.W - ui.MARGIN
  local native = kit("row")
  if native then return native(x0, x1, y, str(label), str(value), selected, value_color) end
  local gfx = badge.gfx
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
  local native = kit("subline")
  if native then return native(x0, x1, y, str(text), selected) end
  if selected then badge.gfx.fill_rect(x0 - 10, y, (x1 - x0) + 20, 13, ui.color("ink")) end
  draw(clip(columns(text), (x1 - x0 - 12) // CHAR_W), x0 + 12, y, ui.color(selected and "paper" or "sub"))
end

-- ui.amount(cx, y, label, value, unit): the label at y, the value as large as fits 136 px in the
-- 34 rows from y+15, the unit at y+56; all centred on cx. `value` is a string and is never cut.
function ui.amount(cx, y, label, value, unit)
  local native = kit("amount")
  if native then return native(cx, y, str(label), str(value), str(unit)) end
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
  local native = kit("barcode")
  if native then return native(x, y, w, h, type(seed) == "string" and seed ~= "" and seed or nil) end
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

-- ui.qr(x, y, size, text) -> boolean: a QR code of `text` (a link) in a size x size square at
-- (x, y), on a light patch in every theme (a phone reads dark on light only). Only the firmware's
-- kit can make one: false, with nothing drawn, without it, and for an empty or too long a text.
function ui.qr(x, y, size, text)
  local native = kit("qr")
  return native ~= nil and native(x, y, size, str(text)) == true
end

local TONES = {ok = "stamp_ok", warn = "stamp_warn", bad = "stamp_bad", mut = "faint"}

-- ui.list{title=, rows={{l=, r=, sub=, tone=}}, sel=, hint=} draws a whole list screen: header,
-- title, rows, footer. `sel` is the index of the selected row (1-based; nil selects none); the
-- window scrolls to keep it in view: 5 rows when any row has a subline, else 9. `tone` colours a
-- row's value: "ok", "warn", "bad" or "mut". The title and the labels are drawn in capitals.
-- Also read: `header` (left header text, default "BADGEOS"), `back` (right footer text, default
-- "CANCEL back"), `empty` (a line shown when there are no rows).
function ui.list(model)
  local rows = model.rows or {}
  ui.page()
  ui.header(model.header or "BADGEOS")
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
