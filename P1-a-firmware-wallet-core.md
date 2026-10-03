# P1-A — Firmware: Wallet Core in Solana OS

Owner: **A** · Part 1 (≈ hours 0–8) · Gate: one badge-signed USDC payment confirmed on devnet
Reads: `00-interfaces.md` (§3 signing domains, §4 Lua API) · Parent: `PRD-verified-payment-key.md`

## 1. Why this exists

The product's security claim is that **only firmware below Lua can produce a payment signature, and only after showing the decoded payment on a screen the app cannot draw over.** Everything else (apps, backend, dashboard) is untrusted for signing. This spec builds that layer.

You are **extending** Solana OS, not writing an OS. Say it that way in the pitch.

## 2. What already exists (don't rebuild)

From the Solana OS README (https://github.com/spacemandev-git/solana-defcon-badge-26/blob/main/firmware/solana-os/README.md):

- Ed25519 identity key generated on first boot. Stored in the SE050 when it answers; otherwise NVS with TweetNaCl. Settings → Identity shows which.
- The SE050 path already includes APDU code for keygen, public key and EdDSA sign, and it self-tests by signing a probe and verifying with TweetNaCl (catches byte-order reversal). **It has never run on a real SE050**; likely failure points are an SE050 configured for SCP03 sessions or a variant without Ed25519. It fails closed to the NVS key.
- Lua 5.4 sandbox with a 250 ms per-callback watchdog; existing blocking calls extend the deadline (copy that pattern).
- `badge.espnow`, `badge.http`, `badge.gfx`, `badge.buttons`, `badge.storage`, `badge.se050` (presence/ATR/test/random only — **no sign binding**).
- Broker registration already signs a challenge with the identity key (useful reference for how signing is called today).
- Serial log tags `id` and `se050` show what the identity code did.

## 3. Scope

### In scope (Part 1)

| ID | Item | Priority |
| --- | --- | --- |
| A1 | Toolchain: build and flash Solana OS from source; confirm serial logs | P0 |
| A2 | SE050 check on all 4 badges; record key location; if it fails, capture the failing step from `se050` logs (time-box 1 h, then move on) | P0 |
| A3 | `wallet` Lua module skeleton exposing §4 functions; stubs return errors until implemented | P0 |
| A4 | Firmware-owned approval screen (modal, drawn by C, Lua cannot draw over it, buttons read by C) | P0 |
| A5 | Solana message parser + `transferChecked` decoder | P0 |
| A6 | `wallet.sign_solana` end to end: parse → decode → approval → sign with identity key → return sig | P0 |
| A7 | Watchdog extension during blocking approval and SE050 signing (~261 ms per SE050 signature) | P0 |
| A8 | `wallet.sign_request`, `wallet.sign_proof` (auto, domain-separated) | P1 |
| A9 | Pinned issuer key + `wallet.check_record` (Ed25519 verify, expiry, status, freshness) | P1 |
| A10 | `wallet.new_nonce` / `wallet.check_proof` (nonce + monotonic timestamp kept in C) | P1 |

### Out of scope for Part 1
Bank payload decoder (P2-A), spending cap (P2-A), app UI (P2), any networking logic beyond what Lua already has.

## 4. Technical design

### 4.1 Module layout (suggested)
- `wallet_core.c/.h` — state machine, prefix enforcement, calls identity sign.
- `wallet_decode.c/.h` — Solana message parser, `transferChecked` decoder.
- `wallet_ui.c/.h` — approval screen rendering + button wait loop.
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
- Reject versioned messages (first byte has high bit set `0x80`) → `undecodable`.
- Allowed instruction set for P1: exactly one SPL Token `transferChecked`, optionally plus one Memo instruction. Anything else → `undecodable` (never blind-sign).
- `transferChecked` = token program, data `[12] ++ amount u64 LE ++ decimals u8`; accounts `[source, mint, destination, owner]`.
- The payer must be signer index 0 and equal `wallet.pubkey()`; otherwise refuse.

### 4.3 Recipient resolution (important)
`transferChecked` names the destination **token account**, not the owner wallet. The registry record carries `solana_wallet` (owner). Two options:
1. **Recommended for 24 h:** Lua passes the expected destination ATA in `ctx.dest_ata`; firmware recomputes the ATA from `record.solana_wallet` + mint (SHA-256-based PDA derivation, `find_program_address` with seeds `[owner, token_program, mint]` against the ATA program `ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL`) and compares to the decoded destination. Needs an on-curve check for the bump search; TweetNaCl can't do that directly — use the ref10/curve point decode from the existing Ed25519 code or a small helper.
2. **Fallback:** U's registry record includes `solana_ata` (precomputed by the backend, signed by issuer). Firmware compares decoded destination to it directly. Zero crypto work. **Coordinate with U — if you pick this, add `solana_ata` to §7 of 00-interfaces.**

### 4.4 Approval screen
- Rendered by C into the full screen; Lua drawing is suspended while it is up.
- Lines: rail · amount (decimals applied, from decoded bytes) · recipient name + state (from `check_record`) · short address.
- Colors: green / amber / red per 00 §4. Red disables SELECT.
- SELECT → sign; CANCEL or `APPROVAL_TIMEOUT_S` → return nil, reason.
- Require a fresh press: ignore any button already held when the screen appears (debounce + "release first").

### 4.5 Prefix enforcement
- `sign_request` / `sign_proof` sign only `pay-req:`/`pay-proof:` + given bytes; never callable with raw bytes.
- `sign_solana` refuses input starting with any reserved ASCII prefix.
- No other path to the identity key from Lua.

## 5. Interfaces you provide
`00-interfaces.md` §4 exactly. Publish a short `wallet.md` with return codes once A6 works, so R and U can call it.

## 6. Dependencies
- From U: issuer public key (for A9); decision on §4.3 option; mint address.
- From R: a laptop script that builds an unsigned `transferChecked` message for a given badge and verifies the returned signature (P1-R R3). Until then, build one test message by hand with `solana-web3.js`/`solders`.

## 7. Done when
- [ ] All 4 badges flashed with your build; key location recorded for each.
- [ ] R's harness sends an unsigned `transferChecked` message → badge shows correct amount and recipient → SELECT → signature verifies on the laptop → submitted tx confirms on devnet.
- [ ] Message with a second (non-memo) instruction → refused as `undecodable`.
- [ ] Lua app cannot obtain a signature without the approval screen (try it).
- [ ] No watchdog reset during a 60 s approval wait or an SE050 signature.

## 8. Risks
| Risk | Mitigation |
| --- | --- |
| SE050 fails on silicon | Time-box; NVS key is fine for the demo; note it on the Badges view |
| ATA derivation eats hours | Use §4.3 fallback |
| Display contention with Lua | Take a global UI lock while the approval screen is up |
| Toolchain setup slow | Start A1 before the event if allowed; otherwise first hour |