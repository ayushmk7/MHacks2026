-- test_vk - lib/vk.lua on the laptop (docs/os/platform/lua-api.md, "lib/vk.lua"), with the
-- `badge` table stubbed: JSON, the frame helpers, the network helpers with no network and with
-- scripted replies, the vk.pay state machine, and the geometry of vk.ui.
--
-- Run by test/host/run.sh as `lua test/host/test_vk.lua` from the firmware folder. The laptop's
-- Lua may be 5.3, 5.4 or 5.5; the badge runs 5.4 with 32-bit integers and floats.

package.path = "lib/?.lua;" .. package.path

local checks = 0
local function check(condition, what)
  checks = checks + 1
  if not condition then error("FAILED: " .. tostring(what), 2) end
end
local function eq(got, want, what)
  checks = checks + 1
  if got ~= want then
    error(string.format("FAILED: %s: got %s, want %s", tostring(what), tostring(got), tostring(want)), 2)
  end
end

-- ---------------------------------------------------------------------------------------------
-- The stub of the badge
-- ---------------------------------------------------------------------------------------------

local function to_hex(bytes)
  return (bytes:gsub(".", function(c) return string.format("%02x", c:byte()) end))
end
local function from_hex(text)
  if #text % 2 ~= 0 or text:find("%X") then return nil end
  return (text:gsub("%x%x", function(pair) return string.char(tonumber(pair, 16)) end))
end

local B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
local function b64enc(bytes)
  local out = {}
  for i = 1, #bytes, 3 do
    local a, b, c = bytes:byte(i, i + 2)
    local n = a * 65536 + (b or 0) * 256 + (c or 0)
    local i1, i2, i3, i4 = n // 262144, (n // 4096) % 64, (n // 64) % 64, n % 64
    out[#out + 1] = B64:sub(i1 + 1, i1 + 1) .. B64:sub(i2 + 1, i2 + 1)
      .. (b and B64:sub(i3 + 1, i3 + 1) or "=") .. (c and B64:sub(i4 + 1, i4 + 1) or "=")
  end
  return table.concat(out)
end
local function b64dec(text)
  if #text % 4 ~= 0 or text:find("[^%w%+/=]") then return nil end
  local out = {}
  for i = 1, #text, 4 do
    local n, pad = 0, 0
    for j = i, i + 3 do
      local c = text:sub(j, j)
      if c == "=" then
        pad = pad + 1
        n = n * 64
      else
        n = n * 64 + B64:find(c, 1, true) - 1
      end
    end
    local bytes = string.char(n // 65536, (n // 256) % 256, n % 256)
    out[#out + 1] = bytes:sub(1, 3 - pad)
  end
  return table.concat(out)
end

local B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
local function b58enc(bytes)
  if #bytes > 64 then return nil end
  local digits = {}
  for i = 1, #bytes do
    local carry = bytes:byte(i)
    for j = 1, #digits do
      carry = carry + digits[j] * 256
      digits[j] = carry % 58
      carry = carry // 58
    end
    while carry > 0 do
      digits[#digits + 1] = carry % 58
      carry = carry // 58
    end
  end
  local out = {}
  for i = 1, #bytes do
    if bytes:byte(i) ~= 0 then break end
    out[#out + 1] = "1"
  end
  for j = #digits, 1, -1 do out[#out + 1] = B58:sub(digits[j] + 1, digits[j] + 1) end
  return table.concat(out)
end

eq(b58enc(string.rep("\0", 32)), string.rep("1", 32), "the stub's base58")
eq(b64enc("VK\1\4"), "VksBBA==", "the stub's base64")
eq(b64dec("VksBBA=="), "VK\1\4", "the stub's base64 decoding")

-- Everything the tests script, reset before each scenario.
local world

local function reset_world()
  world = {
    now = 1000,
    config = {rpc_url = "http://node.test:8899/", listener_url = "http://laptop.test:8788", presence_ms = "1500"},
    http = nil,                 -- function(method, url, body) -> code, reply | nil, message
    http_calls = {},
    provisioned = true,
    token_account = "SourceTokenAccount",
    refresh = nil,              -- function() -> true | nil, reason
    refresh_calls = 0,
    presence = {},              -- successive answers of wallet.presence; the last one repeats
    challenges = {},
    challenge = nil,            -- function(mac, req) -> true | nil, reason
    builds = {},
    build = nil,                -- function(opts) -> msg | nil, reason
    begins = {},
    begin = nil,                -- function(msg, ctx) -> true | nil, reason
    polls = {},                 -- successive answers of wallet.poll: "pending", a signature, or {nil, reason}
    espnow_on = true,
    sent = {},
    draws = {},
    theme = nil,
    wifi_up = true,             -- badge.wifi.connected()
    txs = {},                   -- getTransaction replies by signature (base58): {raw =, err =} or a list
    verifies = {},              -- every wallet.verify_payment call: {raw =, expected =}
    received = {},              -- every wallet.record_received call
    record_received = nil,      -- function(raw, expected) -> true | nil, reason (default: true)
    awake = {},                 -- every badge.screen.keep_awake(on)
    leds = {},                  -- every badge.led.pulse
  }
end
reset_world()

local function http_call(method, url, body)
  world.http_calls[#world.http_calls + 1] = {method = method, url = url, body = body}
  if not world.http then return nil, "wifi not connected" end
  return world.http(method, url, body)
end

local function record_draw(kind)
  return function(...)
    world.draws[#world.draws + 1] = {kind, ...}
  end
end

badge = {
  millis = function() return world.now end,
  log = function() end,
  codec = {
    hex = to_hex, unhex = from_hex, b64enc = b64enc, b64dec = b64dec, b58enc = b58enc,
  },
  http = {
    get = function(url, timeout) return http_call("GET", url, nil) end,
    post = function(url, body, content_type, timeout)
      eq(content_type, "application/json", "the content type of a POST")
      return http_call("POST", url, body)
    end,
  },
  wallet = {
    config = function(name) return world.config[name] end,
    address = function() return "ThisBadgeAddress" end,
    pubkey = function() return string.rep("\x5A", 32) end,
    provisioned = function() return world.provisioned end,
    time_ok = function() return false end,
    token_account = function() return world.token_account end,
    refresh_balance = function()
      world.refresh_calls = world.refresh_calls + 1
      if world.refresh then return world.refresh() end
      return nil, "timeout"
    end,
    challenge = function(mac, req)
      world.challenges[#world.challenges + 1] = {mac = mac, req = req}
      if world.challenge then return world.challenge(mac, req) end
      return true
    end,
    presence = function(req_id)
      local answer = world.presence[1] or "none"
      if #world.presence > 1 then table.remove(world.presence, 1) end
      return answer
    end,
    build_transfer = function(opts)
      world.builds[#world.builds + 1] = opts
      if world.build then return world.build(opts) end
      return "MSG:" .. opts.destination .. ":" .. opts.amount
    end,
    begin_solana = function(msg, ctx)
      world.begins[#world.begins + 1] = {msg = msg, ctx = ctx}
      if world.begin then return world.begin(msg, ctx) end
      return true
    end,
    poll = function()
      local answer = world.polls[1]
      if answer == nil then return nil, "idle" end
      if #world.polls > 1 then table.remove(world.polls, 1) end
      if type(answer) == "table" then return nil, answer[2] end
      return answer
    end,
    wire_tx = function(sig, msg)
      if #sig ~= 64 then return nil, "bad_arg" end
      return b64enc("\1" .. sig .. msg)
    end,
    -- A fetched transaction in the tests is "\1" .. sig(64) .. body. The body "GOOD" is the
    -- payment asked for; "BAD:<reason>:<detail>" is refused with that reason and detail.
    verify_payment = function(raw, expected)
      world.verifies[#world.verifies + 1] = {raw = raw, expected = expected}
      check(type(expected.amount) == "string" and type(expected.req_id) == "string", "verify_payment: amount and req_id are strings")
      local sig, body = raw:sub(2, 65), raw:sub(66)
      if expected.sig ~= nil and expected.sig ~= sig and expected.sig ~= b58enc(sig) then return nil, "mismatch", "sig" end
      if world.verify then return world.verify(raw, expected) end
      if body == "GOOD" then return true, {payer = "PayerKey", sig = b58enc(sig)} end
      local reason, detail = body:match("^BAD:([%w_]+):(%w*)$")
      return nil, reason or "undecodable", detail or "wire"
    end,
    record_received = function(raw, expected)
      world.received[#world.received + 1] = {raw = raw, expected = expected}
      if world.record_received then return world.record_received(raw, expected) end
      return true
    end,
  },
  wifi = {connected = function() return world.wifi_up end},
  screen = {keep_awake = function(on) world.awake[#world.awake + 1] = on end},
  led = {pulse = function(r, g, b, ms) world.leds[#world.leds + 1] = {r, g, b, ms} end},
  espnow = {
    enabled = function() return world.espnow_on end,
    enable = function(on) world.espnow_on = on ~= false return true end,
    send = function(mac, data)
      world.sent[#world.sent + 1] = {mac = mac, data = data}
      return true
    end,
  },
  battery = {charging = function() return false end, percent = function() return 86.6 end},
  gfx = {
    clear = record_draw("clear"), text = record_draw("text"), fill_rect = record_draw("fill_rect"),
    pixel = record_draw("pixel"),
  },
}

local vk = require("vk")
local json = vk.json

-- ---------------------------------------------------------------------------------------------
-- JSON
-- ---------------------------------------------------------------------------------------------

-- Deep comparison of two decoded values.
local function same(a, b)
  if type(a) ~= type(b) then return false end
  if type(a) ~= "table" then return a == b end
  if a == json.null or b == json.null then return a == b end
  for key, value in pairs(a) do
    if not same(value, b[key]) then return false end
  end
  for key in pairs(b) do
    if a[key] == nil then return false end
  end
  return true
end

do
  -- Literals and numbers.
  eq(json.decode("true"), true, "true")
  eq(json.decode("false"), false, "false")
  eq(json.decode(" 42 "), 42, "an integer")
  eq(math.type(json.decode("42")), "integer", "an integer stays an integer")
  eq(json.decode("-7"), -7, "a negative integer")
  eq(json.decode("0"), 0, "zero")
  eq(json.decode("1.5"), 1.5, "a fraction")
  eq(json.decode("-0.25"), -0.25, "a negative fraction")
  eq(json.decode("1e3"), 1000, "an exponent")
  eq(json.decode("2.5E-1"), 0.25, "a negative exponent")
  eq(json.decode('"hi"'), "hi", "a string")
  local value, why = json.decode("null")
  check(value == nil and why == nil, "null decodes to nil with no message")

  -- Escapes and unicode escapes.
  eq(json.decode([["a\"b\\c\/d\b\f\n\r\t"]]), 'a"b\\c/d\b\f\n\r\t', "the simple escapes")
  -- U is a backslash and a "u": the start of a JSON unicode escape (this file stays plain ASCII).
  local U = "\\" .. "u"
  eq(json.decode('"' .. U .. "0041" .. U .. "00e9" .. U .. '20ac"'), "A\xC3\xA9\xE2\x82\xAC", "unicode escapes become UTF-8")
  eq(json.decode('"' .. U .. "00E9" .. '"'), "\xC3\xA9", "upper-case hex digits in an escape")
  eq(json.decode('"' .. U .. "d83d" .. U .. 'de00"'), "\xF0\x9F\x98\x80", "a surrogate pair is one character")
  eq(json.decode('"' .. U .. 'd83d!"'), "\xEF\xBF\xBD!", "a lone high surrogate is the replacement character")
  eq(json.decode('"' .. U .. 'de00!"'), "\xEF\xBF\xBD!", "a lone low surrogate is the replacement character")
  eq(json.decode('"caf\xC3\xA9"'), "caf\xC3\xA9", "UTF-8 passes through")
  eq(json.decode('"' .. U .. '0000"'), "\0", "an escaped NUL")
  eq(json.decode('"x' .. U .. '0031y"'), "x1y", "an escape between plain text")

  -- Nesting.
  local doc = json.decode('{"a":{"b":[1,2,{"c":"d"}],"e":[]},"f":[[1],[2,[3]]],"g":{}}')
  check(type(doc) == "table", "a nested document decodes")
  eq(doc.a.b[3].c, "d", "an object in an array in an object")
  eq(#doc.a.b, 3, "an array's length")
  eq(doc.f[2][2][1], 3, "arrays in arrays")
  eq(#doc.a.e, 0, "an empty array")
  eq(next(doc.g), nil, "an empty object")

  -- null: left out of an object, vk.json.null in an array.
  local nulls = json.decode('{"error":null,"value":[null,1,null]}')
  eq(nulls.error, nil, "a null field is absent")
  eq(#nulls.value, 3, "an array with nulls has no hole")
  check(nulls.value[1] == json.null and nulls.value[3] == json.null, "a null element is vk.json.null")
  eq(tostring(json.null), "null", "vk.json.null prints as null")

  -- Whitespace everywhere.
  local spaced = json.decode(' \n{ "a" :\t[ 1 , 2 ] ,\r\n "b" : { } } ')
  check(spaced and spaced.a[2] == 2 and type(spaced.b) == "table", "whitespace between tokens")

  -- Malformed input returns nil and a message.
  local bad = {
    "", " ", "{", "}", "[", "]", "[1,]", "[1 2]", "{,}", '{"a"}', '{"a":}', '{"a":1,}', "{a:1}", "{'a':1}",
    '"abc', '"a\\x"', '"\\u12"', '"\\u12G4"', '"line\nbreak"', "tru", "nul", "True", "01", "-", "1.", ".5",
    "1e", "1e+", "+1", "0x10", "1 2", "[1] x", '{"a":1} {"b":2}', "nan", "[" .. string.rep("[", 40),
  }
  for i = 1, #bad do
    local result, message = json.decode(bad[i])
    check(result == nil and type(message) == "string", "malformed JSON is refused: " .. bad[i])
  end
  local result, message = json.decode(nil)
  check(result == nil and type(message) == "string", "decode(nil) is refused")
  result, message = json.decode(12)
  check(result == nil and type(message) == "string", "decode(number) is refused")

  -- Encoding.
  eq(json.encode(true), "true", "encode true")
  eq(json.encode(nil), "null", "encode nil")
  eq(json.encode(json.null), "null", "encode vk.json.null")
  eq(json.encode(12), "12", "encode an integer")
  eq(json.encode(-3), "-3", "encode a negative integer")
  eq(json.encode(1.5), "1.5", "encode a float")
  eq(json.encode(0.1), "0.1", "a float is written in its shortest form")
  eq(json.encode("a\"b\\c\n\t\1"), [["a\"b\\c\n\t\u0001"]], "encode escapes")
  eq(json.encode("caf\xC3\xA9"), '"caf\xC3\xA9"', "UTF-8 is written as it is")
  eq(json.encode({1, "two", false}), '[1,"two",false]', "encode an array")
  eq(json.encode({b = 1, a = {c = json.null}}), '{"a":{"c":null},"b":1}', "encode an object, keys sorted")
  eq(json.encode({}), "{}", "an empty table is an object")
  eq(json.encode(json.array()), "[]", "an empty array made with vk.json.array")
  eq(json.encode({[1] = "a", [3] = "c"}), nil, "a table with a hole and number keys cannot be encoded")
  eq(json.encode({{"sig"}, {searchTransactionHistory = true}}), '[["sig"],{"searchTransactionHistory":true}]',
     "RPC parameters")
  check(json.encode(print) == nil, "a function cannot be encoded")
  check(json.encode(0 / 0) == nil, "NaN cannot be encoded")
  check(json.encode(math.huge) == nil, "infinity cannot be encoded")
  local loop = {}
  loop.self = loop
  local text, why2 = json.encode(loop)
  check(text == nil and type(why2) == "string", "a table that contains itself is refused")

  -- Round trips: decode(encode(x)) is x, and encode(decode(text)) is text for canonical text.
  local values = {
    {name = "MHacks Merch", amount = "10.00", list = {1, 2, 3}, nested = {deep = {deeper = {"x", {y = true}}}}},
    {empty_array = json.array(), empty_object = {}, text = "tab\tquote\"slash\\uni\xE2\x82\xAC"},
    {1, -2, 3.25, 1000000, "s", true, false, json.null, {}, json.array()},
    "plain", 0, -1, 2147483647, 0.5, true, false,
  }
  for i = 1, #values do
    local encoded = json.encode(values[i])
    check(type(encoded) == "string", "round trip " .. i .. " encodes")
    local decoded = json.decode(encoded)
    check(same(decoded, values[i]), "round trip " .. i .. ": " .. encoded)
    eq(json.encode(decoded), encoded, "round trip " .. i .. " encodes the same again")
  end
  local canonical = {
    '{"a":[],"b":{},"c":[{}],"d":[[]]}',
    '[1,null,3]',
    '{"jsonrpc":"2.0","id":1,"result":{"context":{"slot":5},"value":{"blockhash":"abc","lastValidBlockHeight":77}}}',
    '"\\u0001\\n"',
    "[]", "{}", "[[],[[]]]",
  }
  for i = 1, #canonical do
    local decoded = json.decode(canonical[i])
    if i ~= 3 then eq(json.encode(decoded), canonical[i], "canonical text " .. i) end
    check(same(json.decode(json.encode(decoded)), decoded), "canonical text " .. i .. " is stable")
  end
  -- A decoded empty array stays an array, a decoded empty object an object.
  eq(json.encode(json.decode("[]")), "[]", "[] round trip")
  eq(json.encode(json.decode("{}")), "{}", "{} round trip")
end

-- The JSON table the badge runs (apps/vktest/config.lua, run there by the vktest app), run here too.
do
  local cfg = dofile("apps/vktest/config.lua")
  check(#cfg.json >= 10 and #cfg.json_bad >= 10, "the vktest JSON table is there")
  for i, case in ipairs(cfg.json) do
    local value, why = json.decode(case.text)
    check(not (value == nil and why ~= nil), "vktest JSON case " .. i .. " decodes")
    eq(json.encode(value), case.encoded or case.text, "vktest JSON case " .. i .. " encodes")
  end
  for i, text in ipairs(cfg.json_bad) do
    local value, why = json.decode(text)
    check(value == nil and type(why) == "string", "vktest bad JSON " .. i .. " is refused")
  end
end

-- ---------------------------------------------------------------------------------------------
-- Frames
-- ---------------------------------------------------------------------------------------------

local REQ_FRAME = from_hex(
  "564b0101012cd3e806c69ed2ca678a382228c10b3faa48850a7de06dd799bd7d2463ab76fde8030000000000004841434b"
  .. "13cec0adb3e69e6ed83db16a0c4d4861636b73204d657263682834ab57d5557891accf6c286347904d0d0cba1dc6770198"
  .. "bce2744e63af89ba3364902696461daf4443f4ab56df0073d83176ea1c39a80e8fb11ab46d034403")
local REQ_ID = "13cec0adb3e69e6e"
local PAYEE_KEY = from_hex("2cd3e806c69ed2ca678a382228c10b3faa48850a7de06dd799bd7d2463ab76fd")

do
  eq(vk.frame_type(REQ_FRAME), 1, "the type of a REQ frame")
  eq(vk.frame_type("VK\1\16"), 16, "a frame with no body still has a type")
  eq(vk.frame_type("VK\2\4rest"), nil, "another version is not a VK frame")
  eq(vk.frame_type("VK\1"), nil, "three bytes are not a frame")
  eq(vk.frame_type("hello world"), nil, "a plain app message")
  eq(vk.frame_type(""), nil, "an empty payload")
  eq(vk.frame_type(nil), nil, "nil")
  eq(vk.frame_type(42), nil, "a number")

  -- RESULT.
  local sig = string.rep("\xAB", 63) .. "\0"
  local frame = vk.result_frame(REQ_ID, 0, sig)
  eq(#frame, 77, "a RESULT frame is 77 bytes")
  eq(to_hex(frame:sub(1, 13)), "564b0104" .. REQ_ID .. "00", "the RESULT header, id and status")
  eq(frame:sub(14), sig, "the RESULT ref")
  local parsed = vk.result_parse(frame)
  check(parsed and parsed.req_id == REQ_ID and parsed.status == 0 and parsed.ref == sig, "RESULT round trip")
  eq(vk.frame_type(frame), 4, "the type of a RESULT frame")

  local rejected = vk.result_frame(REQ_ID:upper(), 1)
  eq(to_hex(rejected:sub(5, 12)), REQ_ID, "an upper-case id is accepted")
  eq(rejected:sub(14), string.rep("\0", 64), "no signature means zeros")
  eq(vk.result_parse(rejected).status, 1, "status rejected")
  eq(vk.result_parse(vk.result_frame(REQ_ID, 2)).status, 2, "status failed")
  eq(vk.RESULT_OK, 0, "RESULT_OK")
  eq(vk.RESULT_REJECTED, 1, "RESULT_REJECTED")
  eq(vk.RESULT_FAILED, 2, "RESULT_FAILED")

  local none, why = vk.result_frame("13ce", 0, sig)
  check(none == nil and why == "bad_arg", "a short req_id is refused")
  check(vk.result_frame("zzcec0adb3e69e6e", 0, sig) == nil, "a req_id that is not hex is refused")
  check(vk.result_frame(REQ_ID, 3, sig) == nil, "status 3 is refused")
  check(vk.result_frame(REQ_ID, "ok", sig) == nil, "a status that is not a number is refused")
  check(vk.result_frame(REQ_ID, 0, "short") == nil, "a short signature is refused")
  check(vk.result_frame(nil, 0, sig) == nil, "no req_id is refused")

  eq(vk.result_parse(frame:sub(1, 76)), nil, "a truncated RESULT")
  eq(vk.result_parse(frame .. "x"), nil, "an over-long RESULT")
  eq(vk.result_parse(frame:sub(1, 12) .. "\3" .. frame:sub(14)), nil, "a RESULT with status 3")
  eq(vk.result_parse(REQ_FRAME), nil, "a REQ is not a RESULT")
  eq(vk.result_parse("junk"), nil, "junk is not a RESULT")
  eq(vk.result_parse(nil), nil, "nil is not a RESULT")

  -- CONTACT_HELLO: header, pubkey[32], nonce[16], name_len, name.
  local function hello(name, name_len)
    return "VK\1\16" .. PAYEE_KEY .. string.rep("\x11", 16) .. string.char(name_len or #name) .. name
  end
  local card = vk.hello_parse(hello("Judge B"))
  check(card and card.name == "Judge B", "a HELLO's name")
  eq(card.address, b58enc(PAYEE_KEY), "a HELLO's address is the key in base58")
  eq(vk.hello_parse(hello(string.rep("n", 32))).name, string.rep("n", 32), "a 32-character name")
  eq(vk.hello_parse(hello("")), nil, "an empty name")
  eq(vk.hello_parse(hello(string.rep("n", 33))), nil, "a 33-character name")
  eq(vk.hello_parse(hello("Judge B", 6)), nil, "a name longer than name_len says")
  eq(vk.hello_parse(hello("Judge B", 8)), nil, "a name shorter than name_len says")
  eq(vk.hello_parse(hello("Jud\xC3\xA9")), nil, "a name that is not printable ASCII")
  eq(vk.hello_parse(frame), nil, "a RESULT is not a HELLO")
  eq(vk.hello_parse("VK\1\16"), nil, "a HELLO with no body")
  eq(vk.hello_parse(nil), nil, "nil is not a HELLO")

  -- App-range frames.
  local invite = vk.app_frame(64, "5.00|game1")
  eq(invite, "VK\1\64" .. "5.00|game1", "an app frame")
  eq(vk.frame_type(invite), 64, "an app frame's type")
  eq(vk.app_body(invite, 64), "5.00|game1", "an app frame's body")
  eq(vk.app_body(invite, 65), nil, "another type does not match")
  eq(vk.app_body("5.00|game1", 64), nil, "a plain message does not match")
  eq(vk.app_body(vk.app_frame(255), 255), "", "an empty body")
  eq(#vk.app_frame(72, string.rep("x", 236)), 240, "the longest body is 236 bytes")
  check(vk.app_frame(72, string.rep("x", 237)) == nil, "a 237-byte body is refused")
  check(vk.app_frame(63, "x") == nil, "type 63 belongs to the firmware")
  check(vk.app_frame(4, "x") == nil, "type 4 belongs to the firmware")
  check(vk.app_frame(256, "x") == nil, "type 256 does not exist")
  check(vk.app_frame("64", "x") == nil, "a type that is not a number is refused")
  eq(vk.app_body(nil, 64), nil, "nil has no body")
end

eq(vk.short("Gn2GQYmcQkatE2594ov2ELKkYWZHwARG7FiAoPWSEcxq"), "Gn2G..Ecxq", "a short address")
eq(vk.short("short"), "short", "a short text is not shortened")

-- ---------------------------------------------------------------------------------------------
-- Network helpers
-- ---------------------------------------------------------------------------------------------

local SIG = string.rep("\x07", 64)
local SIG_B58 = b58enc(SIG)
local PAYEE = b58enc(PAYEE_KEY)
local RECORD = table.concat({
  "v=1", "attestation=GpKipmyKAyJYMdymUMbhjXX91rDxrLENh6YTY64UuPQg", "display_name=MHacks Merch",
  "device_pubkey=2cd3e806c69ed2ca678a382228c10b3faa48850a7de06dd799bd7d2463ab76fd", "kind=merchant",
  "solana_wallet=ea67df107eddb995d02c641549ff5dd48158ea41ef40bc3738ac3bdd0eb5bdca",
  "solana_ata=178d80b67636ac4539a4c92fb3285cb17684ef97a9bdb5c354bf16b5f4e22b5b",
  "bank_ref_hash=", "expiry=1800000000", "status=active", "issued_at=1790000000",
}, "\n")
local RECORD_SIG = string.rep("\x33", 64)
local ATA = b58enc(from_hex("178d80b67636ac4539a4c92fb3285cb17684ef97a9bdb5c354bf16b5f4e22b5b"))
-- The two 404s the listener can give (dashboard/server/src/http.js): the registry route with no
-- record for the key, and no such route at all.
local NO_RECORD = '{"error":{"code":"not_found","message":"no registry record for this key"}}'
local NO_ROUTE = '{"error":{"code":"not_found","message":"no such route"}}'

-- Every network helper, called once. Returns the list of {name, first, second}.
local function call_helpers()
  return {
    {"rpc", vk.rpc("getHealth", {})},
    {"blockhash", vk.blockhash()},
    {"send_tx", vk.send_tx("AQID")},
    {"confirm", vk.confirm(SIG_B58)},
    {"record", vk.record(PAYEE)},
    {"report", vk.report({payee = PAYEE, reason = "unverified"})},
    {"feed", vk.feed(SIG_B58, REQ_FRAME)},
  }
end

do
  -- No network: badge.http returns nil, message. Every helper returns nil and that message.
  reset_world()
  local results = call_helpers()
  for i = 1, #results do
    local name, first, second = results[i][1], results[i][2], results[i][3]
    check(first == nil, "with no network vk." .. name .. " returns nil")
    eq(second, "no_wifi", "with no network vk." .. name .. " says no_wifi")
  end
  eq(#world.http_calls, 7, "one request per helper, no retries")
  eq(vk.last_error, "wifi not connected", "the transport's own message is kept in vk.last_error")

  -- A transport failure with no message at all still gives a message.
  reset_world()
  world.http = function() return nil end
  results = call_helpers()
  for i = 1, #results do
    check(results[i][2] == nil and type(results[i][3]) == "string", "a message for vk." .. results[i][1])
  end

  -- Nothing provisioned: no request is made.
  reset_world()
  world.config = {rpc_url = "", listener_url = ""}
  results = call_helpers()
  for i = 1, #results do
    local name, first, second = results[i][1], results[i][2], results[i][3]
    local want = i <= 4 and "rpc_url not set" or "listener_url not set"
    check(first == nil and second == want, "with no URL vk." .. name .. " says " .. want)
  end
  eq(#world.http_calls, 0, "no request without a URL")
  reset_world()
  world.config = {}
  local first, second = vk.rpc("getHealth")
  check(first == nil and second == "rpc_url not set", "an unregistered config key counts as not set")

  -- Bad replies.
  local bad_replies = {
    {500, "Internal Server Error"}, {200, "not json"}, {200, "[1,2]"}, {200, '{"jsonrpc":"2.0","id":1}'},
    {200, ""}, {502, '{"oops":true}'}, {200, '{"result":null}'},
  }
  for i = 1, #bad_replies do
    reset_world()
    world.http = function() return bad_replies[i][1], bad_replies[i][2] end
    results = call_helpers()
    for j = 1, 5 do   -- rpc, blockhash, send_tx, confirm, record
      check(results[j][2] == nil and type(results[j][3]) == "string",
            "bad reply " .. i .. " makes vk." .. results[j][1] .. " return nil, message")
    end
  end
  reset_world()
  world.http = function() return 500, "no" end
  results = call_helpers()
  eq(results[1][3], "rpc http 500", "the message of an RPC status")
  eq(results[5][3], "registry http 500", "the message of a registry status")
  eq(results[6][3], "listener http 500", "the message of a feed status")
  eq(results[7][3], "listener http 500", "the message of a feed status")

  -- vk.rpc: the request and the result.
  reset_world()
  world.http = function(method, url, body)
    eq(method, "POST", "an RPC call is a POST")
    eq(url, "http://node.test:8899", "the RPC URL, without its trailing slash")
    local call = json.decode(body)
    check(call and call.jsonrpc == "2.0" and call.id == 1 and call.method == "getBalance", "the JSON-RPC envelope")
    eq(call.params[1], "Addr", "the first parameter")
    return 200, '{"jsonrpc":"2.0","result":{"context":{"slot":1},"value":5000},"id":1}'
  end
  local result = vk.rpc("getBalance", {"Addr"})
  check(type(result) == "table" and result.value == 5000, "vk.rpc returns the result table")

  reset_world()
  world.http = function(method, url, body)
    check(body:find('"params":[]', 1, true), "no parameters are sent as an empty array")
    return 200, '{"jsonrpc":"2.0","result":"ok","id":1}'
  end
  eq(vk.rpc("getHealth"), "ok", "a result that is a string")
  eq(vk.rpc("getHealth", {}), "ok", "an empty parameter table")

  reset_world()
  world.http = function()
    return 200, '{"jsonrpc":"2.0","error":{"code":-32002,"message":"Blockhash not found"},"id":1}'
  end
  first, second = vk.rpc("sendTransaction", {"AQID"})
  check(first == nil and second == "Blockhash not found", "an RPC error gives its message")
  world.http = function() return 400, '{"jsonrpc":"2.0","error":{"code":-32600},"id":1}' end
  first, second = vk.rpc("x")
  check(first == nil and second == "rpc error", "an RPC error with no message")

  -- vk.blockhash.
  reset_world()
  world.http = function(method, url, body)
    local call = json.decode(body)
    eq(call.method, "getLatestBlockhash", "the blockhash method")
    eq(call.params[1].commitment, "confirmed", "the blockhash commitment")
    return 200, '{"jsonrpc":"2.0","result":{"context":{"slot":2792},"value":{"blockhash":"EkSnNWid2cvwEVnVx9aBqawnmiCNiDgp3gUdkDPTKN1N","lastValidBlockHeight":3090}},"id":1}'
  end
  eq(vk.blockhash(), "EkSnNWid2cvwEVnVx9aBqawnmiCNiDgp3gUdkDPTKN1N", "vk.blockhash")
  world.http = function() return 200, '{"jsonrpc":"2.0","result":{"value":{}},"id":1}' end
  first, second = vk.blockhash()
  check(first == nil and type(second) == "string", "a reply with no blockhash")

  -- vk.send_tx.
  reset_world()
  world.http = function(method, url, body)
    local call = json.decode(body)
    eq(call.method, "sendTransaction", "the send method")
    eq(call.params[1], "AQID", "the wire transaction")
    eq(call.params[2].encoding, "base64", "the encoding")
    return 200, '{"jsonrpc":"2.0","result":"' .. SIG_B58 .. '","id":1}'
  end
  eq(vk.send_tx("AQID"), SIG_B58, "vk.send_tx returns the signature")
  first, second = vk.send_tx("")
  check(first == nil and second == "bad_arg", "an empty transaction is refused")
  first, second = vk.send_tx(nil)
  check(first == nil and second == "bad_arg", "no transaction is refused")

  -- vk.confirm.
  local function status_reply(value)
    return function(method, url, body)
      local call = json.decode(body)
      eq(call.method, "getSignatureStatuses", "the status method")
      eq(call.params[1][1], SIG_B58, "the signature asked about")
      return 200, '{"jsonrpc":"2.0","result":{"context":{"slot":82},"value":[' .. value .. ']},"id":1}'
    end
  end
  reset_world()
  world.http = status_reply("null")
  eq(vk.confirm(SIG_B58), "pending", "a signature the node has not seen")
  world.http = status_reply('{"slot":72,"confirmations":null,"err":null,"confirmationStatus":"processed"}')
  eq(vk.confirm(SIG_B58), "pending", "processed is still pending")
  world.http = status_reply('{"slot":72,"confirmations":3,"err":null,"confirmationStatus":"confirmed"}')
  eq(vk.confirm(SIG_B58), "confirmed", "confirmed")
  eq(vk.confirm(SIG), "confirmed", "the 64 raw bytes of a RESULT frame are accepted too")
  world.http = status_reply('{"slot":72,"confirmations":null,"err":null,"confirmationStatus":"finalized"}')
  eq(vk.confirm(SIG_B58), "confirmed", "finalized counts as confirmed")
  world.http = status_reply('{"slot":72,"err":{"InstructionError":[0,{"Custom":1}]},"confirmationStatus":"confirmed"}')
  eq(vk.confirm(SIG_B58), "failed", "a transaction that failed on chain")
  first, second = vk.confirm("")
  check(first == nil and second == "bad_arg", "no signature is refused")

  -- vk.record.
  reset_world()
  world.http = function(method, url)
    eq(method, "GET", "the registry is read with GET")
    eq(url, "http://laptop.test:8788/registry/" .. PAYEE, "the registry URL")
    return 200, json.encode({record = b64enc(RECORD), sig = b64enc(RECORD_SIG)})
  end
  local record, sig = vk.record(PAYEE)
  check(record == RECORD and sig == RECORD_SIG, "vk.record returns the record and its signature as bytes")
  world.http = function() return 404, '{"error":"no attestation"}' end
  first, second = vk.record(PAYEE)
  check(first == nil and second == "unverified", "404 is unverified")
  -- Audit finding 2: a 404 that does not say "no record for this key" is the route missing, not an
  -- impostor. Neither is ever a record.
  world.http = function() return 404, NO_RECORD end
  first, second = vk.record(PAYEE)
  check(first == nil and second == "unverified", "404 naming the record is unverified")
  for _, body in ipairs({NO_ROUTE, "Not Found", "", "{}", '{"error":"not_found"}'}) do
    world.http = function() return 404, body end
    first, second = vk.record(PAYEE)
    check(first == nil and second == "registry_missing", "404 " .. body .. " is registry_missing")
  end
  world.http = function() return nil, "connection refused" end
  first, second = vk.record(PAYEE)
  check(first == nil and second == "listener_unreachable", "a listener that does not answer")
  eq(vk.last_error, "connection refused", "with the transport's message kept")
  world.http = function() return nil, "read Timeout" end
  first, second = vk.rpc("getHealth")
  check(first == nil and second == "rpc_unreachable", "a node that does not answer")
  world.http = function() return nil, "bridge unavailable" end
  first, second = vk.rpc("getHealth")
  check(first == nil and second == "no_route", "the phone bridge failing is no_route")
  world.http = function() return nil, "no network" end
  first, second = vk.record(PAYEE)
  check(first == nil and second == "no_wifi", "net_route's own no network is no_wifi")
  for _, code in ipairs({"no_wifi", "no_route", "rpc_unreachable", "listener_unreachable"}) do
    check(vk.offline(code), code .. " is in the no-network family")
  end
  check(not vk.offline("unverified") and not vk.offline(nil), "other reasons are not")
  world.http = function() return 200, json.encode({record = b64enc(RECORD), sig = b64enc("short")}) end
  first, second = vk.record(PAYEE)
  check(first == nil and second == "registry reply not understood", "a signature that is not 64 bytes")
  world.http = function() return 200, json.encode({record = "***", sig = b64enc(RECORD_SIG)}) end
  first, second = vk.record(PAYEE)
  check(first == nil and second == "registry reply not understood", "a record that is not base64")
  world.http_calls = {}
  first, second = vk.record("not/base58?x=1")
  check(first == nil and second == "bad_arg" and #world.http_calls == 0, "an address that is not base58 is not sent")
  first, second = vk.record(nil)
  check(first == nil and second == "bad_arg", "no address")

  -- vk.report.
  reset_world()
  world.http = function(method, url, body)
    eq(method, "POST", "a report is a POST")
    eq(url, "http://laptop.test:8788/feed/event", "the report URL")
    local event = json.decode(body)
    eq(event.payer, "ThisBadgeAddress", "the payer defaults to this badge")
    eq(event.payee, PAYEE, "the payee")
    eq(event.reason, "mismatch", "the reason")
    eq(event.req, b64enc(REQ_FRAME), "the request, base64")
    return 200, '{"ok":true}'
  end
  eq(vk.report({payee = PAYEE, reason = "mismatch", req = REQ_FRAME}), true, "vk.report")
  world.http = function(method, url, body)
    check(body:find('"req":null', 1, true), "no request is sent as null")
    eq(json.decode(body).payer, "Someone", "a payer that was given")
    return 200, '{"ok":true}'
  end
  eq(vk.report({payee = PAYEE, reason = "revoked", payer = "Someone"}), true, "vk.report without a request")
  world.http_calls = {}
  for _, reason in ipairs({"cancelled", "timeout", "over_cap", "undecodable", "wifi not connected"}) do
    first, second = vk.report({payee = PAYEE, reason = reason})
    check(first == nil and second == "not reported", reason .. " is not reported")
  end
  eq(#world.http_calls, 0, "nothing is sent for a reason the feed does not show")
  for _, reason in ipairs({"unverified", "revoked", "expired", "mismatch", "bad_proof"}) do
    world.http = function() return 200, '{"ok":true}' end
    eq(vk.report({payee = PAYEE, reason = reason}), true, reason .. " is reported")
  end
  first, second = vk.report("unverified")
  check(first == nil and second == "bad_arg", "a report that is not a table")

  -- vk.feed.
  reset_world()
  world.http = function(method, url, body)
    eq(url, "http://laptop.test:8788/feed/solana", "the feed URL")
    local row = json.decode(body)
    eq(row.tx_sig, SIG_B58, "the transaction signature")
    eq(row.req, b64enc(REQ_FRAME), "the request, base64")
    return 200, '{"ok":true}'
  end
  eq(vk.feed(SIG_B58, REQ_FRAME), true, "vk.feed")
  world.http = function() return 201, "" end
  eq(vk.feed(SIG_B58, REQ_FRAME), true, "a 2xx with no JSON body is a success")
  -- (2) A payment that answers no request is not posted (the backend refuses req null), and why
  -- is logged.
  world.http_calls = {}
  local logged = {}
  local saved_log = badge.log
  badge.log = function(line) logged[#logged + 1] = line end
  first, second = vk.feed(SIG_B58, nil)
  check(first == nil and second == "no request", "vk.feed without a request is skipped")
  first, second = vk.feed(SIG_B58, "")
  check(first == nil and second == "no request", "an empty request too")
  badge.log = saved_log
  eq(#world.http_calls, 0, "nothing is posted for a payment with no request")
  check(logged[1] and logged[1]:find("no request", 1, true), "the skip is logged")
  first, second = vk.feed(nil, nil)
  check(first == nil and second == "bad_arg", "no signature")

  -- (1) A 200 whose body says ok false is a refusal, for the feed and for a report.
  world.http = function() return 200, '{"ok":false,"reason":"bad_request"}' end
  first, second = vk.feed(SIG_B58, REQ_FRAME)
  check(first == nil and second == "bad_request", "200 ok:false on /feed/solana is nil, its reason")
  first, second = vk.report({payee = PAYEE, reason = "unverified"})
  check(first == nil and second == "bad_request", "200 ok:false on /feed/event is nil, its reason")
  world.http = function() return 200, '{"ok":false}' end
  first, second = vk.feed(SIG_B58, REQ_FRAME)
  check(first == nil and second == "listener refused", "ok:false with no reason")
  world.http = function() return 200, '{"ok":true}' end
  eq(vk.feed(SIG_B58, REQ_FRAME), true, "ok:true is a success")

  -- A registry fetch that times out reads as the network, never as an impostor.
  world.http = function() return nil, "read Timeout" end
  first, second = vk.record(PAYEE)
  check(first == nil and second == "listener_unreachable", "a registry timeout is listener_unreachable")
  check(vk.offline(second), "and in the no-network family")
  check(vk.reason_text(second) ~= vk.reason_text("unverified"), "its text is not the no-record text")
  check(not vk.reason_text(second):lower():find("impostor", 1, true), "its text says nothing of an impostor")
end

-- ---------------------------------------------------------------------------------------------
-- vk.pay
-- ---------------------------------------------------------------------------------------------

-- Calls flow:update() every 100 ms until it is done or failed, as an app does every frame.
-- Returns the final state and detail, and the states passed through. A flow that has not ended
-- after `limit` updates (default 2000: more than three minutes) fails the test.
local function run_flow(flow, limit)
  local seen, last = {}, nil
  for _ = 1, limit or 2000 do
    local state, detail = flow:update()
    if state ~= last then
      seen[#seen + 1] = state
      last = state
    end
    if state == "done" or state == "failed" then return state, detail, table.concat(seen, " ") end
    check(detail == nil, "no detail before the end")
    world.now = world.now + 100
  end
  error("the flow did not end: " .. table.concat(seen, " "), 2)
end

-- An HTTP handler that plays the laptop and the RPC node. `opts` turns parts off.
local function network(opts)
  opts = opts or {}
  local confirms = 0
  return function(method, url, body)
    if url:find("/registry/", 1, true) then
      if opts.no_record then return 404, NO_RECORD end
      if opts.no_route then return 404, NO_ROUTE end
      return 200, json.encode({record = b64enc(RECORD), sig = b64enc(RECORD_SIG)})
    end
    if url:find("/feed/", 1, true) then
      if opts.feed_fails then return nil, "timeout" end
      return 200, '{"ok":true}'
    end
    local call = json.decode(body)
    if call.method == "getLatestBlockhash" then
      return 200, '{"jsonrpc":"2.0","result":{"value":{"blockhash":"Blockhash111"}},"id":1}'
    end
    if call.method == "sendTransaction" then
      if opts.send_fails then
        return 200, '{"jsonrpc":"2.0","error":{"code":-32002,"message":"insufficient funds"},"id":1}'
      end
      return 200, '{"jsonrpc":"2.0","result":"' .. SIG_B58 .. '","id":1}'
    end
    if call.method == "getSignatureStatuses" then
      confirms = confirms + 1
      if opts.never_confirms or confirms < 3 then return 200, '{"jsonrpc":"2.0","result":{"value":[null]},"id":1}' end
      if opts.tx_fails then
        return 200, '{"jsonrpc":"2.0","result":{"value":[{"err":{"x":1},"confirmationStatus":"confirmed"}]},"id":1}'
      end
      return 200, '{"jsonrpc":"2.0","result":{"value":[{"err":null,"confirmationStatus":"confirmed"}]},"id":1}'
    end
    return 500, "unexpected"
  end
end

local function count_calls(needle)
  local n = 0
  for i = 1, #world.http_calls do
    local call = world.http_calls[i]
    if call.url:find(needle, 1, true) or (call.body and call.body:find(needle, 1, true)) then n = n + 1 end
  end
  return n
end

local function request_entry()
  return {
    req = REQ_FRAME, mac = "aa:bb:cc:dd:ee:ff", rssi = -40, name = "MHacks Merch", amount = "10.00",
    currency = "HACK", rail = "solana", payee = PAYEE, req_id = REQ_ID, age_ms = 100, expires_in_s = 50,
  }
end

do
  -- With no network a shop payment fails at its first network call, with the transport's message.
  reset_world()
  local flow = vk.pay.start({to = PAYEE, amount = "5.00", memo = "sword"})
  eq(flow.state, "record", "a payment with no request starts at the record")
  local state, detail, seen = run_flow(flow)
  eq(state, "failed", "no network: the flow fails")
  eq(detail, "no_wifi", "no network: the reason is no_wifi")
  eq(seen, "record failed", "no network: it fails in the record step")
  eq(flow.failed_in, "record", "the step it failed in")
  eq(#world.begins, 0, "no network: no approval was opened")
  local again, again_detail = flow:update()
  check(again == "failed" and again_detail == detail, "a failed flow stays failed")
  eq(#world.http_calls, 1, "a failed flow makes no more requests")

  -- No network, paying a request: two challenges, no proof, then the same failure.
  reset_world()
  world.presence = {"pending"}
  flow = vk.pay.start({request = request_entry()})
  eq(flow.state, "presence", "paying a request starts with presence")
  state, detail, seen = run_flow(flow)
  check(state == "failed" and detail == "no_wifi", "no network: a request payment fails")
  eq(seen, "presence record failed", "no network: presence, then the record")
  eq(#world.challenges, 2, "two challenges, then no more")
  eq(#world.sent, 0, "nothing is sent to the payee before the approval")

  -- The token account is not known and cannot be fetched.
  reset_world()
  world.token_account = nil
  flow = vk.pay.start({to = PAYEE, amount = "5.00"})
  state, detail = run_flow(flow)
  check(state == "failed" and detail == "rpc_unreachable", "an unknown token account that cannot be fetched (joined)")
  eq(world.refresh_calls, 1, "refresh_balance is called once")
  eq(#world.http_calls, 0, "nothing else is tried")
  reset_world()
  world.token_account = nil
  world.wifi_up = false
  state, detail = run_flow(vk.pay.start({to = PAYEE, amount = "5.00"}))
  check(state == "failed" and detail == "no_wifi", "a balance timeout with Wi-Fi not joined is no_wifi")

  -- Not provisioned.
  reset_world()
  world.provisioned = false
  state, detail = run_flow(vk.pay.start({to = PAYEE, amount = "5.00"}))
  check(state == "failed" and detail == "not_provisioned", "an unprovisioned badge")

  -- Bad options: a flow object that has already failed.
  reset_world()
  local bad = {
    false, {}, {to = PAYEE}, {amount = "5.00"}, {to = PAYEE, amount = 5}, {to = PAYEE, amount = ""},
    {to = "", amount = "5.00"}, {to = PAYEE, amount = "5.00", symbol = 7}, {to = PAYEE, amount = "5.00", memo = 7},
    {request = "entry"}, {request = {}}, {request = {req = REQ_FRAME, mac = "aa:bb:cc:dd:ee:ff"}},
    {to = PAYEE, amount = "5.00", destination = 7},
  }
  for i = 1, #bad do
    flow = vk.pay.start(bad[i] or nil)
    state, detail = flow:update()
    check(state == "failed" and detail == "bad_arg", "bad options " .. i .. " give failed, bad_arg")
  end
  local bank = request_entry()
  bank.rail = "bank"
  state, detail = vk.pay.start({request = bank}):update()
  check(state == "failed" and detail == "unsupported", "a bank request is not paid with this flow")
  eq(#world.http_calls, 0, "bad options make no request")

  -- A shop payment that goes through.
  reset_world()
  world.http = network()
  world.polls = {"pending", "pending", SIG}
  flow = vk.pay.start({to = PAYEE, amount = "5.00", symbol = "HACK", memo = "sword"})
  state, detail, seen = run_flow(flow)
  eq(state, "done", "the shop payment is done")
  eq(detail, SIG_B58, "done: the detail is the signature in base58")
  eq(flow.signature, SIG_B58, "flow.signature")
  eq(seen, "record blockhash approve submit confirm done", "the states of a shop payment")
  eq(world.refresh_calls, 0, "a known token account is not fetched again")
  eq(#world.builds, 1, "one transfer was built")
  eq(world.builds[1].destination, ATA, "the destination is the record's token account")
  eq(world.builds[1].amount, "5.00", "the amount is passed as the string it was given")
  eq(world.builds[1].blockhash, "Blockhash111", "the blockhash")
  eq(world.builds[1].symbol, "HACK", "the symbol")
  eq(world.builds[1].memo, "sword", "the memo")
  eq(#world.begins, 1, "one approval was opened")
  eq(world.begins[1].ctx.record, RECORD, "ctx.record")
  eq(world.begins[1].ctx.record_sig, RECORD_SIG, "ctx.record_sig")
  eq(world.begins[1].ctx.req, nil, "no ctx.req without a request")
  eq(count_calls("sendTransaction"), 1, "sent once")
  eq(count_calls("getSignatureStatuses"), 3, "asked for the status until confirmed")
  eq(count_calls("/feed/solana"), 0, "a shop payment is not fed (no request)")
  eq(#world.sent, 0, "no RESULT frame without a request")
  eq(#world.challenges, 0, "no challenge without a request")

  -- The token account is fetched first when it is not known.
  reset_world()
  world.http = network()
  world.token_account = nil
  world.refresh = function()
    world.token_account = "SourceTokenAccount"
    return true
  end
  world.polls = {SIG}
  state = run_flow(vk.pay.start({to = PAYEE, amount = "5.00"}))
  eq(state, "done", "a payment after fetching the token account")
  eq(world.refresh_calls, 1, "refresh_balance was called once")

  -- The feed failing does not fail the payment.
  reset_world()
  world.http = network({feed_fails = true})
  world.polls = {SIG}
  state, detail = run_flow(vk.pay.start({to = PAYEE, amount = "5.00"}))
  check(state == "done" and detail == SIG_B58, "a feed failure is ignored")

  -- Paying a request, with the payee present.
  reset_world()
  world.http = network()
  world.espnow_on = false
  world.presence = {"pending", "pending", "present"}
  world.polls = {"pending", SIG}
  flow = vk.pay.start({request = request_entry()})
  state, detail, seen = run_flow(flow)
  eq(state, "done", "the request payment is done")
  eq(seen, "presence record blockhash approve submit confirm done", "the states of a request payment")
  eq(world.espnow_on, true, "ESP-NOW was turned on for the challenge")
  eq(#world.challenges, 1, "one challenge")
  eq(world.challenges[1].mac, "aa:bb:cc:dd:ee:ff", "the challenge goes to the request's sender")
  eq(world.challenges[1].req, REQ_FRAME, "the challenge names the request")
  eq(world.builds[1].destination, ATA, "the destination is the record's token account")
  eq(world.builds[1].amount, "10.00", "the amount is the request's")
  eq(world.builds[1].symbol, "HACK", "the symbol is the request's currency")
  eq(world.begins[1].ctx.req, REQ_FRAME, "ctx.req is the request frame")
  eq(world.begins[1].ctx.record, RECORD, "ctx.record")
  eq(#world.sent, 1, "one RESULT frame")
  eq(world.sent[1].mac, "aa:bb:cc:dd:ee:ff", "the RESULT goes to the payee")
  local result = vk.result_parse(world.sent[1].data)
  check(result and result.req_id == REQ_ID and result.status == 0 and result.ref == SIG, "the RESULT says paid, with the signature")
  local fed = nil
  for i = 1, #world.http_calls do
    if world.http_calls[i].url:find("/feed/solana", 1, true) then fed = json.decode(world.http_calls[i].body) end
  end
  check(fed and fed.tx_sig == SIG_B58 and fed.req == b64enc(REQ_FRAME), "the feed gets the signature and the request")

  -- The user refuses: failed with the reason, and the payee is told.
  for _, reason in ipairs({"cancelled", "timeout", "unverified", "mismatch", "bad_proof", "sign_failed"}) do
    reset_world()
    world.http = network()
    world.presence = {"present"}
    world.polls = {"pending", {nil, reason}}
    flow = vk.pay.start({request = request_entry()})
    state, detail, seen = run_flow(flow)
    check(state == "failed" and detail == reason, "a refusal ends in failed, " .. reason)
    eq(flow.failed_in, "approve", "it failed in approve")
    eq(count_calls("sendTransaction"), 0, "nothing is sent after a refusal")
    eq(#world.sent, 1, "one RESULT frame after a refusal")
    eq(vk.result_parse(world.sent[1].data).status, 1, "the RESULT says rejected")
    eq(vk.result_parse(world.sent[1].data).ref, string.rep("\0", 64), "a rejected RESULT has no signature")
  end

  -- No record (404): the flow goes on so that the firmware can show the red screen.
  reset_world()
  world.http = network({no_record = true})
  world.presence = {"present"}
  world.polls = {{nil, "unverified"}}
  flow = vk.pay.start({request = request_entry()})
  state, detail = run_flow(flow)
  check(state == "failed" and detail == "unverified", "no record: the approval reports unverified")
  eq(#world.begins, 1, "no record: the approval was still opened")
  eq(world.builds[1].destination, PAYEE, "no record: the transfer names the payee's own address")
  eq(world.begins[1].ctx.record, nil, "no record: no ctx.record")
  eq(world.begins[1].ctx.req, REQ_FRAME, "no record: ctx.req is still given")

  -- The demo override: another destination than the record's.
  reset_world()
  world.http = network()
  world.polls = {{nil, "mismatch"}}
  state, detail = run_flow(vk.pay.start({to = PAYEE, amount = "5.00", destination = "EvilAccount"}))
  check(state == "failed" and detail == "mismatch", "a wrong destination is blocked by the firmware")
  eq(world.builds[1].destination, "EvilAccount", "the override is what is built")
  eq(world.begins[1].ctx.record, RECORD, "the real record is still passed")

  -- build_transfer and begin_solana refusing.
  reset_world()
  world.http = network()
  world.build = function() return nil, "unsupported" end
  state, detail = run_flow(vk.pay.start({to = PAYEE, amount = "5.00"}))
  check(state == "failed" and detail == "unsupported", "build_transfer refusing")
  eq(#world.begins, 0, "nothing is begun when the transfer cannot be built")

  reset_world()
  world.http = network()
  world.begin = function() return nil, "too_long" end
  state, detail = run_flow(vk.pay.start({to = PAYEE, amount = "5.00"}))
  check(state == "failed" and detail == "too_long", "begin_solana refusing")
  eq(#world.begins, 1, "a refusal other than busy is not retried")

  reset_world()
  world.http = network()
  world.begin = function() return nil, "busy" end
  state, detail = run_flow(vk.pay.start({to = PAYEE, amount = "5.00"}))
  check(state == "failed" and detail == "busy", "begin_solana busy for too long")
  eq(#world.begins, vk.pay.begin_tries, "busy is retried a limited number of times")
  eq(#world.builds, 1, "the transfer is built once")

  reset_world()
  world.http = network()
  local busy = 3
  world.begin = function()
    busy = busy - 1
    if busy > 0 then return nil, "busy" end
    return true
  end
  world.polls = {SIG}
  state = run_flow(vk.pay.start({to = PAYEE, amount = "5.00"}))
  eq(state, "done", "a wallet that is busy for a moment")

  -- wallet.challenge refusing (the badge has no key, or the frame is not a request).
  reset_world()
  world.http = network()
  world.challenge = function() return nil, "sign_failed" end
  state, detail = run_flow(vk.pay.start({request = request_entry()}))
  check(state == "failed" and detail == "sign_failed", "challenge refusing")

  -- The node refuses the transaction: tried a limited number of times, then failed.
  reset_world()
  world.http = network({send_fails = true})
  world.presence = {"present"}
  world.polls = {SIG}
  flow = vk.pay.start({request = request_entry()})
  state, detail = run_flow(flow)
  check(state == "failed" and detail == "insufficient funds", "the node refusing the transaction")
  eq(flow.failed_in, "submit", "it failed in submit")
  eq(count_calls("sendTransaction"), vk.pay.send_tries, "sending is retried a limited number of times")
  eq(vk.result_parse(world.sent[1].data).status, 2, "the RESULT says failed")
  eq(flow.signature, SIG_B58, "the signature is still readable")

  -- The transaction fails on chain.
  reset_world()
  world.http = network({tx_fails = true})
  world.presence = {"present"}
  world.polls = {SIG}
  flow = vk.pay.start({request = request_entry()})
  state, detail = run_flow(flow)
  check(state == "failed" and detail == "transaction failed", "a transaction that fails on chain")
  eq(vk.result_parse(world.sent[1].data).status, 2, "the RESULT says failed")
  eq(count_calls("/feed/solana"), 0, "the feed is not told about a failed transaction")

  -- Never confirmed: the flow gives up after confirm_for_ms.
  reset_world()
  world.http = network({never_confirms = true})
  world.presence = {"present"}
  world.polls = {SIG}
  local started = world.now
  flow = vk.pay.start({request = request_entry()})
  state, detail = run_flow(flow)
  check(state == "failed" and detail == "not confirmed", "a transaction that is never confirmed")
  check(world.now - started < vk.pay.confirm_for_ms + 15000, "it gives up in bounded time")
  check(count_calls("getSignatureStatuses") <= vk.pay.confirm_for_ms // vk.pay.confirm_every_ms + 1,
        "the status is asked for every confirm_every_ms, not every frame")
  local late = vk.result_parse(world.sent[1].data)
  check(late.status == 0 and late.ref == SIG, "the payee gets the signature to check on chain itself")

  -- The network goes away after the approval: every later step still ends.
  reset_world()
  world.http = network()
  world.polls = {SIG}
  flow = vk.pay.start({to = PAYEE, amount = "5.00"})
  while flow:update() ~= "submit" do world.now = world.now + 100 end
  world.http = nil
  state, detail = run_flow(flow)
  check(state == "failed" and detail == "no_wifi", "the network going away while sending")

  reset_world()
  world.http = network()
  world.polls = {SIG}
  flow = vk.pay.start({to = PAYEE, amount = "5.00"})
  while flow:update() ~= "confirm" do world.now = world.now + 100 end
  world.http = nil
  state, detail = run_flow(flow)
  check(state == "failed" and detail == "not confirmed", "the network going away while confirming")

  -- One blocking call per update at most: no update makes two HTTP requests, and the update that
  -- opens the approval makes none.
  reset_world()
  world.http = network()
  world.presence = {"present"}
  world.polls = {"pending", SIG}
  flow = vk.pay.start({request = request_entry()})
  for _ = 1, 2000 do
    local requests, begins = #world.http_calls, #world.begins
    local now_state = flow:update()
    local made = #world.http_calls - requests
    check(made <= 1, "at most one HTTP request per update")
    check(made == 0 or #world.begins == begins, "an update that opens the approval makes no HTTP request")
    if now_state == "done" or now_state == "failed" then break end
    world.now = world.now + 100
  end
  eq(flow.state, "done", "the flow ended")

  -- The request memo: a request payment passes req_id (the firmware writes the memo) and no free
  -- memo; a shop payment passes its memo and no req_id.
  reset_world()
  world.http = network()
  world.presence = {"present"}
  world.polls = {SIG}
  state = run_flow(vk.pay.start({request = request_entry(), memo = "ignored"}))
  eq(state, "done", "the request payment with its memo is done")
  eq(world.builds[1].req_id, REQ_ID, "a request payment passes req_id to build_transfer")
  eq(world.builds[1].memo, nil, "and no free memo with it (bad_arg in the firmware)")
  reset_world()
  world.http = network()
  world.polls = {SIG}
  run_flow(vk.pay.start({to = PAYEE, amount = "5.00", memo = "sword"}))
  eq(world.builds[1].req_id, nil, "a shop payment passes no req_id")
  eq(world.builds[1].memo, "sword", "a shop payment keeps its memo")

  -- The registry route missing: the flow goes on to the red screen as for no record, but the
  -- reason to show is registry_missing, not an impostor.
  reset_world()
  world.http = network({no_route = true})
  world.presence = {"present"}
  world.polls = {{nil, "unverified"}}
  flow = vk.pay.start({request = request_entry()})
  state, detail = run_flow(flow)
  check(state == "failed" and detail == "unverified", "route missing: the firmware still says unverified")
  eq(world.begins[1].ctx.record, nil, "route missing: no record is passed, so it can never be green")
  eq(flow:reason(), "registry_missing", "route missing: the reason shown is registry_missing")
  reset_world()
  world.http = network({no_record = true})
  world.polls = {{nil, "unverified"}}
  flow = vk.pay.start({to = PAYEE, amount = "5.00"})
  run_flow(flow)
  eq(flow:reason(), "unverified", "no record for the key: the reason shown stays unverified")
  reset_world()
  world.http = function(method, url, body)
    if url:find("/registry/", 1, true) then return nil, "read Timeout" end
    return network()(method, url, body)
  end
  flow = vk.pay.start({to = PAYEE, amount = "5.00"})
  state, detail = run_flow(flow)
  check(state == "failed" and detail == "listener_unreachable", "a registry timeout fails as the network")
  eq(flow:reason(), "listener_unreachable", "and is shown as the network")
  eq(#world.begins, 0, "no approval is opened on a registry timeout")
  reset_world()
  world.http = network()
  world.polls = {SIG}
  flow = vk.pay.start({to = PAYEE, amount = "5.00"})
  run_flow(flow)
  eq(flow:reason(), nil, "a paid flow has no reason")
end

-- ---------------------------------------------------------------------------------------------
-- vk.receive: the payee checks a RESULT before PAID
-- ---------------------------------------------------------------------------------------------

local function sig_of(n) return string.rep(string.char(n), 64) end
local REAL, JUNK, JUNK2, JUNK3 = sig_of(0x21), sig_of(0x22), sig_of(0x23), sig_of(0x24)

-- A RESULT frame for the vector request.
local function result_of(ref, status) return vk.result_frame(REQ_ID, status or 0, ref) end

-- The RPC node for getTransaction: world.txs[sig58] is {raw =, err =}, or a list of such answers
-- (nil entries: not visible yet) played in turn, the last one repeating.
local function chain()
  return function(method, url, body)
    local call = json.decode(body)
    if call.method ~= "getTransaction" then return 500, "unexpected" end
    eq(call.params[2].encoding, "base64", "getTransaction asks for base64")
    eq(call.params[2].commitment, "confirmed", "getTransaction at vk.commitment")
    eq(call.params[2].maxSupportedTransactionVersion, 0, "getTransaction accepts legacy and v0")
    local answer = world.txs[call.params[1]]
    if type(answer) == "table" and answer.raw == nil and answer.err == nil and #answer > 0 then
      local first = answer[1]
      if #answer > 1 then table.remove(answer, 1) end
      answer = first
    end
    if answer == nil or answer == false then return 200, '{"jsonrpc":"2.0","result":null,"id":1}' end
    local meta = answer.err and '{"err":{"InstructionError":[0,{"Custom":1}]}}' or '{"err":null}'
    return 200, '{"jsonrpc":"2.0","result":{"slot":9,"meta":' .. meta .. ',"transaction":["'
      .. b64enc(answer.raw) .. '","base64"]},"id":1}'
  end
end

local function tx(ref, body) return {raw = "\1" .. ref .. (body or "GOOD")} end

-- Calls watch:update() every 100 ms until it is paid or failed. Returns state, detail, the states
-- passed through.
local function run_watch(watch, limit)
  local seen, last = {}, nil
  for _ = 1, limit or 2000 do
    local requests = #world.http_calls
    local state, detail = watch:update()
    check(#world.http_calls - requests <= 1, "at most one HTTP request per update")
    if state ~= last then
      seen[#seen + 1] = state
      last = state
    end
    if state == "paid" or state == "failed" then return state, detail, table.concat(seen, " ") end
    world.now = world.now + 100
  end
  error("the watch did not end: " .. table.concat(seen, " "), 2)
end

local function watch_for()
  return vk.receive.start({req_id = REQ_ID, amount = "10.00"})
end

do
  -- Bad options: already failed, never raises.
  reset_world()
  for i, opts in ipairs({false, {}, {req_id = REQ_ID}, {amount = "10.00"}, {req_id = "13ce", amount = "10.00"},
                         {req_id = REQ_ID, amount = 10}, {req_id = "zz" .. REQ_ID:sub(3), amount = "10.00"},
                         {req_id = REQ_ID, amount = "10.00", to = 7}}) do
    local w = vk.receive.start(opts or nil)
    local state, detail = w:update()
    check(state == "failed" and detail == "bad_arg", "bad receive options " .. i)
    eq(w:result(result_of(REAL)), false, "a failed watch takes no RESULT")
  end

  -- Nothing heard: waiting, no request.
  reset_world()
  world.http = chain()
  local w = watch_for()
  eq(w:update(), "waiting", "no RESULT yet: waiting")
  eq(#world.http_calls, 0, "nothing is looked up before a RESULT")

  -- Status 1 and 2 are noted, never decisive; another request's RESULT is ignored.
  eq(w:result(result_of(nil, 1)), false, "a rejected RESULT is not queued")
  eq(w.reported, 1, "but noted")
  eq(w:result(result_of(nil, 2)), false, "a failed RESULT is not queued")
  eq(w.reported, 2, "but noted")
  eq(w:update(), "waiting", "still waiting after status 1 and 2")
  eq(w:result(vk.result_frame("00000000000000aa", 0, REAL)), false, "another request's RESULT")
  eq(w:result(vk.result_frame(REQ_ID, 0)), false, "status 0 with an all-zero ref")
  eq(w:result("junk"), false, "not a RESULT frame")
  eq(w:update(), "waiting", "still waiting")

  -- The honest payment, visible at once: paid with the payer and the signature, and recorded.
  reset_world()
  world.http = chain()
  world.txs[b58enc(REAL)] = tx(REAL)
  w = watch_for()
  eq(w:result(result_of(REAL)), true, "a status-0 RESULT is queued")
  eq(w:result(result_of(REAL)), false, "the same ref twice is queued once")
  local state, detail, seen = run_watch(w)
  eq(state, "paid", "the honest payment is paid")
  eq(seen, "pending paid", "pending, then paid")
  check(type(detail) == "table" and detail.payer == "PayerKey" and detail.sig == b58enc(REAL), "paid: payer and sig")
  check(w.payer == "PayerKey" and w.sig == b58enc(REAL), "w.payer and w.sig")
  eq(#world.verifies, 1, "verified once")
  local expected = world.verifies[1].expected
  check(expected.amount == "10.00" and expected.req_id == REQ_ID and expected.sig == REAL, "the expected payment")
  eq(world.verifies[1].raw, "\1" .. REAL .. "GOOD", "the raw wire bytes are checked, not base64")
  eq(#world.received, 1, "the received payment is recorded once")
  eq(w.recorded, true, "w.recorded")
  eq(w:result(result_of(JUNK)), false, "nothing is queued after paid")
  eq(w:update(), "paid", "paid stays paid")
  eq(count_calls("getTransaction"), 1, "one look-up")

  -- PAID is never shown for a ref that does not verify: every refusal of verify_payment.
  for _, case in ipairs({{"mismatch", "amount"}, {"mismatch", "memo"}, {"mismatch", "recipient"},
                         {"bad_proof", "signature"}, {"undecodable", "shape"}}) do
    reset_world()
    world.http = chain()
    world.txs[b58enc(JUNK)] = tx(JUNK, "BAD:" .. case[1] .. ":" .. case[2])
    w = watch_for()
    w:result(result_of(JUNK))
    state, detail, seen = run_watch(w)
    check(state == "failed" and detail == case[1], "a " .. case[1] .. "/" .. case[2] .. " transaction is not paid")
    check(not seen:find("paid", 1, true), "never paid on the way")
    eq(w.last_detail, case[2], "the detail is kept")
    eq(#world.received, 0, "nothing is recorded")
    eq(w:result(result_of(JUNK)), false, "a refused ref is not tried again")
  end

  -- A transaction that failed on chain moved nothing.
  reset_world()
  world.http = chain()
  world.txs[b58enc(JUNK)] = {raw = "\1" .. JUNK .. "GOOD", err = true}
  w = watch_for()
  w:result(result_of(JUNK))
  state, detail = run_watch(w)
  check(state == "failed" and detail == "transaction failed", "meta.err: not paid")
  eq(#world.verifies, 0, "a failed transaction is not even verified")

  -- Retry while the transaction is not visible yet: null twice, then the transaction.
  reset_world()
  world.http = chain()
  world.txs[b58enc(REAL)] = {false, false, tx(REAL)}
  w = watch_for()
  w:result(result_of(REAL))
  local started = world.now
  state = run_watch(w)
  eq(state, "paid", "paid once the transaction is visible")
  eq(count_calls("getTransaction"), 3, "looked up three times")
  check(world.now - started >= 2 * vk.receive.every_ms, "every every_ms, not every update")

  -- Never visible: given up after for_ms with "not confirmed"; bounded requests.
  reset_world()
  world.http = chain()
  w = watch_for()
  w:result(result_of(JUNK))
  started = world.now
  state, detail = run_watch(w)
  check(state == "failed" and detail == "not confirmed", "never visible: not confirmed")
  check(world.now - started <= vk.receive.for_ms + vk.receive.every_ms, "given up in bounded time")
  check(count_calls("getTransaction") <= vk.receive.for_ms // vk.receive.every_ms + 1, "a bounded number of look-ups")
  -- again() re-queues what failed for a reason that may pass (not found, no network).
  eq(w:retryable(), 1, "one ref can be tried again")
  world.txs[b58enc(JUNK)] = tx(JUNK)
  eq(w:again(), 1, "again() re-queues it")
  state = run_watch(w)
  eq(state, "paid", "found on the second try")

  -- A forged RESULT with a junk ref first, then the real one: the real one is paid.
  reset_world()
  world.http = chain()
  world.txs[b58enc(JUNK)] = tx(JUNK, "BAD:mismatch:memo")
  world.txs[b58enc(REAL)] = tx(REAL)
  w = watch_for()
  w:result(result_of(JUNK))
  eq(w:update(), "pending", "checking the forged ref")
  w:result(result_of(REAL))
  state, detail = run_watch(w)
  check(state == "paid" and detail.sig == b58enc(REAL), "the real payment after a forged RESULT is paid")

  -- A forged ref that is never visible does not delay the real one by its whole window.
  reset_world()
  world.http = chain()
  world.txs[b58enc(REAL)] = tx(REAL)
  w = watch_for()
  w:result(result_of(JUNK))
  w:result(result_of(REAL))
  started = world.now
  state = run_watch(w)
  eq(state, "paid", "a junk ref that is never found does not hide the real one")
  check(world.now - started < vk.receive.every_ms * 2, "the refs are checked in turn")

  -- Three junk refs, then the real one: the real one is still checked (the most-tried junk ref
  -- makes room), and the newest ref is never refused for want of a slot.
  reset_world()
  world.http = chain()
  world.txs[b58enc(REAL)] = tx(REAL)
  w = watch_for()
  for _, junk in ipairs({JUNK, JUNK2, JUNK3}) do w:result(result_of(junk)) end
  for _ = 1, 3 do w:update() world.now = world.now + 100 end   -- each junk ref tried once
  eq(w:pending(), vk.receive.slots, "three refs held")
  eq(w:result(result_of(REAL)), true, "the fourth ref is still taken")
  eq(w:pending(), vk.receive.slots, "and the slots stay three")
  state = run_watch(w)
  eq(state, "paid", "the real ref after three junk refs is paid")

  -- The badge's own token account is not known yet: refresh, then the check passes.
  reset_world()
  world.http = chain()
  world.txs[b58enc(REAL)] = tx(REAL)
  local asked = 0
  world.verify = function(raw, expected)
    asked = asked + 1
    if asked == 1 then return nil, "unsupported", "to" end
    return true, {payer = "PayerKey", sig = b58enc(REAL)}
  end
  world.refresh = function() return true end
  w = watch_for()
  w:result(result_of(REAL))
  state = run_watch(w)
  eq(state, "paid", "paid after learning the token account")
  eq(world.refresh_calls, 1, "refresh_balance once")

  -- The history cannot be written (or the app has no history permission): still paid.
  reset_world()
  world.http = chain()
  world.txs[b58enc(REAL)] = tx(REAL)
  world.record_received = function() error("permission 'history' not granted") end
  w = watch_for()
  w:result(result_of(REAL))
  eq(run_watch(w), "paid", "a record that raises does not undo a verified payment")
  eq(w.recorded, false, "w.recorded is false")
  check(type(w.record_error) == "string", "w.record_error says why")
  reset_world()
  world.http = chain()
  world.txs[b58enc(REAL)] = tx(REAL)
  w = vk.receive.start({req_id = REQ_ID, amount = "10.00", record = false})
  w:result(result_of(REAL))
  eq(run_watch(w), "paid", "record = false: paid")
  eq(#world.received, 0, "record = false: nothing recorded")

  -- No network: tried until the window ends, with the no-network reason; then again() works.
  reset_world()
  world.wifi_up = false
  w = watch_for()
  w:result(result_of(REAL))
  state, detail = run_watch(w)
  check(state == "failed" and detail == "no_wifi", "no network: failed, no_wifi")
  check(vk.offline(detail), "an offline reason")
  world.http = chain()
  world.txs[b58enc(REAL)] = tx(REAL)
  w:again()
  eq(run_watch(w), "paid", "paid once the network is back")

  -- A transaction whose fetched signature is not the RESULT's is refused (the firmware compares).
  reset_world()
  world.http = chain()
  world.txs[b58enc(REAL)] = tx(JUNK)
  w = watch_for()
  w:result(result_of(REAL))
  state, detail = run_watch(w)
  check(state == "failed" and detail == "mismatch", "another transaction under that signature is refused")

  -- The firmware with no verify_payment: no watch can be paid.
  reset_world()
  local saved = badge.wallet.verify_payment
  badge.wallet.verify_payment = nil
  state, detail = watch_for():update()
  check(state == "failed" and detail == "unsupported", "no verify_payment: unsupported")
  badge.wallet.verify_payment = saved
end

-- ---------------------------------------------------------------------------------------------
-- Reason texts
-- ---------------------------------------------------------------------------------------------

do
  -- Every Lua reason code of docs/os/reference/reasons.md has a text in the shared table.
  local file = io.open("../docs/os/reference/reasons.md", "r")
  check(file, "docs/os/reference/reasons.md can be read from the firmware folder")
  local codes = 0
  for line in file:lines() do
    local code = line:match("^| %d+ | `VK_[%u_]+` | `([%l_]+)` |")
    if code then
      codes = codes + 1
      check(type(vk.reasons[code]) == "string" and vk.reasons[code] ~= "", "a text for " .. code)
      local text = vk.reason_text(code)
      check(text ~= code and #text <= vk.REASON_MAX, "reason_text(" .. code .. ") is words that fit")
    end
  end
  file:close()
  check(codes >= 21, "the reasons table was found (" .. codes .. " codes)")

  -- The library's own reasons, the no-network family with its next step.
  for _, code in ipairs({"no_wifi", "no_route", "rpc_unreachable", "listener_unreachable", "registry_missing",
                         "bad_url", "transaction failed", "not confirmed"}) do
    check(type(vk.reasons[code]) == "string", "a text for " .. code)
  end
  check(vk.reason_text("no_wifi"):find("Settings > Wi-Fi", 1, true), "no_wifi says where to join Wi-Fi")
  check(vk.reason_text("over_daily"):find("limit", 1, true), "over_daily")
  check(vk.reason_text("low_battery"):find("USB", 1, true), "low_battery says to plug in")
  check(vk.reason_text("registry_missing") ~= vk.reason_text("unverified"), "a missing route does not read as an impostor")
  check(not vk.reason_text("registry_missing"):lower():find("impostor", 1, true), "registry_missing does not say impostor")

  -- Messages with a value in them, and anything unknown.
  check(vk.reason_text("rpc_url not set"):find("rpc_url", 1, true), "<key> not set names the key")
  check(vk.reason_text("rpc http 503"):find("503", 1, true), "rpc http N keeps N")
  check(vk.reason_text("registry http 500"):find("500", 1, true), "registry http N")
  check(vk.reason_text("listener http 502"):find("502", 1, true), "listener http N")
  check(vk.reason_text("insufficient funds"):find("insufficient funds", 1, true), "an unknown message is shown as it is")
  check(type(vk.reason_text(nil)) == "string", "nil gives a text")

  -- Contexts and overrides.
  check(vk.reason_text("mismatch", "receive") ~= vk.reason_text("mismatch"), "the payee's mismatch has its own words")
  check(vk.reason_text("mismatch", "contact"):find("another badge", 1, true), "a contact card for another badge")
  eq(vk.reason_text("mismatch", {mismatch = "mine"}), "mine", "an app's override table")
  eq(vk.reason_text("busy", {mismatch = "mine"}), vk.reason_text("busy"), "an override table falls back")
  check(#vk.reason_text("over_cap", "short") <= 24, "the short words fit a row")
  for context, words in pairs(vk.reason_contexts) do
    for code, text in pairs(words) do
      check(#text <= vk.REASON_MAX, context .. "." .. code .. " fits")
    end
  end
end

-- ---------------------------------------------------------------------------------------------
-- vk.peers: badges heard nearby (Contacts' swap list)
-- ---------------------------------------------------------------------------------------------

do
  reset_world()
  local list = vk.peers.new({max = 2, timeout_ms = 4000, hold_ms = 10000})
  local function hello(address, nonce, rssi) return {address = address, name = "n" .. address, frame = address .. nonce, rssi = rssi} end
  local p, how = list:heard("m1", hello("A", "1", -40), 0)
  check(p and how == "new", "a new badge")
  list:heard("m2", hello("B", "1", -50), 10)
  eq(#list.items, 2, "two badges")
  -- A third badge: the one heard longest ago makes room; new badges are never refused.
  list:heard("m1", hello("A", "1", -40), 20)
  p, how = list:heard("m3", hello("C", "1", -60), 30)
  check(p and how == "new", "a third badge is taken")
  eq(#list.items, 2, "the list stays at max")
  check(list:find("m2") == nil and list:find("m1") and list:find("m3"), "the least recently heard left")
  -- Made-up HELLOs from many MACs do not lock an honest badge out: it comes back on its next HELLO.
  for i = 1, 10 do list:heard("fake" .. i, hello("F" .. i, "1", -30), 40 + i) end
  p, how = list:heard("m1", hello("A", "1", -40), 60)
  check(p and how == "new" and list:find("m1"), "the honest badge is listed again")
  -- The selected badge is kept while others come and go.
  list = vk.peers.new({max = 2, timeout_ms = 4000, hold_ms = 10000})
  list:heard("m1", hello("A", "1", -40), 0)
  list:heard("m2", hello("B", "1", -50), 10)
  list:heard("m3", hello("C", "1", -60), 20, "m1")
  check(list:find("m1") and list:find("m3") and not list:find("m2"), "the kept badge is not evicted")
  -- A known MAC with another key is ignored: it cannot replace the badge's name or frame.
  p, how = list:heard("m1", hello("EVIL", "9", -20), 30)
  eq(how, "ignored", "another key from a known MAC")
  eq(list:find("m1").address, "A", "the address is kept")
  eq(list:find("m1").frame, "A1", "the frame is kept")
  -- The same key with a new nonce updates the frame; but not while a card sent to it is pending.
  p, how = list:heard("m1", hello("A", "2", -40), 40)
  check(how == "updated" and p.frame == "A2", "a new nonce updates the frame")
  list:sent("m1", 50)
  check(list:find("m1").sent, "marked sent")
  p, how = list:heard("m1", hello("A", "3", -40), 60)
  check(how == "held" and p.frame == "A2" and p.sent, "a HELLO during a swap in progress does not replace the frame")
  p, how = list:heard("m1", hello("A", "3", -40), 50 + 10000)
  check(how == "updated" and p.frame == "A3" and not p.sent, "after hold_ms the new frame is taken")
  -- A pinned (sent) badge is not evicted; when everything is pinned, a newcomer waits.
  list = vk.peers.new({max = 1, timeout_ms = 4000, hold_ms = 10000})
  list:heard("m1", hello("A", "1", -40), 0)
  list:sent("m1", 0)
  p, how = list:heard("m2", hello("B", "1", -40), 10)
  check(p == nil and how == "full", "a full list of pinned badges")
  -- Expiry.
  list = vk.peers.new({max = 4, timeout_ms = 4000, hold_ms = 10000})
  list:heard("m1", hello("A", "1", -40), 0)
  list:heard("m2", hello("B", "1", -70), 3000)
  list:expire(4500)
  check(not list:find("m1") and list:find("m2"), "a badge not heard for timeout_ms leaves")
  -- Strongest first.
  list:heard("m3", hello("C", "1", -30), 4600)
  eq(list.items[1].mac, "m3", "strongest signal first")
end

-- ---------------------------------------------------------------------------------------------
-- Small helpers: keep_awake, led_pulse
-- ---------------------------------------------------------------------------------------------

do
  reset_world()
  vk.keep_awake(true)
  vk.keep_awake(false)
  check(world.awake[1] == true and world.awake[2] == false, "vk.keep_awake passes on/off")
  local saved = badge.screen
  badge.screen = nil
  vk.keep_awake(true)                         -- an older firmware: nothing, no error
  badge.screen = saved
  vk.led_pulse("green", 600)
  local led = world.leds[1]
  check(led and led[4] == 600, "vk.led_pulse")
  check(led[1] >= 0 and led[1] <= 255 and led[2] >= 0 and led[2] <= 255 and led[3] >= 0 and led[3] <= 255, "0..255")
  check(led[1] == 24 and led[2] == 190 and led[3] == 115, "green 0x1FBF75 through RGB565 (3, 47, 14)")
end

-- ---------------------------------------------------------------------------------------------
-- vk.ui
-- ---------------------------------------------------------------------------------------------

local function rgb565(rgb)
  return ((rgb >> 16 & 0xF8) << 8) | ((rgb >> 8 & 0xFC) << 3) | (rgb & 0xFF) >> 3
end

-- The recorded draw calls of one kind, as "x,y,..." strings.
local function drawn(kind)
  local out = {}
  for i = 1, #world.draws do
    local call = world.draws[i]
    if call[1] == kind then
      local parts = {}
      for j = 2, #call do parts[#parts + 1] = tostring(call[j]) end
      out[#out + 1] = table.concat(parts, ",")
    end
  end
  return out
end

local function has(list, text)
  for i = 1, #list do
    if list[i] == text then return true end
  end
  return false
end

do
  local ui = vk.ui
  reset_world()

  -- Colours: receipt-light when badge.theme is absent; the theme's when it is there.
  eq(ui.color("paper"), rgb565(0xF3EFE4), "the fallback paper")
  eq(ui.color("ink"), rgb565(0x1B1A17), "the fallback ink")
  eq(ui.color("faint"), rgb565(0x8A8474), "the fallback faint")
  eq(ui.color("sub"), rgb565(0x6D6759), "the fallback sub")
  eq(ui.color("stamp_ok"), rgb565(0x17804F), "the fallback stamp_ok")
  eq(ui.color("stamp_warn"), rgb565(0xB56A00), "the fallback stamp_warn")
  eq(ui.color("stamp_bad"), rgb565(0xC8321E), "the fallback stamp_bad")
  eq(ui.color("led"), rgb565(0xFFE2AA), "the fallback led")
  eq(ui.color("green"), rgb565(0x1FBF75), "green")
  eq(ui.color("amber"), rgb565(0xFFB020), "amber")
  eq(ui.color("red"), rgb565(0xFF4545), "red")
  eq(ui.color("nonsense"), ui.color("ink"), "an unknown token is ink")
  badge.theme = {
    name = function() return "receipt-dark" end,
    color = function(token) return ({paper = 1, ink = 2, faint = 3, sub = 4})[token] end,
  }
  eq(ui.color("paper"), 1, "the theme's paper")
  eq(ui.color("ink"), 2, "the theme's ink")
  eq(ui.color("green"), rgb565(0x1FBF75), "a token the theme does not answer falls back")
  local PAPER, INK, FAINT, SUB = 1, 2, 3, 4

  -- page.
  ui.page()
  eq(drawn("clear")[1], "1", "page fills with paper")

  -- header: left at x 10, right ending at x 310, both at y 7; the rule at y 19 from 10 to 310.
  reset_world()
  ui.header("BADGEOS", "USB")
  local texts = drawn("text")
  check(has(texts, "BADGEOS,10,7,2,1"), "the header's left text")
  check(has(texts, "USB,292,7,2,1"), "the header's right text ends at x 310")
  local rects = drawn("fill_rect")
  check(has(rects, "10,19,3,1,2"), "the header rule starts at x 10")
  check(has(rects, "305,19,3,1,2"), "the header rule's last full dash")
  check(has(rects, "310,19,1,1,2"), "the header rule ends at x 310")
  eq(#rects, 61, "a rule from 10 to 310 is 61 dashes")

  -- The default right side: the battery (86.6 rounds to 87), no time without a clock.
  eq(ui.status(), "87%", "the status with no clock")
  reset_world()
  ui.header("PAY")
  check(has(drawn("text"), "87%,292,7,2,1"), "the header shows the status by default")
  badge.battery.charging = function() return true end
  eq(ui.status(), "USB", "the status on external power")
  badge.battery.charging = function() return false end

  -- The time is the firmware clock's (wallet.time()): nil while it has no trusted source, an
  -- integer otherwise, a float past 2038.
  badge.wallet.time = function() return nil end
  eq(ui.status(), "87%", "the status when wallet.time() is nil")
  badge.wallet.time = function() return 1790000000 + 14 * 3600 + 32 * 60 - 1790000000 % 86400 end
  eq(ui.status(), "14:32 \xC2\xB7 87%", "the status with the firmware clock")
  badge.wallet.time = function() return 86400.0 * 30000 + 3 * 3600 + 5 * 60 + 59 end
  eq(ui.status(), "03:05 \xC2\xB7 87%", "the status with a float time")
  badge.wallet.time = nil

  -- A middle dot is drawn by hand, between two runs of text.
  reset_world()
  ui.header("PAY", "14:32 \xC2\xB7 87%")
  texts = drawn("text")
  check(has(texts, "14:32 ,244,7,2,1"), "the text before the middle dot")      -- 11 columns: 310 - 66
  check(has(texts, " 87%,286,7,2,1"), "the text after the middle dot")
  check(has(drawn("fill_rect"), "281,9,2,2,2"), "the middle dot is a 2 x 2 square")

  -- A long left text is cut and ends in "..".
  reset_world()
  ui.header(string.rep("A", 60), "USB")
  local cut = nil
  for _, call in ipairs(world.draws) do
    if call[1] == "text" and call[3] == 10 then cut = call[2] end
  end
  eq(#cut, 45, "the left text is cut to the room left of the right text")   -- (300 - 18 - 12) / 6
  eq(cut:sub(-2), "..", "a cut text ends in two dots")

  -- footer: clears its strip, rule edge to edge at y 216, text at y 224.
  reset_world()
  ui.footer("SELECT open", "CANCEL back")
  rects = drawn("fill_rect")
  eq(rects[1], "0,216,320,24,1", "the footer clears its strip first")
  check(has(rects, "0,216,3,1,2") and has(rects, "315,216,3,1,2"), "the footer rule runs edge to edge")
  texts = drawn("text")
  check(has(texts, "SELECT open,10,224,2,1"), "the footer's left text")
  check(has(texts, "CANCEL back,244,224,2,1"), "the footer's right text")

  -- row: label at x0, value ending at x1, a dotted leader on a 3 px grid at y + 4.
  reset_world()
  ui.row(48, "STATUS", "provisioned")
  texts = drawn("text")
  check(has(texts, "STATUS,10,48,2,1"), "the row's label")
  check(has(texts, "provisioned,244,48,2,1"), "the row's value")
  local dots = drawn("pixel")
  eq(dots[1], "51,52,3", "the leader starts 4 px after the label, on the grid")   -- 10 + 35 + 4 = 49 -> 51
  eq(dots[#dots], "237,52,3", "the leader stops 4 px before the value")           -- to = 310 - 66 - 4 = 240
  eq(#drawn("fill_rect"), 0, "a row that is not selected has no fill")

  -- A selected row is inverted, 18 px high from y - 5, from x0 - 10 to x1 + 10.
  reset_world()
  ui.row(66, "KEY", "software", true)
  eq(drawn("fill_rect")[1], "0,61,320,18,2", "the selected row's fill")
  check(has(drawn("text"), "KEY,10,66,1,1"), "a selected row's text is paper")
  eq(drawn("pixel")[1]:match(",(%d+)$"), "1", "a selected row's leader is paper")

  -- A value colour, and a row in the body column of a two-column screen.
  reset_world()
  ui.row(58, "FROM", "Judge B", false, 99, ui.BODY_X0, ui.BODY_X1)
  texts = drawn("text")
  check(has(texts, "FROM,156,58,2,1"), "a body row's label starts at 156")
  check(has(texts, "Judge B,268,58,99,1"), "a body row's value ends at 310, in the colour given")

  -- A value too long for its row is cut; the label is kept.
  reset_world()
  ui.row(48, "ADDRESS", string.rep("x", 60))
  local value = nil
  for _, call in ipairs(world.draws) do
    if call[1] == "text" and call[2]:sub(1, 1) == "x" then value = call[2] end
  end
  eq(#value, 41, "the value gets the columns left of the label and the leader")   -- 50 - 7 - 2
  eq(value:sub(-2), "..", "a cut value ends in two dots")

  -- subline: 12 px in, in the sub colour; selected: a 13 px fill and paper text.
  reset_world()
  ui.subline(61, "Gn2G..Ecxq \xC2\xB7 name is a claim")
  check(has(drawn("text"), "Gn2G..Ecxq ,22,61,4,1"), "a subline starts 12 px in, in the sub colour")
  reset_world()
  ui.subline(61, "find a badge nearby", true)
  eq(drawn("fill_rect")[1], "0,61,320,13,2", "a selected subline's fill")
  check(has(drawn("text"), "find a badge nearby,22,61,1,1"), "a selected subline's text is paper")

  -- title: the built-in font at twice its size, 13 px per character, centred.
  reset_world()
  ui.title("MENU", 27)
  texts = drawn("text")
  eq(#texts, 4, "a title is drawn one character at a time")
  eq(texts[1], "M,136,26,2,2", "the title's first letter")       -- ink width 4*12 + 3 - 2 = 49; 160 - 24
  eq(texts[4], "U,175,26,2,2", "the title's last letter")
  reset_world()
  ui.title("REQUEST", 30, ui.BODY_CX)
  eq(drawn("text")[1], "R,189,29,2,2", "a title centred on the body column")   -- width 7*12 + 6 - 2 = 88

  -- amount: label at y, the value as large as fits 136 px, the unit at y + 56.
  reset_world()
  ui.amount(73, 58, "AMOUNT", "10.00", "HACK")
  texts = drawn("text")
  check(has(texts, "10.00,15,76,2,4"), "five characters are drawn at size 4")   -- 120 - 4 = 116 wide; top 58 + 15 + 3
  check(has(texts, "A,51,58,2,1"), "the amount's label is letter-spaced and centred")   -- 6*6 + 5*2 - 1 = 45; 73 - 22
  check(has(texts, "H,60,114,2,1"), "the amount's unit is at y + 56")                  -- 4*6 + 3 - 1 = 26
  reset_world()
  ui.amount(73, 58, "BALANCE", "142.50", "HACK")
  check(has(drawn("text"), "142.50,21,79,2,3"), "six characters are drawn at size 3")   -- 108 - 3 = 105, 73 - 52; top 73 + 6
  reset_world()
  ui.amount(73, 58, "BALANCE", "1234567.89", "HACK")
  check(has(drawn("text"), "1234567.89,14,83,2,2"), "ten characters are drawn at size 2")   -- 120 - 2 = 118
  reset_world()
  ui.amount(73, 58, "BALANCE", "123456789012.50", "HACK")
  check(has(drawn("text"), "123456789012.50,29,86,2,1"), "a very long amount is drawn whole at size 1")   -- 89 wide

  -- perforation and barcode.
  reset_world()
  ui.perforation(146, 24, 210)
  rects = drawn("fill_rect")
  eq(rects[1], "146,24,1,3,2", "the perforation's first dash")
  eq(#rects, 38, "a perforation from 24 to 210")
  reset_world()
  ui.barcode(16, 150, 114, 22, "\x15\x92")
  rects = drawn("fill_rect")
  eq(rects[1], "16,150,2,22,2", "the first bar of the barcode")       -- nibble 1: bar 2, gap 1
  eq(rects[2], "19,150,2,22,2", "the second bar of the barcode")      -- nibble 5: bar 2, gap 2
  eq(rects[3], "23,150,2,22,2", "the third bar of the barcode")       -- nibble 9: bar 2, gap 3
  eq(rects[4], "28,150,3,22,2", "the fourth bar of the barcode")      -- nibble 2: bar 3, gap 1
  for i = 1, #rects do
    local x, _, w = rects[i]:match("^(%d+),(%d+),(%d+)")
    check(tonumber(x) + tonumber(w) <= 130, "no bar passes the barcode's right edge")
  end

  -- list: a whole screen.
  reset_world()
  ui.list({
    title = "Pay", hint = "SELECT pay", sel = 2,
    rows = {
      {l = "MHacks Merch", r = "10.00", sub = "Gn2G..Ecxq"},
      {l = "Judge B", r = "2.50", sub = "FdBS..17XD", tone = "bad"},
    },
  })
  eq(world.draws[1][1], "clear", "a list starts with the page")
  texts = drawn("text")
  check(has(texts, "BADGEOS,10,7,2,1"), "the list's header")
  check(has(texts, "P,142,25,2,2"), "the list's title, in capitals")       -- width 3*12 + 2 - 2 = 36
  check(has(texts, "MHACKS MERCH,10,46,2,1"), "the first row at y 46, in capitals")
  check(has(texts, "Gn2G..Ecxq,22,59,4,1"), "the first row's subline at y 59")
  check(has(texts, "JUDGE B,10,77,1,1"), "the second row at y 77 (pitch 31), selected")
  check(has(texts, "2.50,286,77," .. rgb565(0xC8321E) .. ",1"), "the tone colours the value")
  check(has(texts, "FdBS..17XD,22,90,1,1"), "the selected row's subline is inverted too")
  check(has(texts, "SELECT pay,10,224,2,1"), "the list's hint")
  check(has(texts, "CANCEL back,244,224,2,1"), "the list's CANCEL back")
  rects = drawn("fill_rect")
  check(has(rects, "0,72,320,18,2") and has(rects, "0,90,320,13,2"), "the selected row and its subline are filled")

  -- A list without sublines shows 9 rows at pitch 18 and scrolls to keep the selection in view.
  local many = {}
  for i = 1, 20 do many[i] = {l = "row " .. i, r = tostring(i)} end
  reset_world()
  ui.list({title = "Wallet", rows = many, sel = 1, hint = "read only"})
  texts = drawn("text")
  check(has(texts, "ROW 1,10,46,1,1") and has(texts, "ROW 9,10,190,2,1"), "rows 1 to 9 at pitch 18")
  check(not has(texts, "ROW 10,10,208,2,1"), "a tenth row is not drawn")
  reset_world()
  ui.list({title = "Wallet", rows = many, sel = 12})
  texts = drawn("text")
  check(has(texts, "ROW 8,10,46,2,1") and has(texts, "ROW 12,10,118,1,1") and has(texts, "ROW 16,10,190,2,1"),
        "the selection is kept in the middle of the window")
  reset_world()
  ui.list({title = "Wallet", rows = many, sel = 20})
  texts = drawn("text")
  check(has(texts, "ROW 12,10,46,2,1") and has(texts, "ROW 20,10,190,1,1"), "the window stops at the last row")

  -- An empty list.
  reset_world()
  ui.list({title = "Pay", rows = {}, empty = "No requests nearby", hint = ""})
  check(has(drawn("text"), "No requests nearby,107,112,4,1"), "the empty line is centred")   -- 18*6 - 1 = 107 wide

  -- Bytes outside ASCII are drawn as "?"; the triangles are drawn by hand.
  reset_world()
  ui.text("caf\xC3\xA9\t", 10, 100)
  eq(drawn("text")[1], "caf???,10,100,2,1", "bytes outside printable ASCII")
  reset_world()
  ui.text("\xE2\x97\x82", 100, 50)
  rects = drawn("fill_rect")
  eq(#rects, 4, "a triangle is four columns")
  eq(rects[1], "101,53,1,1,2", "the left triangle's point is on the left")
  eq(rects[4], "104,50,1,7,2", "the left triangle's base is 7 px tall")
  reset_world()
  ui.text("\xE2\x96\xB8", 100, 50)
  eq(drawn("fill_rect")[1], "104,53,1,1,2", "the right triangle's point is on the right")
  eq(#drawn("text"), 0, "a triangle draws no text")

  badge.theme = nil
end

-- With badge.receipt (the firmware's kit) every vk.ui element is one call into it, with the kit's
-- argument order, and nothing is drawn with badge.gfx.
do
  local ui = vk.ui
  reset_world()
  local calls = {}
  local function record(name, result)
    return function(...)
      local args = table.pack(...)
      for i = 1, args.n do args[i] = tostring(args[i]) end
      calls[#calls + 1] = name .. "(" .. table.concat(args, ",", 1, args.n) .. ")"
      return result
    end
  end
  badge.receipt = {}
  for _, name in ipairs({"page", "header", "footer", "title", "rule", "perforation", "row", "subline",
                         "amount", "barcode"}) do
    badge.receipt[name] = record(name)
  end
  badge.receipt.qr = record("qr", true)

  ui.page()
  ui.header("BADGEOS")
  ui.header("PAY", "USB")
  ui.title("MENU", 26)
  ui.title("REQUEST", 30, ui.BODY_CX)
  ui.rule(100)
  ui.rule(100, 156, 310)
  ui.perforation(146, 24, 212)
  ui.row(48, "KEY", "software")
  ui.row(66, "CLOCK", nil, true, 99, ui.BODY_X0, ui.BODY_X1)
  ui.subline(61, "find a badge nearby", true)
  ui.amount(73, 40, "BALANCE", 12, nil)
  ui.barcode(16, 122, 114, 40)
  ui.barcode(16, 122, 114, 40, "\x15\x92")
  ui.footer("SELECT open", nil)
  eq(ui.qr(22, 98, 102, "https://example.org/x"), true, "ui.qr answers what the kit answers")
  local expected = {
    "page()", "header(BADGEOS,nil)", "header(PAY,USB)", "title(MENU,26,160)", "title(REQUEST,30,233)",
    "rule(100,10,310)", "rule(100,156,310)", "perforation(146,24,212)",
    "row(10,310,48,KEY,software,nil,nil)", "row(156,310,66,CLOCK,,true,99)",
    "subline(10,310,61,find a badge nearby,true)", "amount(73,40,BALANCE,12,)",
    "barcode(16,122,114,40,nil)", "barcode(16,122,114,40,\x15\x92)", "footer(SELECT open,)",
    "qr(22,98,102,https://example.org/x)",
  }
  eq(#calls, #expected, "one kit call per element")
  for i = 1, #expected do eq(calls[i], expected[i], "kit call " .. i) end
  eq(#world.draws, 0, "with the kit nothing is drawn through badge.gfx")

  -- A list is drawn by the same functions: the title goes to the kit, in capitals.
  calls = {}
  ui.list({title = "Pay", rows = {{l = "Judge B", r = "5.00", sub = "nearby", tone = "ok"}}, sel = 1, hint = "SELECT pay"})
  check(has(calls, "title(PAY,26,160)"), "the list's title is the kit's")
  check(has(calls, "row(10,310,46,JUDGE B,5.00,true," .. ui.color("stamp_ok") .. ")"), "the list's row is the kit's")
  check(has(calls, "subline(10,310,59,nearby,true)"), "the list's subline is the kit's")

  -- A kit without qr (a firmware between the two), and no kit: ui.qr draws nothing and says so.
  badge.receipt.qr = nil
  eq(ui.qr(22, 98, 102, "https://example.org/x"), false, "no qr in the kit")
  badge.receipt = nil
  reset_world()
  eq(ui.qr(22, 98, 102, "https://example.org/x"), false, "no kit")
  eq(#world.draws, 0, "ui.qr without the kit draws nothing")
  ui.page()
  eq(#drawn("clear"), 1, "without the kit the page is drawn with badge.gfx again")
end

-- ui.frame: draw only when the screen can have changed.
do
  local ui = vk.ui
  local keys_down = false
  local saved_input = badge.input
  badge.input = {any = function() return keys_down end}
  local frames = 0
  local function draw() frames = frames + 1 end
  local function pass(ms, period)
    world.now = world.now + ms
    return ui.frame(draw, period)
  end

  world.now = 100000
  check(pass(0) == true and frames == 1, "the first frame is always drawn")
  check(pass(1) == false and pass(1) == false and frames == 1, "an unchanged screen is not drawn again")
  ui.dirty()
  check(pass(1) == true and frames == 2, "ui.dirty() draws the next frame")
  check(pass(1) == false and frames == 2, "...once")
  keys_down = true
  check(pass(1) == true and pass(1) == true and frames == 4, "a key that is down draws every pass")
  keys_down = false
  check(pass(1) == true and frames == 5, "the pass after the release is drawn")
  check(pass(1) == false and frames == 5, "then nothing")
  for _ = 1, 24 do pass(10) end
  check(frames == 5, "nothing for 240 ms")
  check(pass(10) == true and frames == 6, "the period (250 ms) draws a frame")
  check(pass(1, 0) == true and pass(1, 0) == true and frames == 8, "period 0 draws every pass")
  check(pass(1) == false, "back to the default period")
  check(pass(ui.pause_ms) == true and frames == 9, "a pause between calls (an approval was up) draws")

  badge.input = saved_input
end

print(string.format("%d checks", checks))
print("all vk tests passed")
