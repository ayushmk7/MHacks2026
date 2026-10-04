# Lua API

Everything Badge OS adds to the `badge` table, and the shared Lua library `lib/vk.lua`. Upstream's own modules (`badge.gfx`, `input`, `led`, `system`, `storage`, `battery`, `mic`, `se050`, `wifi`, `http`, `espnow`, `ble`) are documented in upstream's `README.md` and are unchanged, except that some now need a permission ([app host](app-host.md#permissions)).

Conventions:

- A refusal returns `nil, "<reason>"` with a reason from [../reference/reasons.md](../reference/reasons.md). A wrong argument type raises a Lua error.
- **Amounts are strings** in display units (`"12.50"`). Lua numbers here are 32-bit integers and floats; never do arithmetic on token amounts.
- Binary values (keys, signatures, frames, messages) are Lua strings of raw bytes. Addresses returned for display are base58 strings. Use `badge.codec` to convert.
- "Permission" is what the app's `app.ini` must list. A function called without it raises `permission '<name>' not granted`.

## `badge.wallet`: identity

No permission needed.

| Function | Returns |
|---|---|
| `wallet.pubkey()` | 32-byte string |
| `wallet.address()` | base58 string |
| `wallet.key_location()` | `"se050"`, `"software"` or `"none"` |
| `wallet.provisioned()` | boolean |
| `wallet.time_ok()` | boolean: the clock has a trusted source |
| `wallet.tokens()` | array of `{symbol, mint, decimals, cap, max}` (`mint` base58; `cap`, `max` strings) |
| `wallet.config(name)` | the text value of a config key, or `nil` if unknown. All config values are public |
| `wallet.balance([symbol])` | last known balance of the default token as a string; `nil` if not fetched yet or if `symbol` is not the default token (feature `balance`) |
| `wallet.token_account([symbol])` | this badge's token account for the default token, base58; `nil` if not known yet or for another symbol |

## `badge.wallet`: payments

Permission `sign`.

| Function | Returns | Notes |
|---|---|---|
| `wallet.check_record(record, sig)` | `{ok, reason, display_name, device_pubkey, kind, solana_wallet, solana_ata, expiry, status, issued_at}` | Verifies the issuer signature, status and expiry. For list UIs only: the approval verifies the record again itself. Keys are base58; `kind` is `"merchant"` or `"person"`; `status` `"active"` or `"revoked"` |
| `wallet.build_transfer{destination=, amount=, blockhash=, [symbol=], [source=], [memo=]}` | message bytes | `destination`, `source`, `blockhash` base58. `source` defaults to `wallet.token_account(symbol)`; `symbol` defaults to the first token. Fails with `bad_arg` or, when the source account is unknown, `unsupported` |
| `wallet.begin(domain, bytes, [ctx])` | `true` | Opens the firmware approval for a button domain. The app stops running until it closes |
| `wallet.begin_solana(msg, [ctx])` | `true` | Same as `wallet.begin("solana", msg, ctx)` |
| `wallet.begin_bank(payload, [ctx])` | `true` | Same as `wallet.begin("bank", payload, ctx)` (feature `bank`) |
| `wallet.poll()` | `"pending"` · a 64-byte signature · `nil, reason` | Call from `on_update` after `begin`. A result is returned once. With nothing begun: `nil, "idle"` |
| `wallet.wire_tx(sig, msg)` | base64 string | The wire transaction (`0x01 ‖ sig ‖ msg`), ready for the RPC method `sendTransaction` |
| `wallet.requests()` | array of `{req, mac, rssi, name, amount, currency, rail, payee, req_id, age_ms, expires_in_s}` | Payment requests heard recently (feature `requests`). `req` is the raw frame to pass as `ctx.req`; `name` is the sender's *claim*; `req_id` is 16 hex characters |
| `wallet.challenge(mac, req)` | `true` | Starts a presence check with the badge at `mac` for that request |
| `wallet.presence(req_id)` | `"none"`, `"pending"`, `"present"`, `"late"`, `"bad_sig"` | For the app's own UI. The approval reads the same result itself |

`ctx` is a table with any of `record` (registry record bytes), `record_sig` (64 bytes), `req` (REQ frame bytes). Nothing in it is trusted; the firmware verifies each part ([checks](../wallet/checks.md)).

`wallet.begin` is in `wallet/lua_wallet.cpp`; `wallet.begin_solana` and `wallet.begin_bank` are in their feature folders, and features may not include each other. All three parse `ctx` the same way by calling one function, declared in `src/vk/wallet/lua_wallet.h`:

```cpp
int vk::wallet::luaBegin(lua_State *L, const char *domain, int bytesIndex, int ctxIndex);
```

`bytesIndex` and `ctxIndex` are the Lua stack positions of the bytes and of the optional `ctx` table. It pushes `true` or `nil, reason` and returns the number of Lua results, so `begin_solana` is `return luaBegin(L, "solana", 1, 2);`.

`begin` reasons: `not_provisioned`, `busy`, `too_long`, `unsupported`, `bad_arg`. (A Lua app without the permission never reaches `begin`: the call raises an error. `denied` exists for native apps.) `poll` reasons: `cancelled`, `timeout`, `sign_failed`, and for a blocked (red) approval the cause: `undecodable`, `unverified`, `revoked`, `expired`, `mismatch`, `bad_proof`, `over_cap`, `no_time`.

## `badge.wallet`: requests

Permission `request`. Feature `requests`.

| Function | Returns | Notes |
|---|---|---|
| `wallet.request_open{amount=, [symbol=], [rail=], [name=], [ttl_s=]}` | `{req_id, frame, expiry}` | The firmware builds, signs and broadcasts the request and answers presence checks for it. `rail` is `"solana"` (default) or `"bank"`; `name` defaults to the badge's `display_name`; `ttl_s` defaults to `req_ttl_s`. Reasons: `not_provisioned`, `no_time`, `busy` (two are already open), `bad_arg`, `sign_failed` |
| `wallet.request_close(req_id)` | boolean | |
| `wallet.request_status(req_id)` | `{state, proofs}` or `nil` | `state` is `"open"`, `"closed"` or `"expired"`; `proofs` counts presence checks answered |

The payer's RESULT frame arrives in `on_espnow` (needs `espnow`). It is unauthenticated: confirm the transaction on chain before showing "paid" (`vk.confirm`).

## `badge.wallet`: contacts

Permission `contacts`. Feature `contacts`. Frames are sent and received by the app with `badge.espnow` (needs `espnow`).

| Function | Returns | Notes |
|---|---|---|
| `wallet.contact_hello()` | HELLO frame bytes | Carries this badge's swap nonce (valid 60 s; the same frame is returned until it expires). Broadcast it about once a second |
| `wallet.contact_card(hello_frame)` | CARD frame bytes | A card signed for the badge that sent that HELLO. Send it unicast to that badge. Reasons: `bad_arg`, `sign_failed` |
| `wallet.contact_accept(card_frame)` | `{name, address}` | Verifies the card against this badge's current nonce and key, saves the contact, rotates the nonce. Reasons: `bad_arg`, `bad_proof` (signature), `expired` (nonce no longer current), `mismatch` (card made for another badge) |
| `wallet.contacts()` | array of `{name, address, added}` | `added` is unix seconds |
| `wallet.contact_remove(address)` | boolean | |

A contact's name is what its owner chose to call themselves. It is **never** shown on the approval screen; only an issuer-signed record's name is.

## `badge.wallet`: history

Permission `history`. Feature `history`.

| Function | Returns |
|---|---|
| `wallet.history([max])` | array, newest first, of `{time, domain, outcome, reason, amount, symbol, name, address, app, sig, dev}`. `outcome` is `"signed"`, `"cancelled"`, `"timeout"`, `"blocked"`, `"failed"` or `"approved"` (a confirmation that is not a signature); `sig` is base58 or `nil`; `dev` is true if a dev-build override was used. `max` defaults to 20, limit 64 |

## `badge.wallet`: balance

| Function | Permission | Returns |
|---|---|---|
| `wallet.refresh_balance()` | `net` | `true`, or `nil, reason`. Fetches now (blocks up to 3 s) |

## `badge.theme`

No permission needed. Lets an app match the active theme (light or dark).

| Function | Returns |
|---|---|
| `theme.name()` | `"receipt-light"` or `"receipt-dark"` (or another registered theme) |
| `theme.color(token)` | the RGB565 colour for `"paper"`, `"ink"`, `"faint"`, `"sub"`, `"stamp_ok"`, `"stamp_warn"`, `"stamp_bad"`, `"led"`; also the fixed `"green"`, `"amber"`, `"red"` |

## `badge.codec`

No permission needed. Implemented in `src/vk/wallet/lua_wallet.cpp`.

| Function | Returns |
|---|---|
| `codec.b64enc(bytes)` / `codec.b64dec(text)` | string / bytes or `nil` |
| `codec.b58enc(bytes)` / `codec.b58dec(text)` | string / bytes or `nil` |
| `codec.hex(bytes)` / `codec.unhex(text)` | lower-case hex / bytes or `nil` |

## `lib/vk.lua`

Shared, pure Lua. Source: `os/lib/vk.lua`. Upstream's push can write only under `/apps/<id>/`, so `scripts/push-apps.sh` copies this file into each app's folder as `vk.lua` when it pushes the app; `require` finds it there first. `local vk = require("vk")`.

| Function | Permission the app needs | Does |
|---|---|---|
| `vk.json.decode(text)` | — | JSON text → Lua tables. Numbers become Lua numbers (do not rely on them for amounts; RPC amounts are strings) |
| `vk.json.encode(value)` | — | Lua value → JSON text |
| `vk.rpc(method, params)` | `net` | POSTs a JSON-RPC 2.0 call to `wallet.config("rpc_url")`; returns the `result` table, or `nil, message` |
| `vk.blockhash()` | `net` | recent blockhash, base58 (`getLatestBlockhash`) |
| `vk.send_tx(wire_b64)` | `net` | `sendTransaction` with base64 encoding; returns the signature (base58) |
| `vk.confirm(sig_b58)` | `net` | `"confirmed"`, `"pending"` or `"failed"` (`getSignatureStatuses`) |
| `vk.record(address)` | `net` | `GET <listener_url>/registry/<address>`; returns `record, sig` (bytes), or `nil, "unverified"` on 404 |
| `vk.report(event_table)` | `net` | `POST <listener_url>/feed/event` (a badge-side refusal for the dashboard feed) |
| `vk.frame_type(data)` | — | the VK frame type of an ESP-NOW payload, or `nil` |
| `vk.result_frame(req_id_hex, status, sig_bytes)` | — | RESULT frame bytes |
| `vk.result_parse(data)` | — | `{req_id, status, ref}` (`req_id` hex, `ref` bytes), or `nil` if not a RESULT frame |
| `vk.hello_parse(data)` | — | `{address, name}` from a CONTACT_HELLO frame, or `nil` |
| `vk.feed(sig_b58, req)` | `net` | `POST <listener_url>/feed/solana` after a confirmed payment; `req` may be nil |
| `vk.app_frame(type, body)` / `vk.app_body(data, type)` | — | build / match an app-range frame |
| `vk.pay.start(opts)` | `sign`, `net`, `espnow` | starts the whole payer flow; returns a flow object (below) |
| `vk.ui.page()`, `vk.ui.header(left, right)`, `vk.ui.title(text, y)`, `vk.ui.rule(y)`, `vk.ui.row(y, label, value, selected)`, `vk.ui.subline(y, text, selected)`, `vk.ui.amount(cx, y, label, value, unit)`, `vk.ui.footer(left, right)`, `vk.ui.stamp(cx, cy, text, token)`, `vk.ui.list(model)` | — | the receipt look for Lua apps, drawn with `badge.gfx` in the active theme's colours; same geometry as the firmware's receipt kit ([ui](../ui/ui.md#the-receipt-kit)). `vk.ui.list{title=, rows={{l=, r=, sub=, tone=}}, sel=, hint=}` draws a whole list screen. The stamp is drawn unrotated in Lua |

### `vk.pay`

The payer flow as a small state machine, so an app that takes payments is a few lines. `opts` is one of:

- `{request = entry}`: pay a request from `wallet.requests()` (challenge, record, build, approve, submit, RESULT);
- `{to = address, amount = "5.00", symbol = "HACK", memo = "..."}`: pay a registered recipient with no request (a shop; the approval will be amber).

```lua
local flow = vk.pay.start{ to = SHOP, amount = "5.00", memo = "sword" }
function on_update(dt)
  local state, detail = flow:update()   -- call every frame
  -- state: "presence" | "record" | "blockhash" | "approve" | "submit" | "confirm" | "done" | "failed"
  -- detail: on "done" the transaction signature (base58); on "failed" the reason
end
```

`flow:update()` does at most one blocking step per call. During `"approve"` the app is paused by the firmware and resumes when the user has decided. If `wallet.token_account()` is nil the flow first calls `wallet.refresh_balance()`. After `"confirm"` succeeds it calls `vk.feed` (failures there are ignored) and, when paying a request, sends the RESULT frame.

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
