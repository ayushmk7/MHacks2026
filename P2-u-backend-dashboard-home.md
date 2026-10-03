# P2-U — Authorization Backend, Dashboard, Home/History Apps, and Demo

Owner: **U** · Part 2 (≈ hours 8–20, then demo prep) · Status: **draft — finalize with U6**
Reads: `00-interfaces.md` (§6, §7, §8) · Builds on: `P1-U` · Parent: `PRD-verified-payment-key.md`

## 1. Why this exists
U turns the skeleton into the system judges see: the backend that is Capital One's authorization layer in front of Nessie, the on-chain audit trail, the dashboard on the screen behind the judges, the badge home screens, and the demo itself.

## 2. Scope

| ID | Item | Layer | Priority |
| --- | --- | --- | --- |
| PU1 | `/bank/authorize` full check chain (§3.1) + Nessie purchase/transfer calls | Backend | P0 |
| PU2 | Audit memos: write `appr:v1:<rail>:<sha256(payload)>` to devnet for every approval on both rails (issuer key pays fees) | Backend | P1 |
| PU3 | Registry live revocation: `/registry/revoke` closes the attestation and flips the served record to `status=revoked` | Backend | P0 |
| PU4 | Dashboard: Live feed, Badges, Issuer, Attack views (parent PRD Dashboard section) | Web | P1 |
| PU5 | Home app on the badge: USDC balance (RPC), Nessie balance (`/balance/:pubkey`), name, key location | Lua | P1 |
| PU6 | History app (last 10, both rails) | Lua | P2 |
| PU7 | Settings app: address, key location, spend cap display | Lua | P2 |
| PU8 | Read-only feed on the .tech domain | Web | P2 |
| PU9 | Demo script, rehearsal, backup screen recording, Devpost write-up | — | P0 (hours 20–24) |

## 3. Technical notes

### 3.1 `/bank/authorize` check chain (any failure → `{ok:false, reason}` and a "blocked" feed row)
1. `payer_pubkey` is enrolled; resolve its Nessie account.
2. Ed25519 verify `sig` over `bank-auth:` + `payload` with `payer_pubkey` (R's `vk_verify.py`).
3. Parse payload in fixed order; `issued_at` within 60 s; `proof_nonce` unused (insert into `nonces`).
4. Load payee record by `payee_pubkey`; status active, not expired; `payload.payee_ref == record.bank_ref_hash`; `payload.attestation` matches.
5. Verify `proof_sig` over `pay-proof:` ‖ req_id ‖ nonce ‖ payer_pubkey with the payee's attested `device_pubkey`.
6. `from_acct` equals the enrolled account; `amount_cents` > 0; resolve plaintext Nessie target from DB.
7. Call Nessie (purchase for merchants, transfer for persons) with exactly the payload's amount.
8. Write memo (PU2), insert `approvals` row, return `{ok, nessie_id, memo_sig}`.

Never expose a Nessie money-movement route that skips steps 1–6.

### 3.2 Feed
Single `approvals` table drives the dashboard: time, rail, payer name, payee name, verified state, amount, status (approved / blocked + reason), links (Solana explorer `https://explorer.solana.com/tx/<sig>?cluster=devnet`, Nessie id, memo).
Push to the dashboard with polling every 1–2 s (simplest) or server-sent events.

### 3.3 Dashboard
- Stack: one page (React/Vite or plain HTML + fetch) served by the backend.
- Issuer and Attack views on `localhost` only; feed view also published read-only.
- Large type; readable from ~2 m.

## 4. Demo (PU9) — from the parent PRD Demo script
1. Hook · 2. Crypto payment · 3. Bank payment · 4. Impostor · 5. Tampered relay + direct Nessie call · 6. Revocation · 7. Close.
Rehearse twice with someone who hasn't seen it holding the payer badge. Record beats 2–6 as backup.

## 5. Dependencies
- P2-A payer flow posts to `/bank/authorize` and `/feed/solana`; P2-R attack console targets the backend; A's pinned issuer key matches your issuer.

## 6. Done when
- [ ] Bank payment from a judge badge creates a Nessie purchase, a devnet memo and a green feed row.
- [ ] Every attack scenario in P2-R §3.3 appears on the feed as blocked with a readable reason.
- [ ] Revocation flips a payee to red on the judge badge within one refresh.
- [ ] Full demo rehearsed end to end twice; backup recording saved.

## 7. Pending decisions (U6)
- Which of Home / History / Settings ship.
- Whether audit memos cover Solana-rail payments too (adds one transaction per payment) or bank only.
- Polling vs SSE for the feed.