# P2-R — Payee Flow, Presence Handshake, and Attack Console

Owner: **R** · Part 2 (≈ hours 8–20) · Status: **draft — finalize after U6 (app decision doc)**
Reads: `00-Interfaces.md` (§3, §5, §8) · Builds on: `P1-r` (R4 timing, R5 verifier) · Parent: `Prd-verified-payment-key.md`

## 1. Why this exists
Every verified payment starts with a payee asking for money and proving it is physically there. R owns that side, plus the tools that let judges watch attacks fail.

## 2. Scope

| ID | Item | Layer | Priority |
| --- | --- | --- | --- |
| PR1 | Request / Merchant app: enter amount (or pick preset item per U6) and rail; build the REQ frame, sign it **once** with `wallet.sign_request`, rebroadcast the same bytes ≈1 Hz until RESULT, local CANCEL, or expiry | Lua | P0 |
| PR2 | Handshake responder: on CHAL for our `req_id`, add the payer as an ESP-NOW peer and unicast PROOF via `wallet.sign_proof` | Lua | P0 |
| PR3 | RESULT handling: on status=0, confirm the tx signature over RPC (Solana) or the backend (bank) before showing "PAID ✓" and flashing `badge.led`; otherwise "Rejected" | Lua | P0 |
| PR4 | Impostor mode: a menu option that broadcasts REQs with the merchant's display name but its own unattested key (expected: red "unverified") | Lua | P0 |
| PR5 | Attack scripts (Python, from R5 + R3): tamper proxy, replay, unsigned backend call. Wired into the dashboard Attack page or standalone, per U6 | Python | P1 |
| PR6 | Merchant price list (if U6 says yes) | Lua | P2 |
| PR7 | Final `PRESENCE_DEADLINE_MS` with A from R4 numbers; written into 00 §4 | — | P0 |

## 3. Technical notes

### 3.1 Timing budget
The PROOF must arrive within `PRESENCE_DEADLINE_MS`, measured on the payer from `new_nonce`. Signing on the SE050 alone may take ~261 ms (unmeasured), which is why the deadline depends on where the key lives (00 §4). Check in the Solana OS source whether the ESP-NOW receive callback runs on the Lua task.
- **If it does:** sign directly in the callback, since the signing binding extends the Lua deadline.
- **If not:** queue the CHAL and sign on the next tick. Measure the added latency with the R4 harness.

### 3.2 REQ rebroadcast
Broadcast the REQ periodically (≈1 Hz) so a payer who opens Pay late still sees it. Stop on RESULT, local CANCEL or expiry. Use a fresh `req_id` for each new request and never reuse one. If two payers send CHAL for the same `req_id`, answer both; whoever pays first wins, and the merchant ends the request on the first confirmed RESULT.

### 3.3 Attack scenarios (each must show on the badge, and on the feed as blocked)
| Scenario | Method | Expected | How it reaches the feed |
| --- | --- | --- | --- |
| Impostor merchant | PR4 badge | Payer red "unverified" | Payer `POST /feed/event` |
| Impostor using the real merchant's pubkey in its REQ | PR4 variant | REQ sig fails against the record → red | Payer `/feed/event` |
| Replayed PROOF | A spare badge re-sends an old PROOF for a new CHAL | `check_proof` → `bad_sig` → red "bad proof" | Payer `/feed/event` |
| Replayed REQ, payee absent | Re-broadcast an old REQ from another badge | No PROOF → amber "not present" (bank: blocked) | Payer `/feed/event` if cancelled |
| Tampered bank payload | Laptop proxy between payer badge and backend edits `amount_cents` after signing | Backend signature check fails → refused | Backend writes blocked row |
| Tampered Solana tx (compromised app) | Laptop serves a tx with different amount/recipient via `/badge/pending` (existing Attack page) | Firmware red MISMATCH vs REQ/record | Payer `/feed/event` |
| Tampered registry response | Proxy edits the `/registry` JSON | Issuer sig fails → red | Payer `/feed/event` |
| Unsigned backend call | `POST /bank/authorize` without a valid badge sig | Refused, logged. Pitch it honestly as "the bank's API refuses unsigned payments", not as calling Nessie directly | Backend writes blocked row |
| Revoked payee | U revokes on the dashboard mid-demo | Payer red "revoked" | Payer `/feed/event` |

For the tamper proxy, point the payer badge's backend IP at the laptop proxy (mitmproxy or a 20-line relay) during the attack beat. Settings, or a Pay-app config, must allow changing that IP (coordinate with A/U).

## 4. Dependencies
- P1-A `sign_request`, `sign_proof`; P2-A payer handshake and `/feed/event` posts; U's registry, feed, revoke and Attack page (P2-U).

## 5. Done when
- [ ] Merchant badge request → judge badge green → paid → merchant badge shows PAID (verified on chain) within 5 s on the Solana rail, on 10 consecutive runs. Approval time doesn't count; switch the RPC to a provider if public devnet rate-limits.
- [ ] Every scenario in §3.3 produces the expected result on both the badge and the feed.
- [ ] Deadline value fixed and documented in 00.

## 6. Pending decisions (U finalizes in U6)
- Typed amount vs preset price list.
- Attack console ownership (existing web Attack page vs standalone).
- Which physical badge is merchant vs impostor in the demo.
- Demo beat 5 format (team decision, later).
