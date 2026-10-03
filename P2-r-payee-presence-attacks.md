# P2-R — Payee Flow, Presence Handshake, and Attack Console

Owner: **R** · Part 2 (≈ hours 8–20) · Status: **draft — finalize after U6 (app decision doc)**
Reads: `00-interfaces.md` (§3, §5, §8) · Builds on: `P1-R` (R4 RTT numbers, R5 verifier) · Parent: `PRD-verified-payment-key.md`

## 1. Why this exists
Every verified payment starts with a payee asking for money and proving it is physically there. R owns that side, plus the tools that let judges watch attacks fail.

## 2. Scope

| ID | Item | Layer | Priority |
| --- | --- | --- | --- |
| PR1 | Request / Merchant app: enter amount (or pick preset item per U6), choose rail (per U6), broadcast signed REQ (`wallet.sign_request`) every ~1 s until answered or expired | Lua | P0 |
| PR2 | Handshake responder: on CHAL for our `req_id`, reply PROOF via `wallet.sign_proof` immediately (inside one callback; budget the 250 ms) | Lua | P0 |
| PR3 | RESULT handling: show "PAID $1 ✓" / "Rejected" and flash LEDs | Lua | P0 |
| PR4 | Impostor mode: a build flag or menu that makes a badge broadcast REQs with the merchant's display name but its own unattested key | Lua | P0 |
| PR5 | Attack console (laptop, from R5 + R3): tamper relay (change amount/recipient in a payload in transit), replay an old REQ/PROOF, unsigned direct Nessie call against the backend | Python | P1 |
| PR6 | Merchant price list (if U6 says yes) | Lua | P2 |
| PR7 | Hand off RTT data → final `PRESENCE_DEADLINE_MS` with A | — | P0 |

## 3. Technical notes

### 3.1 Timing budget
PROOF must leave within one Lua callback. Do the signing call directly in the ESP-NOW receive handler if Solana OS allows blocking there (SE050 sign ≈ 261 ms needs the deadline extension A provides); otherwise queue it and process on the next tick, and measure the added latency with R4's harness.

### 3.2 REQ rebroadcast
Broadcast REQ periodically (≈1 Hz) so a payer who opens Pay late still sees it. Stop on RESULT, CANCEL or expiry. Use a fresh `req_id` per request; never reuse.

### 3.3 Attack console scenarios (each must show on the feed as blocked)
| Scenario | Method | Expected |
| --- | --- | --- |
| Impostor merchant | PR4 badge | Payer red "unverified" |
| Replayed PROOF | Re-send an old PROOF for a new CHAL | `check_proof` → bad_sig |
| Tampered amount (bank) | Laptop sits between badge and backend (proxy) and edits `amount_cents` | Backend signature check fails → refused |
| Tampered amount (Solana, backend-built fallback only) | Laptop returns a message with a different amount | Badge displays the true decoded amount |
| Direct Nessie call | `POST /bank/authorize` without valid badge sig, or try calling purchase endpoints via backend | Refused, logged |
| Revoked payee | U revokes on dashboard mid-demo | Payer red "revoked" |

For the tamper proxy: point the payer badge's backend URL at the laptop proxy (mitmproxy or a 20-line Flask relay) during the attack beat.

## 4. Dependencies
- P1-A `sign_request`, `sign_proof`; P2-A payer handshake for end-to-end tests; U's registry, feed and revoke (P2-U).

## 5. Done when
- [ ] Merchant badge request → judge badge green → paid → merchant badge shows PAID within 5 s (Solana) on 10 consecutive runs.
- [ ] Every scenario in §3.3 produces the expected result on both the badge and the feed.
- [ ] Deadline value fixed and documented in 00.

## 6. Pending decisions (U finalizes in U6)
- Typed amount vs preset price list.
- Who picks the rail.
- Which physical badge is merchant vs impostor in the demo.