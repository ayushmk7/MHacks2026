# P2-A — Payer Flow: Pay App, Bank Signing, Approval States

Owner: **A** · Part 2 (≈ hours 8–20) · Status: **draft — finalize after U6 (app decision doc)**
Reads: [`00-Interfaces.md`](00-Interfaces.md) (§4, §4.1, §5, §6, §8.1, §9) · Builds on: [P1-A](P1-a-firmware-wallet-core.md) · Parent: [`Prd-verified-payment-key.md`](Prd-verified-payment-key.md)

## 1. Why this exists
This is the judge's side of every demo beat. They hold the payer badge, see a request, read the approval screen, and press SELECT or CANCEL. It must be impossible to misread and impossible to fool, even if the Lua Pay app itself is buggy or malicious. That is why every trust decision sits in the firmware (00 §4).

## 2. Scope — ✏️ Changed (routing)

| ID | Item | Layer | Priority |
| --- | --- | --- | --- |
| PA1 | Pay app: list or pop-up of nearby REQs (per U6), select one; show the REQ name only as "claims: …" until the record verifies | Lua | P0 |
| PA2 | Payer side of the handshake: `wallet.new_nonce(req_id)` → unicast CHAL → receive PROOF → `wallet.check_proof` (firmware keeps the result) | Lua | P0 |
| PA3 | Fetch `GET /registry/:pubkey` for the REQ's payee fresh for every payment; pass record + sig + the full REQ frame in `ctx` | Lua | P0 |
| PA4 | Solana rail: fetch blockhash, build legacy `transferChecked` (+ zero-account memo with hex `req_id`) to `record.solana_ata`, `wallet.begin_solana`, `wallet.poll` per tick, `sendTransaction`, poll status per tick, send RESULT, `POST /feed/solana {tx_sig, req}` | Lua | P0 |
| PA5 | Bank payload decoder in firmware (00 §6, checks of 00 §4 step 5); `wallet.begin_bank` with the same approval screen showing "via Capital One" | C | P1 |
| PA6 | Bank rail in the app: get `account_id` from `GET /balance/:pubkey`, build payload, sign, `POST /bank/authorize {payload, sig, payer_pubkey, req, proof_sig}`, show result, send RESULT | Lua | P1 |
| PA7 | Approval states exactly per the 00 §4 table (green / amber / red incl. MISMATCH, bad proof, clock not set) | C | P0 |
| PA8 | On every red outcome or CANCEL: send RESULT status=1 to the payee and `POST /feed/event {payer_pubkey, req, reason}` so the dashboard shows it as blocked | Lua | P0 |
| PA9 | Spending cap: above `SPEND_CAP` a second confirmation screen | C | P2 |
| PA10 | Remote-payment behaviour per U6 (Solana amber warn or block; bank always blocked) | C + Lua | P1 |

### 2.1 Routed payments — 🆕 Routing
Built only **after the direct-payment gate passes**. Direct payment (PA1–PA10) stays the fallback demo. Contract: 00 §4.1 and §9.

| ID | Item | Layer | Priority |
| --- | --- | --- | --- |
| PA11 | Pay app lists forwarded REQs (`REQ_FWD`) as "Name · N relays away"; choosing one runs the routed flow | Lua | P1 |
| PA12 | `wallet.new_route()`, broadcast RREQ, collect RREPs for `ROUTE_COLLECT_MS`, CHAL/PROOF with each first hop, rank with `wallet.check_route()` | Lua | P1 |
| PA13 | Routed decoder (00 §4.1 Q6): exactly one payment leg plus one fee leg per relay, each fee to that relay's attested `solana_ata` for its quoted fee | C | P1 |
| PA14 | Route verification in C (00 §4.1 Q1–Q5): every compact record (`registry-c:`), the `route-att:` chain, next-hop proofs, destination REQ + `dest_proof`, gateway `route-quote:` freshness, fee caps, and enforcing the cheapest valid quote | C | P1 |
| PA15 | Routed approval screen: route line `via N relays ✓ · fees X`, total line, hop detail view on `right`, routed states (green "via N relays", amber "a hop was slow", red "unverified hop" / "broken chain" / "fee mismatch" / "stale quote"). Second confirmation if fees > `ROUTE_FEE_AUTO_CAP` | C | P1 |
| PA16 | `wallet.begin_route_solana`; send RPAY (FRAG) to the first hop; wait for the routed RESULT | Lua | P1 |
| PA17 | `wallet.begin_route_bank`: one SELECT signs the bank payload (with `route_id`, `route_fee_total`) **and** the HACK fee transaction; send both in RPAY | C + Lua | P2 |
| PA18 | FRAG send/reassemble for RREP and RPAY on the payer (shared code with R's relay app, P2-R) | C or Lua | P1 |
| PA19 | Payer's own HACK source ATA and SOL balance cached on the badge at setup, so an offline payer can build the transaction | Lua | P1 |

## 3. Technical notes

### 3.1 Building the Solana transaction on the badge
Two options; pick by hour 9:
1. **Badge builds it (preferred for the pitch: no laptop in the path).** Lua or a small C helper serializes a legacy message.
   - Header `[1,0,3]`.
   - Accounts in order: payer (signer, writable) · source ATA, destination ATA (writable) · mint, token program, memo program (read-only).
   - Use R's builder output as a byte-for-byte reference test.
   - The payer's own source ATA comes from `badges.json` via the backend, or from config in the app.
   - Needs base58 decoding of the blockhash and base64 for `sendTransaction`. Expose the firmware's existing base58 code to Lua rather than writing it in Lua.
2. **Backend builds it (fallback).** Add `POST /solana/build {payer, req}` to 00 §8.1. It returns unsigned message bytes. The badge still decodes and checks them itself, so the guarantee holds.

Send with `{"encoding":"base64"}`. Poll `getSignatureStatuses` once per tick, about every 400 ms, at `confirmed` commitment, giving up after 20 s. Every HTTP call runs in its own callback, so the 12 s cap is never reached.

### 3.2 Bank payload decoder (C)
- Parse `key=value\n` in the fixed order of 00 §6. Reject unknown keys, missing keys, reordered keys and extra whitespace.
- Display: `PAY $40.00 via Capital One` / payee name + state / `purchase` or `transfer`.
- Run the checks in 00 §4 step 5: `payee_ref`, `payee_name`, `action` vs `kind`, `req_id`, `amount_cents` vs REQ, and `proof_nonce` vs the stored nonce.
- Sign `bank-auth:` + payload only after SELECT.

### 3.3 Screen states — ✏️ Changed (routing)
Defined once, in 00 §4 (direct) and 00 §4.1 (routed). Do not redefine them here. Note that the bank rail requires `present`, because the backend verifies the proof. On a route, that means every hop must be `present`.

### 3.4 Building the routed transaction — 🆕 Routing
- **Accounts, in order:**
  - payer (signer, writable);
  - source ATA, destination ATA, relay 1 ATA … relay K ATA (writable);
  - mint, token program, memo program (read-only).
  - Header `[1,0,3]`, unchanged.
- **Instructions:** the payment `transferChecked` first, then one fee `transferChecked` per relay in route order, then the memo hex(route_id). R's builder provides byte-for-byte reference vectors for K = 1, 2, 3.
- **Blockhash:** use the one from the quote (`route-quote:`), not one fetched by the payer, since the payer may be offline.
- **Size:** K = 2 → 363 B message, 438 B RPAY (3 FRAG frames). K = 3 → 412 B (00 §9.7).
- **Bank fee transaction:** the same layout without the payment leg and without the destination ATA.
- **Timing:** the routed approval times out after `ROUTE_APPROVAL_TIMEOUT_S` (30 s). After a timeout, start a new route: don't retry with the old quote.

## 4. Dependencies
- P1-A wallet core; R's handshake responder (P2-R) for PROOF; U's `/registry`, `/balance`, `/bank/authorize`, `/feed/*` (P2-U).

## 5. Done when — ✏️ Changed (routing)
- [ ] Judge-style run on the Solana rail: request → green screen → SELECT → confirmed on the feed in under 5 s, on 10 consecutive runs.
- [ ] Same on the bank rail with a Nessie purchase.
- [ ] Impostor badge (no attestation) → red, SELECT disabled, a blocked row appears on the feed.
- [ ] A Lua build that lies in `ctx` (another payee's record, an edited REQ, no PROOF) never produces a green screen.
- [ ] A transaction whose amount or destination differs from the REQ → red MISMATCH showing both values.
- [ ] Revoked payee → red on the next payment attempt.
- [ ] 30 minutes of Pay-app use without a watchdog reset.
- [ ] 🆕 2-hop Solana run (judge → relay A → relay B → merchant): green "via 2 relays ✓ · fees 0.02". SELECT puts the payment leg **and** both fee legs in one confirmed transaction, and the feed shows the route. Do it 5 times in a row.
- [ ] 🆕 Each tampered quote is refused with its named red state:
  - a relay without a relay attestation → "unverified hop";
  - an altered fee → "fee mismatch";
  - a swapped next-hop key → "broken chain";
  - an old quote → "stale quote".
- [ ] 🆕 Bytes paying a valid but more expensive quote → red MISMATCH "not the cheapest verified route".
- [ ] 🆕 The direct flow still passes every check above with routing code present (fallback intact).

## 6. Pending decisions (U finalizes in U6)
- List vs auto pop-up for nearby requests.
- Remote Solana payments: warn or block.
- Whether PA9 spending cap ships.
- Demo beat 5 format (team decision, later).
- 🆕 Routing: hop limit, fee defaults, auto-approve cap (U6 routing questions, P1-U §5). Whether PA17 (bank over the route) ships.
