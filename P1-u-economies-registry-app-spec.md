# P1-U — Economies, Registry, Backend Skeleton, and the App Decision

Owner: **U** · Part 1 (≈ hours 0–8) · Gate contribution: funded accounts and a live registry for the first devnet payment
Reads: `00-interfaces.md` (§2, §6, §7, §8) · Parent: `PRD-verified-payment-key.md`

## 1. Why this exists

Part 1 has three jobs for U:
1. **Make money exist** on both rails (Solana devnet tokens, Nessie accounts and merchants).
2. **Make identity exist**: the issuer key, the on-chain payee registry, and a backend that serves issuer-signed registry records to badges.
3. **Finalize what Part 2 builds**: lock the app list and screen flows so P2-A, P2-R and P2-U can be finalized.

## 2. Scope

| ID | Deliverable | Priority |
| --- | --- | --- |
| U1 | Issuer keypair (laptop only, never on a badge); give the public key to A for pinning | P0 |
| U2 | Mint decision (USDC devnet vs team mint), token accounts (ATAs) for all 4 badges, devnet SOL + tokens funded | P0 |
| U3 | Nessie: API key; 1 customer + checking account per badge holder; 1 merchant ("MHacks Merch"); one test purchase and one test transfer made by script | P0 |
| U4 | Backend skeleton (FastAPI or Express) on the laptop: `/health`, `/registry/:pubkey`, `/registry/issue`, `/registry/revoke`, `/enroll`, `/balance/:pubkey`, `/feed`, `/feed/solana`; `/bank/authorize` returning a stub | P0 |
| U5 | Registry on-chain: SAS credential + `payee_v1` schema + attestations for the merchant and person badges (fallback: issuer-signed records only, see §4.3) | P1 |
| U6 | **App decision doc** (§5) finalized and shared — unblocks all Part 2 specs | P0 by hour 6 |
| U7 | Dashboard skeleton: feed table + issuer buttons (issue / revoke / enroll) | P1 |

## 3. Economies

### 3.1 Solana rail
| Option | Pros | Cons |
| --- | --- | --- |
| USDC devnet (`4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU`, faucet https://faucet.circle.com/) | Reads as dollars; real stablecoin story | Faucet gives limited amounts per request; verify mint |
| Team mint ("HACK", 6 decimals) via `spl-token create-token` | Unlimited supply, you control mint authority | Less "real"; judges ask why not USDC |

Steps: create or pick the mint → `spl-token create-account` for each badge pubkey (ATA) → airdrop devnet SOL to each badge for fees (faucet is rate-limited; do this before the event) → fund tokens. Record every address in `config/addresses.json` (shared).

### 3.2 Bank rail (Nessie)
- Get a key at https://prod.nessieisreal.com/; calls go to `https://prod-api.nessieisreal.com/...?key=KEY`.
- Script `seed_nessie.py`: create customers + checking accounts with starting balances; create merchant "MHacks Merch"; make one purchase and one transfer; print all IDs to `config/nessie.json`.
- Verify current request fields in the docs (purchases historically take `merchant_id`, `medium`, `purchase_date`, `amount`, `description`; transfers take `medium`, `payee_id`, `amount`, `transaction_date`, `description`).
- Keep the API key on the laptop only.

## 4. Registry and backend

### 4.1 Issuer key
`solana-keygen new -o issuer.json`. The public key is pinned in firmware (A9). If it changes, every badge must be reflashed — generate it once, early.

### 4.2 On-chain registry (SAS)
- Credential: "MHacks Verified Payees", authority = issuer.
- Schema `payee_v1`: `display_name, device_pubkey, kind, solana_wallet, bank_ref_hash, expiry` (parent PRD Data model).
- `bank_ref_hash = sha256(salt || nessie_id)`; store salt and plaintext Nessie id only in the backend DB.
- Revocation = close the attestation.
- Docs: https://solana.com/docs/tools/attestations · https://attest.solana.com/ (JS/Rust SDKs). Time-box SAS integration to 2 h.

### 4.3 What the badge actually verifies
Badges do **not** read SAS directly. The backend reads the attestation (or its own DB in fallback mode), builds the canonical record (00 §7), signs it with the issuer key, and serves it. Firmware checks the issuer signature, status, expiry and `issued_at` freshness.
- Fallback if SAS slips: skip on-chain attestations, keep issuer-signed records. Same demo, less Solana credit. Say so honestly if asked.
- If A chooses the ATA fallback (P1-A §4.3), add `solana_ata` to the record.

### 4.4 Backend skeleton
- Python FastAPI suggested (imports R's `vk_verify.py`); SQLite with tables `enrollments, payees, approvals, nonces` (parent PRD Data model).
- `/bank/authorize` returns `{ok:false, reason:"not_implemented"}` until P2-U.
- `/feed/solana` records badge-reported Solana payments and (P2) writes the audit memo.
- Bind `0.0.0.0:8080`; print the laptop's hotspot IP on start.

## 5. App decision doc (U6) — the Part 2 blocker

Decide by hour 6 and share. The candidate list is already in the parent PRD (AP1–AP8). For each, decide **in / out / stretch**, the screen sequence, and the button map.

| App / flow | Candidate behaviour | Built by (Part 2) | Default |
| --- | --- | --- | --- |
| Home | Balances (USDC + Nessie), name, key location | U | In |
| Pay (payer) | Choose nearby request → approval → submit → result | A | In |
| Request / Merchant (payee) | Enter amount + rail → broadcast REQ → answer CHAL → show RESULT | R | In |
| Presence handshake | REQ / CHAL / PROOF on both sides | R | In |
| Bank submit | Signed payload → `/bank/authorize` | A (badge) + U (backend) | In |
| History | Last 10 payments, both rails | U | Stretch |
| Settings | Show pubkey/address, key location, spend cap | U | Stretch |
| Merchant price list | Preset items ("Sticker $1") instead of typed amounts | R | Decide |
| Remote payment (no presence) | Allowed with amber warning, or blocked | A | Decide |

Questions to settle in the doc:
1. Does the payer pick from a list of nearby requests, or does the strongest-signal request pop up automatically?
2. Is the rail chosen by the payee (in REQ) or the payer (on the approval screen)?
3. Are remote (not present) payments allowed with a warning, or blocked?
4. Typed amounts vs preset price list for the merchant?
5. Which badge plays which role in the demo (2 judge payers, 1 merchant, 1 impostor)?

Button map baseline (6 buttons): D-pad up/down/left/right = navigate/adjust, SELECT = confirm, CANCEL = back. Confirm the silkscreen names on the badge and use them in every hint.

## 6. Dependencies
- A: issuer pubkey pinned; ATA option chosen.
- R: `vk_verify.py`; R2 hitting `/health`.

## 7. Done when
- [ ] `config/addresses.json` and `config/nessie.json` committed; every badge funded on both rails.
- [ ] `GET /registry/<merchant pubkey>` returns a record that R's verifier accepts; a revoked key returns `status=revoked`.
- [ ] Attestations exist on devnet (or the fallback is chosen explicitly).
- [ ] App decision doc shared; P2 specs updated from it.

## 8. Risks
| Risk | Mitigation |
| --- | --- |
| Faucet limits | Fund before the event; team mint fallback |
| Nessie fields differ from memory | Seed script first thing; read docs |
| SAS SDK friction | 2 h time-box, then issuer-signed fallback |
| App decisions slip | Defaults in the table above are the decision if nothing else is agreed by hour 6 |