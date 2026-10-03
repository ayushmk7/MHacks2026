# P2-A — Payer Flow: Pay App, Bank Signing, Approval States

Owner: **A** · Part 2 (≈ hours 8–20) · Status: **draft — finalize after U6 (app decision doc)**
Reads: `00-interfaces.md` (§4, §5, §6, §8) · Builds on: `P1-A` · Parent: `PRD-verified-payment-key.md`

## 1. Why this exists
This is the judge's side of every demo beat: they hold the payer badge, see a request, read the approval screen, and press SELECT or CANCEL. It must be impossible to misread and impossible to fool.

## 2. Scope

| ID | Item | Layer | Priority |
| --- | --- | --- | --- |
| PA1 | Pay app: list or pop-up of nearby REQs (per U6), select one | Lua | P0 |
| PA2 | Run payer side of the handshake: send CHAL with `wallet.new_nonce()`, receive PROOF, call `wallet.check_proof` | Lua | P0 |
| PA3 | Fetch registry record for `payee_pubkey` (`GET /registry/:pubkey`), pass to the approval via `ctx` | Lua | P0 |
| PA4 | Solana rail: fetch blockhash, build `transferChecked` (+ memo with `req_id`) to the payee's ATA, call `wallet.sign_solana`, `sendTransaction`, poll status, send RESULT, post `/feed/solana` | Lua | P0 |
| PA5 | Bank payload decoder in firmware (00 §6); `wallet.sign_bank` with the same approval screen showing "via Capital One" | C | P1 |
| PA6 | Bank rail in the app: build payload, sign, `POST /bank/authorize`, show result, send RESULT | Lua | P1 |
| PA7 | Approval states: green / amber / red exactly per 00 §4, including **MISMATCH** when decoded recipient ≠ record | C | P0 |
| PA8 | Spending cap: above `SPEND_CAP` a second confirmation screen | C | P2 |
| PA9 | Remote-payment behaviour per U6 (amber warning or block) | C + Lua | P1 |

## 3. Technical notes

### 3.1 Building the Solana transaction on the badge
Two options; pick by hour 9:
1. **Badge builds it (preferred for the pitch: no laptop in the path).** Lua or a small C helper serializes a legacy message: header `[1,0,3]` (payer signs; token program, mint, memo read-only unsigned) — compute exact counts from the account list; accounts ordered signer-writable, non-signer-writable, readonly. Use R's laptop builder output as a byte-for-byte reference test.
2. **Backend builds it (fallback).** `POST /solana/build {payer, payee, amount, req_id}` returns the unsigned message bytes; the badge still decodes and displays it itself, so the guarantee holds. Add the endpoint to 00 §8 if used.

Use base64 for `sendTransaction` (`{"encoding":"base64"}`); poll `getSignatureStatuses` up to ~20 s.

### 3.2 Bank payload decoder (C)
- Parse `key=value\n` in the fixed order of 00 §6; reject unknown keys, missing keys, reordered keys, or extra whitespace.
- Display: `PAY $40.00 via Capital One` / payee name + state / `purchase` or `transfer`.
- Compare `payee_ref` to `record.bank_ref_hash` (MISMATCH if different) and `proof_nonce` to the stored nonce from `wallet.new_nonce()`.
- Sign `bank-auth:` + payload only after SELECT.

### 3.3 Screen states (00 §4)
| Condition | Colour | SELECT |
| --- | --- | --- |
| record ok + proof present + no mismatch | green ✓ ● | enabled |
| record ok + proof late/absent | amber | enabled with warning (or disabled, per U6) |
| record missing / revoked / expired / bad issuer sig | red | disabled |
| decoded recipient ≠ record | red MISMATCH | disabled |
| undecodable bytes | red "cannot read this payment" | disabled |

## 4. Dependencies
- P1-A wallet core; R's handshake responder (P2-R) for PROOF; U's `/registry`, `/bank/authorize`, `/feed/solana` (P2-U).

## 5. Done when
- [ ] Judge-style run on the Solana rail: request → green screen → SELECT → confirmed on the feed in under 5 s.
- [ ] Same on the bank rail with a Nessie purchase.
- [ ] Impostor badge (no attestation) → red, SELECT disabled.
- [ ] R's attack console alters amount in transit → screen shows the altered amount (truth of the bytes) and MISMATCH or the true value; judge can cancel.
- [ ] Revoked payee → red within one registry refresh.

## 6. Pending decisions (U finalizes in U6)
- List vs auto pop-up for nearby requests.
- Who chooses the rail (payee in REQ or payer on screen).
- Remote payments: warn or block.
- Whether PA8 spending cap ships.