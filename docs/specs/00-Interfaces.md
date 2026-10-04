# 00 — Shared Interfaces and Contracts

Owner: all three (A, R, U) · Status: agree at hour 0, then change only by telling everyone
Parent: [`Prd-verified-payment-key.md`](Prd-verified-payment-key.md) (product, threat model, demo)

Every other spec depends on this file. If a field, prefix or endpoint changes, change it **here first**, then in code.

## 0. Decisions already made — ✏️ Changed (routing)

| Decision | Choice | Consequence |
| --- | --- | --- |
| 🆕 Relay network (DePIN) | **Additive.** Badges route payments hop by hop to a destination badge; relays earn a per-hop HACK fee. **Direct payment (§4–§8) stays fully specified and is the fallback demo** | New §9; routed variants of the wallet API (§4.1) and two new backend routes (§8) |
| 🆕 Routed presence | A routed payment shows **"✓ verified · via N relays"**, never "● present". Presence is timed at **every hop**. The destination's proof over the payer's nonce travels back **signed but untimed** | Routed payments are allowed on both rails when every hop is present (§4.1) |
| 🆕 Records on the route | Each hop carries its **own compact issuer-signed record** (§9.3), refreshed while online | The payer can verify every hop with no Wi-Fi of its own. Size handled by fragmentation |
| 🆕 Radio size limit | **Fragmentation** message (FRAG, §9.4) for anything over one ESP-NOW frame | Quotes (≈1.3 KB) and signed transactions (≈0.5 KB) cross the radio |
| 🆕 Relay identity | `kind=relay` in the existing attestation (no new schema field). Policy: **one relay attestation per verified operator** | §7 `kind` gains `relay` |
| 🆕 Demo topology | All **4 badges in one chain**: judge (payer) → relay → relay → merchant. Badges only accept routing frames from configured neighbours (`DEMO_NEIGHBORS`) | On a table every badge is in radio range; the chain is enforced in software for the demo and stated as such |
| 🆕 x402 | The route quote is **x402-style** (a "payment required" quote over ESP-NOW). We do not claim real x402 unless the gateway's HTTP side speaks actual x402 headers | Prior art to credit: PlaiPin's ESP32-S3 x402 SDK |
| ✏️ Clock | `wallet.time_ok()` is true **only after SNTP has synced since boot**; a payer that later goes offline keeps time from that sync. The record-based floor is now anti-rollback only | Previously: `time_ok()` could also become true from a verified record, which made freshness checks circular |
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

## 2. Constants — ✏️ Changed (routing)

Routing constants are in §9.8.

| Name | Value | Note |
| --- | --- | --- |
| Cluster | devnet, RPC `https://api.devnet.solana.com` | swap to a provider URL if rate-limited |
| SPL Token program | `TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA` | |
| Associated Token program | `ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL` | backend only (computes `solana_ata`) |
| Memo program | `MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr` | |
| Payment mint | **HACK**, 2 decimals; address = `.env` `HACK_MINT` after `npm run devnet:setup` | **Pinned in firmware** with its decimals and symbol. Any other mint → `undecodable` |
| Issuer key | the existing registry authority `server/.keys/authority.json` | Its public key is **pinned in firmware** by A. Back the file up: losing it means reflashing every badge |
| Time source | SNTP `pool.ntp.org` at boot (ESP-IDF `esp_sntp`) | `time_ok()` requires an SNTP sync since boot. Anti-rollback: the clock never goes below the newest issuer-signed `issued_at` verified, but that alone never sets `time_ok()`. Never take time from unsigned HTTP. *Previously: a verified record could also set the clock and `time_ok()`* |
| Badge listener (hotspot) | `http://<laptop hotspot IP>:8788` (`BADGE_LISTEN_HOST`, `BADGE_LISTEN_PORT`) | All badge-facing routes (§8). Plain HTTP |
| Admin API (laptop only) | `http://127.0.0.1:8787` | Dashboard only; never reachable from the hotspot |
| Nessie base URL | `https://prod-api.nessieisreal.com` (API key as `?key=` query param) | verify at https://prod.nessieisreal.com/ ; key lives in `.env` only |
| Pubkey encoding | base58 in URLs and JSON; lowercase hex (64 chars) inside canonical signed text (§6, §7) | |

## 3. Signing domains (domain separation) — ✏️ Changed (routing)

Every badge signature is over `prefix || bytes`. The firmware enforces which prefixes need a button press.

| Prefix | Signed bytes | Button required | Who calls |
| --- | --- | --- | --- |
| *(none — raw Solana message)* | serialized legacy Solana message (direct payment, routed payment, or routed bank fee transaction) | **Yes** | payer, via `wallet.begin_solana` / `begin_route_solana` / `begin_route_bank` |
| `bank-auth:` | canonical bank payload (§6) | **Yes** | payer, via `wallet.begin_bank` / `begin_route_bank` |
| `pay-req:` | REQ frame from header to name (§5) | No (auto) | payee, via `wallet.sign_request` |
| `pay-proof:` | `req_id ‖ nonce ‖ payer_pubkey`. On a route, `route_id` takes the `req_id` slot and the upstream hop's key takes the `payer_pubkey` slot (§9.2) | No (auto) | payee or next hop, via `wallet.sign_proof` |
| 🆕 `route-att:` | neighbour attestation (§9.5) | No (auto) | relay, via `wallet.sign_route_att` |
| 🆕 `route-quote:` | `route_id ‖ blockhash ‖ quote_time` (§9.5) | No (auto) | gateway relay, via `wallet.sign_route_quote` |
| `registry:` | registry record (§7) | — (issuer key on the laptop, never on a badge) | backend |
| 🆕 `registry-c:` | compact registry record (§9.3) | — (issuer key) | backend |

Why a raw Solana message can't be mistaken for a prefixed one: its first byte is `num_required_signatures`. Every prefix starts with `p` (0x70 = 112), `b` (0x62 = 98) or `r` (0x72 = 114). Matching one would need at least 98 signer keys (98 × 32 = 3,136 B), and `r` would need 114 (3,648 B). Both exceed the 1,232 B limit. Firmware also **requires `num_required_signatures == 1`** and refuses raw bytes that begin with any prefix above. The backend signs only `registry:`/`registry-c:`-prefixed bytes it built itself; memo transactions are ordinary Solana messages. *Previously: the argument covered only `p` and `b`.*

## 4. Lua wallet API (implemented by A in C, used by everyone) — ✏️ Changed (routing)

The direct-payment API below is unchanged apart from `time_ok()`. Routed additions are in §4.1.

```lua
-- identity
wallet.pubkey()            --> 32-byte string
wallet.address()           --> base58 string
wallet.key_location()      --> "se050" | "nvs"
wallet.time_ok()           --> bool  (true only after an SNTP sync since boot; previously also from a verified record)

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

### 4.1 Routed payments — 🆕 Routing

```lua
-- payer side
wallet.new_route()          --> route_id [8], e2e_nonce [16]; firmware stores {route_id, e2e_nonce, t0}
                            -- presence with hop 1 reuses new_nonce(route_id) / check_proof(route_id, hop1_pubkey, sig)
wallet.check_route(rrep)    --> table {ok, reason?, fee_total, hops = {{name, fee, present}, ...}}
                            -- for list UI only; firmware re-verifies everything in begin_route_*
wallet.begin_route_solana(msg_bytes, ctx)              --> true | nil, reason
wallet.begin_route_bank(payload_str, fee_msg_bytes, ctx) --> true | nil, reason
-- ctx = { quotes = {rrep_1, rrep_2, ...} }   every RREP received for this route_id (§9.6)
-- poll() returns: Solana → sig ; bank → payload_sig, fee_sig

-- relay side (auto-sign, no screen)
wallet.sign_route_att(route_id, next_hop_pubkey, amount, rail, fee)
                            --> sig, proof_flag, nonce
                            -- firmware takes proof_flag and nonce from ITS OWN presence slot for
                            -- (route_id, next_hop_pubkey); refuses with nil if there is no valid proof
wallet.sign_route_quote(route_id, blockhash, quote_time) --> sig   (gateway relay only; requires time_ok())
wallet.check_crec(crec_bytes) --> same table as check_record, for the compact record (§9.3)

-- new reasons: "unverified_hop" | "broken_chain" | "fee_mismatch" | "stale_quote"
```

Before the routed approval screen, the firmware checks **every** quote in `ctx.quotes` (Q1–Q5). It then requires the bytes to pay the **cheapest valid** quote (Q6/Q7). Ties go to fewer hops, then the earliest quote. Bytes that pay any other route → red MISMATCH "not the cheapest verified route".

- **Q1 Records.** Every compact record must be signed by the pinned issuer key (`registry-c:`), `status=active`, unexpired, and `issued_at` within `ROUTE_RECORD_TTL_S`. Relays must be `kind=relay`; the destination must be `merchant` or `person`. A bad relay → red "unverified hop"; a bad destination → red "unverified".
- **Q2 Chain.** The chain must hold together:
  - Hop 1's key equals the key that answered the payer's own CHAL for this `route_id`.
  - For each relay *i*:
    - its `route-att:` signature verifies with its record key;
    - the attested `route_id`, `amount` and `rail` match;
    - `next_hop_pubkey` equals hop *i+1*'s record key (the destination for the last relay);
    - the next hop's proof verifies over `pay-proof:` ‖ route_id ‖ nonce_i ‖ relay_i_pubkey.
  - Keys are all distinct, and relays ≤ `ROUTE_MAX_RELAYS`.

  Any failure → red "broken chain".
- **Q3 Destination.**
  - Verify the REQ with the destination's record key. Check `req.payee_pubkey` equals that key, the REQ hasn't expired, and the rail matches.
  - `dest_proof` verifies over `pay-proof:` ‖ req_id ‖ e2e_nonce ‖ payer_pubkey, using the `e2e_nonce` the firmware stored for this `route_id`. A bad proof → red "bad proof".
- **Q4 Quote freshness.** The gateway's `route-quote:` signature verifies, `quote_time` is within `ROUTE_QUOTE_TTL_S` of the payer's clock, and the message blockhash equals the quoted blockhash. Any failure → red "stale quote".
- **Q5 Fees.** Each `fee_i` ≤ `ROUTE_FEE_HOP_CAP`. `fee_total` = Σ fee_i.
- **Q6 Bytes, Solana.**
  - A legacy message with one signer (the payer = owner = fee payer) and the pinned mint and decimals.
  - **Exactly one** payment `transferChecked` to the destination's `solana_ata` for `req.amount`.
  - **Exactly one** fee `transferChecked` per relay, to that relay's `solana_ata`, for `fee_i`.
  - Optionally, one zero-account memo = hex(route_id).

  A wrong payment leg → red MISMATCH. A wrong, missing or extra fee leg → red "fee mismatch".
- **Q7 Bytes, bank.**
  - The direct bank checks (§4 step 5) apply, with the destination's compact record in place of the full record.
  - `route_id` equals the route, and `route_fee_total` equals the quote's total.
  - `fee_msg` contains only the Q6 fee legs, plus an optional memo.

| Routed state | When | SELECT |
| --- | --- | --- |
| Green **✓ verified · via N relays ✓** | Q1–Q7 pass and every hop proof is `present` | enabled on both rails |
| Amber "via N relays · a hop was slow" | Q1–Q7 pass, but some hop proof (or the payer↔hop 1 proof) is `late` | Solana: enabled with warning. Bank: **disabled** |
| Red "unverified hop" / "broken chain" / "fee mismatch" / "stale quote" | Q1, Q2, Q5–Q7 or Q4 fails | disabled |
| Red (direct states) | unverified destination, bad proof, MISMATCH, undecodable, clock not set | disabled |

- The routed screen shows `PAY 1.00 HACK → MHacks Merch ✓`, then `via 2 relays ✓ · fees 0.02`, then `TOTAL 1.02 HACK`. Pressing `right` opens the hop detail view: each hop's name, short id, fee and ✓.
- If `fee_total` > `ROUTE_FEE_AUTO_CAP`, a second confirmation screen follows. Within the cap, the fees are approved by the same single SELECT.
- A routed destination is **never** shown as "● present". The payer only knows it is near the last relay.

## 5. ESP-NOW messages (binary, little-endian) — ✏️ Changed (routing)

Common header (4 B): `magic "VK"` (2) · `version u8 = 1` · `type u8`

Types 1–4 below are the direct flow and are unchanged. Routing adds types 5–10 (RREQ, RREP, FRAG, RPAY, REQ_FWD, FRAG_ACK), specified in §9.4. On a route, RESULT (type 4) is forwarded hop by hop back to the payer.

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

## 6. Bank authorization payload — ✏️ Changed (routing)

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
proof_nonce=<hex 16 B nonce from the handshake; on a route, the payer's e2e_nonce>
issued_at=<unix seconds>
route_id=<hex 8 B, or "none" for a direct payment>
route_fee_total=<HACK minor units; 0 for a direct payment>
```

`issued_at` is set when the approval screen opens. The backend accepts it within `APPROVAL_TIMEOUT_S` + 30 s (or `ROUTE_APPROVAL_TIMEOUT_S` + 30 s on a route).

> Previously: the payload ended at `issued_at`. The two new lines are **always present** (`none` / `0` when direct), so the field order stays fixed. They bind the separately signed HACK fee transaction (§9.7) to this bank payment.

## 7. Registry record (what the badge verifies) — ✏️ Changed (routing)

> Routing changes: `kind` gains **`relay`**. A relay record carries `solana_ata`, where its fees go, and an empty `bank_ref_hash`. A compact binary form of the same record, carried over the radio, is in §9.3. Policy: the issuer grants **one relay attestation per verified operator** (Sybil limit; the operator id is kept only in the backend DB). Previously: `kind=merchant|person`.

Served by `GET /registry/:pubkey` on the badge listener. The backend builds it from the on-chain SAS attestation (schema `payee_v1`) and signs **`registry:` + canonical bytes** with the issuer key. Canonical bytes use the same `key=value\n` style; hex is lowercase, 64 characters for 32 bytes:

```
v=1
attestation=<address>
display_name=<≤32 printable ASCII>
device_pubkey=<hex 32>
kind=merchant|person|relay
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

## 8. Backend endpoints (existing Node server, extended) — ✏️ Changed (routing)

### 8.1 Badge listener — hotspot, port 8788 (badge routes only, no admin)

| Method | Path | Body → Response | Status |
| --- | --- | --- | --- |
| GET | `/health` | → `{ok:true}` | new |
| GET | `/registry/:pubkey` | → §7 | new |
| GET | 🆕 `/registry/:pubkey?format=compact` | → `{crec: "<base64 compact record>"}` (§9.3), freshly signed; 404 if unknown. Relays and destinations fetch their **own** compact record every `CREC_REFRESH_S` | new (routing) |
| POST | 🆕 `/feed/route` | `{route_id, rail, req, hops:[{pubkey, fee}], tx_sig?, nessie_id?, status, reason?}` → `{ok}`. Sent by the gateway relay. For Solana, the backend confirms on chain that the tx holds the payment leg and **one fee leg per hop to that relay's attested ATA** before it writes the route and the per-relay earnings | new (routing) |
| GET | `/balance/:pubkey` | → `{usd_cents, account_id}` (`account_id` = this badge's `from_acct`) | new |
| POST | `/bank/authorize` | `{payload, sig, payer_pubkey, req, proof_sig, route?}` (base64 binaries; `req` = full REQ frame) → `{ok, nessie_id?, memo_sig?, fee_tx_sig?, reason?}`. ✏️ Routing: optional `route = {route_id, hops:[pubkey], fee_tx}` (`fee_tx` = the payer-signed HACK fee transaction). The backend checks the fee legs against the attested relay ATAs and `route_fee_total`, and submits `fee_tx` **only after the Nessie call succeeds** (§9.7). Sent by the gateway on a route | new |
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
| GET | `/api/events` | SSE push to the dashboard (🆕 plus `route` events) | exists |
| GET/POST | `/api/attacks…` | attack console | exists |
| GET | 🆕 `/api/routes` | recent routes: `{route_id, time, rail, payer, payee, hops:[{name, pubkey, fee, present}], fee_total, status, latency_ms}` | new (routing) |
| GET | 🆕 `/api/relays` | relay leaderboard: `{pubkey, name, routes, fees_earned, last_seen}` | new (routing) |

`POST /api/attestations` also accepts `kind: "relay"` (routing).

All bodies are JSON, with binary fields in base64. The badge listener has no authentication. That is acceptable on devnet and stated as a limitation, so **never** mount admin routes on it.

## 9. Routing — relay network (DePIN) — 🆕 Routing

Routing is additive. Every direct-payment rule in §4–§8 still holds, and direct payment is the fallback demo.

### 9.1 Roles

| Role | Who | Needs |
| --- | --- | --- |
| Payer | Judge badge. May be offline (no Wi-Fi) for the payment, but must have SNTP time from earlier (§0) | Its own HACK source ATA cached from setup (P2-A) |
| Relay | Any badge with a `kind=relay` attestation | A fresh compact record (§9.3), refreshed while online |
| Gateway | The **last relay** before the destination; it must reach devnet RPC and the backend | Wi-Fi. It signs the `route-quote:` (blockhash + time), submits, and reports |
| Destination | Merchant or person badge with an open REQ | Its own fresh compact record |

### 9.2 Flow

1. **Advertise.** The destination broadcasts its signed REQ as today. Each relay rebroadcasts REQs it hears as `REQ_FWD` (hop_count + 1), once per `req_id` per second, up to `ROUTE_MAX_RELAYS`. The payer's Pay app lists direct REQs as today and forwarded ones as "MHacks Merch · 2 relays away".
2. **Discover (RREQ).** The payer calls `wallet.new_route()` and broadcasts an RREQ with `ttl = ROUTE_MAX_RELAYS`. A relay drops it if it has seen this (route_id, path), if it is already in `path`, if `ttl = 0`, or if the sender isn't in `DEMO_NEIGHBORS` (when set). Otherwise it stores the upstream MAC for (route_id, path) and:
   - if it heard the destination's REQ directly, it **unicasts** the RREQ to the destination;
   - otherwise it rebroadcasts with `ttl − 1` and its 8-byte id appended to `path`.
3. **Destination answers.** The RREQ must match the destination's open REQ (`req_id`, `amount`, `rail`). The destination signs `dest_proof = sign_proof(req_id, e2e_nonce, payer_pubkey)` and sends the RREP base (route_id, its REQ, its compact record, dest_proof) to its upstream relay, the gateway.
4. **Quote travels back (RREP).** Each relay, on receiving an RREP from downstream:
   1. Runs CHAL/PROOF with the downstream hop. The downstream hop signs `pay-proof:` ‖ route_id ‖ nonce ‖ this_relay_pubkey.
   2. Calls `wallet.sign_route_att(...)`. The firmware fills in the proof flag and nonce from its own presence slot.
   3. **The gateway only:** fetches a recent blockhash and calls `wallet.sign_route_quote(route_id, blockhash, quote_time)`.
   4. Prepends its hop entry and forwards the RREP upstream (FRAG).
5. **Payer checks.** The payer collects RREPs for `ROUTE_COLLECT_MS`. It runs CHAL/PROOF with each first hop (`new_nonce(route_id)` / `check_proof(route_id, hop1_pubkey, sig)`; presence slots are keyed by (route_id, peer)). It ranks the quotes with `wallet.check_route()`, builds the transaction for the cheapest valid one, and calls `begin_route_*` with **all** quotes. The firmware enforces the choice (§4.1).
6. **Settle and forward (RPAY).** After SELECT, the payer sends RPAY down the chosen route. Each relay forwards it to its stored downstream MAC.
   - Solana: the gateway calls `sendTransaction`, polls for confirmation, and sends RESULT upstream (relays forward it) and to the destination. It then posts `POST /feed/route`.
   - Bank: see §9.7.
7. **Clean-up.** Relays drop route state on RESULT or after `ROUTE_STATE_TTL_S`. There is **no store-and-forward**: the whole route must be live from quote to submit, because the blockhash expires in ~60–90 s.

### 9.3 Compact registry record (CREC)

Binary, little-endian. The issuer signs `registry-c:` ‖ everything before `sig`.

| Field | Size |
| --- | --- |
| `v u8 = 1` | 1 |
| `device_pubkey` | 32 |
| `kind u8` (1 merchant, 2 person, 3 relay) | 1 |
| `status u8` (1 active, 2 revoked) | 1 |
| `attestation` (zeros in issuer-only fallback) | 32 |
| `solana_ata` (zeros if none) | 32 |
| `bank_ref_hash` (zeros if none) | 32 |
| `expiry u32`, `issued_at u32` | 8 |
| `name_len u8` + `name [≤32]` | ≤ 33 |
| `sig` | 64 |
| **Total** | **≤ 236 B** |

It carries the same facts as §7, so the backend builds both forms from the same attestation. Each hop carries its own CREC. That way an offline payer can verify every hop, and the gateway can't swap records, because each one is issuer-signed.

### 9.4 Messages (types 5–10)

| Type | Name | Body after the 4 B header | Size | Sent as |
| --- | --- | --- | --- | --- |
| 5 | RREQ | `route_id [8]` · `req_id [8]` · `dest_pubkey [32]` · `payer_pubkey [32]` · `rail u8` · `amount u64` · `e2e_nonce [16]` · `ttl u8` · `path_len u8` · `path [path_len × 8]` | ≤ 135 B (3 relays) | broadcast / unicast to the destination |
| 6 | RREP | `route_id [8]` · `req_len u8` · `req [≤158]` · `dcrec_len u8` · `dest_crec [≤236]` · `dest_proof [64]` · `blockhash [32]` · `quote_time u32` · `quote_sig [64]` · `hop_count u8` · hops, payer side first, each: `crec_len u8` · `crec [≤236]` · `fee u32` · `proof_flag u8` (0 present, 1 late) · `nonce [16]` · `next_proof [64]` · `att_sig [64]` | 569 B + 386 B per relay. **2 relays: 1,341 B (7 frames). 3 relays: 1,727 B (9 frames)** | FRAG, unicast upstream |
| 7 | FRAG | `msg_id u32` · `inner_type u8` · `seq u8` · `total u8` · `msg_hash [16]` (first 16 B of SHA-256 of the whole inner message) · `payload [≤213]` | ≤ 240 B | unicast |
| 8 | RPAY | `route_id [8]` · `rail u8` · Solana: `sig [64]` · `msg_len u16` · `msg`; bank: `payload_len u16` · `payload` · `bank_sig [64]` · `fee_len u16` · `fee_msg` · `fee_sig [64]` | Solana, 2 relays: 438 B (3 frames). Bank, 2 relays: ≈ 855 B (5 frames) | FRAG, unicast downstream |
| 9 | REQ_FWD | `hop_count u8` · `req [≤158]` (the destination's signed REQ frame, unchanged) | ≤ 163 B | broadcast |
| 10 | FRAG_ACK | `msg_id u32` · `status u8` (0 ok, 1 hash mismatch) | 9 B | unicast |

Fragmentation rules:
- Reassemble per (sender MAC, msg_id). The message is complete when every `seq` up to `total − 1` has arrived and its SHA-256 prefix matches `msg_hash`.
- Drop the message after `FRAG_REASSEMBLY_MS`. The receiver sends FRAG_ACK when a message is complete.
- The sender re-sends the **whole** message up to `FRAG_RETRIES` times if no ACK arrives within 300 ms.
- The hash protects reassembly, not authenticity: every content field is covered by a signature.
- Max message: 10 frames × 213 B = 2,130 B.

### 9.5 Signed formats

- **Neighbour attestation:** `route-att:` ‖ route_id [8] ‖ relay_pubkey [32] ‖ next_hop_pubkey [32] ‖ amount u64 ‖ rail u8 ‖ fee u32 ‖ proof_flag u8 ‖ nonce [16].
  - It binds the fee to this route and amount.
  - `nonce` lets the payer check the next hop's `pay-proof:` signature itself. The firmware takes `proof_flag` and `nonce` from its own presence slot, never from Lua.
- **Gateway quote:** `route-quote:` ‖ route_id [8] ‖ blockhash [32] ‖ quote_time u32. Only the gateway (the last relay) signs this.

### 9.6 Fees and route selection

- **Per-hop fee:** `fee_i = base_i + floor(rate_ppm_i × amount / 1,000,000)` in HACK minor units, chosen by each relay. The defaults are `ROUTE_FEE_BASE = 1` (0.01 HACK) and `ROUTE_FEE_RATE_PPM = 1000` (0.1 %). Each `fee_i` ≤ `ROUTE_FEE_HOP_CAP`.
  - Example: 1.00 HACK over 2 relays → 0.01 + 0.01 = **0.02 HACK**.
  - Example: a $40.00 bank payment over 2 relays → 0.05 + 0.05 = 0.10 HACK.
- **Two fees per Solana payment:** the Solana network fee (SOL, paid by the payer to validators) and the relay fees (HACK, paid to the route). Relays earn for **reach**. They are not miners: validators still verify and earn the SOL fee.
- **Selection:** the cheapest valid route (Σ fee), then the fewest hops, then the earliest quote. The firmware enforces it (§4.1).
  - With a flooded RREQ and a hop limit of 3, the payer sees every route, so picking the cheapest is the same answer Dijkstra would give on this small graph.
  - **Scaling path** (documented, not built): relays gossip their channels and fees, and the payer runs Dijkstra from the destination backward, as Lightning does.
  - Routing is computed **on the payer, never on the backend**, so the network isn't centralised.

### 9.7 Settlement

- **Solana (atomic).** One transaction holds the payment `transferChecked`, one fee `transferChecked` per relay to that relay's attested `solana_ata`, and a memo of hex(route_id).
  - If it lands, everyone is paid. If it doesn't, nobody is.
  - The payer is the fee payer (SOL). Relays never sign, so they need no SOL.
  - Message size with 2 relays: 8 accounts → **363 B** (412 B with 3 relays). Wire size ≤ 477 B, well under 1,232 B.
- **Bank over the route.** Nessie can't move money and pay relays atomically. So, on the same approval screen, the payer signs the bank payload (with `route_id` and `route_fee_total`, §6) **and** a separate HACK fee transaction holding only the fee legs. RPAY carries both to the gateway, which calls `POST /bank/authorize` with `route`.
  - The backend runs its usual checks plus the fee-transaction checks.
  - It calls Nessie, and submits `fee_tx` **only if Nessie succeeded**. If Nessie fails, nobody is paid.
  - The fee transaction's blockhash still expires, so the Nessie call must come back within about 30 s.

### 9.8 Constants

| Name | Value | Note |
| --- | --- | --- |
| `ROUTE_MAX_RELAYS` (TTL) | 3 | The demo uses 2 |
| `ROUTE_COLLECT_MS` | 1500 | Payer waits for RREPs |
| `ROUTE_QUOTE_TTL_S` | 30 | From `quote_time` to `begin_route_*` |
| `ROUTE_APPROVAL_TIMEOUT_S` | 30 | Quote TTL + approval stays under the ~60 s minimum blockhash life. The direct payment keeps 45 |
| `ROUTE_RECORD_TTL_S` | 60 | Compact records on a route. The direct `RECORD_TTL_S` stays 30 |
| `CREC_REFRESH_S` | 20 | How often relays and destinations re-fetch their own compact record while online |
| `ROUTE_FEE_BASE` / `ROUTE_FEE_RATE_PPM` | 1 / 1000 | Relay defaults |
| `ROUTE_FEE_HOP_CAP` | 10 (0.10 HACK) | Per hop |
| `ROUTE_FEE_AUTO_CAP` | 10 (0.10 HACK) | Total; above it, a second confirmation screen |
| `ROUTE_STATE_TTL_S` | 90 | Relays forget a route after this |
| `FRAG_PAYLOAD` / `FRAG_MAX_FRAMES` | 213 B / 10 | |
| `FRAG_REASSEMBLY_MS` / `FRAG_RETRIES` | 500 / 2 | |
| `DEMO_NEIGHBORS` | per-badge list of neighbour pubkeys | Routing frames (types 5–10) from anyone else are dropped. Direct-flow frames (1–4) are unaffected, so the direct fallback still works |

### 9.9 Timing budget (2 relays)

| Step | Estimate |
| --- | --- |
| RREQ flood | ~10 ms per hop |
| RREP per hop: CHAL/PROOF + `route-att` sign + 7 frames | ≤ `PRESENCE_DEADLINE_MS` + sign time + ~20 ms ≈ 0.3–0.8 s |
| Gateway `getLatestBlockhash` | 0.3–0.8 s |
| Request → routed approval screen | **≈ 2–4 s** (direct target stays under 2 s) |
| RPAY forwarding (3 frames × 2 hops) | ~0.1 s |
| Submit + confirm | 1–2 s |

### 9.10 Demo topology

- All four badges form one chain: **judge (payer) → relay A → relay B (gateway) → merchant.**
- On a table every badge is within ESP-NOW range, so `DEMO_NEIGHBORS` forces the chain in software. Say this plainly if asked.
- The impostor and "unverified relay" beats reuse chain badges:
  - revoke relay A's attestation → the route goes red;
  - switch a relay into impostor mode → a direct scene.
