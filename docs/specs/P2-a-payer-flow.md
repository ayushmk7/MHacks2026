# P2-A — Payer Flow: Pay App, Bank Signing, Approval States

Owner: **A** · Part 2 (≈ hours 8–20) · Status: **draft — finalize after U6 (app decision doc)**
Reads: [`00-Interfaces.md`](00-Interfaces.md) (§4, §5, §6, §8.1) · Builds on: [P1-A](P1-a-firmware-wallet-core.md) · Parent: [`Prd-verified-payment-key.md`](Prd-verified-payment-key.md)

## 1. Why this exists
This is the judge's side of every demo beat. They hold the payer badge, see a request, read the approval screen, and press SELECT or CANCEL. It must be impossible to misread and impossible to fool, even if the Lua Pay app itself is buggy or malicious. That is why every trust decision sits in the firmware (00 §4).

## 2. Scope

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

### 3.3 Screen states
Defined once, in 00 §4. Do not redefine them here. Note that the bank rail requires `present`, because the backend verifies the proof.

## 4. Dependencies
- P1-A wallet core; R's handshake responder (P2-R) for PROOF; U's `/registry`, `/balance`, `/bank/authorize`, `/feed/*` (P2-U).

## 5. Done when
- [ ] Judge-style run on the Solana rail: request → green screen → SELECT → confirmed on the feed in under 5 s, on 10 consecutive runs.
- [ ] Same on the bank rail with a Nessie purchase.
- [ ] Impostor badge (no attestation) → red, SELECT disabled, a blocked row appears on the feed.
- [ ] A Lua build that lies in `ctx` (another payee's record, an edited REQ, no PROOF) never produces a green screen.
- [ ] A transaction whose amount or destination differs from the REQ → red MISMATCH showing both values.
- [ ] Revoked payee → red on the next payment attempt.
- [ ] 30 minutes of Pay-app use without a watchdog reset.

## 6. Pending decisions (U finalizes in U6)
- List vs auto pop-up for nearby requests.
- Remote Solana payments: warn or block.
- Whether PA9 spending cap ships.
- Demo beat 5 format (team decision, later).
