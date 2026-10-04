-- R2 network probe: the knobs. harness/r2_probe.py rewrites `listener` when it pushes the app.
return {
  -- The left header text of every screen.
  header = "BADGEOS",

  -- The laptop's badge listener, without a trailing slash. GET <listener>/health must answer
  -- {"ok":true}: the dashboard server's listener does, and so does `r2_probe.py serve`.
  listener = "http://192.168.0.0:8788",

  -- The RPC node the second check sends getHealth to.
  rpc_url = "https://api.devnet.solana.com",

  -- Timeout of each check, in ms. Three checks stay well inside the 12 s per-callback cap.
  timeout_ms = 3000,
}
