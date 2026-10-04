-- R3 signing harness: the knobs. harness/r3_app.py rewrites `listener` and `badge` when it pushes
-- the app.
return {
  -- The left header text of every screen.
  header = "BADGEOS",

  -- The laptop's R3 listener (`npm run r3`, port 8789), without a trailing slash. It is not the
  -- dashboard's badge listener: R3 queues its own transfer so it never shows on the dashboard.
  listener = "http://192.168.0.0:8789",

  -- This badge's base58 public key, as in dashboard/server/config/badges.json. The firmware's own
  -- wallet.address() wins; if the two differ the app refuses, because the laptop queued the
  -- transfer for another badge and the signature could never verify.
  badge = "",

  -- Timeout of the GET that fetches the transfer, in ms.
  get_timeout_ms = 3000,

  -- Timeout of the POST that returns the signature, in ms. The laptop answers only after devnet
  -- confirms, so this is the binding's cap.
  post_timeout_ms = 10000,
}
