# Measurements

The numbers that must be measured on real badges before the non-functional requirements can be claimed: what each one is, how to take it, the log line it comes from, its target, and an empty table for the result.

Audience: firmware engineers.

Status: design, not yet built on hardware.

**Every result table in this document is empty on purpose.** Nothing has run on a badge, so there are no on-device numbers. Fill a cell only with a value you measured; leave it empty otherwise. The few figures quoted below are labelled with where they come from (host measurement, upstream statement, or the PRD's citation) and none of them is a badge measurement. Upstream is `firmware/solana-os/` at commit `812b8c7` of the [upstream repository](https://github.com/spacemandev-git/solana-defcon-badge-26/tree/812b8c7aca5c366d18c0b040fafd2999f7204d84/firmware/solana-os).

## Summary

| Id | Quantity | How | Target | Measured in |
|---|---|---|---|---|
| [M1](#m1) | PROOF round trip `elapsed` | `[pay] proof ok elapsed=<n>ms` logged by the payer for every PROOF; run 50 challenges on a `PAY_MEASURE 1` build, take p50/p99 | set `deadline_ms` ≈ p99 × 1.3; PRD target 250 ms | WP8 |
| [M2](#m2) | Ed25519 sign and verify time per backend | `[wallet] sign <n>ms verify <n>ms` at each signature | informs M1 and the Monocypher decision | WP7 |
| [M3](#m3) | Request to approval screen | `[pay] req first …` at first receipt of a request id and `[wallet] modal open …` when the approval screen opens; subtract the two `t` values | < 2 s (PRD) | WP10 |
| [M4](#m4) | SELECT to confirmed on dashboard | `[wallet] select …` at SELECT and `[rpc] status … confirmed …`; subtract; dashboard `lagMs` covers chain → feed | < 5 s (PRD) | WP10 |
| [M5](#m5) | RPC call time, first and reused connection | `[rpc] <method> <n>ms status=<code>` | informs polling intervals | WP4 |
| [M6](#m6) | Free heap and stack high-water mark during a payment | `[wallet] mem heap=<bytes> stack=<bytes> at=modal` and `… at=sent` | heap > 60 KB, stack headroom > 2 KB | WP10 |
| [M7](#m7) | ATA / PDA derivation time | `[wallet] ata <n>ms` at boot | < 100 ms | WP3 |

"Measured in" is the work package of the [implementation plan](../roadmap/implementation-plan.md) at which the quantity can first be taken; M3, M4 and M6 wait for WP10 because that is the first point at which the whole flow exists.

The log lines are ours [OURS]; they are written with tags `pay`, `wallet` and `rpc` ([../guides/troubleshooting.md](../guides/troubleshooting.md#log-tags)). The exact text:

| Id | Line | Written by, when |
|---|---|---|
| M1 | `[pay] proof ok elapsed=<n>ms` (also `proof late elapsed=<n>ms`, `proof bad_sig`) | `pay_session`, each PROOF |
| M2 | `[wallet] sign <n>ms verify <n>ms` | gate, each signature |
| M3 | `[pay] req first id=<16 hex> t=<ms>` and `[wallet] modal open screen=<A\|B\|C> id=<16 hex or -> t=<ms>` | first receipt of a request id; approval screen opens |
| M4 | `[wallet] select sig=<sig16> t=<ms>` and `[rpc] status sig=<sig16> confirmed t=<ms>` | SELECT accepted; first `confirmed`/`finalized` answer for that signature |
| M5 | `[rpc] <method> <n>ms status=<code>` | each RPC call |
| M6 | `[wallet] mem heap=<bytes> stack=<bytes> at=modal` and `… at=sent` | modal open; after each successful `sendTransaction` |
| M7 | `[wallet] ata <n>ms` | boot |

`t` is `millis()`. `<16 hex>` is the 8-byte request id; `<sig16>` is the first 16 base58 characters of the signature, as in the audit log.

## Capturing logs

The on-badge log ring holds 64 lines [UPSTREAM `src/badge_log.h`], which is too short for a run of 50 samples. Capture serial to a file instead:

```bash
arduino-cli monitor -p /dev/cu.usbserial-XXXX -c baudrate=115200 | tee serial.log
```

Percentiles from a captured log (nearest-rank; shown for M1, change the pattern for other lines). The pattern takes `proof ok` and `proof late` lines alike, because a PROOF that missed the current deadline is still a sample of the round trip:

```bash
grep -oE 'proof (ok|late) elapsed=[0-9]+' serial.log | cut -d= -f2 | sort -n \
  | awk '{a[NR]=$1} END {if (NR) print "n="NR, "p50="a[int((NR+1)/2)], "p99="a[int(NR*0.99+0.999)], "max="a[NR]}'
```

Record with every result:

| Field | Value |
|---|---|
| Date | |
| Firmware build (commit of the fork) | |
| Arduino-ESP32 core version | |
| Badges used (labels, key location of each) | |
| `WALLET_ED25519_BACKEND` | |
| Hotspot (phone model, band) | |
| `rpc_url` | |
| Distance between badges | |

## M1

**PROOF round trip.** The time the payer measures between sending a challenge and receiving the proof:

- `t0` is `millis()` taken immediately before `esp_now_send()` of the CHAL on the payer.
- `t1` is the receive timestamp stamped in the Wi-Fi-task callback when the PROOF arrives (patch P2), so the payer's own main-loop latency is excluded.
- `elapsed = t1 - t0` includes the air time both ways, the payee's main-loop latency until it drains the frame, and the payee's signing time.

See [Timing](../protocol/payment-protocol.md#timing). The payee is "present" only if the signature verifies and `elapsed <= deadline_ms`.

A normal build cannot collect 50 samples from one request: a session answers at most 8 challenges and at most 3 from one payer, and the payer makes at most 3 attempts per request ([rate limits](../wallet-core/signing-gate.md#rate-limits)). The build switch `PAY_MEASURE` (default `0`, in `wallet_defaults.h`) exists for this measurement. When it is `1`, the per-session cap of 8 PROOFs, the cap of 3 per MAC and the payer's cap of 3 attempts are lifted; the minimum gap of 200 ms between PROOFs stays. [OURS]

Procedure:

1. Flash the payee (M) and the payer (J) with `PAY_MEASURE 1` ([build and flash, Compile-time switches](../guides/build-and-flash.md#compile-time-switches)). Put both on the hotspot at table distance; capture serial on J.
2. On M open Request, choose an amount and press SELECT on the request screen. One request is enough.
3. On J push and run the `m1` app below once. It sends 50 challenges for the first request in the inbox, 400 ms apart, and shows the count.
4. Read 50 `[pay] proof` lines from J's capture and compute p50 and p99 with the snippet above. `proof bad_sig` lines, and challenges that got no answer, are not samples; note how many there were.
5. Set `deadline_ms` to about p99 × 1.3 and push it to all badges ([../guides/configure.md](../guides/configure.md#later-changes)).
6. Repeat per key backend present in the fleet. In a mixed fleet the deadline is set for the slowest signer.
7. Flash both badges with the normal build (`PAY_MEASURE 0`) again. **Never use the measurement build for the demo.**

`apps/m1/app.ini`:

```ini
name=M1
permissions=radio
```

`apps/m1/main.lua`:

```lua
-- apps/m1/main.lua   (app.ini: name=M1 / permissions=radio)
local n, last = 0, 0
function on_update()
  local req = badge.pay.inbox()[1]
  if req and n < 50 and badge.millis() - last >= 400 then
    last = badge.millis(); n = n + 1
    badge.pay.challenge(req.mac, req.id)
  end
end
function on_draw() badge.gfx.clear(); badge.gfx.text_center("M1 challenges sent: " .. n, 160, 110) end
function on_button(k, p) if p and k == "b" then badge.system.exit() end end
```

The listing has not been run.

Target: `deadline_ms` ≈ p99 × 1.3. The PRD's target is under 250 ms. [UNVERIFIED: the default of 400 ms. The PRD target is not reachable with an SE050 key, whose signature alone is cited at about 261 ms. Fallback values: 800 ms if any badge signs with the SE050; 2500 ms if the fast Ed25519 backend is not shipped.]

| Payee key backend | n | p50 (ms) | p99 (ms) | max (ms) | `deadline_ms` chosen |
|---|---|---|---|---|---|
| Software key, Monocypher | | | | | |
| Software key, TweetNaCl | | | | | |
| SE050 | | | | | |

## M2

**Ed25519 sign and verify time per backend.** Logged at each signature as `[wallet] sign <n>ms verify <n>ms`. The verify figure is the wallet's self-check of the signature it just produced.

Procedure:

1. Build with the backend under test (`WALLET_ED25519_BACKEND` 1 or 0; a badge reporting `secure element` for the SE050 row).
2. Produce at least 10 signatures of each kind: a payment (214-byte message), a request (99 signed bytes), a proof (66 signed bytes).
3. Record the median of each.

Target: none of its own. The result decides the backend and bounds M1.

| Backend | Sign, 214 B (ms) | Verify, 214 B (ms) | Sign, 99 B (ms) | Sign, 66 B (ms) | n |
|---|---|---|---|---|---|
| Monocypher (software key) | | | | | |
| TweetNaCl (software key) | | | | | |
| SE050 | | | | | |

For orientation only, not badge measurements:

| Source | Figure |
|---|---|
| Host (Apple Silicon, `cc -O2`), 214-byte message | TweetNaCl: sign 0.627 ms, verify 1.259 ms. Monocypher 4.0.2: sign 0.017 ms, verify 0.044 ms. Ratio about 37 (sign) and 29 (verify). Signatures are byte-identical. |
| Upstream's own statement | TweetNaCl costs about a second of CPU per signature on the badge [UPSTREAM `src/identity/TWEETNACL-README:15-22`]; not measured by us |
| PRD citation (wolfSSL benchmark) | SE050 Ed25519 sign about 261 ms; with transport turnaround the design expects 300 to 350 ms [UNVERIFIED] |

## M3

**Request to approval screen.** The payer logs `[pay] req first id=<16 hex> t=<ms>` when it first receives a REQ for a request id, and `[wallet] modal open screen=<A|B|C> id=<16 hex or -> t=<ms>` when the approval screen opens. The difference of the two `t` values for one id is the result. The `id` of the second line is `-` when the signing call carried no request id.

Procedure:

1. Payee opens a request; the tester on the payer chooses it as soon as it is listed and does nothing else.
2. Take the two `t` values for that request id from the payer's log and subtract:

   ```bash
   grep -E 'req first id=|modal open ' serial.log
   ```

3. Repeat 10 times; record median and worst.

The interval covers the challenge and proof, the attestation fetch (or cache hit), the blockhash fetch, building the message, and the time the tester takes to press SELECT in Pay. Note which runs had a cached attestation.

Target: under 2 s (PRD). [UNVERIFIED. Fallback: report the measured value honestly if the target is missed.]

| Condition | n | Median (ms) | Worst (ms) |
|---|---|---|---|
| Attestation fetched | | | |
| Attestation cached | | | |

## M4

**SELECT to confirmed on the dashboard.** Two parts, added together:

- On the badge: `[wallet] select sig=<sig16> t=<ms>` when SELECT is accepted, and `[rpc] status sig=<sig16> confirmed t=<ms>` at the first `confirmed` or `finalized` answer for that signature. Subtract the two `t` values with the same `sig16`.
- On the dashboard: `lagMs`, the time from block time to database insert, per payment in `GET /api/payments` and as p50/p95 in `GET /api/stats` ([dashboard API](../../dashboard/API.md)).

Procedure:

1. Make 10 payments.
2. For each, subtract the two badge timestamps and add that payment's `lagMs`.

Target: under 5 s on devnet (PRD). [UNVERIFIED. Fallback: report the measured value honestly if the target is missed.]

| n | Badge: SELECT to confirmed, median (ms) | Dashboard `lagMs`, median (ms) | Sum, median (ms) | Sum, worst (ms) |
|---|---|---|---|---|
| | | | | |

## M5

**RPC call time.** One line per call: `[rpc] <method> <n>ms status=<code>`. The first call on a connection includes the TLS handshake; later calls reuse the session if connection reuse holds.

Procedure:

1. Reboot the badge on the hotspot, open Home, capture a few minutes of serial.
2. Make one payment so that every method appears.
3. For each method record the first call after boot and the median of the later calls. Count calls with a status other than 200.

Target: none of its own. The result sets the polling intervals (Home makes one call per 3 s) and tells whether a second RPC URL is needed. [UNVERIFIED: HTTPS latency through a hotspot, and that connection reuse holds across calls. Fallback: lengthen polling; second RPC URL. Devnet rate limits with four badges polling are also unverified; Home backs off on HTTP 429.]

| Method | First call (ms) | Reused connection, median (ms) | n | Non-200 responses |
|---|---|---|---|---|
| `getBalance` | | | | |
| `getTokenAccountBalance` | | | | |
| `getLatestBlockhash` | | | | |
| `getAccountInfo` | | | | |
| `sendTransaction` | | | | |
| `getSignatureStatuses` | | | | |

## M6

**Free heap and stack high-water mark during a payment.** The firmware logs `ESP.getFreeHeap()` and `uxTaskGetStackHighWaterMark(NULL)` as `[wallet] mem heap=<bytes> stack=<bytes> at=modal` when an approval screen opens, and with `at=sent` after each successful `sendTransaction`.

Procedure:

1. Make a payment from Pay (Lua VM running, TLS session open, approval modal on screen).
2. Read the two `[wallet] mem` lines. `heap` is in bytes; the table below takes KB.
3. Repeat from Checkout and from the C++ Tip Jar if they are built.

Target: free heap above 60 KB; stack headroom above 2 KB. [UNVERIFIED: the loop task stack is set to 16 KB (upstream default is 8 KB); one TLS session is estimated at about 40 KB of heap. Fallback: raise the stack to 24 KB.]

| Where | Free heap at modal open (KB) | Stack headroom at modal open (bytes) | Free heap after `rpc.send` (KB) | Stack headroom after `rpc.send` (bytes) |
|---|---|---|---|---|
| Pay | | | | |
| Checkout | | | | |
| Tip Jar (C++) | | | | |

## M7

**Token-account and PDA derivation time.** Logged at boot as `[wallet] ata <n>ms` when the badge derives its own associated token account. A derivation is a SHA-256 plus a curve-membership test per attempt, repeated until a bump is found.

Procedure:

1. Reboot; read the line.
2. Repeat on each badge (the number of attempts depends on the key).

Target: under 100 ms. [UNVERIFIED. Fallback: cache token accounts per payee.]

| Badge | `[wallet] ata` (ms) |
|---|---|
| Merchant | |
| Impostor | |
| Judge A | |
| Judge B | |

## Other quantities to record

These have no measurement id but are unverified and cheap to note while the badges are on the table.

| Quantity | How | Why | Value |
|---|---|---|---|
| Image size of the fork | size reported at the end of `arduino-cli compile` | the additions are estimated at about 120 KB over upstream's 1,849,888 bytes; the slot is 3,342,336 bytes | |
| Arduino-ESP32 core version that builds | `arduino-cli core list` | to pin in [../guides/build-and-flash.md](../guides/build-and-flash.md#toolchain) | |
| Signal strength at table distance and across the room | Settings → ESP-NOW radar, or `badge.espnow.peers()` | tuning `rssi_min` (default -75; -100 disables the filter); and whether Pay's signal bars (five at -50 dBm or better, then -60, -70, -80, -90) tell near from far at table distance [UNVERIFIED; fallback: show the RSSI number instead] | |
| Badge and laptop reach each other on the hotspot | `curl -i "http://<laptop-ip>:8788/badge/pending?badge=<pubkey>"` from another hotspot client | the attack demo's delivery path | |
| Clock sync | Settings → Wallet, row `Clock` | attestation expiry is evaluated only when synced | |
| SNTP sync callback | the build compiles with `sntp_set_time_sync_notification_cb`, and `Clock` turns to `synced` only after Wi-Fi is up | `clock_synced` depends on it [UNVERIFIED; fallback: the time threshold alone, noting that the WPA2-Enterprise path can then read "synced"] | |
| Pinned TLS with an unset clock | install `rpc-ca`, reboot, read the first `[rpc]` lines | whether a pinned handshake checks certificate dates before the clock is set [UNVERIFIED; the client waits up to 5 s for SNTP; fallback: `rmcert rpc-ca` and run unpinned, shown on screen] | |
| Node's message for an expired blockhash | hold an approval screen past the blockhash lifetime, approve, read the message `rpc.send` returns | Pay matches `lockhash` to show `Expired, try again` [UNVERIFIED; fallback: the generic line `Send failed, nothing moved`] | |
| LED colours at brightness 72 | look at the two LEDs during Screens A, B and C | green, amber and red must be told apart [UNVERIFIED; fallback: raise LED brightness in the modal only] | |
| `__has_include("local_config.h")` | compile once with and once without the file | the baked-in Wi-Fi is optional only if it works [UNVERIFIED; fallback: commit an empty `local_config.h` and drop the `#if`] | |
| HTTP 429 responses from the RPC endpoint | count `status=429` in `[rpc]` lines over 10 minutes with four badges on Home | devnet rate limits | |

## Requirements covered

The PRD's non-functional requirements: latency (M3, M4), sandbox fit (M2, M6), reliability (M5). F9 depends on M1 for its deadline; F17 on M2 for the SE050 timing.

## Open items

- [UNVERIFIED] All of M1 to M7: nothing has been measured on a badge. Fallbacks are given with each.
- [UNVERIFIED] PROOF deadline 400 ms and the PRD's 250 ms target (M1). Fallback: 800 ms with the SE050; 2500 ms with TweetNaCl.
- [UNVERIFIED] Ed25519 time on the ESP32-S3 for each backend (M2). Fallback: choose the backend and `deadline_ms` from the result.
- [UNVERIFIED] "Request to approval under 2 s" and "SELECT to dashboard under 5 s" (M3, M4). Fallback: report honestly.
- [UNVERIFIED] HTTPS RPC latency and connection reuse (M5). Fallback: lengthen polling; second RPC URL.
- [UNVERIFIED] Stack and heap headroom (M6). Fallback: 24 KB stack.
- [UNVERIFIED] PDA derivation time (M7). Fallback: cache token accounts per payee.
- [UNVERIFIED] Whether a pinned TLS handshake checks certificate validity dates while the badge clock is unset. Fallback: wait up to 5 s for SNTP before the first pinned request; if it still fails, remove `rpc-ca` and run unpinned (shown on screen).
- [UNVERIFIED] Exact wording of the node's preflight error for an expired blockhash. Fallback: the generic line `Send failed, nothing moved`.
- [UNVERIFIED] `__has_include("local_config.h")` under the installed core's compiler. Fallback: commit an empty `local_config.h` and drop the `#if`.
- [UNVERIFIED] Signal-bar thresholds (-50/-60/-70/-80/-90 dBm) at table distance. Fallback: show the RSSI number instead.
- [UNVERIFIED] LED colours distinguishable at brightness 72. Fallback: raise LED brightness in the modal only.
- [UNVERIFIED] SNTP sync callback availability in the Arduino core. Fallback: the time threshold alone.
