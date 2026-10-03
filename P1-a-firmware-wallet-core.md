# P1-A — Firmware: Wallet Core in Solana OS

Owner: **A** · Part 1 (≈ hours 0–8) · Gate: one badge-signed HACK payment confirmed on devnet
Reads: `00-Interfaces.md` (§0 decisions, §1 platform facts, §3 signing domains, §4 Lua API) · Parent: `Prd-verified-payment-key.md`

## 1. Why this exists

The product's security claim is that **only firmware below Lua can produce a payment signature, and only after showing the decoded payment on a screen the app cannot draw over.** Everything else (apps, backend, dashboard) is untrusted for signing *and* for trust decisions: the firmware verifies the registry record, the payee's signed request and the presence proof itself. This spec builds that layer.

You are **extending** Solana OS, not writing an OS. Say it that way in the pitch.

## 2. What already exists (don't rebuild)

From the Solana OS README (https://github.com/spacemandev-git/solana-defcon-badge-26/blob/main/firmware/solana-os/README.md):

- **Identity key:** Ed25519, generated on first boot. It is stored in the SE050 when that answers; otherwise in NVS with TweetNaCl.
- **SE050 path:**
  - It already includes APDU code for keygen, public key and EdDSA sign.
  - It self-tests by signing a probe and verifying it with TweetNaCl, which catches byte-order reversal.
  - **It has never run on a real SE050.** Likely failure points are an SE050 configured for SCP03 sessions, or a variant without Ed25519. It fails closed to the NVS key.
- **Lua 5.4 sandbox:** a 250 ms per-callback watchdog. Blocking bindings can extend it, but **never past 12 s per callback**, so approval must be asynchronous (§4.4).
- **Lua bindings:**
  - `badge.espnow`, `badge.http`, `badge.gfx`, `badge.storage`, `badge.led`.
  - `badge.input` (keys `a`/`b` = SELECT/CANCEL in our specs).
  - `badge.se050` (presence/ATR/test/random only — **no sign binding**).
- **No RTC:** `os.time()` counts seconds since boot until the clock is set.
- **Broker registration** already signs a challenge with the identity key. It's a useful reference for how signing is called today.
- **Serial log tags** `id` and `se050` show what the identity code did.
- **Build:** arduino-cli, FQBN `esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom,CDCOnBoot=cdc` (README).

## 3. Scope

### In scope (Part 1)

| ID | Item | Priority |
| --- | --- | --- |
| A1 | Toolchain: build and flash Solana OS from source; confirm serial logs | P0 |
| A2 | Flash all 4 badges; read each pubkey and key location (R1 records them, U enters them in `server/config/badges.json`). If SE050 fails, capture the failing step from `se050` logs (time-box 1 h, then move on) | P0 |
| A3 | `wallet` Lua module skeleton exposing 00 §4; stubs return errors until implemented | P0 |
| A4 | Firmware-owned approval screen: modal, drawn by C, Lua drawing suspended, buttons read by C, fresh press required | P0 |
| A5 | Solana legacy-message parser + `transferChecked` decoder with the strict checks in §4.2 | P0 |
| A6 | Async `wallet.begin_solana` / `wallet.poll`: parse → decode → checks → approval → sign with identity key | P0 |
| A7 | Watchdogs: signing (~261 ms on SE050, unmeasured) extends the Lua deadline; the C approval task feeds the ESP-IDF task watchdog | P0 |
| A8 | SNTP at boot + `wallet.time_ok()`; clock floor from verified `issued_at` | P0 |
| A9 | Pinned issuer key + pinned HACK mint/decimals/symbol; `wallet.check_record` (Ed25519 verify over `registry:` + record, expiry, status, freshness) | P1 |
| A10 | `wallet.sign_request`, `wallet.sign_proof` (auto, domain-separated) | P1 |
| A11 | `wallet.new_nonce(req_id)` / `wallet.check_proof` with the presence state kept in C (up to 4 slots, keyed by `req_id`) | P1 |
| A12 | Firmware-side REQ verification inside `begin_*` (00 §4 step 2) and the amount/recipient comparison (steps 4–5) | P1 |
| A13 | `VK_DEV_ALLOW_UNVERIFIED` dev-build flag with a "DEV BUILD" banner, so R's harness can sign before the registry exists | P0 |

### Out of scope for Part 1
Bank payload decoder and `begin_bank` (P2-A), spending cap (P2-A), app UI (P2), any networking logic beyond what Lua already has.

## 4. Technical design

### 4.1 Module layout (suggested)
- `wallet_core.c/.h` — state machine (idle → pending → done), prefix enforcement, presence slots, calls identity sign.
- `wallet_decode.c/.h` — Solana message parser, `transferChecked` decoder, REQ parser.
- `wallet_ui.c/.h` — approval screen rendering + button handling, run from a C task, not from a Lua callback.
- `wallet_lua.c` — Lua bindings registering the `wallet` table.
Find the existing identity sign function (grep for the broker challenge signing) and call it; do not touch key generation.

### 4.2 Solana message parsing (legacy messages only)
```
header: num_required_signatures u8, num_readonly_signed u8, num_readonly_unsigned u8
account_keys: compact-u16 count, then count × 32 B
recent_blockhash: 32 B
instructions: compact-u16 count, each:
   program_id_index u8
   accounts: compact-u16 count, then count × u8 indices
   data: compact-u16 len, then len bytes
```
Reject → `undecodable` unless all of these hold (never blind-sign):
- Not a versioned message (first byte high bit `0x80` clear); total ≤ 1232 B; no trailing bytes; every index in range.
- `num_required_signatures == 1` and account 0 == `wallet.pubkey()`.
- Exactly one SPL Token `transferChecked`, optionally plus one Memo instruction with **zero accounts** and UTF-8 data.
- `transferChecked`: data exactly 10 B = `[12] ++ amount u64 LE ++ decimals u8`; exactly 4 accounts `[source, mint, destination, owner]`; owner == account 0; mint == pinned HACK mint; decimals == pinned decimals (2).

### 4.3 Recipient resolution (decided)
`transferChecked` names the destination **token account**, not the owner wallet. The registry record carries `solana_ata` (precomputed by the backend, signed by the issuer). Firmware compares the decoded destination to `record.solana_ata` byte for byte; different → red MISMATCH. On-badge ATA derivation (PDA + off-curve check) is a stretch item only.

### 4.4 Async approval and the screen
- `begin_*` validates the input and runs the 00 §4 checks.
  - Bad input → returns `nil, reason`.
  - Otherwise → it starts the approval task and returns `true`.
  - If an approval is already running → returns `nil, "busy"`.
- The approval task renders into the full screen, and Lua drawing is suspended while it is up (global UI lock).
  - Lines: rail · amount (decimals applied, from the decoded bytes) · recipient name and state (from the record) · short address.
  - A red MISMATCH screen also shows what was expected, e.g. "requested 1.00 HACK".
- SELECT signs, then `poll()` returns the sig.
  - CANCEL, or `APPROVAL_TIMEOUT_S` → `poll()` returns `nil, reason`.
  - Red states ignore SELECT.
- Require a fresh press: ignore any button already held when the screen appears (debounce + "release first"). Hold CANCEL for 2 s to force-close any app (master PRD risk table).

### 4.5 Prefix enforcement
- `sign_request` / `sign_proof` sign only `pay-req:`/`pay-proof:` + given bytes; never callable with raw bytes.
- `begin_solana` refuses input starting with any reserved ASCII prefix (also impossible structurally; see 00 §3).
- No other path to the identity key from Lua.

## 5. Interfaces you provide
`00-Interfaces.md` §4 exactly. Publish a short `wallet.md` with return codes once A6 works, so R and U can call it.

## 6. Dependencies
- From U: issuer public key (from `server/.keys/authority.json`), HACK mint address, at least one registry record from `GET /registry/:pubkey` for A9 testing.
- From R: R3 harness (a laptop that builds unsigned `transferChecked` messages and serves them over `/badge/pending`, then verifies the returned signature). Until then, build one test message by hand with `@solana/kit` or `solders`.

## 7. Done when
- [ ] All 4 badges flashed with your build; pubkey and key location recorded for each.
- [ ] R's harness serves an unsigned `transferChecked` message. The badge shows the correct amount and recipient. SELECT produces a signature that verifies on the laptop, and the submitted tx confirms on devnet. A dev build is allowed before A9.
- [ ] Each of these is refused as `undecodable`: a second (non-memo) instruction, a wrong mint, two signers, a versioned message, trailing bytes. R6 supplies the fuzz set.
- [ ] A Lua app cannot obtain a signature without the approval screen, and cannot make the screen green by lying in `ctx` (wrong record, wrong REQ, no proof).
- [ ] No reset of any watchdog during a 45 s approval wait or an SE050 signature.
- [ ] After a reboot with the hotspot up, `wallet.time_ok()` is true within 10 s.

## 8. Risks
| Risk | Mitigation |
| --- | --- |
| SE050 fails on silicon | Time-box; NVS key is fine for the demo; note it on the Badges view |
| Async approval task fights the Lua task for the display | Global UI lock; Lua `on_draw` skipped while approval is active |
| SNTP blocked on the hotspot | Clock floor from verified records; check on the venue hotspot early |
| Toolchain setup slow | Start A1 before the event if allowed; otherwise first hour |
