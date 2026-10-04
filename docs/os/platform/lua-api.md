# Lua API

Everything BadgeOS adds to the `badge` table, and the shared Lua library `lib/vk.lua`. Upstream's own modules (`badge.gfx`, `input`, `led`, `system`, `storage`, `battery`, `mic`, `se050`, `wifi`, `http`, `espnow`, `ble`) are documented in upstream's `README.md` and are unchanged, except that some now need a permission ([app host](app-host.md#permissions)).

Conventions:

- A refusal returns `nil, "<reason>"` with a reason from [../reference/reasons.md](../reference/reasons.md). A wrong argument type raises a Lua error.
- **Amounts are strings** in display units (`"12.50"`). Lua numbers here are 32-bit integers and floats; never do arithmetic on token amounts.
- Binary values (keys, signatures, frames, messages) are Lua strings of raw bytes. Addresses returned for display are base58 strings. Use `badge.codec` to convert.
- "Permission" is what the app's `app.ini` must list. A function called without it raises `permission '<name>' not granted`.
- Unix times (`expiry`, `issued_at`, `time`, `added`) are Lua integers up to 2^31−1 and floats above that, because Lua integers here are 32-bit signed.

## `badge.wallet`: identity

No permission needed.

| Function | Returns |
|---|---|
| `wallet.pubkey()` | 32-byte string; `""` if the badge has no identity |
| `wallet.address()` | base58 string; `""` if the badge has no identity |
| `wallet.key_location()` | `"se050"`, `"software"` or `"none"` |
| `wallet.provisioned()` | boolean |
| `wallet.time_ok()` | boolean: the clock has a trusted source |
| `wallet.time()` | unix seconds from the firmware clock (`vk::clock::now()`), or `nil` when the clock has no trusted source. This is the clock the header and the checks read, whether it was set by SNTP, raised by a verified record or set with `VKTIME`; Lua's `os.time()` is the raw system time and is not. A float above 2^31−1, like every other time |
| `wallet.tokens()` | array of `{symbol, mint, decimals, cap, max}` (`mint` base58; `cap`, `max` strings in display units; a `cap` or `max` of zero, meaning none, reads `"0.00"` with the token's decimals) |
| `wallet.config(name)` | the text value of a config key, or `nil` if the key is not registered. A registered key with no value and no default returns `""`. All config values are public |
| `wallet.balance([symbol])` | last known balance of the default token as a string; `nil` if not fetched yet or if `symbol` is not the default token (feature `balance`) |
| `wallet.token_account([symbol])` | this badge's token account for the default token, base58; `nil` if not known yet or for another symbol |

## `badge.wallet`: payments

Permission `sign`.

| Function | Returns | Notes |
|---|---|---|
| `wallet.check_record(record, sig)` | `{ok, reason, display_name, device_pubkey, kind, solana_wallet, solana_ata, expiry, status, issued_at}` | Verifies the issuer signature, status and expiry. For list UIs only: the approval verifies the record again itself. Keys are base58; `kind` is `"merchant"` or `"person"`; `status` `"active"` or `"revoked"`. Details below |
| `wallet.build_transfer{destination=, amount=, blockhash=, [symbol=], [source=], [memo=], [req_id=]}` | message bytes | `destination`, `source`, `blockhash` base58. `source` defaults to `wallet.token_account(symbol)`; `symbol` defaults to the first token. `req_id` (16 hex characters, as `wallet.requests()` gives it) adds the request memo a payment to that request must carry ([request memo](../wallet/solana-payments.md#request-memo)). Fails with `bad_arg` or `unsupported`. Details below |
| `wallet.begin(domain, bytes, [ctx])` | `true` | Opens the firmware approval for a button domain. The app stops running until it closes |
| `wallet.begin_solana(msg, [ctx])` | `true` | Same as `wallet.begin("solana", msg, ctx)` |
| `wallet.begin_bank(payload, [ctx])` | `true` | Same as `wallet.begin("bank", payload, ctx)` (feature `bank`) |
| `wallet.poll()` | `"pending"` · a 64-byte signature · `nil, reason` | Call from `on_update` after `begin`. A result is returned once. With nothing begun: `nil, "idle"` |
| `wallet.wire_tx(sig, msg)` | base64 string | The wire transaction (`0x01 ‖ sig ‖ msg`), ready for the RPC method `sendTransaction`. `nil, "bad_arg"` when `sig` is not 64 bytes or `msg` is empty or longer than 1232 bytes |
| `wallet.requests()` | array of `{req, mac, rssi, name, amount, currency, rail, payee, req_id, age_ms, expires_in_s}` | Payment requests heard recently (feature `requests`). `req` is the raw frame to pass as `ctx.req`; `name` is the sender's *claim*; `req_id` is 16 hex characters; `mac` is `aa:bb:cc:dd:ee:ff`; `rail` is `"solana"` or `"bank"`; `payee` is base58; `amount` is a string in display units (the raw number if the badge has no token with that currency); `expires_in_s` is 0 when the clock has no source |
| `wallet.challenge(mac, req)` | `true` | Starts a presence check with the badge at `mac` for that request. `nil, "bad_arg"` for a bad MAC or frame; `nil, "sign_failed"` when the badge has no key. It returns `true` even if the radio refuses the frame; the slot then stays pending |
| `wallet.presence(req_id)` | `"none"`, `"pending"`, `"present"`, `"late"`, `"bad_sig"` | For the app's own UI. The approval reads the same result itself. An id that is not 16 hex characters gives `"none"` |

`ctx` is a table with any of `record` (registry record bytes), `record_sig` (64 bytes), `req` (REQ frame bytes). Nothing in it is trusted; the firmware verifies each part ([checks](../wallet/checks.md)). A field that is not a string raises a Lua error; a `record_sig` that is not exactly 64 bytes gives `nil, "bad_arg"`; an empty `record` or `req` counts as absent.

`wallet.check_record` always returns one table, never `nil, reason`:

- `reason` is `"ok"`, `"unverified"`, `"revoked"` or `"expired"`; revoked is judged before expired, as in the check chain. `ok` is true only for `"ok"`. Expiry is judged only when the clock has a source.
- Anything that cannot verify gives `{ok=false, reason="unverified"}` with no other fields: a wrong signature length, an empty or oversize record, a record that does not parse, a bad signature, or no `issuer_key`.
- The record's fields are present whenever the issuer signature verified, also for a revoked or expired record. `solana_wallet` and `solana_ata` are absent when the record has none.
- It does not check `record_ttl_s` freshness and does not raise the clock floor; only the approval does those.

`wallet.build_transfer`:

- A missing required field, or a field that is not a string (a numeric `amount` included), raises a Lua error.
- `bad_arg`: unknown `symbol`, bad base58, a bad or zero amount, source equal to destination, a memo that is not UTF-8 or is too long, a `req_id` that is not 16 hex characters, or a `req_id` together with a non-empty `memo`. An empty `memo` means no memo.
- **Paying a request:** pass `req_id = entry.req_id` and `ctx.req = entry.req` to `begin_solana`. Without the memo, or with any other memo, the approval is red WRONG MEMO (`mismatch`).
- `unsupported`: no token table, a default source that is not known yet, or no identity.
- A bad argument that was supplied is reported before a missing default.

`wallet.begin` is in `wallet/lua_wallet.cpp`; `wallet.begin_solana` and `wallet.begin_bank` are in their feature folders, and features may not include each other. All three parse `ctx` the same way by calling one function, declared in `src/vk/wallet/lua_wallet.h`:

```cpp
int vk::wallet::luaBegin(lua_State *L, const char *domain, int bytesIndex, int ctxIndex);
```

`bytesIndex` and `ctxIndex` are the Lua stack positions of the bytes and of the optional `ctx` table. It pushes `true` or `nil, reason` and returns the number of Lua results, so `begin_solana` is `return luaBegin(L, "solana", 1, 2);`.

Before calling `begin` it extends the callback deadline by 2500 ms once, once more when both `record` and `record_sig` are present, and once more when `req` is present (7500 ms for a full `ctx`; the two verifications it covers took about 0.85 s together with TweetNaCl and take about 40 ms with Monocypher, the backend since Batch 4).

The same header declares three helpers for bindings in feature folders: `int luaRefuse(lua_State *, Reason)` (pushes `nil, reason` and returns 2), `size_t base64Length(size_t)` and `void base64Encode(const uint8_t *, size_t, char *out)`. Base64 is implemented in `lua_wallet.cpp` because upstream's is in `src/identity/`, which features may not include.

`begin` reasons: `not_provisioned`, `busy`, `too_long`, `unsupported`, `bad_arg`. (A Lua app without the permission never reaches `begin`: the call raises an error. `denied` exists for native apps.) `poll` reasons: `cancelled`, `timeout`, `sign_failed`, and for a blocked (red) approval the cause: `undecodable`, `unverified`, `revoked`, `expired`, `mismatch`, `bad_proof`, `over_cap`, `over_daily` (the token's `day_limit`, [checks](../wallet/checks.md#daily-limit)), `no_time`. A red approval reports its cause however it was closed (CANCEL or the timeout).

## `badge.wallet`: requests

Permission `request`. Feature `requests`.

| Function | Returns | Notes |
|---|---|---|
| `wallet.request_open{amount=, [symbol=], [rail=], [name=], [ttl_s=]}` | `{req_id, frame, expiry}` | The firmware builds, signs and broadcasts the request and answers presence checks for it. `rail` is `"solana"` (default) or `"bank"`; `name` defaults to the badge's `display_name`, else upstream's device name, cut to 32 characters with non-ASCII as `?`; `ttl_s` defaults to `req_ttl_s`. Reasons, checked in this order: `not_provisioned`, `no_time`, `busy` (two are already open), `bad_arg` (also: a zero amount, more decimals than the token has, `ttl_s` outside 10 to 600, a symbol not in the token table), `sign_failed`. On the bank rail the currency is `symbol` or `USD`, with 2 decimals |
| `wallet.request_close(req_id)` | boolean | `false` for an id that is not 16 hex characters |
| `wallet.request_status(req_id)` | `{state, proofs}` or `nil` | `state` is `"open"`, `"closed"` or `"expired"`; `proofs` counts presence checks answered. The firmware remembers the last four closed requests; an older or malformed id gives `nil` |

`request_open` costs one signature (about 0.2 s with a software key) and does not turn ESP-NOW on: the app calls `badge.espnow.enable(true)` (upstream enables it at boot by default). A request belongs to the app that opened it and closes when that app stops.

The payer's RESULT frame arrives in `on_espnow` (needs `espnow`). It is unauthenticated: before showing "paid", fetch the transaction it names and check it with `wallet.verify_payment` ([below](#badgewallet-received-payments)); [`vk.receive`](#vkreceive) does all of it. A confirmed status alone (`vk.confirm`) proves only that *some* transaction landed.

## `badge.wallet`: received payments

The payee's check that a transaction pays its request ([solana-payments](../wallet/solana-payments.md#checking-a-received-payment)). Implemented in `src/vk/wallet/lua_wallet.cpp` (`verify_payment`) and `src/vk/features/history/lua_history.cpp` (`record_received`).

| Function | Permission | Returns |
|---|---|---|
| `wallet.verify_payment(tx, expected)` | none | `true, {payer, sig}` (both base58: the paying badge's key and the transaction signature), or `nil, reason, detail` |
| `wallet.record_received(tx, expected)` | `history` | `true`, or `nil, reason, detail`. Runs the same check, then writes a `received` row to the history from the transaction's own bytes. Calling it again for the same transaction writes nothing and returns `true` |

`tx` is the **raw wire transaction** (bytes, not base64): `badge.codec.b64dec(result.transaction[1])`, where `result` is the reply of the RPC call

```json
{"method": "getTransaction",
 "params": ["<signature base58>", {"encoding": "base64", "commitment": "confirmed", "maxSupportedTransactionVersion": 0}]}
```

`result.transaction` is the array `["<base64>", "base64"]`; `result` is null (Lua `nil`) until the transaction is confirmed. **The caller must also check `result.meta.err == nil`**: a failed transaction is on chain too, with the same bytes, and moved nothing. The signature to fetch is the RESULT frame's `ref` (`badge.codec.b58enc(ref)`).

`expected` is a table of strings:

| Field | Required | Meaning |
|---|---|---|
| `amount` | yes | the requested amount in display units (`"12.50"`), as passed to `request_open`. A string, never a number |
| `req_id` | yes | the request's id, 16 hex characters (`request_open(...).req_id`) |
| `symbol` | no | the token; default the first token of the table |
| `to` | no | the token account that must be credited, base58; default this badge's own (`wallet.token_account(symbol)`) |
| `sig` | no | the transaction signature the RESULT frame carried: its 64 raw bytes, or base58. When given, the fetched transaction must be that one |
| `payer` | no | base58 key the transaction's fee payer must be (the payer badge's key) |

A field of the wrong type (or a missing `amount` or `req_id`) raises a Lua error. Reasons, with `detail` saying which check failed (the order of the [table](../wallet/solana-payments.md#checking-a-received-payment)):

| Reason | `detail` |
|---|---|
| `bad_arg` | `symbol`, `amount`, `req_id`, `to`, `payer`, `sig`: that field is not valid |
| `unsupported` | `token` (no token table), `to` (this badge's token account is not known yet: call `wallet.refresh_balance()` first), `write` (`record_received` only: the history file could not be written) |
| `undecodable` | `wire` (not a one-signature transaction), `shape` (not one transfer plus at most one Memo) |
| `bad_proof` | `signature`: the transaction's signature does not verify over its message |
| `mismatch` | `sig`, `payer`, `mint`, `recipient`, `amount`, `memo`: that part is not what was expected |

Each call extends the callback deadline by 2500 ms for one Ed25519 verification (about 18 ms with Monocypher); `record_received` adds 1500 ms for the history scan and write.

What [`vk.receive`](#vkreceive) does for each reported transaction, in outline (an app uses `vk.receive`, not this):

```lua
-- after a RESULT with status 0 for request `req` (from request_open) and amount `amount`
local sig58 = badge.codec.b58enc(result.ref)
local tx = vk.rpc("getTransaction", {sig58, {encoding = "base64", commitment = vk.commitment,
                                             maxSupportedTransactionVersion = 0}})
if tx and tx.meta and tx.meta.err == nil then
  local raw = badge.codec.b64dec(tx.transaction[1])
  local ok, why, detail = badge.wallet.verify_payment(raw, {amount = amount, req_id = req.req_id, sig = result.ref})
  if ok then
    badge.wallet.record_received(raw, {amount = amount, req_id = req.req_id, sig = result.ref})   -- needs "history"
    -- PAID
  end
end
```

## `badge.wallet`: contacts

Permission `contacts`. Feature `contacts`. Frames are sent and received by the app with `badge.espnow` (needs `espnow`).

| Function | Returns | Notes |
|---|---|---|
| `wallet.contact_hello()` | HELLO frame bytes | Carries this badge's swap nonce (valid 60 s; the same frame is returned until it expires). Broadcast it about once a second. `nil, "sign_failed"` when the badge has no identity |
| `wallet.contact_card(hello_frame)` | CARD frame bytes | A card signed for the badge that sent that HELLO. Send it unicast to that badge. Reasons: `bad_arg`, `sign_failed` |
| `wallet.contact_accept(card_frame)` | `{name, address}` | Verifies the card against this badge's current nonce and key, saves the contact, rotates the nonce. Reasons: `bad_arg`, `mismatch` (card made for another badge), `expired` (nonce no longer current), `bad_proof` (signature), checked in that order; `unsupported` when the card is valid but the contacts file could not be written (the nonce is kept, so the same card can be tried again, and no note is posted) |
| `wallet.contacts()` | array of `{name, address, added}` | Oldest first. `added` is unix seconds, 0 when the clock had no source |
| `wallet.contact_remove(address)` | boolean | |

After a successful accept the nonce is invalid until the next `contact_hello()` draws a new one; every card in between is `expired`. A successful accept posts the note `Contact saved` / `Saved <name>` for app `contacts`, whichever app is running. The name in HELLO and CARD is config `display_name`, or upstream's device name when that is empty.

A contact's name is what its owner chose to call themselves. It is **never** shown on the approval screen; only an issuer-signed record's name is.

## `badge.wallet`: history

Permission `history` ("read and add to payment history"). Feature `history`.

| Function | Returns |
|---|---|
| `wallet.history([max])` | array, newest first, of `{kind, time, domain, outcome, reason, amount, symbol, name, address, app, sig, dev, req_id, count, items}`. `max` defaults to 20, limit 64 (rows, not signatures). The call extends the callback deadline by 1500 ms (one file read per entry) |
| `wallet.record_received(tx, expected)` | see [received payments](#badgewallet-received-payments) |

The fields of a `wallet.history` row ([stores](../wallet/stores.md#history)):

- `kind`: `"approval"` (an approval outcome), `"auto"` (signatures the badge made with no screen: payment requests, presence proofs, contact cards, store registration) or `"received"` (a payment `record_received` checked).
- `outcome`: `"signed"`, `"cancelled"`, `"timeout"`, `"blocked"`, `"failed"`, `"approved"` (a confirmation that is not a signature) or `"received"`. An auto row is `"signed"` or `"failed"` (the key refused).
- `amount` is always a string in display units (`"0"` for a confirmation and for an auto row); `symbol` and `name` may be `""`.
- `address` is base58, or `""` when there is none: the other party (the recipient of an approval, the payer of a received payment).
- `app` is the app id, cut to 15 characters.
- `sig` (base58) is present only for a signed approval and for a received payment (the transaction's signature); `req_id` (16 hex) only when the row answered or received a request; otherwise these are `nil`.
- `dev` is true if a dev-build override was used.
- Auto rows only: `count` (1 to 8 signatures in this row) and `items`, an array of `{time, digest}`: when each signature was made and the first 16 bytes of SHA-256 of what was signed, as 32 hex characters. `time` of the row is its newest item's.
- `time` is 0 when the clock had no source.

## `badge.wallet`: balance

| Function | Permission | Returns |
|---|---|---|
| `wallet.refresh_balance()` | `net` | `true`, or `nil, reason`. Fetches now (connecting and reading each get 3 s; the call extends the callback deadline by 7 s). Reasons: `not_provisioned` (no token table, `rpc_url` or key), `timeout` (no route, or the request failed), `unsupported` (the node answered with no usable account, or a status other than 200) |

## `badge.theme`

No permission needed. Lets an app match the active theme (light or dark).

| Function | Returns |
|---|---|
| `theme.name()` | `"receipt-light"` or `"receipt-dark"` (or another registered theme) |
| `theme.color(token)` | the RGB565 colour for `"paper"`, `"ink"`, `"faint"`, `"sub"`, `"stamp_ok"`, `"stamp_warn"`, `"stamp_bad"` (the status inks for signed, warning and blocked text; nothing draws a stamp), `"led"`; also the fixed `"green"`, `"amber"`, `"red"`. `nil` for any other name |

## `badge.receipt`

No permission needed. The firmware's receipt kit ([ui](../ui/ui.md#the-receipt-kit)), so that a Lua app's screen is drawn by the same code and in the same fonts as the shell's. Implemented in `src/vk/ui/lua_receipt.cpp`: each function is a thin binding over the `vk::ui::receipt` function of the same name and draws into the canvas with the active theme's colours. Apps normally reach them through `vk.ui` ([below](#libvklua)), which has the same picture as a fallback for a firmware without this module.

| Function | Draws |
|---|---|
| `receipt.page()` | the paper: fills the screen |
| `receipt.header(left, [right])` | text at y 7 and the dashed rule at y 19. `right` nil: the time and the battery, as the shell's header (`receipt::statusRight`) |
| `receipt.title(text, y, [cx])` | a title in the kit's title font, centred on `cx` (default 160) |
| `receipt.rule(y, [x0, x1])` | a dashed line (defaults 10 and 310) |
| `receipt.perforation(x, y0, y1)` | the dashed vertical line between a stub and its body |
| `receipt.row(x0, x1, y, label, value, [selected], [value_token])` | label left, value right, dotted leader between; `selected` inverts the row. `value_token` colours the value: a `badge.theme.color` name (`"stamp_ok"`, `"stamp_warn"`, `"stamp_bad"`, `"faint"`, …, mapped in C; a name that is not a colour leaves the row's ink), or an RGB565 number |
| `receipt.subline(x0, x1, y, text, [selected])` | the second line of a row, at the row's y + 13 |
| `receipt.amount(cx, y, label, value, unit)` | label, the value in the large serif (never cut: a smaller font is chosen), unit; centred on `cx` |
| `receipt.barcode(x, y, w, h, [seed])` | bars from the bytes of `seed`; by default this badge's public key |
| `receipt.footer(left, right)` | clears the strip under y 216, the rule, text at y 224 |
| `receipt.hold_bar(progress)` | the hold bar at y 204, `progress` from 0 to 1 |
| `receipt.qr(x, y, size, text)` | a QR code of `text` centred in a light `size` × `size` square, in both themes ([the QR code](../ui/ui.md#the-receipt-kit)). Returns `true`, or `false` when nothing was drawn (an empty text, or one over 154 bytes) |

A `y` is the top of the capital letters, as everywhere in the kit. What an app passes cannot hurt the firmware: an argument of the wrong type raises a Lua error (`luaL_check*`); coordinates may be integers or floats and are clamped to one screen beyond each edge (x −320 to 640, y −240 to 480), so no drawing loop runs long; a text is cut to 160 bytes before the kit sees it (which then cuts it to its space with `..`), may be a number, and may be nil, which is the empty text. The module is additive: the API version stays 2, and an app that must run on an older firmware checks `badge.receipt` for nil, as `vk.ui` does.

## `badge.screen`

No permission needed: keeping the screen lit costs battery, not trust, and it ends with the app. Implemented in `src/vk/ui/lua_screen.cpp` ([screen dim and sleep](../ui/ui.md#screen-dim-and-sleep)).

| Function | Does |
|---|---|
| `screen.keep_awake(on)` | while `on` is true the screen neither dims nor sleeps (a game being played, a code being shown); `false` lets the timers run again, starting from that moment. Cleared when the app stops. Returns nothing |

An app need not call it to stay usable: any key wakes the screen. Without it, the first key pressed on a dimmed or dark screen only wakes it and never reaches `on_button`. The module is additive (API version 2); an app for an older firmware checks `badge.screen` for nil.

## `badge.codec`

No permission needed. Implemented in `src/vk/wallet/lua_wallet.cpp`.

| Function | Returns |
|---|---|
| `codec.b64enc(bytes)` / `codec.b64dec(text)` | string / bytes or `nil` |
| `codec.b58enc(bytes)` / `codec.b58dec(text)` | string / bytes or `nil` |
| `codec.hex(bytes)` / `codec.unhex(text)` | lower-case hex / bytes or `nil` |

`b58enc` and `b58dec` handle at most 64 bytes (a longer input gives `nil`). `b64dec` is strict: standard alphabet, padded, no whitespace, unused bits zero. `unhex` accepts both cases. The empty string encodes and decodes to the empty string in all three.

## `lib/vk.lua`

Shared, pure Lua. Source: `os/lib/vk.lua`. Upstream's push can write only under `/apps/<id>/`, so `scripts/push-apps.sh` copies this file into each app's folder as `vk.lua` when it pushes the app; `require` finds it there first. `local vk = require("vk")`.

| Function | Permission the app needs | Does |
|---|---|---|
| `vk.json.decode(text)` | — | JSON text → Lua tables, or `nil, message` for malformed text. Numbers become Lua numbers (do not rely on them for amounts; RPC amounts are strings). A `null` object field is left out (so `reply.error` is nil); a `null` array element is `vk.json.null`, so arrays have no holes |
| `vk.json.encode(value)` | — | Lua value → JSON text, or `nil, message` for a value JSON cannot hold. A plain empty table encodes as `{}`; `vk.json.array(t)` marks a table as an array, so an empty one encodes as `[]` (decoded arrays are marked the same way) |
| `vk.rpc(method, params)` | `net` | POSTs a JSON-RPC 2.0 call to `wallet.config("rpc_url")`; returns the `result` table, or `nil, message`. A null `result` gives `nil, "rpc reply has no result"` |
| `vk.blockhash()` | `net` | recent blockhash, base58 (`getLatestBlockhash`, commitment `vk.commitment`) |
| `vk.send_tx(wire_b64)` | `net` | `sendTransaction` with base64 encoding and `vk.commitment` as the preflight commitment (so the preflight knows the blockhash `vk.blockhash` returned); returns the signature (base58) |
| `vk.confirm(sig)` | `net` | `"confirmed"`, `"pending"` or `"failed"` (`getSignatureStatuses`). `sig` is base58, or the 64 raw bytes a RESULT frame's `ref` carries. It says only that *some* transaction landed: a payee checks a RESULT with `vk.receive`, never with this |
| `vk.record(address)` | `net` | `GET <listener_url>/registry/<address>`; returns `record, sig` (bytes), or `nil, reason`: `"unverified"` for a 404 whose JSON body names the missing record (`{"error":{"message":"no registry record for this key"}}`, or any error text with "record" or "attestation" in it), `"registry_missing"` for any other 404 (the listener has no registry route: not an impostor warning, and not a record either), a transport code on no answer |
| `vk.report{payee=, reason=, [req=], [payer=]}` | `net` | `POST <listener_url>/feed/event` (a badge-side refusal for the dashboard feed). `payer` defaults to `wallet.address()`; a raw REQ frame in `req` is sent as base64. Only `unverified`, `revoked`, `expired`, `mismatch` and `bad_proof` are posted ([reasons](../reference/reasons.md)); any other reason gives `nil, "not reported"` and no request |
| `vk.frame_type(data)` | — | the VK frame type of an ESP-NOW payload, or `nil`. Compare it with `vk.T_RESULT` (4), `vk.T_CONTACT_HELLO` (16) or `vk.T_CONTACT_CARD` (17) |
| `vk.result_frame(req_id_hex, status, sig_bytes)` | — | RESULT frame bytes |
| `vk.result_parse(data)` | — | `{req_id, status, ref}` (`req_id` hex, `ref` bytes), or `nil` if not a RESULT frame |
| `vk.hello_parse(data)` | — | `{address, name}` from a CONTACT_HELLO frame, or `nil` |
| `vk.feed(sig_b58, req)` | `net` | `POST <listener_url>/feed/solana` after a confirmed request payment. With no `req` (a shop payment) nothing is posted, because the backend refuses `req: null`: it returns `nil, "no request"` and logs `vk feed skipped: no request (the backend refuses req null)` |
| `vk.app_frame(type, body)` / `vk.app_body(data, type)` | — | build / match an app-range frame |
| `vk.pay.start(opts)` | `sign`, `net`, `espnow` | starts the whole payer flow; returns a flow object (below) |
| `vk.receive.start(opts)` | `net` (`history` to record) | the payee's check of RESULT frames before PAID; returns a watch object ([below](#vkreceive)) |
| `vk.reason_text(reason, [context])`, `vk.reasons`, `vk.offline(reason)` | — | one text for every reason, shared by every app ([below](#reason-texts)) |
| `vk.peers.new{max=, timeout_ms=, hold_ms=}` | — | badges heard nearby by MAC, for a swap list ([below](#vkpeers)) |
| `vk.keep_awake(on)` | — | `badge.screen.keep_awake(on)`, or nothing on a firmware without it |
| `vk.led_pulse(token, ms)` | — | the LEDs in a theme colour (`vk.ui.color(token)` turned from RGB565 into 0..255 per channel) |
| `vk.ui.page()`, `vk.ui.header(left, right)`, `vk.ui.title(text, y)`, `vk.ui.rule(y)`, `vk.ui.row(y, label, value, selected)`, `vk.ui.subline(y, text, selected)`, `vk.ui.amount(cx, y, label, value, unit)`, `vk.ui.footer(left, right)`, `vk.ui.list(model)` | — | the receipt look for Lua apps. Each function calls the firmware's kit through [`badge.receipt`](#badgereceipt) when the firmware has it, and otherwise draws the same picture with `badge.gfx` in the active theme's colours; same geometry as the firmware's receipt kit ([ui](../ui/ui.md#the-receipt-kit)) either way. `vk.ui.list{title=, rows={{l=, r=, sub=, tone=}}, sel=, hint=}` draws a whole list screen |
| `vk.ui.qr(x, y, size, text)` | — | a QR code of `text` (a link) on a light patch, through `badge.receipt.qr`. Returns `true` when it drew; `false`, with nothing drawn, on a firmware without the kit's QR code, for an empty text and for one over 154 bytes. Home shows `wallet.config("repo_url")` with it |
| `vk.ui.frame(draw, [period_ms])`, `vk.ui.dirty()` | — | draw only when the screen changed (below, "Drawing only when something changed") |
| `vk.short(text)`, `vk.timeout_ms`, `vk.commitment`, `vk.RESULT_OK` / `RESULT_REJECTED` / `RESULT_FAILED`, `vk.last_error` | — | first 4 + `..` + last 4 of an address; the timeout of one HTTP request (4000); the commitment used for the blockhash, the send preflight, the confirmation and `getTransaction` (`"confirmed"`); RESULT status 0, 1, 2; the transport's own message of the last request that got no answer |

Every network helper returns `nil, message` on any failure; nothing loops or raises. When a request gets no answer at all, the message is one of these codes, so that an app can say what to do next (the transport's own text is in `vk.last_error`):

| Code | When | Text says |
|---|---|---|
| `no_wifi` | not joined: `badge.http` has no route (`"wifi not connected"`, `"no network"`) | join Wi-Fi in Settings > Wi-Fi |
| `no_route` | the phone bridge, the only route, failed (`"bridge unavailable"`, `"bridge failed"`) | join Wi-Fi in Settings > Wi-Fi |
| `rpc_unreachable` | a route, but the node at `rpc_url` did not answer (refused, timed out, lost) | check the hotspot's internet |
| `listener_unreachable` | a route, but the laptop at `listener_url` did not answer | start the dashboard |
| `bad_url` | `"bad url"`: the key's value is not a usable address | fix it over USB |

`vk.offline(code)` is true for the first four. A registry fetch that times out is `listener_unreachable`: it reads as the network, never as an impostor. A listener reply with status 2xx whose JSON body is `{"ok": false, ...}` is a refusal: `vk.report` and `vk.feed` return `nil` and its `reason` (`"listener refused"` when it has none). Other messages are unchanged: `"<key> not set"`, `"rpc http N"`, `"rpc reply not understood"`, `"rpc reply has no result"`, a node's own error message, `"registry http N"`, `"registry reply not understood"`, `"listener http N"`, `"not reported"`, `"no request"`.

`vk.ui`, beyond the table:

- `row` takes optional `value_color, x0, x1` after `selected`; `subline` takes optional `x0, x1`; `title` takes an optional centre `cx`; `rule` takes optional `x0, x1`.
- `header(left)` with no right text shows `vk.ui.status()`: the time and the battery, as the firmware's `receipt::statusRight` builds it. The time comes from `wallet.time()`, so it is left out exactly when the firmware's own header leaves it out.
- Also: `ui.perforation(x, y0, y1)`, `ui.barcode(x, y, w, h, [seed])` (the kit's bars; the seed defaults to `wallet.pubkey()`), `ui.color(token)` (`badge.theme.color` with the Receipt-light values as the fallback when `badge.theme` is absent), `ui.text`, `ui.text_center`, and the layout constants `ui.W`, `MARGIN`, `CONTENT_Y`, `ROW_PITCH`, `SUB_PITCH`, `SPLIT_X`, `STUB_CX`, `TITLE_Y`, `LIST_Y`.
- `list` draws the title and the labels in capitals, takes a 1-based `sel` (nil selects none) and scrolls to keep it in view, and also reads `header` (left header text), `back` (right footer text, default `CANCEL back`) and `empty` (a line shown when there are no rows). `tone` is `"ok"`, `"warn"`, `"bad"` or `"mut"`. Geometry is the native lists' ([ui](../ui/ui.md#screens)).
- Through `badge.receipt` a title is the kit's own title font and an amount its large serif, as on the shell's screens. Only in the fallback (no `badge.receipt`) is a title the built-in font at size 2 (13 px per character, 14 px capitals; the kit's title has 11 px capitals) and an amount the built-in font at size 2 to 4.
- The fallback is chosen per function and per call (`badge.receipt` and the function's name are looked up each time), so `lib/vk.lua` runs unchanged on an older firmware and in the host suite `test_vk.lua`, which checks both paths. `ui.text`, `ui.text_center` and `ui.color` have no kit function and always draw with `badge.gfx`.
- `list` defaults its header to `BADGEOS`.

#### Drawing only when something changed

The runtime calls `on_update` and `on_draw` on every loop pass, with no frame limit. A full frame costs about 15 ms of `vk.ui` drawing (through `badge.receipt`; about 20 ms with the pure-Lua fallback) and a 34 ms transfer of the canvas to the panel, so an app that draws on every pass holds the loop at about 20 passes a second (measured: 45 to 53 ms a pass), and the badge feels slow. Every shipped app therefore keeps its drawing in one function and lets `vk.ui.frame` decide:

```lua
local function draw() ... end                       -- the whole screen
function on_draw() vk.ui.frame(draw) end
```

`vk.ui.frame(draw, [period_ms])` calls `draw()` and returns true when the picture can have changed, and otherwise returns false without touching the canvas (a pass then costs about 1 ms and nothing is sent to the panel):

| Draws when | Why |
|---|---|
| `vk.ui.dirty()` was called since the last frame | the app changed something it shows (a frame arrived, a network step finished) |
| a key is down, or was down on the last pass (`badge.input.any()`) | key handlers change state; a held key scrolls a list |
| `on_draw` was not called for `vk.ui.pause_ms` (150) | an approval was up: the canvas holds its picture, not the app's |
| `period_ms` passed since the last frame; the default is `vk.ui.frame_ms` (250) | clocks and countdowns, and any state change whose code did not call `dirty()` |

`period_ms` 0 draws on every pass: use it for an animation (the Game's playfield does) and for nothing else. A state change that must be on the screen at once calls `vk.ui.dirty()` (Duel does when its state changes, so the flash is not late). An app that calls `on_draw` code directly, without `frame`, still works; it is only slow.

### `vk.pay`

The payer flow as a small state machine, so an app that takes payments is a few lines. `opts` is one of:

- `{request = entry}`: pay a request from `wallet.requests()` (challenge, record, build, approve, submit, RESULT);
- `{to = address, amount = "5.00", symbol = "HACK", memo = "..."}`: pay a registered recipient with no request (a shop; the approval will be amber).

A request payment carries the request memo: the flow passes `req_id = request.req_id` to `wallet.build_transfer` and no free `memo` (a `memo` option is ignored with a request), so the firmware writes the memo the approval (check 12a) and the payee (`verify_payment`) look for.

```lua
local flow = vk.pay.start{ to = SHOP, amount = "5.00", memo = "sword" }
function on_update(dt)
  local state, detail = flow:update()   -- call every frame
  -- state: "presence" | "record" | "blockhash" | "approve" | "submit" | "confirm" | "done" | "failed"
  -- detail: on "done" the transaction signature (base58); on "failed" the reason
end
```

`flow:update()` does at most one blocking step per call. During `"approve"` the app is paused by the firmware and resumes when the user has decided. If `wallet.token_account()` is nil the flow first calls `wallet.refresh_balance()` (its `timeout` becomes `no_wifi` when `badge.wifi.connected()` is false, else `rpc_unreachable`). After `"confirm"` succeeds it calls `vk.feed` (failures there are ignored; a payment with no request is not fed) and, when paying a request, sends the RESULT frame.

`flow:reason()` is the reason to show once the flow failed (nil otherwise): `flow.detail`, except that a red UNVERIFIED RECIPIENT whose record fetch got `registry_missing` is `"registry_missing"`, so a missing route does not read as an impostor. The approval was red either way: with no record nothing can be signed.

As built (WP35):

- `pay.start` with bad options returns a flow that is already `"failed"` with `bad_arg` (`unsupported` for a request on the bank rail); it never raises.
- `refresh_balance` runs in the first `update()`, before presence or record.
- Presence: at most 2 challenges, each waited `presence_ms` + 300 ms; with no proof the flow goes on and the approval is amber.
- No record (404): the flow continues with no record and builds the transfer to the address itself, so the firmware shows red UNVERIFIED RECIPIENT, as the attack table in [protocol](../protocol/espnow.md) describes. With a record the destination is the record's `solana_ata`, read from the record text; `wallet.check_record` is not called (the approval verifies the record itself, and one verification is saved).
- `begin` answering `busy` is retried 10 times, 300 ms apart; send is tried twice; confirm polls every 2 s for 30 s. The tunables are fields of `vk.pay` (`presence_tries`, `presence_margin_ms`, `begin_tries`, `begin_retry_ms`, `send_tries`, `send_retry_ms`, `confirm_every_ms`, `confirm_for_ms`).
- On `"failed"` the detail is a reason code, or the message of the network call that failed (`"transaction failed"` and `"not confirmed"` for the chain outcomes).
- **Fields of the flow object an app may read** (supported: the shipped apps do). Never write them.

  | Field | Value |
  |---|---|
  | `flow.state` | the state `update()` last returned |
  | `flow.detail` | on `"done"` the transaction signature (base58); on `"failed"` the reason; otherwise nil |
  | `flow.ready` | false until the first `update()` has run its balance step (`refresh_balance` when the token account is unknown), then true. Game shows "starting" until then |
  | `flow.sig` | the 64 signature bytes, once the approval signed; nil before. This is what a RESULT frame's `ref` carries |
  | `flow.signature` | the same signature in base58 (what `vk.confirm` and a block explorer take); nil before signing |
  | `flow.result_sent` | true once the flow has sent a RESULT frame to the payee (request payments only). An app that abandons a flow early checks it to decide whether to send a RESULT itself (Duel does) |
  | `flow.request` | the request being paid (`opts.request`), or nil for a shop payment |
  | `flow.failed_in` | on `"failed"`, the state it failed in |
  | `flow.record_miss` | `"unverified"` or `"registry_missing"` when the record fetch got a 404 (the flow then went on with no record); nil otherwise |

  `flow.sig` and `flow.signature` are the same signature in two encodings, not two spellings of one field.
- When paying a request, RESULT is also sent on failure: status 1 (rejected) when the approval did not sign, 2 (failed) when send or the chain failed, and 0 with the signature when the node accepted the transaction but it was not seen confirmed within 30 s (the payee confirms on chain itself).
- Option `destination` overrides the token account the transfer is built to (the evil game's WRONG RECIPIENT demo).
- A shop payment (`to=`) never touches `badge.espnow`, so it works for an app without the `espnow` permission.

### `vk.receive`

The payee's half: turns RESULT frames into PAID only for a transaction that pays this request. Duel's winner and the Request app use it.

```lua
local watch = vk.receive.start{ req_id = req.req_id, amount = "10.00" }   -- req from wallet.request_open
function on_espnow(mac, data) watch:result(data) end   -- any frame; others are ignored
function on_update(dt)
  local state, detail = watch:update()                 -- every frame
  -- "waiting" | "pending" | "paid" (detail {payer, sig}) | "failed" (detail: the reason)
end
```

Options: `req_id` (16 hex) and `amount` (a string) are required; `symbol`, `to`, `payer` are passed to `wallet.verify_payment` as given ([received payments](#badgewallet-received-payments)); `record` (default true) writes the received row with `wallet.record_received`; `every_ms` and `for_ms` override `vk.receive.every_ms` (2000) and `for_ms` (30000). Bad options give a watch that is already `"failed"` with `bad_arg` (`unsupported` on a firmware without `verify_payment`); it never raises.

- `watch:result(data)` takes a RESULT frame (or a table from `vk.result_parse`) for this request. Status 1 and 2 are only noted in `watch.reported`: anyone can send them. Status 0 with a non-zero `ref` queues that transaction signature and returns true; a ref already queued or already refused returns false.
- `watch:update()` makes at most one blocking call: one `getTransaction` (`encoding` base64, `commitment` `vk.commitment`, `maxSupportedTransactionVersion` 0), or one `wallet.refresh_balance()` when `verify_payment` answers `unsupported`/`to` (this badge's token account is not known yet; once per ref), or the check itself (`wallet.verify_payment` on the raw bytes, about 18 ms, then `wallet.record_received`). A `null` result means not visible yet; each ref is looked up every `every_ms` for up to `for_ms` from its first look-up, so a transaction that takes a few seconds to appear is still found.
- **Paid** only when the transaction was fetched, `meta.err` is null, and `verify_payment` returned true. `detail` and `watch.payer`, `watch.sig` are the verified payer and signature (base58). `watch.recorded` is true when the history row was written; a failure there (no `history` permission, the file could not be written) leaves the payment paid and puts the reason in `watch.record_error`. Paid is final: later RESULTs are ignored.
- **Several RESULTs.** Up to `vk.receive.slots` (3) refs are held and looked up in turn (each update takes the next one that is due), so a forged RESULT naming a junk ref does not delay the real one by its window; the first ref that verifies wins. A fourth ref is still taken: the held ref tried most often (the oldest on a tie) makes room. A ref refused for `mismatch`, `bad_proof`, `undecodable` or `"transaction failed"` is remembered (the last 8) and never tried again.
- **Failed** when no ref is left to check; `detail` (and `watch.last_reason`, `watch.last_detail`) is why the last one was refused: `verify_payment`'s reason, `"transaction failed"` (`meta.err`), `"not confirmed"` (never visible in `for_ms`), or a network code. It is not final: another RESULT goes back to `"pending"`, and `watch:again()` re-queues the refs given up for a reason that may pass (not found, no network; `watch:retryable()` counts them). `watch.fatal` is true only for a watch that cannot work (`bad_arg`, `unsupported`).
- `watch:pending()` is the number of refs held; `watch:checking()` the base58 signature of the next one.

What it cannot stop: a sender that keeps sending fresh junk refs faster than they are checked delays PAID (a denial of service, as in [protocol limits](../protocol/espnow.md#limits)). Nothing it sends makes PAID appear. A lying RPC node is out of reach of any badge-side check.

### Reason texts

`vk.reason_text(reason, [context])` returns a sentence for the user, at most `vk.REASON_MAX` (96) characters, with the next step when there is one. Pay, Request, History and Contacts use it and keep no table of their own; Duel and Game (and so Evil game) still have their own until they are moved to it. `reason` is any code of [reasons](../reference/reasons.md) (`vk.reasons` has a text for each, `over_daily` and `low_battery` included), a code of this library (the no-network codes above, `bad_url`, `registry_missing`, `"transaction failed"`, `"not confirmed"`), or a helper's message: `"<key> not set"`, `"rpc http N"`, `"registry http N"` and `"listener http N"` get a text with the key or the status in it; anything else (a node's own message) is returned as it is.

`context` picks other words for codes whose meaning depends on who asks: `"receive"` (the payee checking a transaction: `mismatch` is "That transaction is not this payment."), `"request"` (`request_open`), `"contact"` (`contact_accept`, `contact_card`), `"short"` (a few words for a row's value, as History uses). It may instead be a table of the app's own overrides. Missing words fall back to the common table. The context tables are `vk.reason_contexts`.

`registry_missing` and `unverified` have different texts: the first says the listener has no registry and the recipient could not be checked; only the second warns of an impostor. Both came from a red approval.

### `vk.peers`

A list of badges heard nearby by MAC, strongest signal first (`list.items`), for Contacts' swap screen.

| Call | Does |
|---|---|
| `list:heard(mac, {address=, name=, frame=, rssi=}, now, [keep_mac])` | returns `peer, how`. `how` is `"new"`; `"updated"`; `"ignored"` (a listed MAC with another key: a made-up HELLO cannot rename or re-key a listed badge); `"held"` (the same key with a new frame while a card sent to it is pending: the frame is kept); `"full"` (every listed badge is kept, see below; the call returns `nil`) |
| `list:sent(mac, now)` | a card went to that badge: its frame is held for `hold_ms` (default 10000) |
| `list:expire(now)` | drops badges not heard for `timeout_ms` (default 4000) |
| `list:find(mac)` | the peer and its index, or nil |

When the list holds `max` (default 4) badges, a new one takes the place of the one heard longest ago, except `keep_mac` (the selected badge) and badges with a card pending. A new badge is therefore never refused while one of those can leave, and made-up HELLOs from many MACs cannot lock an honest badge out: it is listed again on its next HELLO.

## The smallest paying app

`apps/tipjar/app.ini`:

```ini
name=Tip jar
version=1.0.0
permissions=sign,net,espnow
min_api=2
```

`apps/tipjar/main.lua`:

```lua
local vk = require("vk")
local gfx, wallet = badge.gfx, badge.wallet
local RECIPIENT = "Gn2GQYmcQkatE2594ov2ELKkYWZHwARG7FiAoPWSEcxq"   -- must have a registry record
local flow, line = nil, "SELECT to tip 1.00"

function on_button(key, pressed)
  if pressed and key == "a" and not flow then
    flow = vk.pay.start{ to = RECIPIENT, amount = "1.00" }
  elseif pressed and key == "b" then
    badge.system.exit()
  end
end

function on_update(dt)
  if not flow then return end
  local state, detail = flow:update()
  line = state
  if state == "done" then line = "Thank you!"; flow = nil end
  if state == "failed" then line = "Not sent: " .. detail; flow = nil end
end

function on_draw()
  gfx.clear()
  gfx.text_center("Tip jar", 160, 60, gfx.WHITE, 2)
  gfx.text_center(line, 160, 120, gfx.SOLANA_GREEN, 1)
end
```

The app never sees a key, cannot change what the approval shows, and cannot approve for the user.

## Adding a Lua function

One line next to the C++ implementation, in the feature that owns it:

```cpp
static int l_ping(lua_State *L) { lua_pushstring(L, "pong"); return 1; }
VK_LUA_FUNCTION(ping, "wallet", "ping", nullptr, l_ping);     // badge.wallet.ping(), no permission
```

Then add the row to this document and bump the API version if apps need to detect it ([app host](app-host.md#api-version)).
