# 00 — Shared Interfaces and Contracts

Owner: all three (A, R, U) · Status: agree at hour 0, then change only by telling everyone
Parent: [`Prd-verified-payment-key.md`](Prd-verified-payment-key.md) (product, threat model, demo)

Every other spec depends on this file. If a field, prefix or endpoint changes, change it **here first**, then in code.

## 0. Decisions already made

| Decision | Choice | Consequence |
| --- | --- | --- |
| Backend | **Extend the existing Node server** in `dashboard/server/` (see the [repository README](../../README.md) and [dashboard API docs](../dashboard/API.md)). No rewrite | New routes are added to `dashboard/server/src/http.js`; verification code is JavaScript; R keeps a Python twin for the attack console |
| Solana token | **HACK**, our own SPL mint, **2 decimals** | Created by `npm run devnet:setup`; address in `.env` `HACK_MINT`. 2 decimals = the same minor unit as cents on the bank rail |
| Signing model | **Asynchronous approval**; firmware owns REQ verification and presence state | Lua never passes trust decisions to the firmware; see §4 |
| Recipient check | Registry record carries **`solana_ata`** (computed by the backend, signed by the issuer) | Firmware compares bytes; no PDA math on the badge |
| Time | **SNTP** on the badge at boot | Needed for record expiry, REQ expiry and `issued_at` |
| Demo beat 5 format | Open — decide later | — |

---

## 1. Fixed facts about the platform

| Fact | Value | Source / note |
| --- | --- | --- |
| Badge MCU | ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB PSRAM) | https://solanadefcon.com/ |
| Display | 2.8" ILI9341, 320×240 canvas (landscape) | Solana OS README |
| Input | 6 buttons via TCA9534; Lua module `badge.input`, keys `"up" "down" "left" "right" "a" "b"` | Solana OS README. In these specs **SELECT = `a`, CANCEL = `b`**; confirm against the silkscreen |
| LEDs | 2 × WS2812B, Lua module `badge.led` (`set(i,r,g,b)`, `pulse`, `show`) | Solana OS README |
| Secure element | NXP SE050C2 on I²C | same |
| Firmware | Solana OS (C/C++ core, Lua 5.4 apps) | https://github.com/spacemandev-git/solana-defcon-badge-26 |
| Identity key | Ed25519, generated on first boot; SE050 if it answers, else NVS + TweetNaCl | Solana OS README; SE050 path untested on silicon |
| Lua callback budget | 250 ms; blocking bindings may extend it, **but never past 12 s per callback** | Solana OS README. Anything longer (approval, confirmation polling) must be asynchronous |
| Clock | **No RTC.** `os.time()` is seconds since boot until something sets the clock | Solana OS README → SNTP required (§2) |
| Serial | No Lua serial-read API; the console only speaks the push protocol | Solana OS README → laptop-to-badge test traffic goes over HTTP |
| ESP-NOW payload | ≤ 240 bytes; only between badges on the same Wi-Fi channel | Solana OS README |
| Solana tx size limit | 1232 bytes | Solana docs |
| Solana signature | Ed25519, 64 bytes, over the serialized message | Solana docs |

Rule: **all badges and the laptop join one phone hotspot** on 2.4 GHz (on iPhone turn on "Maximize Compatibility"). This keeps ESP-NOW on one channel and avoids captive portals.

## 2. Constants

| Name | Value | Note |
| --- | --- | --- |
| Cluster | devnet, RPC `https://api.devnet.solana.com` | swap to a provider URL if rate-limited |
| SPL Token program | `TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA` | |
| Associated Token program | `ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL` | backend only (computes `solana_ata`) |
| Memo program | `MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr` | |
| Payment mint | **HACK**, 2 decimals; address = `.env` `HACK_MINT` after `npm run devnet:setup` | **Pinned in firmware** with its decimals and symbol. Any other mint → `undecodable` |
| Issuer key | the existing registry authority `server/.keys/authority.json` | Its public key is **pinned in firmware** by A. Back the file up: losing it means reflashing every badge |
| Time source | SNTP `pool.ntp.org` at boot (ESP-IDF `esp_sntp`) | Fallback: firmware never lets its clock go below the newest issuer-signed `issued_at` it has verified. Never take time from unsigned HTTP |
| Badge listener (hotspot) | `http://<laptop hotspot IP>:8788` (`BADGE_LISTEN_HOST`, `BADGE_LISTEN_PORT`) | All badge-facing routes (§8). Plain HTTP |
| Admin API (laptop only) | `http://127.0.0.1:8787` | Dashboard only; never reachable from the hotspot |
| Nessie base URL | `https://prod-api.nessieisreal.com` (API key as `?key=` query param) | verify at https://prod.nessieisreal.com/ ; key lives in `.env` only |
| Pubkey encoding | base58 in URLs and JSON; lowercase hex (64 chars) inside canonical signed text (§6, §7) | |

## 3. Signing domains (domain separation)

Every badge signature is over `prefix || bytes`. The firmware enforces which prefixes need a button press.

| Prefix | Signed bytes | Button required | Who calls |
| --- | --- | --- | --- |
| *(none — raw Solana message)* | serialized legacy Solana message | **Yes** | payer, via `wallet.begin_solana` |
| `bank-auth:` | canonical bank payload (§6) | **Yes** | payer, via `wallet.begin_bank` |
| `pay-req:` | REQ frame from header to name (§5) | No (auto) | payee, via `wallet.sign_request` |
| `pay-proof:` | `req_id ‖ nonce ‖ payer_pubkey` | No (auto) | payee, via `wallet.sign_proof` |
| `registry:` | registry record (§7) | — (issuer key on the laptop, never on a badge) | backend |

Why a raw Solana message can't be mistaken for a prefixed one: its first byte is `num_required_signatures`. A message starting with `p` (112) or `b` (98) would have to list at least 98 signer keys (3,136 B), more than the 1,232 B limit. Firmware also **requires `num_required_signatures == 1`** and refuses raw bytes that begin with any prefix above. The backend signs only `registry:`-prefixed bytes it built itself; memo transactions are ordinary Solana messages.

## 4. Lua wallet API (implemented by A in C, used by everyone)

```lua
-- identity
wallet.pubkey()            --> 32-byte string
wallet.address()           --> base58 string
wallet.key_location()      --> "se050" | "nvs"
wallet.time_ok()           --> bool  (clock set by SNTP or a verified record)

-- presence, payer side. Firmware keeps the authoritative state; Lua only gets a copy for UI.
wallet.new_nonce(req_id)   --> 16-byte nonce; firmware stores {req_id, nonce, t0}. Up to 4 slots, oldest evicted
wallet.check_proof(req_id, payee_pubkey, proof_sig)
                           --> "present" | "late" | "bad_sig" | "no_nonce"
                           -- verifies sig over pay-proof:req_id||nonce||own_pubkey with payee_pubkey,
                           -- records {req_id, payee_pubkey, result} in C

-- presence + request, payee side (auto-sign, no screen)
wallet.sign_request(req_frame_bytes)           --> 64-byte sig (sign once; rebroadcast the same bytes)
wallet.sign_proof(req_id, nonce, payer_pubkey) --> 64-byte sig

-- registry (Lua may call this for list UI; the firmware re-runs it itself before approval)
wallet.check_record(record_bytes, issuer_sig)
                           --> table {ok=bool, reason=?, display_name, device_pubkey, kind,
                           --         solana_wallet, solana_ata, bank_ref_hash, expiry, status}

-- signing with approval: ASYNCHRONOUS (the approval can outlast the 12 s callback cap)
wallet.begin_solana(msg_bytes, ctx)   --> true | nil, reason     -- firmware takes the screen
wallet.begin_bank(payload_str, ctx)   --> true | nil, reason
wallet.poll()                         --> "pending" | sig | nil, reason
-- call poll() from on_tick until it stops returning "pending"
-- ctx = { record = <record bytes>, record_sig = <sig>, req = <full REQ frame incl. its sig> }
-- reason ∈ "cancelled" | "timeout" | "undecodable" | "unverified" | "mismatch"
--          | "over_cap" | "busy" | "no_time"
```

Only the bytes being signed and `ctx` go in; **no trust decision comes from Lua.** Before showing the screen, the firmware does all of this itself:

1. **Record:** issuer signature against the pinned key, `status=active`, not expired, `issued_at` within `RECORD_TTL_S`. Missing or failing → red "unverified" / "revoked" / "expired".
2. **Request:** parse `ctx.req`. Verify its `pay-req:` signature with `record.device_pubkey`. Check `req.payee_pubkey == record.device_pubkey`, that the REQ hasn't expired, and that `req.rail` matches the call. Any failure → red.
3. **Presence:** look up the stored `check_proof` result for `req.req_id`. Its `payee_pubkey` must equal `record.device_pubkey`.
4. **Bytes, Solana:**
   - Legacy message with exactly one signer, which is `wallet.pubkey()` and is also the token owner.
   - One `transferChecked` plus at most one Memo with zero accounts.
   - Mint and decimals equal the pinned HACK mint.
   - Destination equals `record.solana_ata`.
   - Amount equals `req.amount`.
5. **Bytes, bank** (§6):
   - `payee_ref == record.bank_ref_hash`, `payee_name == record.display_name`.
   - `action` matches `record.kind`: purchase for a merchant, transfer for a person.
   - `req_id` and `amount_cents` equal the REQ's values.
   - `proof_nonce` equals the nonce stored for that `req_id`.

The screen shows amount, token/currency and recipient **only from the decoded bytes** and the name **only from the verified record**.

| State | When | SELECT |
| --- | --- | --- |
| Green ✓ verified ● present | checks 1–5 pass and presence = `present` | enabled |
| Amber "not present" | checks 1, 2, 4/5 pass; presence = `late` or no PROOF | Solana: enabled with warning (or disabled, per U6). Bank: **disabled** (the backend requires a proof) |
| Red "unverified / revoked / expired" | check 1 fails | disabled |
| Red "MISMATCH" | a decoded field differs from the record or REQ (recipient, amount, mint, payee) | disabled |
| Red "bad proof" | presence = `bad_sig` | disabled |
| Red "cannot read this payment" | undecodable bytes | disabled |
| Red "clock not set" | `wallet.time_ok()` is false | disabled |

Constants owned by A, tuned with R's measurements:

| Constant | Start value | How it is set |
| --- | --- | --- |
| `PRESENCE_DEADLINE_MS` | 250 for an NVS key, 500 for an SE050 key | R4: p95 of CHAL→PROOF measured *including* the payee's signature, plus 50% margin |
| `RECORD_TTL_S` | 30 | Short, so revocation shows "within one refresh". The badge fetches a fresh record for every payment |
| `APPROVAL_TIMEOUT_S` | 45 | Below the ~60–90 s blockhash lifetime |
| `SPEND_CAP` | P2 | — |

Dev builds only: the compile flag `VK_DEV_ALLOW_UNVERIFIED` enables SELECT on a red screen, with a permanent "DEV BUILD" banner. It is used by R's harness before the registry exists. Judge badges are never flashed with it.

## 5. ESP-NOW messages (binary, little-endian)

Common header (4 B): `magic "VK"` (2) · `version u8 = 1` · `type u8`

| Type | Name | Body after header | Total size |
| --- | --- | --- | --- |
| 1 | REQ | `rail u8` (1=solana, 2=bank) · `payee_pubkey [32]` · `amount u64` (minor units: HACK at 2 decimals, or cents) · `currency [4]` ("HACK" / "USD\0") · `req_id [8]` · `expiry u32` (unix s) · `name_len u8` · `name [≤32]` · `sig [64]` over `pay-req:` + everything from header to name | ≤ 158 B |
| 2 | CHAL | `req_id [8]` · `nonce [16]` · `payer_pubkey [32]` | 60 B |
| 3 | PROOF | `req_id [8]` · `sig [64]` over `pay-proof:` ‖ req_id ‖ nonce ‖ payer_pubkey | 76 B |
| 4 | RESULT | `req_id [8]` · `status u8` (0=ok, 1=rejected, 2=failed) · `ref [64]` (Solana tx sig or Nessie id, zero-padded) | 77 B |

- REQ is broadcast; CHAL, PROOF and RESULT are unicast to the sender's MAC (register it as an ESP-NOW peer first).
- The payee signs a REQ **once**, rebroadcasts the identical bytes at about 1 Hz, and uses a fresh `req_id` for each new request.
- The REQ name is only a claim. The payer displays the name from the verified record.
- **RESULT is unauthenticated.** Before showing PAID, the payee confirms the Solana tx signature over RPC (or asks the backend for bank payments).
- In memos, `req_id` is written as 16 hex characters (the Memo program requires UTF-8).

## 6. Bank authorization payload

UTF-8, `key=value` per line, `\n` separated, **exactly this order**, no trailing newline. Signed bytes = `bank-auth:` + payload.

```
v=1
rail=nessie
action=purchase|transfer
amount_cents=4000
currency=USD
from_acct=<payer Nessie account id, from GET /balance>
payee_name=<record.display_name>
payee_ref=<hex record.bank_ref_hash>
attestation=<SAS attestation address, or "none" in issuer-only fallback>
req_id=<hex 8 B, from the REQ>
proof_nonce=<hex 16 B nonce from the handshake>
issued_at=<unix seconds>
```

`issued_at` is set when the approval screen opens. The backend accepts it within `APPROVAL_TIMEOUT_S` + 30 s.

## 7. Registry record (what the badge verifies)

Served by `GET /registry/:pubkey` on the badge listener. The backend builds it from the on-chain SAS attestation (schema `payee_v1`) and signs **`registry:` + canonical bytes** with the issuer key. Canonical bytes use the same `key=value\n` style; hex is lowercase, 64 characters for 32 bytes:

```
v=1
attestation=<address>
display_name=<≤32 printable ASCII>
device_pubkey=<hex 32>
kind=merchant|person
solana_wallet=<hex 32 or empty>
solana_ata=<hex 32 or empty; HACK ATA of solana_wallet>
bank_ref_hash=<hex 32 or empty>
expiry=<unix s>
status=active|revoked
issued_at=<unix s>
```

Response JSON: `{ "record": "<base64 canonical bytes>", "sig": "<base64 64 B>" }`.
- A revoked key returns a signed record with `status=revoked`.
- An unknown key returns HTTP 404, which the badge treats as unverified.
- An expired attestation is served with its real `expiry`, and the badge rejects it. The existing code's "expiry ignored" shortcut must go.

## 8. Backend endpoints (existing Node server, extended)

### 8.1 Badge listener — hotspot, port 8788 (badge routes only, no admin)

| Method | Path | Body → Response | Status |
| --- | --- | --- | --- |
| GET | `/health` | → `{ok:true}` | new |
| GET | `/registry/:pubkey` | → §7 | new |
| GET | `/balance/:pubkey` | → `{usd_cents, account_id}` (`account_id` = this badge's `from_acct`) | new |
| POST | `/bank/authorize` | `{payload, sig, payer_pubkey, req, proof_sig}` (base64 binaries; `req` = full REQ frame) → `{ok, nessie_id?, memo_sig?, reason?}` | new |
| POST | `/feed/solana` | `{tx_sig, req}` → `{ok}`. The backend confirms on chain that the tx moved `req.amount` HACK to the payee's ATA before it writes the feed row and memo | new |
| POST | `/feed/event` | `{payer_pubkey, req?, reason}` → `{ok}`. Badge-side refusals (impostor, revoked, bad proof, mismatch). Unauthenticated; shown on the feed as "reported by badge" | new |
| GET | `/badge/pending?badge=<pubkey>` | → unsigned tx for the attack demo / R's signing harness (`messageBase64`) | exists |
| POST | `/badge/outcome` | `{id, outcome}` | exists |

### 8.2 Admin API — 127.0.0.1:8787 only (dashboard)

| Method | Path | Body → Response | Status |
| --- | --- | --- | --- |
| GET | `/api/status` | health, config, gaps | exists |
| POST | `/api/attestations` | `{pubkey, name, kind, nessie_ref?}` → attestation | exists; extend to `payee_v1` fields |
| POST | `/api/attestations/revoke` | `{pubkey}` | exists |
| POST | `/api/enroll` | `{pubkey, nessie_customer_id?, nessie_account_id?}` → `{customer_id, account_id}` | new |
| GET | `/api/payments` | feed, both rails, incl. blocked rows: `{time, rail, payer, payee, verified, amount, status, reason, links}` | exists; extend |
| GET | `/api/events` | SSE push to the dashboard | exists |
| GET/POST | `/api/attacks…` | attack console | exists |

All bodies are JSON, with binary fields in base64. The badge listener has no authentication. That is acceptable on devnet and stated as a limitation, so **never** mount admin routes on it.
