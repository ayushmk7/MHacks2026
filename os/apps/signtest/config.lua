-- Sign test: the knobs. The addresses it talks to are provisioned settings (listener_url and
-- rpc_url, read with wallet.config), not values in this file.
return {
  -- How often the laptop is asked for a pending transfer.
  poll_ms = 2000,

  -- How long the checkout's claimed amount is shown before the firmware's approval opens.
  claim_ms = 3000,

  -- How long to wait before a network call that failed is tried again.
  retry_ms = 2000,

  -- Timeout of one HTTP request (the firmware caps it at 10000).
  http_timeout_ms = 4000,

  -- How long "sent ..." or "refused: ..." stays on the screen before "waiting" replaces it.
  result_ms = 8000,

  -- How many times a signed transaction is submitted before the app gives up on it. A blockhash
  -- lives for about 90 s, so more than that many seconds of retries cannot succeed.
  send_tries = 10,

  -- How many times a refusal is reported to the laptop before the app gives up on it.
  report_tries = 5,
}
