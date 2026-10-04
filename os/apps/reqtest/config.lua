-- Request test: what to ask for. The device tests push their own copy of this file.
return {
  amount = "10.00",   -- display units, a string (never a number)
  count  = 1,         -- how many requests to open on start (the firmware allows 2 at a time)
  -- Optional. Left out, the firmware's defaults apply:
  -- symbol = "HACK",   -- default: the first token of the provisioned table
  -- name   = "Shop",   -- default: the badge's display_name
  -- ttl_s  = 60,       -- default: config req_ttl_s
}
