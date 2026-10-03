# P1-U — Economies, Registry, Badge Routes on the Existing Server, and the App Decision

Owner: **U** · Part 1 (≈ hours 0–8) · Gate contribution: funded accounts and a live registry for the first devnet payment
Reads: `00-Interfaces.md` (§0, §2, §6, §7, §8) · Parent: `Prd-verified-payment-key.md` · Existing system: `README.md`, `docs/ARCHITECTURE.md`, `docs/API.md`, `docs/BADGE-GAPS.md`

## 1. Why this exists

Part 1 has three jobs for U:
1. **Make money exist** on both rails: HACK on Solana devnet, and Nessie accounts and merchants.
2. **Make identity exist.** This means the issuer key, the on-chain payee registry, and badge-facing routes that serve issuer-signed registry records.
3. **Finalize what Part 2 builds.** Lock the app list and screen flows so P2-A, P2-R and P2-U can be finalized.

**Extend, don't rewrite.** The repo already has a Node server (`server/`), a TimescaleDB store, a React dashboard (`web/`) and `npm run devnet:setup`. Between them they already provide:
- SAS issue and revoke;
- a live HACK payment feed with SSE;
- an attack page;
- a badge listener on the hotspot (port 8788).

Everything below adds to that code.

## 2. Scope

| ID | Deliverable | Priority |
| --- | --- | --- |
| U1 | Issuer key = existing `server/.keys/authority.json`. Back it up; give its public key to A for pinning. Fund it with devnet SOL (it has 0; the faucet rate-limited this laptop, so use https://faucet.solana.com or another machine) | P0 |
| U2 | HACK mint at **2 decimals** via `npm run devnet:setup` (confirm `HACK_DECIMALS=2` before the first run — the mint can't change later). Replace stand-in pubkeys in `server/config/badges.json` with R1's real ones, re-run setup to fund SOL + ATA + HACK for every badge | P0 |
| U3 | Nessie: API key in `.env` (`NESSIE_KEY`); `scripts/seed-nessie.mjs` creates 1 customer + checking account per badge holder, 1 merchant ("MHacks Merch"), one test purchase and one test transfer; writes IDs to `server/config/nessie.json` (gitignored if it holds anything sensitive) | P0 |
| U4 | Badge listener routes (00 §8.1): `/health`, `/registry/:pubkey`, `/balance/:pubkey`, `/feed/solana`, `/feed/event`; `/bank/authorize` returning `{ok:false, reason:"not_implemented"}` until P2-U. Admin `/api/enroll`. Badge routes on 8788 only, admin on 127.0.0.1:8787 only | P0 |
| U5 | Registry: change the SAS schema from `badge-identity {name}` to `payee_v1` (00 §7 fields) and credential name to "MHacks Verified Payees" (`.env` `SAS_*`); enforce expiry; compute `solana_ata`; serve issuer-signed records; attest the merchant badge (fallback: issuer-signed records from the DB only, `attestation=none`) | P1 |
| U6 | **App decision doc** (§5) finalized and shared; U then updates P2-A, P2-R, P2-U from it (budget 1 h, hours 6–7) | P0 by hour 6 |
| U7 | Dashboard: extend existing pages — Registry page gets `kind` + Nessie ref + Enroll; Feed gets rail, blocked rows and reasons | P1 |

## 3. Economies

### 3.1 Solana rail (HACK)
- **Mint.** `npm run devnet:setup` creates the HACK mint with the authority as mint authority. It also writes `HACK_MINT` into `.env`, gives every configured badge 0.05 SOL, a token account and 1000 HACK, and is idempotent (`README.md`).
- **Decimals.** 2, so 1 HACK displays like $1.00 and REQ amounts use the same minor unit as cents.
- **Order.** Real badge pubkeys in `badges.json` → `devnet:setup` → restart the server. Every address lives in `.env` and `badges.json`; there is no separate addresses file.
- **Pitch line if asked "why not USDC?":** "Devnet USDC is faucet-limited. HACK is a stand-in stablecoin; the firmware pins one mint, so swapping to USDC is a config change."

### 3.2 Bank rail (Nessie)
- Get a key at https://prod.nessieisreal.com/; calls go to `https://prod-api.nessieisreal.com/...?key=KEY`.
- Check the current request fields in the docs. Purchases historically take `merchant_id`, `medium`, `purchase_date`, `amount` and `description`; transfers take `medium`, `payee_id`, `amount`, `transaction_date` and `description`.
  - `amount` is in **dollars**, so convert from cents.
  - Merchants and customers may need address/geocode objects.
  - Check whether purchases sit as "pending" and whether balances update immediately.
- Keep the API key on the laptop only.

## 4. Registry and backend

### 4.1 Issuer key
`server/.keys/authority.json` already exists (pubkey shown by `npm run devnet:setup` and `/api/status`). It is the SAS credential authority, signs registry records (`registry:` prefix) and pays memo fees. Its public key is pinned in firmware (A9); if it changes, every badge must be reflashed. Copy the file somewhere safe now.

### 4.2 On-chain registry (SAS)
- Credential: "MHacks Verified Payees", authority = issuer. Schema `payee_v1`: `display_name, device_pubkey, kind, solana_wallet, bank_ref_hash` + attestation-level `expiry` (parent PRD Data model).
- SAS schemas cannot be edited after creation — get the layout right the first time (nothing is on chain yet).
- `bank_ref_hash = sha256(salt ‖ nessie_id)` with a **random 16-byte salt per payee**; `nessie_id` = merchant id for `kind=merchant`, account id for `kind=person`. Store salt, kind and plaintext id only in the backend DB.
- Revocation = close the attestation (existing `revoke`). Re-issue re-creates the same PDA, so records always carry a fresh `issued_at`.
- Docs: https://solana.com/docs/tools/attestations · https://attest.solana.com/. Time-box the schema change to 2 h.

### 4.3 What the badge actually verifies
Badges do **not** read SAS directly. The backend reads the attestation (or its own DB in fallback mode), builds the canonical record (00 §7, including `solana_ata`), signs `registry:` + record with the issuer key, and serves it. Firmware checks the issuer signature, status, expiry and `issued_at` freshness (`RECORD_TTL_S` = 30 s; sign on every request, no caching).
- Fallback if SAS slips: skip on-chain attestations, keep issuer-signed records with `attestation=none`. Same demo, less Solana credit. Say so honestly if asked.

### 4.4 Server changes
- **Two listeners.** Badge routes go on the existing badge listener (`BADGE_LISTEN_HOST` = hotspot IP, port 8788). Admin routes stay on 127.0.0.1:8787 behind the existing Host/Origin checks. Never put admin routes on 8788.
- **Signature code.** Ed25519 verify/sign through R's `server/src/verify.js` (`node:crypto`, or tweetnacl).
- **New tables** in `db/schema.sql`, all idempotent:
  - `enrollments` (badge_pubkey, nessie_customer_id, nessie_account_id, enrolled_at)
  - `payees` (pubkey, kind, nessie_ref, salt, attestation)
  - `approvals` (time, rail, payer, payee, amount, status, reason, refs, memo_sig)
  - `nonces` (payer, nonce, req_id, UNIQUE)
- **Feed.** Extend the existing `/api/payments` to include `approvals` rows (bank and blocked) next to the chain-ingested HACK payments. Keep the existing SSE.
- **`/feed/solana`.** Don't trust the badge: confirm the transaction from chain data (the ingest already sees every HACK transfer) and match the amount and destination to the REQ.

## 5. App decision doc (U6) — the Part 2 blocker

Decide by hour 6 and share. The candidate list is already in the parent PRD (AP1–AP8). For each, decide **in / out / stretch**, the screen sequence, and the button map.

| App / flow | Candidate behaviour | Built by (Part 2) | Default |
| --- | --- | --- | --- |
| Home | Balances (HACK + Nessie), name, key location | U | In |
| Pay (payer) | Choose nearby request → approval → submit → result | A | In |
| Request / Merchant (payee) | Enter amount + rail → broadcast REQ → answer CHAL → show RESULT | R | In |
| Presence handshake | REQ / CHAL / PROOF on both sides | R (payee) + A (payer) | In |
| Bank submit | Signed payload → `/bank/authorize` | A (badge) + U (backend) | In |
| History | Last 10 payments, both rails | U | Stretch |
| Settings | Show pubkey/address, key location, spend cap, backend IP | U | Stretch |
| Merchant price list | Preset items ("Sticker 1.00") instead of typed amounts | R | Decide |
| Remote payment (no presence) | Solana: allowed with amber warning, or blocked. Bank: always blocked | A | Decide |
| Attack console | One owner: existing web Attack page (U) + R's scripts behind it, or R's standalone console | R + U | Decide |

Questions to settle in the doc:
1. Does the payer pick from a list of nearby requests, or does the strongest-signal request pop up automatically?
2. The payee sets the amount and rail in REQ. The parent's AP3 "amount via D-pad" applies only to the merchant entering its price. Confirm.
3. Remote (not present) Solana payments: allow with a warning, or block?
4. Typed amounts vs preset price list for the merchant?
5. Which badge plays which role in the demo? Current `badges.json`: 1 merchant, 2 impostor, 3 and 4 judges. There is no person payee, so transfers are tested by script only.
6. Do audit memos cover Solana-rail payments too, or the bank rail only?

Button map baseline (`badge.input`): up/down/left/right = navigate/adjust, `a` = SELECT/confirm, `b` = CANCEL/back, hold `b` 2 s = exit app. Use the silkscreen names in every hint.

## 6. Dependencies
- A: issuer pubkey pinned; HACK mint pinned.
- R: real pubkeys (R1); `verify.js` + vectors (R5); R2 hitting `/health`.

## 7. Done when
- [ ] Real pubkeys in `badges.json`; every badge funded with SOL + HACK and enrolled to a Nessie account; issuer key backed up.
- [ ] From a device on the hotspot, `GET /registry/<merchant pubkey>` returns a record that R's verifier accepts. A revoked key returns `status=revoked`, an expired one fails, and admin routes are unreachable.
- [ ] Attestations exist on devnet under `payee_v1` (or the fallback is chosen explicitly).
- [ ] App decision doc shared; P2 specs updated from it.

## 8. Risks
| Risk | Mitigation |
| --- | --- |
| Faucet limits | Fund the authority before the event from a second machine or provider faucet; HACK itself is unlimited |
| Nessie fields differ from memory | Seed script first thing; read docs |
| SAS schema change friction | 2 h time-box, then issuer-signed fallback |
| Hotspot IP changes | Settings shows/edits the backend IP on badges; static DHCP lease if the phone allows |
| App decisions slip | Defaults in the table above are the decision if nothing else is agreed by hour 6 |
