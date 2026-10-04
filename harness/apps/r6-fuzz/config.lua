-- R6 fuzz app: the knobs. harness/r6_app.py rewrites `listener` and `badge` when it pushes the app.
return {
  -- The left header text of every screen.
  header = "BADGEOS",

  -- The laptop's R6 listener (harness/r6_fuzz.mjs, port 8790), without a trailing slash.
  listener = "http://192.168.0.0:8790",

  -- This badge's base58 public key, as in dashboard/server/config/badges.json. The firmware's own
  -- wallet.address() wins; if the two differ the app refuses.
  badge = "",

  -- Timeouts of the GET that fetches a case and the POST that reports it, in ms.
  get_timeout_ms = 4000,
  post_timeout_ms = 4000,

  -- How many times in a row a GET or POST may fail before the run stops with an error.
  retries = 3,

  -- The storage.kv key that holds the case in flight (an app's kv keys are at most 8 characters).
  marker = "inflight",
}
