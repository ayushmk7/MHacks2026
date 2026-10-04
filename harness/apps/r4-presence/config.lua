-- R4 presence timing: the knobs. harness/r4_timing.py rewrites `listener` when it pushes the app.
return {
  -- The left header text of every screen.
  header = "BADGEOS",

  -- The laptop's collector (`r4_timing.py serve`, port 8790), without a trailing slash. Runs are
  -- POSTed to <listener>/r4/result.
  listener = "http://192.168.0.0:8790",

  -- CHAL -> PROOF exchanges in one run.
  runs = 200,

  -- No PROOF by then and the exchange counts as lost, in ms. A PROOF that arrives later is still
  -- recorded, as late.
  lost_ms = 1000,

  -- Pause between exchanges, in ms.
  gap_ms = 50,

  -- How often this badge announces itself to the payer, in ms (about 1 Hz, like a payee's REQ).
  hello_ms = 1000,

  -- How often the screen is drawn while a run is going, in ms: rarely, so the transfer to the
  -- panel stays out of the timings. And when idle.
  redraw_ms = 500,
  idle_ms = 1000,

  -- The file in the app's storage that keeps finished runs until they are uploaded.
  results = "runs.jsonl",

  -- Timeout of the upload, in ms.
  upload_timeout_ms = 5000,
}
