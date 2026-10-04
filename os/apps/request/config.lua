-- Request: everything a person might want to change. Edit and push again.
--
-- Amounts here are whole numbers of the token's smallest unit ("minor units"): with a token of
-- 2 decimals, 1000 is 10.00. The app never does arithmetic on a fraction.
return {
  header = "BADGEOS",            -- the left text of the header

  start = 1000,                  -- the amount shown when the app opens
  step = 50,                     -- UP and DOWN change the amount by this; LEFT and RIGHT by ten times it
  min = 50,                      -- the amount never goes below this (a request for zero is refused)
  max = 100000,                  -- or above this (keep it under 2147483647: Lua integers are 32-bit)
  decimals = 2,                  -- used only while the badge has no token table (not set up yet)

  -- Optional. Left out, the firmware's defaults apply:
  -- symbol = "HACK",            -- default: the first token of the provisioned table
  -- name = "My stall",          -- default: the badge's display name
  -- ttl_s = 60,                 -- default: config req_ttl_s (10 to 600)

  status_ms = 500,               -- how often the waiting screen reads the request's status
  confirm_every_ms = 2000,       -- how often a reported transaction is looked up on chain
  confirm_for_ms = 30000,        -- and for how long, before it counts as not found
  led_ms = 1500,                 -- how long the LEDs stay green after a verified payment
}
