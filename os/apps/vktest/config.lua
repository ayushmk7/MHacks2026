-- Library test: the knobs and the JSON test table.
--
-- The same table is run on the laptop by test/host/test_vk.lua and on the badge by this app, so it
-- holds only cases that give the same answer with 64-bit and with 32-bit Lua numbers.

-- A backslash and a "u": the start of a JSON unicode escape (this file stays plain ASCII).
local U = "\\" .. "u"

return {
  -- Pause between two steps, so that the badge keeps answering the test script.
  step_ms = 100,

  -- The payment tried with no network. An empty `pay_to` means this badge's own address.
  pay_to = "",
  pay_amount = "1.00",
  -- How long the payment may take to end in "failed" before the app calls it stuck.
  pay_timeout_ms = 30000,

  -- What the read-only helpers are called with. Nothing here needs to exist anywhere.
  rpc_method = "getHealth",
  signature = string.rep("1", 88),      -- the length of a transaction signature in base58
  wire = "AQID",

  -- JSON texts that decode. `encoded` is what vk.json.encode gives for the decoded value; left
  -- out, it is the text itself.
  json = {
    {text = '{"a":[1,2,{"b":"c"}],"d":{"e":[]},"f":{}}'},
    {text = "[]"},
    {text = "{}"},
    {text = "[[],[[]],{}]"},
    {text = "[true,false,null]"},
    {text = "[0,-1,7,2147483647,-2147483647]"},
    {text = "[0.5,-2.25,1.5]"},
    {text = "[1e2,2.5E-1]", encoded = "[100,0.25]"},
    {text = '"plain"'},
    {text = '"tab\\tquote\\"slash\\\\newline\\n"'},
    {text = '"' .. U .. '0001"'},
    {text = '"' .. U .. "0041" .. U .. "00e9" .. U .. '20ac"', encoded = '"A\xC3\xA9\xE2\x82\xAC"'},
    {text = '"' .. U .. "d83d" .. U .. 'de00"', encoded = '"\xF0\x9F\x98\x80"'},
    {text = '"a\\/b"', encoded = '"a/b"'},
    {text = ' { "a" : [ 1 , 2 ] ,\n "b" : { } } ', encoded = '{"a":[1,2],"b":{}}'},
    {text = '{"b":1,"a":2}', encoded = '{"a":2,"b":1}'},
    {text = '{"error":null,"value":[null]}', encoded = '{"value":[null]}'},
    {text = '{"id":1,"jsonrpc":"2.0","result":{"context":{"slot":82},"value":{"blockhash":"EkSnNWid2cvwEVnVx9aBqawnmiCNiDgp3gUdkDPTKN1N"}}}'},
    {text = '{"id":1,"jsonrpc":"2.0","result":{"value":[{"confirmationStatus":"confirmed","slot":72}]}}'},
    {text = '{"record":"dj0x","sig":"AAAA"}'},
  },

  -- Texts that are not JSON: decode returns nil and a message.
  json_bad = {
    "", " ", "{", "}", "[", "]", "[1,]", "[1 2]", "{,}", '{"a"}', '{"a":}', '{"a":1,}', "{a:1}",
    '"abc', '"a\\x"', '"' .. U .. '12"', "tru", "nul", "True", "01", "-", "1.", ".5", "1e", "+1", "0x10",
    "1 2", "[1] x", '{"a":1} {"b":2}', '"line\nbreak"',
  },
}
