# Differences from `docs/specs/`

`docs/specs/` holds the team's product and track specifications, written before the OS was designed in detail. `docs/os/` is the build specification for the firmware and **wins wherever the two differ**. This page lists every difference so that the owners of the other tracks (R: test harness and payee flow; U: backend and dashboard) can see what changed for them.

Anything not listed here is unchanged from [`00-Interfaces.md`](../../specs/00-Interfaces.md): the registry record format (§7), the frame bytes of REQ, CHAL, PROOF and RESULT (§5), the signing prefixes (§3), the bank payload (§6), the decoder rules (P1-A §4.2).

## Lua wallet API (00 §4)

| `00-Interfaces.md` | BadgeOS | Why |
|---|---|---|
| `wallet.key_location()` returns `"se050"` or `"nvs"` | `"se050"`, `"software"` or `"none"` | matches upstream's and the dashboard's wording (`keyLocation: "software"`) |
| `wallet.sign_request(req_frame_bytes)`: Lua builds the frame, firmware signs | `wallet.request_open{amount=, ...}`: the firmware builds, signs, rebroadcasts and remembers the request | Lua cannot pack a u64; the firmware must know the active request to answer presence |
| `wallet.sign_proof(req_id, nonce, payer_pubkey)`: the payee's app answers CHAL | removed; the payee's **firmware** answers CHAL | faster and steadier latency; the app is not in the timing path |
| `wallet.new_nonce(req_id)` + app sends CHAL + `wallet.check_proof(...)` | `wallet.challenge(mac, req)` + `wallet.presence(req_id)`; the firmware sends CHAL and judges PROOF | timing measured from radio receive, not from when Lua ran |
| `begin_solana` / `begin_bank` only | also `wallet.begin(domain, bytes, ctx)`; the two old names remain | new signing domains need no new function |
| reasons: 8 strings | 19 ([reasons](reasons.md)) | a blocked approval reports its real cause |
| a payment with no REQ is red | **record-only payment is amber** ("VERIFIED - NOT PRESENT"), hold to approve | shops inside apps have no payee badge |
| amber: SELECT "enabled with warning" | amber is always hold-SELECT | one rule; stronger signal |
| — | new: `wallet.build_transfer`, `wire_tx`, `requests`, `tokens`, `config`, `balance`, `token_account`, `history`, `contacts`, `contact_*`, `request_close`, `request_status`, `provisioned`; module `badge.codec` | needed by the apps |

## Signing domains (00 §3)

Added: `contact:` (contact cards, auto) and `store-reg` (upstream's app-store registration text, auto, exact format checked). The others are unchanged.

## ESP-NOW (00 §5)

Frame bytes unchanged. Added: a type registry (1–15 payments, 16–31 contacts, 32–63 firmware, 64–255 apps), frame types 16 and 17, and a firmware router. CHAL and PROOF are no longer delivered to apps.

## Constants (00 §2, §4)

| `00-Interfaces.md` | BadgeOS |
|---|---|
| issuer key, mint, decimals and symbol **pinned in firmware** | provisioned config (`issuer_key`, `tokens`); nothing compiled in. Changing them later needs a confirmation on the badge |
| one payment token | a token table of up to 3, each with a cap and a max |
| `PRESENCE_DEADLINE_MS` 250 / 500 | config `presence_ms`, default 1500 until measured (M1) |
| `RECORD_TTL_S`, `APPROVAL_TIMEOUT_S` constants | config `record_ttl_s`, `approval_tmo_s` |
| `wallet.time_ok()` true after SNTP or a verified record | same, but a clock set only from a record caps the approval at amber ("CLOCK UNSYNCED") |

## Firmware design (P1-A §4)

| P1-A | BadgeOS |
|---|---|
| approval runs in a separate C task with a global UI lock | approval runs in the main loop while app code is paused ([overview](../architecture/overview.md#5-main-loop)); no second task |
| A7: the approval task feeds the task watchdog | not needed: there is no approval task, and the signature is made outside any Lua callback |
| module layout `wallet_core.c`, `wallet_decode.c`, `wallet_ui.c`, `wallet_lua.c` | `src/vk/` tree with features ([overview](../architecture/overview.md#4-source-tree)) |
| undecodable input → `begin` returns `nil, "undecodable"` | the red screen "CANNOT READ PAYMENT" is shown and `poll` returns `undecodable`, so the user sees that something was refused |
| hold CANCEL 2 s to force-close | upstream's 1.5 s (`APP_ESCAPE_HOLD_MS`) is kept |

## What other tracks need to do

| Track | Change |
|---|---|
| R (payee app, attack console) | the Request app calls `wallet.request_open`; there is no Lua handshake responder to write. The replay attack still works by rebroadcasting a captured REQ with `badge.espnow.broadcast`. The Python verifier is unaffected (frame bytes and prefixes are the same) |
| R (signing harness) | unchanged: serve unsigned legacy messages on `/badge/pending`; the Sign test app consumes them |
| U (backend) | build `GET /registry/<address>` exactly as in [backend](../integration/backend.md#registry-exact-behaviour), signing per request; set `ATTACK_TX_VERSION=legacy`; after `devnet:setup`, badges are provisioned with `vkdev.py provision` instead of reflashing with pinned values |
| U (dashboard) | `keyLocation` values are `se050` and `software` (already so); feed rows for record-only payments have no REQ |
