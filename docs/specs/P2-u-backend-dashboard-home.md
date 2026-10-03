# P2-U — Authorization Backend, Dashboard, Home/History Apps, and Demo

Owner: **U** · Part 2 (≈ hours 8–20, then demo prep) · Status: **draft — finalize with U6**
Reads: [`00-Interfaces.md`](00-Interfaces.md) (§6, §7, §8) · Builds on: [P1-U](P1-u-economies-registry-app-spec.md) · Parent: [`Prd-verified-payment-key.md`](Prd-verified-payment-key.md) · Existing system: [repository overview](../../README.md) and [dashboard docs](../dashboard/)

## 1. Why this exists
U turns the existing server and dashboard into the system judges see:
- the backend that acts as Capital One's authorization layer in front of Nessie;
- the on-chain audit trail;
- the dashboard on the screen behind the judges;
- the badge home screens;
- the demo itself.

All of it extends the existing Node server and React dashboard; nothing is rewritten.

## 2. Scope

| ID | Item | Layer | Priority |
| --- | --- | --- | --- |
| PU1 | `/bank/authorize` full check chain (§3.1) + Nessie purchase/transfer calls | Backend | P0 |
| PU2 | Audit memos: write `appr:v1:<rail>:<sha256(payload)>` to devnet for approvals (bank always; Solana per U6), issuer key pays fees, sent asynchronously so payments don't wait on it | Backend | P1 |
| PU3 | Live revocation: the existing revoke closes the attestation, and `/registry/:pubkey` then serves `status=revoked` | Backend | P0 |
| PU4 | Dashboard: extend existing Feed (rails, blocked rows, reasons), Badges (Nessie balance, enrollment), Registry → "Issuer (Capital One)" (kind, Nessie ref, enroll), Attack (scenarios per U6) | Web | P1 |
| PU5 | Home app on the badge: HACK balance (RPC), Nessie balance (`/balance/:pubkey`), name, key location | Lua | P1 |
| PU6 | History app (last 10, both rails) | Lua | P2 |
| PU7 | Settings app: address, key location, backend IP (editable), spend cap display | Lua | P2 |
| PU9 | Demo script, rehearsal, backup screen recording, Devpost write-up | — | P0 (hours 20–24) |

## 3. Technical notes

### 3.1 `/bank/authorize` check chain (any failure → `{ok:false, reason}` and a blocked `approvals` row)
1. `payer_pubkey` is enrolled; resolve its Nessie account.
2. Ed25519 verify `sig` over `bank-auth:` + `payload` with `payer_pubkey` (R's `verify.js`).
3. Parse the payload in fixed order and check:
   - `v=1`, `rail=nessie`, `currency=USD`;
   - `issued_at` within `APPROVAL_TIMEOUT_S` + 30 s of now.
4. Parse `req`: verify its `pay-req:` sig with `req.payee_pubkey`, and check that it hasn't expired. Then:
   - `payload.req_id == req.req_id`;
   - `payload.amount_cents == req.amount`;
   - `req.rail == 2`.
5. Load the payee by `req.payee_pubkey` and check:
   - status active and not expired;
   - `payload.payee_ref == record.bank_ref_hash` and `payload.attestation` matches;
   - `payload.payee_name == record.display_name`;
   - `action` matches `kind` (purchase ↔ merchant, transfer ↔ person).
6. Verify `proof_sig` over `pay-proof:` ‖ req_id ‖ proof_nonce ‖ payer_pubkey with the payee's attested `device_pubkey`.
7. `from_acct` equals the enrolled account and `amount_cents` > 0. Insert `(payer, proof_nonce)` into `nonces`; a UNIQUE violation → `replay`.
8. Insert a `pending` approvals row, then call Nessie with exactly the payload's amount (converted to dollars). The purchase or transfer target is the plaintext id from the DB.
9. Mark the row approved, queue the memo (PU2) and return `{ok, nessie_id}`. The memo signature appears on the feed when it lands.

Never expose a Nessie money-movement route that skips steps 1–7.

### 3.2 Feed
- **Sources:** `/api/payments` combines chain-ingested HACK payments (existing) with `approvals` rows (bank payments, blocked rows from `/bank/authorize`, and badge-reported refusals from `/feed/event`).
- **Columns:** time, rail, payer name, payee name, verified state, amount, status (approved / blocked + reason), links.
  - Links: Solana explorer `https://explorer.solana.com/tx/<sig>?cluster=devnet`, Nessie id, memo.
  - Badge-reported rows are labelled "reported by badge".
- **Push:** keep the existing SSE (`/api/events`).

### 3.3 Dashboard
- Extend the existing `web/` pages; keep the design system.
- The whole dashboard runs on `localhost` only; nothing is published.
- Large type, readable from ~2 m.
- In LIVE mode, judges must never see DEMO fixtures. Start the demo in LIVE.

## 4. Demo (PU9) — from the parent PRD Demo script
1. Hook · 2. Crypto payment · 3. Bank payment · 4. Impostor · 5. Tamper beat (format to be decided by the team) · 6. Revocation · 7. Close.
Rehearse twice with someone who hasn't seen it holding the payer badge. Record beats 2–6 as backup.

## 5. Dependencies
- P2-A posts to `/bank/authorize`, `/feed/solana` and `/feed/event`.
- P2-R's attack scripts target the backend.
- A's pinned issuer key matches `authority.json`.
- R's `verify.js` and vectors.

## 6. Done when
- [ ] Bank payment from a judge badge creates a Nessie purchase, a devnet memo and a green feed row.
- [ ] Each step of the §3.1 chain rejects a request crafted to fail only that step (R's vectors).
- [ ] Every attack scenario in P2-R §3.3 appears on the feed as blocked with a readable reason.
- [ ] Revocation flips a payee to red on the judge badge on the next payment attempt.
- [ ] Admin routes unreachable from another device on the hotspot.
- [ ] Full demo rehearsed end to end twice; backup recording saved.

## 7. Pending decisions (U6)
- Which of Home / History / Settings ship.
- Whether audit memos cover Solana-rail payments too (adds one transaction per payment) or bank only.
