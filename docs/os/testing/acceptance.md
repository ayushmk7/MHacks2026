# On-device acceptance tests

One check per requirement, run on real badges: the procedure, and what counts as a pass.

Audience: testers.

Status: design, not yet built on hardware.

No test in this document has been run. No badge has been flashed, and the firmware these tests exercise is not written yet. The procedures are the acceptance criteria the implementation is built against; a work package is done when its tests here pass ([../roadmap/implementation-plan.md](../roadmap/implementation-plan.md)). Upstream behaviour referred to here is `firmware/solana-os/` at commit `812b8c7` of the [upstream repository](https://github.com/spacemandev-git/solana-defcon-badge-26/tree/812b8c7aca5c366d18c0b040fafd2999f7204d84/firmware/solana-os).

Tests that need no hardware (decoder, derivations, frame codec, domain separation, attestation parser) are in [host-tests.md](host-tests.md). The four attack demonstrations are in [attack-scripts.md](attack-scripts.md). Timing targets are in [measurements.md](measurements.md).

## Setup

| Item | State |
|---|---|
| Badges | at least three, flashed with the fork and labelled as in `badges.json`: **M** = Merchant, **I** = Impostor, **J** = Judge A |
| Network | all badges and the laptop on one phone hotspot |
| Wallet config | pushed to every badge ([../guides/configure.md](../guides/configure.md)); defaults: cap 100.00, max 1000.00, `deadline_ms` 400, `attest_ttl` 30, `block_red` 1 |
| Funds | every badge holds HACK and SOL (`npm run devnet:setup` inside `dashboard/`) |
| Registry | M is attested as `MHacks Merch`; I and J are not attested |
| Known names | J has seen M verified once (the seeding step, [pre-event checklist, item 10](../guides/pre-event-checklist.md#10-seed-known-names-on-each-judge-badge)); needed by T-F15 |
| Dashboard | server and web running, LIVE mode ([dashboard runbook](../../dashboard/RUNBOOK.md)) |
| Logs | serial monitor open on the badge under test: `arduino-cli monitor -p <port> -c baudrate=115200` |

Buttons are named by silkscreen. Example amounts: 10.00 HACK is raw `1000`.

## Summary

| Id | Requirement | Procedure | Pass |
|---|---|---|---|
| [T-F1](#t-f1) | F1 | Push a 5-line Lua app that prints `badge.identity.pubkey()` and calls `identity.sign` on a valid message | key equals Settings → Identity; `identity.sign` shows the approval screen, amber Screen B for this bare app (`token account` or `unverified badge`, `presence not checked`); returns 64 bytes after holding SELECT 2 s; `nil,"rejected"` after CANCEL |
| [T-F1b](#t-f1b) | F1 | Same app without `permissions=sign` | `nil,"denied"`, no screen |
| [T-F2](#t-f2) | F2 | From the test app, sign with SELECT held down before the call | no signature until SELECT is released and pressed again after 600 ms |
| [T-F2b](#t-f2b) | F2 | App sets `gfx.brightness(0)`, draws a full-screen fake, then signs | wallet screen visible at backlight ≥ 160, fake gone; backlight restored afterwards |
| [T-F2c](#t-f2c) | F2 | During the prompt run `badge-push.py stop` | prompt stays; stop takes effect after the decision |
| [T-F3](#t-f3) | F3 | Sign a message whose instruction tag is 3 (`Transfer`), one with a ComputeBudget instruction added, one for another mint | blocked screens "Unknown instruction" (`reason: ix_data`), "Unknown instruction" (`reason: too_long`), "Unknown token" (`reason: unknown_mint`); nothing signed; audit lines present |
| [T-F4](#t-f4) | F4 | Boot on hotspot | Home shows HACK, SOL, name, short address within 10 s |
| [T-F5](#t-f5) | F5 | Two other badges at different distances | list order follows distance; amount moves with the D-pad and stops at `cap` |
| [T-F6](#t-f6) | F6 | Request 10.00 on badge M | appears in badge J's Pay list within 2 s |
| [T-F7](#t-f7) | F7 | Complete a payment | both badges flash green; dashboard feed shows it; History updated on both |
| [T-F8](#t-f8) | F8 | Flip one byte of a captured REQ and rebroadcast (replay app) | not listed; log shows `[pay] req bad_sig from <mac>` |
| [T-F9](#t-f9) | F9 | Normal request, then repeat with the payee app closed | first `present`; second `NOT PRESENT` |
| [T-F10](#t-f10) | F10 | Issue an attestation on the dashboard | payer's screen shows the attested name with `verified`; second payment within 30 s makes no RPC call for it |
| [T-F11](#t-f11) | F11 | Three payees: attested, unattested, attested with a different claimed name | green, amber, red respectively |
| [T-F11c](#t-f11c) | G3 | Sign a kit-built transfer with a wrong `recipient` hint | row 2 `token account`, amber |
| [T-F14](#t-f14) | F14 | M requests 150.00 with the `bigreq` app and J pays it from Pay; then the test app signs 1500.00 | green Screen A, SELECT, then Screen D with a 2 s hold, signed; then blocked "Above the maximum" |
| [T-F15](#t-f15) | F15 | Revoke on the dashboard, wait 30 s, request again | red `REVOKED` |
| [T-F16](#t-f16) | F16 | After several payments, power-cycle | History lists them |
| [T-SE1](#t-se1) | F17 | On a badge reporting `secure element`: sign a 214-byte transfer | signature verifies (self-check passes, transaction confirms); time logged |
| [T-F19](#t-f19) | F19 | Stretch | History shows `co-signed` |
| [T-NFR-honesty](#t-nfr-honesty) | NFR | Compare Settings → Identity, Settings → Wallet, `/api/identity`, dashboard Badges page | all four agree on key location |
| [T-NFR-cancel](#t-nfr-cancel) | NFR | In every app screen and every wallet screen press CANCEL; hold CANCEL 1.5 s in each app | always leaves; never stuck |
| [T-APP1](#t-app1) | platform | Run Tip Jar (Lua) and Tip Jar (C++) | identical behaviour; the wallet screens are identical except the context line (`app: tipjar` and `app: tipjar-native`) |
| [T-APP2](#t-app2) | platform | Launch Home → Pay → Home → Request and send ESP-NOW frames each time | `on_espnow`/inbox works in every app (P1 applied) |

G3 is the guarantee "the recipient shown is the recipient paid" from [../security/security-model.md](../security/security-model.md).

## The test app

T-F1 to T-F3, T-F11, T-F11c and the second half of T-F14 use one small Lua app. It builds a valid transfer message with the on-badge builder and asks the wallet to sign it. It never submits anything.

`apps/signtest/app.ini`:

```ini
name=Sign test
permissions=sign,net
```

`apps/signtest/main.lua`:

```lua
-- Acceptance-test helper: builds a transfer and asks the wallet to sign it. Nothing is sent.
local TO     = "Gn2GQYmcQkatE2594ov2ELKkYWZHwARG7FiAoPWSEcxq"  -- replace with badge M's public key
local AMOUNT = "1000"                                           -- raw units: 10.00 HACK
local HINTS  = nil                                              -- e.g. { recipient = TO }
local line   = "SELECT sign   CANCEL exit"

local function mutate(msg) return msg end                       -- replaced in T-F3

local function try_sign()
  badge.log("pubkey " .. tostring(badge.identity.pubkey()))
  local blockhash, e1 = badge.rpc.blockhash()
  if not blockhash then line = "blockhash: " .. tostring(e1) return end
  local msg, e2 = badge.sol.transfer_message{ to = TO, amount = AMOUNT, blockhash = blockhash }
  if not msg then line = "build: " .. tostring(e2) return end
  local sig, err, detail = badge.identity.sign(mutate(msg), HINTS)
  if sig then
    line = "signed, " .. #sig .. " bytes"
  else
    line = "nil, " .. tostring(err) .. (detail and (" (" .. tostring(detail) .. ")") or "")
  end
  badge.log(line)
end

function on_button(key, pressed)
  if not pressed then return end
  if key == "a" then try_sign() elseif key == "b" then badge.system.exit() end
end

function on_draw()
  local g = badge.gfx
  g.clear()
  g.text(tostring(badge.identity.pubkey()), 8, 60)
  g.text(line, 8, 100)
end
```

Push and run it [UPSTREAM tool]:

```bash
cd firmware/solana-os
tools/badge-push.py --host <badge-ip> --token <code> push apps/signtest --run
```

The listing uses only functions of the badge API ([../app-platform/api-reference.md](../app-platform/api-reference.md)). It has not been run.

## Tests

### T-F1

Requirement: F1. Needs: one badge, the test app.

1. Push and run the test app.
2. Compare the key on the app's screen (and in the log line `pubkey …`) with Settings → Identity.
3. Press SELECT in the app. The wallet's approval screen opens: amber [Screen B](../wallet-core/screens.md#screen-b).
4. Hold SELECT for 2 s on the wallet screen. The app shows `signed, 64 bytes`.
5. Press SELECT in the app again, then CANCEL on the wallet screen. The app shows `nil, rejected`.

Pass: the key equals Settings → Identity; `identity.sign` shows the approval screen, amber Screen B for this bare app (`token account` or `unverified badge`, `presence not checked`); it returns 64 bytes after holding SELECT 2 s and `nil,"rejected"` after CANCEL.

Why amber. The green form (Screen A, a single SELECT press) needs a payee that is verified and present. The bare test app gives neither: with `HINTS = nil` the wallet cannot resolve the recipient wallet, so row 2 reads `token account`; with `HINTS = { recipient = TO }` and an unattested `TO` it reads `unverified badge`. In both cases the presence line is `presence not checked`. Screen A is exercised by [T-F7](#t-f7) and [T-F11](#t-f11). See [severity and gestures](../wallet-core/signing-gate.md#severity-and-gestures).

### T-F1b

Requirement: F1. Needs: one badge, the test app.

1. Change `app.ini` to `permissions=net` and push again.
2. Press SELECT in the app.

Pass: the app shows `nil, denied`; no wallet screen appears.

### T-F2

Requirement: F2. Needs: one badge, the test app.

1. Press SELECT in the app and keep it held down.
2. The wallet screen opens. Keep holding for 5 s.
3. Release SELECT, wait a second, then make the gesture the footer asks for.

Pass: no signature while SELECT stays held from before the call. Every gesture starts from a SELECT press edge seen after the arming delay: a signature is produced only after SELECT has been released and pressed again, and not within the first 600 ms after the screen opened. See [Approval modal](../wallet-core/signing-gate.md#approval-modal).

### T-F2b

Requirement: F2. Needs: one badge, the test app.

1. Add these lines at the top of `try_sign()` and push again:

   ```lua
   local g = badge.gfx
   g.brightness(0)
   g.fill_rect(0, 0, 320, 240, g.RED)
   g.text_center("PAYMENT APPROVED", 160, 110, g.WHITE, 2)
   g.flush()
   ```

2. Press SELECT in the app.

Pass: the wallet screen is visible with the backlight at 160 or more; nothing of the fake remains; after the decision the backlight returns to what the app had set (dark, in this test). Leave the app by holding CANCEL 1.5 s and restore the brightness in Settings → Display / LEDs.

### T-F2c

Requirement: F2. Needs: one badge on Wi-Fi, the test app, a laptop shell.

1. Press SELECT in the app so that the wallet screen is open.
2. On the laptop, within the 60 s the prompt lives:

   ```bash
   tools/badge-push.py --host <badge-ip> --token <code> --timeout 60 stop
   ```

3. Watch the badge for 10 s, then decide on the wallet screen.

Pass: the prompt stays on screen while the command waits; the stop takes effect only after the decision.

### T-F3

Requirement: F3. Needs: one badge, the test app.

Three messages, signed one after the other. Replace `mutate` in the test app for each.

1. Instruction tag 3 (`Transfer`). The tag is byte offset 204 of the 214-byte legacy message:

   ```lua
   local function mutate(msg) return msg:sub(1, 204) .. string.char(3) .. msg:sub(206) end
   ```

2. A message with a ComputeBudget instruction added. This one cannot be made by patching a byte, so a fixed message is used: `tx.legacy_compute_budget` in [`reference/code/vectors.json`](../reference/code/vectors.json), built with `@solana/kit` (`SetComputeUnitPrice` plus the vector transfer, 6 account keys, 258 bytes). Paste its base64 into the app:

   ```lua
   local CB = "AQADBjo6Tr7kAwsbSUntaqNiS1FrgEJA/3IYyF23U+YrOBUqF42AtnY2rEU5pMkvsyhcsXaE75epvbXDVL8WtfTiK1uyN7BiQ+728WbRcRvjDvxJ3Qyn+4YROF8GKmiJUp28BAMGRm/lIRcy/+ytunLDm+e8jOW7xfcSayxDmzpAAAAABpuIV/6rgYT7aH9jRhjANdrEOdwa6ztVmKDwAAAAAAEG3fbh12Whk9nL4UbO63msHLSF7V9bN5E6jPWFfv8AqQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAgMACQPoAwAAAAAAAAUEAgQBAAoM6AMAAAAAAAAC"
   local function mutate(msg) return badge.codec.b64decode(CB) end
   ```

   Its payer is the stand-in Judge A, not the badge under test. That does not matter: the decoder refuses the message before the signer check. The same bytes are refused with `too_long` in the host suite (`V_CB_LEGACY` in `test_sol.c`).
3. Another mint. Replace the configured mint's 32 bytes in the message with a different mint:

   ```lua
   local function mutate(msg)
     local mint  = badge.codec.b58decode(badge.wallet.info().mint)
     local other = badge.codec.b58decode("So11111111111111111111111111111111111111112")
     local at = msg:find(mint, 1, true)
     return msg:sub(1, at - 1) .. other .. msg:sub(at + 32)
   end
   ```

4. After the three attempts read the audit log: Settings → Wallet → Audit log, or `curl -s -H "X-Badge-Token: <code>" "http://<badge-ip>/api/wallet/audit"`.

Pass, per message:

| # | Headline on Screen E | `reason:` line | The app shows |
|---|---|---|---|
| 1 | `Unknown instruction` | `reason: ix_data` | `nil, unknown_instruction (ix_data)` |
| 2 | `Unknown instruction` | `reason: too_long` (258 bytes is more than the 256 the wallet looks at) | `nil, unknown_instruction (too_long)` |
| 3 | `Unknown token` | `reason: unknown_mint` | `nil, unknown_mint` |

Nothing is signed; there is one `TX` audit line per attempt with that result. The blocked screen is [Screen E](../wallet-core/screens.md#screen-e); it closes on CANCEL or after 15 s.

Rate limits apply to this test: wait at least 2 s between attempts, and expect the app to be locked out for 30 s after three non-approvals within 60 s (`nil, rate_limited`, no screen). See [rate limits](../wallet-core/signing-gate.md#rate-limits).

The `mutate` snippets for messages 1 and 3 are derived from the message layout and have not been run. The message for case 2 is host-tested as bytes; pasting it into the app has not been run.

### T-F4

Requirement: F4. Needs: one configured, funded badge.

1. Power-cycle the badge on the hotspot. Home starts by itself.
2. Start a stopwatch when `[os] ready` appears on serial.

Pass: within 10 s Home shows the HACK balance, the SOL balance, the name and the short address. The name and status match the dashboard's Badges page; the short address matches Settings → Identity.

### T-F5

Requirement: F5. Needs: three badges.

1. Put one badge next to J and one a few metres away.
2. On J open Pay.
3. Choose a nearby badge and move the amount with UP/DOWN (1.00 steps) and LEFT/RIGHT (10.00 steps).

Pass: the nearby list is ordered strongest signal first and the order follows the distances; swapping the two badges swaps the order; the amount starts at 10.00, stops at `cap` (100.00) and does not go below 0.01.

### T-F6

Requirement: F6. Needs: M and J.

1. On J open Pay.
2. On M open Request, choose 10.00, press SELECT on the wallet's request screen ([Screen F](../wallet-core/screens.md#screen-f)). Start a stopwatch.

Pass: the request appears under `REQUESTS` in J's Pay list within 2 s.

### T-F7

Requirement: F7. Needs: M, J, the dashboard.

1. Continue from T-F6: on J choose the request and approve it on the wallet screen.
2. Watch both badges and the dashboard's Feed.
3. Open History on both badges.

Pass: both badges flash their LEDs green after confirmation; the payment appears on the dashboard's Feed; History shows it on J (outgoing) and on M (incoming).

### T-F8

Requirement: F8. Needs: M, I, J; the replay app from [attack-scripts.md](attack-scripts.md) on I.

1. On M open a request for 10.00. On J open Pay and see it listed as `10.00 HACK`. Do not choose or dismiss it on J: a request that reached a terminal state on J goes to J's replay ring and would then be hidden for that reason instead.
2. On I, capture the request with the replay app: it shows `REQ recorded from <M's MAC>`.
3. On I, press RIGHT in the replay app. It rebroadcasts the stored frame with one bit flipped in the lowest byte of the amount field (payload offset 36): raw `1000` becomes `1001`, which would be listed as `10.01 HACK` if it were accepted.

Pass: J's list never shows `10.01 HACK` (the `10.00` entry stays until it expires); J's log shows `[pay] req bad_sig from <mac>`, with I's MAC in lower case.

### T-F9

Requirement: F9. Needs: M and J.

1. On M open Request for 10.00. On J choose it in Pay and watch the progress line `Badge is present`; continue to the wallet screen, then pay or cancel.
2. On M open a new Request for 10.00. Wait until it is listed on J. Do not choose it yet.
3. On M close Request with CANCEL. The session ends, so M answers no more challenges.
4. On J choose the request while it is still listed (an inbox entry lives `ttl_s` seconds from first receipt, 120 by default).

Pass: the first run shows `present`; the second shows `NOT PRESENT` and signing is blocked. See [Timing](../protocol/payment-protocol.md#timing).

The second run needs a new request because the first one's id is in J's replay ring after step 1 and is no longer listed. The same result with a recorded request is attack flow 2 in [attack-scripts.md](attack-scripts.md).

### T-F10

Requirement: F10. Needs: M, J, the dashboard, serial on J.

1. On the dashboard's Registry page issue `MHacks Merch` to M (skip if already issued).
2. Pay M from J. Note the `[rpc] getAccountInfo …` line on J's serial.
3. Within 30 s pay M again from J.

Pass: J's approval screen shows `MHacks Merch` in the name row with `verified`; during the second payment J logs no `getAccountInfo` call for M's attestation (the cached result is fresh for `attest_ttl` seconds). See [Cache](../identity/attestation.md#cache).

### T-F11

Requirement: F11. Needs: M (attested), I (not attested), J.

1. Attested payee: M requests 10.00, J pays. Expect green ([Screen A](../wallet-core/screens.md#screen-a)).
2. Unattested payee: I requests 10.00 under its own device name, J opens it. Expect amber ([Screen B](../wallet-core/screens.md#screen-b)): `UNVERIFIED`, row 2 `unverified badge`.
3. Attested payee with a different claimed name. One way: on J run the test app with `TO` set to M's key and `HINTS = { recipient = TO, claimed_name = "Other Shop" }`. Expect red ([Screen C](../wallet-core/screens.md#screen-c)): `NAME MISMATCH`.

Pass: green, amber, red respectively; a claimed name never appears in the name row. See [Decision](../identity/attestation.md#decision).

### T-F11c

Requirement: guarantee G3. Needs: J, the test app.

1. In the test app set `HINTS = { recipient = "<any public key other than TO>" }`.
2. Press SELECT in the app.

Pass: row 2 of the approval screen reads `token account`, row 3 shows the destination token account, the detail line `recipient is a token account` is present, and the screen is amber.

The on-badge builder's output decodes to the same fields as a message built with `@solana/kit` (host-tested; the bytes are equal for the first vector and differ in account order for the second), so the test app's message stands in for a kit-built one.

### T-F14

Requirement: F14. Needs: M (attested), J with the normal Pay app and the test app, the `bigreq` app below.

The amount pickers of Pay and Request stop at `cap`, so a payment above the cap is driven by a request, not by a picker. Pay does not filter requests by `cap`.

`apps/bigreq/app.ini`:

```ini
name=Big request
permissions=sign,radio
```

`apps/bigreq/main.lua`:

```lua
-- apps/bigreq/main.lua   (app.ini: name=Big request / permissions=sign,radio)
local id, err = badge.pay.request("15000")          -- 150.00 HACK, above cap 100.00
function on_draw() badge.gfx.clear(); badge.gfx.text_center(id and "asking 150.00" or ("failed: " .. tostring(err)), 160, 110) end
function on_button(k, p) if p and k == "b" then badge.pay.cancel(); badge.system.exit() end end
```

Above the cap:

1. Push `bigreq` to M and run it. Press SELECT on the wallet's request screen ([Screen F](../wallet-core/screens.md#screen-f)). M shows `asking 150.00`.
2. On J open Pay and choose the request.
3. The wallet shows green [Screen A](../wallet-core/screens.md#screen-a) with `PAY 150.00 HACK`. Press SELECT.
4. The wallet shows `LARGE PAYMENT` ([Screen D](../wallet-core/screens.md#screen-d)). Hold SELECT for 2 s.

Above the maximum:

5. On J, in the test app set `AMOUNT = "150000"` (1500.00 HACK) and sign.
6. On M, change `bigreq` to `badge.pay.request("150000")`, push and run it.

Pass: in step 3 the first screen is green and a single SELECT leads to Screen D, not to a signature; a signature is returned only after the hold in step 4, and Pay then submits the 150.00 payment as usual. In step 5 the blocked screen "Above the maximum" appears and the app shows `nil, over_limit`. In step 6 `pay.request` returns `nil, "over_limit"` with no screen, and M shows `failed: over_limit`.

The `bigreq` listing has not been run.

### T-F15

Requirement: F15. Needs: M (attested), J seeded so that M is in its known-names store (setup table), the dashboard.

1. On the dashboard's Registry page revoke M's attestation.
2. Wait 30 s (`attest_ttl`).
3. On M request 10.00. On J open the request.

Pass: J's approval screen is red with `REVOKED`; signing is blocked. On a J that never saw M verified the screen is amber `UNVERIFIED` instead, which is the design and not a pass of this test. Re-issue M's attestation afterwards.

### T-F16

Requirement: F16. Needs: one badge with several completed payments.

1. Note the payments made so far.
2. Power-cycle the badge.
3. Open History.

Pass: History lists them, newest first. A payment that was signed appears even if the app that made it stopped right after signing.

### T-SE1

Requirement: F17. Needs: a badge whose Settings → Identity reads `secure element`; firmware with patch P6.

1. Make a normal 10.00 HACK payment from that badge (the message is 214 bytes).
2. Read the `[wallet] sign <n>ms verify <n>ms` line on serial.
3. Wait for the transaction to confirm.

Pass: the signature verifies (the wallet's self-check passes, so the call does not return `sign_failed`) and the transaction confirms on devnet; the signing time is logged. Record the time in [M2](measurements.md#m2).

[UNVERIFIED: the whole SE050 path. Fallback if this test fails on any badge: `WALLET_FORCE_SOFTWARE_KEY 1`, a new software identity on that badge, and the secure-element claim is dropped ([../guides/build-and-flash.md](../guides/build-and-flash.md#resetting-identity)).]

### T-F19

Requirement: F19 (stretch). Needs: M and J with the receipts feature.

1. Complete a request-initiated payment from J to M. After M's Request app has confirmed the payment on chain it calls `pay.receipt()`, and the firmware sends the RCPT to the badge the PAID hint came from.
2. Open History on J.

Pass: the entry shows `co-signed` in place of `confirmed`.

### T-NFR-honesty

Requirement: PRD non-functional requirement "Honesty". Needs: each badge, the dashboard.

1. On the badge: Settings → Identity, line `key lives in`.
2. On the badge: Settings → Wallet, first row: `Key  secure element` (green) or `Key  software` (amber).
3. `curl -s -H "X-Badge-Token: <code>" "http://<badge-ip>/api/identity"`: fields `source` and `key_location`.
4. Dashboard, Badges page: the key location chip.

Pass: all four agree (`secure element` / `Key  secure element` / `se050` / `SECURE ELEMENT`, or `software` / `Key  software` / `software` / `SOFTWARE`).

### T-NFR-cancel

Requirement: PRD non-functional requirement "Judge UX". Needs: one badge with all apps. This test is a release gate.

1. In every screen of Home, Pay, Request, History, Checkout and Tip Jar press CANCEL.
2. On every wallet screen (A to H, where reachable) press CANCEL.
3. In each app hold CANCEL for 1.5 s.

Pass: CANCEL always goes back or exits; the hold always returns to the launcher; the badge is never stuck. Every footer names the silkscreen key.

### T-APP1

Requirement: app platform. Needs: one badge with Tip Jar (Lua) pushed and Tip Jar (C++) compiled in.

1. Run the Lua Tip Jar and go through a tip up to the wallet screen.
2. Run the C++ Tip Jar and do the same.

Pass: identical behaviour; the wallet screens are identical except the context line (`app: tipjar` and `app: tipjar-native`). See [../app-platform/examples.md](../app-platform/examples.md).

### T-APP2

Requirement: app platform; checks patch P1. Needs: two badges.

1. On J launch Home, then Pay, then Home, then Request, in that order, without rebooting.
2. In each app have the other badge send ESP-NOW application frames (for example an open request from M).

Pass: frames arrive in every app (`on_espnow` is called and the wallet's inbox fills), not only in the first one launched after boot.

## Results

Fill in one row per run. Leave a cell empty rather than guess.

| Id | Date | Build (commit) | Badges used | Result (pass / fail) | Notes |
|---|---|---|---|---|---|
| T-F1 | | | | | |
| T-F1b | | | | | |
| T-F2 | | | | | |
| T-F2b | | | | | |
| T-F2c | | | | | |
| T-F3 | | | | | |
| T-F4 | | | | | |
| T-F5 | | | | | |
| T-F6 | | | | | |
| T-F7 | | | | | |
| T-F8 | | | | | |
| T-F9 | | | | | |
| T-F10 | | | | | |
| T-F11 | | | | | |
| T-F11c | | | | | |
| T-F14 | | | | | |
| T-F15 | | | | | |
| T-F16 | | | | | |
| T-SE1 | | | | | |
| T-F19 | | | | | |
| T-NFR-honesty | | | | | |
| T-NFR-cancel | | | | | |
| T-APP1 | | | | | |
| T-APP2 | | | | | |

## Requirements covered

F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F14, F15, F16, F17, F19, and the PRD's non-functional requirements "Honesty" and "Judge UX". F12 is checked by attack flow 3 in [attack-scripts.md](attack-scripts.md). F13 and F18 are outside the OS.

## Open items

- [UNVERIFIED] Every test: nothing has run on a badge. Fallback: none; this document is the list of what has to be shown.
- [UNVERIFIED] T-SE1 depends on the SE050 path, which has never run on silicon. Fallback: software key, stated on the badge.
- [UNVERIFIED] T-F9 and T-F6 depend on radio timing (`deadline_ms`, 2 s listing). Fallback: measure M1 and M3, then adjust `deadline_ms`.
- T-F15 shows red only on a badge that has seen the merchant verified once. Procedure: the seeding step of the pre-event checklist; otherwise the screen is amber `UNVERIFIED`.
- [UNVERIFIED] LED colours distinguishable at brightness 72 (T-F7 and the approval screens). Fallback: raise LED brightness in the modal only.
