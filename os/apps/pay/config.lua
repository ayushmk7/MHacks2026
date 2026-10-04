-- Pay: everything a person might want to change. Edit and push again.
return {
  header = "BADGEOS",            -- the left text of the header

  refresh_ms = 500,              -- how often the list of nearby requests is read again

  -- Signal bars from the request's RSSI (dBm): at or above the first value 4 bars, the second 3,
  -- the third 2, weaker 1.
  signal_dbm = {-55, -67, -78},

  led_ms = 1500,                 -- how long the LEDs stay green after a confirmed payment

  -- The list with no request heard: what to do about it.
  empty = "No requests nearby: open Request on the payee",
  -- The list's footer while Wi-Fi is not joined (a payment would fail at its first network step).
  offline_hint = "No Wi-Fi: Settings > Wi-Fi",
}
