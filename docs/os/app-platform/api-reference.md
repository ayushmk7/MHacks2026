# Badge API reference

Purpose: every function an app can call, in its Lua form and its C/C++ form, with parameters, results, errors, blocking behaviour and required permission.

Audience: app authors (Lua and C++), and the firmware engineers who implement `src/app_host/badge_api.cpp` and `src/lua_sdk/lib_wallet.cpp`.

Status: design, not yet built on hardware. The upstream Lua modules exist in Solana OS and were read in source. Everything tagged [OURS] is specified and not yet implemented. `badge_api.h` was syntax-checked only (`cc -std=c99 -fsyntax-only`); it has never been linked or run.

Upstream means Solana OS, `firmware/solana-os/` at commit `812b8c7` of the [upstream repository](https://github.com/spacemandev-git/solana-defcon-badge-26/tree/812b8c7/firmware/solana-os). Upstream paths below are relative to that directory.

Related documents: [platform overview](overview.md), [Lua guide](lua-apps.md), [C++ guide](cpp-apps.md), [worked example](examples.md), [error codes](../reference/error-codes.md#badge_err_t).

## Contents

- [Conventions](#conventions)
- [Top level](#top-level)
- Upstream modules: [badge.gfx](#badgegfx), [badge.input](#badgeinput), [badge.led](#badgeled), [badge.system](#badgesystem), [badge.storage](#badgestorage), [badge.battery](#badgebattery), [badge.mic](#badgemic), [badge.se050](#badgese050), [badge.wifi](#badgewifi), [badge.http](#badgehttp), [badge.espnow](#badgeespnow), [badge.ble](#badgeble)
- Our modules: [badge.identity](#badgeidentity), [badge.wallet](#badgewallet), [badge.sol](#badgesol), [badge.codec](#badgecodec), [badge.json](#badgejson), [badge.rpc](#badgerpc), [badge.attest](#badgeattest), [badge.pay](#badgepay), [badge.history](#badgehistory), [badge.app](#badgeapp)
- [The C ABI in full](#the-c-abi-in-full)
- [Parity exceptions](#parity-exceptions)

## Conventions

### One function, three spellings

| Language | Form | Example |
|---|---|---|
| Lua | `badge.<module>.<fn>(...)` | `badge.rpc.blockhash()` |
| C | `badge_<module>_<fn>(...)` | `badge_rpc_blockhash(out)` |
| C++ SDK | `badge::<module>::<fn>(...)`, an inline wrapper with the same arguments as the C function | `badge::rpc::blockhash(out)` |

[OURS: one contract, implemented once in C/C++ and bound twice.]

The C++ SDK header in the repository writes out four wrappers (`badge::gfx::clear`, `badge::gfx::text_center`, `badge::system::millis`, `badge::system::exit`); the remaining wrappers follow the same pattern and are not written yet. A C++ app may call the C function directly at any time, which is what the Tip Jar example does. See [cpp-apps.md](cpp-apps.md#the-sdk-header).

In the tables below the "C" column gives the C declaration. The C++ form is the same name with `badge_<module>_` replaced by `badge::<module>::`.

### Return rule

| | Success | Failure |
|---|---|---|
| Lua, our modules | the value(s) | `nil, "<error string>"`, sometimes followed by a detail string |
| Lua, upstream modules | the value(s) | upstream's own conventions: `nil, "reason"` or `false, "reason"` with free-text reasons; programmer errors (unknown key name, oversized payload, path outside the app directory) raise a Lua error [UPSTREAM `src/lua_sdk/lib_*.cpp`] |
| C | `BADGE_OK` (0), results written through out-parameters | a non-zero `badge_err_t` |
| C getters that cannot fail | the value directly | — |

Strings out of C are written into caller buffers and are NUL-terminated. Functions that return `size_t` return the number of bytes or characters written, and 0 on failure.

### How values cross the boundary

| Value | Lua | C |
|---|---|---|
| Public key, token account, mint | base58 string | `uint8_t[32]` |
| Amount of HACK | decimal string of raw base units (`"1000"` is 10.00 HACK at 2 decimals) | `uint64_t` |
| Lamports | decimal string | `uint64_t` |
| Transaction message, wire transaction | byte string | `const uint8_t *` + `size_t` |
| Ed25519 signature from `identity.sign`, into `sol.wire` | 64-byte string | `uint8_t[64]` |
| Transaction signature used as an id (`rpc.send` result, `rpc.status`, `history.mark`, `pay.paid`, `pay.status().tx`, `history.list()[i].sig`) | base58 string | `uint8_t[64]` |
| Payment request id | 16 hex characters | `uint8_t[8]` |
| MAC address | `"aa:bb:cc:dd:ee:ff"`, lowercase [UPSTREAM `lib_net.cpp:147-160`] | `uint8_t[6]` |
| Colour | RGB565 integer | `uint16_t` |
| Button | `"up"`, `"down"`, `"left"`, `"right"`, `"a"`, `"b"` (case-insensitive) or an index 0..5 | `badge_key_t` |

The two signature rows are the same 64 bytes in two encodings: `identity.sign` returns the raw bytes, and `codec.b58encode(sig)` gives the base58 transaction signature that every other call takes. [OURS]

Amounts are strings in Lua because the badge's Lua is built with 32-bit integers and 32-bit floats [UPSTREAM `src/lua/luaconf.h:125`]; a `u64` cannot be a Lua number. See [lua-apps.md](lua-apps.md#the-32-bit-number-rule).

### Buttons

| Silkscreen | Lua key | Index | C |
|---|---|---|---|
| UP | `"up"` | 0 | `BADGE_KEY_UP` |
| LEFT | `"left"` | 1 | `BADGE_KEY_LEFT` |
| RIGHT | `"right"` | 2 | `BADGE_KEY_RIGHT` |
| DOWN | `"down"` | 3 | `BADGE_KEY_DOWN` |
| SELECT | `"a"` | 4 | `BADGE_KEY_A` |
| CANCEL | `"b"` | 5 | `BADGE_KEY_B` |

[UPSTREAM `src/config.h:110-115`; `badge_key_t` equals the upstream `BTN_*` values.]

### Errors

`badge_err_t` is `int32_t`. Lua receives the string name as the second return value; C and C++ receive the number. The canonical table is in [reference/error-codes.md](../reference/error-codes.md#badge_err_t); it is repeated here for convenience.

| Value | C name | Lua string | Meaning |
|---|---|---|---|
| 0 | `BADGE_OK` | — | success |
| 1 | `BADGE_ERR_BAD_ARG` | `bad_arg` | malformed argument (wrong length, not base58, …) |
| 2 | `BADGE_ERR_DENIED` | `denied` | the calling app lacks the permission |
| 3 | `BADGE_ERR_NOT_READY` | `not_ready` | identity or wallet config missing |
| 4 | `BADGE_ERR_BUSY` | `busy` | another wallet prompt is active, or (`pay.request`, `pay.receive`) a session is already open |
| 5 | `BADGE_ERR_RATE_LIMITED` | `rate_limited` | see [rate limits](../wallet-core/signing-gate.md#rate-limits) |
| 6 | `BADGE_ERR_NO_NETWORK` | `no_network` | no Wi-Fi and no bridge |
| 7 | `BADGE_ERR_TIMEOUT` | `timeout` | network timeout |
| 8 | `BADGE_ERR_IO` | `io` | transport or storage failure |
| 9 | `BADGE_ERR_RPC` | `rpc` | JSON-RPC returned an `error` object |
| 10 | `BADGE_ERR_PARSE` | `parse` | response did not parse |
| 11 | `BADGE_ERR_NO_ACCOUNT` | `no_account` | account does not exist on chain. Returned only by `rpc.token_owner`; a token account that does not exist reads as a balance of `0` in `rpc.token_balance`, not as an error |
| 12 | `BADGE_ERR_TOO_LONG` | `too_long` | input exceeds a fixed limit |
| 13 | `BADGE_ERR_NO_MEMORY` | `no_memory` | allocation failed or app heap cap reached |
| 20 | `BADGE_ERR_UNKNOWN_INSTRUCTION` | `unknown_instruction` | decoder rejected the message (detail = decoder reason) |
| 21 | `BADGE_ERR_WRONG_SIGNER` | `wrong_signer` | fee payer/authority is not this badge |
| 22 | `BADGE_ERR_UNKNOWN_MINT` | `unknown_mint` | mint is not the configured mint |
| 23 | `BADGE_ERR_DECIMALS` | `decimals` | instruction decimals differ from config |
| 24 | `BADGE_ERR_BAD_SOURCE` | `bad_source` | source is not this badge's token account |
| 25 | `BADGE_ERR_OVER_LIMIT` | `over_limit` | amount > `max` |
| 26 | `BADGE_ERR_BLOCKED` | `blocked` | red state and `block_red`: returned when the red approval screen is closed with CANCEL or by the 60 s limit, because signing was never possible |
| 27 | `BADGE_ERR_REJECTED` | `rejected` | CANCEL pressed on a prompt that could have been approved |
| 28 | `BADGE_ERR_APPROVAL_TIMEOUT` | `approval_timeout` | no decision within the call's 60 s budget |
| 29 | `BADGE_ERR_SIGN_FAILED` | `sign_failed` | key backend failed, or the produced signature did not verify |
| 30 | `BADGE_ERR_NO_DISPLAY` | `no_display` | framebuffer or buttons unavailable; the wallet refuses rather than sign blind |
| 31 | `BADGE_ERR_NO_SESSION` | `no_session` | PROOF or RCPT asked for with no matching open session; for `pay.receipt`, no session in state `paid` |

Shown on [Screen E](../wallet-core/screens.md#screen-e) before the call returns: codes 20–25, `too_long` (12) when policy check 11 raises it, and `sign_failed` (29). `blocked` (26) is returned after the red [Screen C](../wallet-core/screens.md#screen-c). `no_display` (30) is never drawn: the call returns at once and logs `[wallet] no_display: refusing to sign`.

Decoder reasons (the `detail` value that accompanies `unknown_instruction`): `too_long`, `truncated`, `version`, `header`, `accounts`, `lookups`, `ix_count`, `program`, `ix_accounts`, `ix_data`, `authority`, `roles`, `amount_zero`, `trailing`. [OURS, host-tested: `sol_tx_err_name()` in [`sol_tx.c`](../reference/code/sol_tx.c)]

Codes returned by the C functions that mirror an upstream Lua binding [OURS]. The Lua forms of these functions keep upstream's own conventions (see [Return rule](#return-rule)).

| C function | Codes besides `BADGE_OK` |
|---|---|
| `badge_storage_read` | `bad_arg` (path leaves the app directory), `io` (missing or unreadable), `too_long` (file larger than `cap`) |
| `badge_storage_write`, `badge_storage_append` | `bad_arg`, `io` |
| `badge_http_get`, `badge_http_post` | `denied`, `bad_arg` (URL not `http://` or `https://`), `no_network`, `timeout`, `io`, `too_long` (body larger than `cap`; the first `cap−1` bytes are written). An HTTP status of 400 or above is `BADGE_OK`; read `*status` |
| `badge_espnow_broadcast`, `badge_espnow_send` | `denied`, `bad_arg`, `too_long` (more than 240 bytes), `io` (ESP-NOW off or the send was refused) |
| `badge_system_set_name` | `denied`, `bad_arg` (not 1..23 printable ASCII) |
| `badge_system_launch` | `denied`, `bad_arg` (no such app) |
| `badge_system_reboot` | `denied` |

The codes of the wallet functions are listed with each module below.

### Permissions

| Name in `app.ini` | C bit | Grants |
|---|---|---|
| `sign` | `BADGE_CAP_SIGN` `0x01` | `identity.sign`, `pay.request`, `pay.receive` (each still shows a firmware screen) |
| `net` | `BADGE_CAP_NET` `0x02` | `wifi.*`, `http.*`, `rpc.*`, `attest.*` |
| `radio` | `BADGE_CAP_RADIO` `0x04` | `espnow.*`, `ble.*`, `pay.*` other than request/receive |
| `wallet` | `BADGE_CAP_WALLET` `0x08` | `history.*` |
| `system` | `BADGE_CAP_SYSTEM` `0x10` | `system.launch`, `system.name` (set), `system.reboot` |

No permission is needed for `gfx`, `input`, `led`, `storage`, `battery`, `mic`, `se050`, the rest of `system`, `identity.pubkey/badge_id/source/decode`, `wallet.info/format/parse`, `sol.*`, `codec.*`, `json.*`, `app.*`. [OURS]

`pay.request` and `pay.receive` need both `sign` and `radio`.

What a missing permission looks like:

| Caller | Result |
|---|---|
| Lua, our modules | `nil, "denied"` |
| Lua, guarded upstream bindings (`wifi`, `http`, `espnow`, `ble`, and the three guarded `system` functions) | a Lua error naming the permission; the app stops unless it uses `pcall` |
| C, functions returning `badge_err_t` | `BADGE_ERR_DENIED` |
| C, guarded functions returning anything else (`badge_wifi_connected`, `badge_espnow_peers`, `badge_ble_send`, `badge_pay_inbox`, `badge_pay_peer`, `badge_pay_presence`, `badge_pay_status`, `badge_history_list`, `badge_attest_cached` and the like) | the function's empty value: `false`, `0`, `""`, `BADGE_PRESENCE_NONE`, or a zeroed struct with state `BADGE_PAY_IDLE`. The firmware logs `[app] <id>: <function> needs permission <name>` once per launch |

The empty value cannot be told from a real "nothing there". Call `badge_app_has(cap)` first when the difference matters. The Lua forms of the same wallet functions (`pay.inbox`, `pay.peer`, `pay.presence`, `pay.status`, `history.list`, `attest.cached`) return `nil, "denied"`. [OURS]

### Blocking and time budgets

Every callback has 250 ms (`on_start`: 5000 ms). See [overview.md](overview.md#blocking-rules) for the rules; this table says which class each function is in.

| Class | Functions | Effect on the Lua budget | Effect on the native budget |
|---|---|---|---|
| Immediate | everything not listed below | counts against the callback's 250 ms | counts |
| Timed block | `system.sleep`, `http.get`, `http.post`, `se050.random`, `se050.random_available`, every `rpc.*`, `attest.check` and `attest.self` when they fetch | the deadline is extended by what the call intends to spend; total extension per callback ≤ 12 000 ms [UPSTREAM `src/config.h:210`, `lua_runtime.cpp:198-206`] | time inside the call is subtracted from the measured callback time [OURS] |
| User block | `identity.sign`, `pay.request`, `pay.receive` | the deadline is paused for the whole call, up to the 60 s approval timeout, and does not count against the 12 000 ms cap [OURS] | subtracted [OURS] |

Extensions granted per call:

| Call | Timeout | Extension |
|---|---|---|
| `system.sleep(ms)` | `ms`, capped at 2000 | `ms` + 50 ms [UPSTREAM `lib_system.cpp:42-50`] |
| `http.get` / `http.post` | default 5000 ms, clamped to 100..10 000 | timeout + 500 ms over Wi-Fi, + 2500 ms over the phone bridge [UPSTREAM `src/net/net_route.cpp:133-139`] |
| `se050.random`, `se050.random_available` | one I²C exchange | 180 ms [UPSTREAM `lib_sensors.cpp:145`] |
| every `rpc.*`, and an `attest` fetch | 4000 ms | 4500 ms [OURS] |

Three RPC calls in one callback can exceed the 12 000 ms cap (3 × 4500 ms). Make at most two timed-block network calls per callback and move the rest to later frames.

All of these numbers are from source or design. None has been measured on a badge [UNVERIFIED; fallback: lengthen polling intervals and spread calls over more frames].

### Tags

[UPSTREAM] exists in Solana OS at commit `812b8c7`, with the path. [OURS] is this project's design. [UNVERIFIED] must be confirmed on hardware; a fallback is given. For every upstream module the Lua binding is [UPSTREAM] and the C form is [OURS].

## Top level

[UPSTREAM `src/lua_sdk/lua_bindings.cpp:55-86`]

| Lua | C | Value | Notes |
|---|---|---|---|
| `badge.version` | — (Lua only) | `"0.1.0"` | OS version string |
| `badge.api_version` | `BADGE_API_VERSION` | `2` | bumped from upstream's 1 because bindings were added [OURS]. `min_api` in `app.ini` is compared against it. Not the same number as `BADGE_ABI_VERSION` (1), which versions the native app descriptor. |
| `badge.device_name` | `const char *badge_system_name(void)` | e.g. `"badge-4F2A"` | read once at launch in Lua |
| `badge.log(...)` | `void badge_log(const char *fmt, ...)` | — | tagged log line (`[app]`); Lua form takes any values and honours `__tostring`; C form is `printf`-style |
| `print(...)` | — (Lua only; use `badge_log`) | — | same destination as `badge.log` |
| `badge.millis()` | `uint32_t badge_system_millis(void)` | ms since boot | alias of `system.millis` |
| `badge.sleep(ms)` | `void badge_system_sleep(uint32_t ms)` | — | alias of `system.sleep` |

## badge.gfx

Lua bindings: [UPSTREAM `src/lua_sdk/lib_gfx.cpp:399-445`]. Permission: none. Blocking: none.

The framebuffer is 320×240, landscape, RGB565. Everything draws into one PSRAM sprite; the runtime pushes it after `on_draw` returns [UPSTREAM `src/hal/display.h`]. The font is ASCII only, 6×8 px at size 1, scaled by an integer `size`.

In Lua every colour argument is optional and defaults to white, except `clear`, which defaults to the theme background [UPSTREAM `lib_gfx.cpp:31-34, 57-61`]. In C every colour is explicit.

| Lua | C | Returns | Notes |
|---|---|---|---|
| `gfx.width()` | `int badge_gfx_width(void)` | 320 | |
| `gfx.height()` | `int badge_gfx_height(void)` | 240 | |
| `gfx.clear([c])` | `void badge_gfx_clear(uint16_t c)` | — | fills the frame. C++: `badge::gfx::clear(c = BADGE_BG)` |
| `gfx.pixel(x, y [, c])` | `void badge_gfx_pixel(int x, int y, uint16_t c)` | — | |
| `gfx.line(x0, y0, x1, y1 [, c])` | `void badge_gfx_line(int x0, int y0, int x1, int y1, uint16_t c)` | — | |
| `gfx.rect(x, y, w, h [, c])` | `void badge_gfx_rect(int x, int y, int w, int h, uint16_t c)` | — | outline |
| `gfx.fill_rect(x, y, w, h [, c])` | `void badge_gfx_fill_rect(int x, int y, int w, int h, uint16_t c)` | — | |
| `gfx.round_rect(x, y, w, h [, r] [, c])` | `void badge_gfx_round_rect(int x, int y, int w, int h, int r, uint16_t c)` | — | Lua `r` defaults to 4; radius is clamped to the canvas |
| `gfx.fill_round_rect(x, y, w, h [, r] [, c])` | `void badge_gfx_fill_round_rect(int x, int y, int w, int h, int r, uint16_t c)` | — | |
| `gfx.circle(x, y, r [, c])` | `void badge_gfx_circle(int x, int y, int r, uint16_t c)` | — | |
| `gfx.fill_circle(x, y, r [, c])` | `void badge_gfx_fill_circle(int x, int y, int r, uint16_t c)` | — | |
| `gfx.triangle(x0, y0, x1, y1, x2, y2 [, c])` | `void badge_gfx_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c)` | — | |
| `gfx.fill_triangle(x0, y0, x1, y1, x2, y2 [, c])` | `void badge_gfx_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c)` | — | |
| `gfx.text(s, x, y [, c] [, size])` | `void badge_gfx_text(const char *s, int x, int y, uint16_t c, int size)` | — | top-left anchored; Lua `size` defaults to 1 |
| `gfx.text_center(s, cx, y [, c] [, size])` | `void badge_gfx_text_center(const char *s, int cx, int y, uint16_t c, int size)` | — | centred on `cx`. C++: `badge::gfx::text_center(s, cx, y, c = BADGE_WHITE, size = 1)` |
| `gfx.text_right(s, rx, y [, c] [, size])` | `void badge_gfx_text_right(const char *s, int rx, int y, uint16_t c, int size)` | — | right edge at `rx` |
| `gfx.text_width(s [, size])` | `int badge_gfx_text_width(const char *s, int size)` | pixels | |
| `gfx.text_height([size])` | `int badge_gfx_text_height(int size)` | pixels | |
| `gfx.color(r, g, b)` | `uint16_t badge_gfx_color(uint8_t r, uint8_t g, uint8_t b)` | RGB565 | 8-bit components; Lua clamps to 0..255 |
| `gfx.hsv(h [, s] [, v])` | `uint16_t badge_gfx_hsv(float h, float s, float v)` | RGB565 | `h` in degrees, `s` and `v` in 0..1 (Lua defaults 1) |
| `gfx.gradient(t)` | `uint16_t badge_gfx_gradient(float t)` | RGB565 | the Solana ramp: 0.0 purple, 1.0 green |
| `gfx.image(path, x, y [, scale])` | `bool badge_gfx_image(const char *path, int x, int y, float scale)` | Lua: `true`, or `false, reason`. C: `true`/`false` | PNG or JPEG from the app's own directory, at most 256 KB [UPSTREAM `lib_gfx.cpp:216-303`] |
| `gfx.image_size(path)` | `bool badge_gfx_image_size(const char *path, int *w, int *h)` | Lua: `w, h`, or `nil, reason` | reads the header only |
| `gfx.brightness([v])` | `uint8_t badge_gfx_brightness(int set_or_minus1)` | current backlight 0..255 | with an argument, sets the backlight first. In C pass -1 to read only. The wallet's approval screen forces at least 160 while it is up and restores the app's value afterwards [OURS] |
| `gfx.flush()` | `void badge_gfx_flush(void)` | — | pushes the framebuffer now; only needed outside `on_draw` |

Constants:

| Lua | C | Colour |
|---|---|---|
| `gfx.BLACK` | `BADGE_BLACK` `0x0000` | |
| `gfx.WHITE` | `BADGE_WHITE` `0xFFFF` | |
| `gfx.SOLANA_PURPLE` | `BADGE_SOLANA_PURPLE` `0x9A3F` | rgb(0x99, 0x45, 0xFF) |
| `gfx.SOLANA_GREEN`, `gfx.GREEN` | `BADGE_SOLANA_GREEN` `0x1792` | rgb(0x14, 0xF1, 0x95) |
| `gfx.RED` | `BADGE_RED` `0xFA28` | rgb(0xFF, 0x45, 0x45) |
| `gfx.ORANGE` | `BADGE_ORANGE` `0xFD84` | rgb(0xFF, 0xB0, 0x20) |
| `gfx.MUTED` | `BADGE_MUTED` `0x9495` | rgb(0x93, 0x93, 0xA8) |
| `gfx.BG` | `BADGE_BG` `0x0842` | rgb(0x0B, 0x0B, 0x12) |
| `gfx.SOLANA_TEAL`, `gfx.SOLANA_MAGENTA`, `gfx.PANEL`, `gfx.BORDER`, `gfx.YELLOW`, `gfx.CYAN`, `gfx.BLUE` | — (Lua only, a declared [parity exception](#parity-exceptions); build with `badge_gfx_color`) | `YELLOW` rgb(0xFF, 0xE0, 0x40), `CYAN` rgb(0x00, 0xE5, 0xFF), `BLUE` rgb(0x40, 0x80, 0xFF) |

## badge.input

Lua bindings: [UPSTREAM `src/lua_sdk/lib_input.cpp:97-108`]. Permission: none. Blocking: none.

`key` is a key name or index in Lua (an unknown name raises a Lua error) and a `badge_key_t` in C. Events arrive through `on_button`; these functions poll the same state.

| Lua | C | Returns | Notes |
|---|---|---|---|
| `input.down(key)` | `bool badge_input_down(badge_key_t k)` | boolean | held right now |
| `input.pressed(key)` | `bool badge_input_pressed(badge_key_t k)` | boolean | went down this frame |
| `input.released(key)` | `bool badge_input_released(badge_key_t k)` | boolean | came up this frame |
| `input.repeated(key)` | `bool badge_input_repeated(badge_key_t k)` | boolean | pressed, or an auto-repeat tick while held (420 ms, then every 110 ms) [UPSTREAM `src/config.h:99-100`] |
| `input.held_ms(key)` | `uint32_t badge_input_held_ms(badge_key_t k)` | ms held, 0 when up | |
| `input.any()` | `bool badge_input_any(void)` | boolean | anything held |
| `input.keys()` | — (Lua only) | array of the six key names | |
| `input.label(key)` | `const char *badge_input_label(badge_key_t k)` | `"SELECT"`, `"CANCEL"`, `"UP"`, … | the silkscreen word, for on-screen hints |
| `input.present()` | `bool badge_input_present(void)` | boolean | false if the button expander did not answer |

Lua constants: `input.UP`, `input.DOWN`, `input.LEFT`, `input.RIGHT`, `input.A`, `input.B` (the indices above) and `input.COUNT` (6). C uses the `BADGE_KEY_*` enumerators; `input.COUNT` is Lua only, and a native app writes the literal 6.

Holding CANCEL for 1500 ms force-quits the running app from the main loop; an app cannot intercept it [UPSTREAM `src/config.h:119`, `solana-os.ino:71-78`].

## badge.led

Lua bindings: [UPSTREAM `src/lua_sdk/lib_led.cpp:89-97`]. Permission: none. Blocking: none.

Two WS2812B LEDs, index 0 and 1. Writes are buffered until `show`.

| Lua | C | Returns | Notes |
|---|---|---|---|
| `led.set(index, r, g, b)` | `void badge_led_set(int index, uint8_t r, uint8_t g, uint8_t b)` | — | Lua raises on an index outside 0..1 |
| `led.all(r, g, b)` | `void badge_led_all(uint8_t r, uint8_t g, uint8_t b)` | — | |
| `led.gradient(index, t [, intensity])` | `void badge_led_gradient(int index, float t, float intensity)` | — | a point on the Solana ramp; Lua `intensity` defaults to 1.0 |
| `led.show()` | `void badge_led_show(void)` | — | latches staged values |
| `led.off()` | `void badge_led_off(void)` | — | |
| `led.pulse(r, g, b [, ms])` | `void badge_led_pulse(uint8_t r, uint8_t g, uint8_t b, uint16_t ms)` | — | self-decaying flash driven by the OS; Lua `ms` defaults to 400 |
| `led.brightness([v])` | `uint8_t badge_led_brightness(int set_or_minus1)` | current 0..255 | global scale; default 72 [UPSTREAM `src/config.h:124-125`] |
| `led.take()` | `void badge_led_take(void)` | — | stops the OS animation so the app owns the LEDs; call it in `on_start` |

Lua constant: `led.COUNT` (2). It is Lua only; a native app writes the literal 2.

The wallet's approval screen sets both LEDs to the severity colour while it is up and turns them off when it closes [OURS].

## badge.system

Lua bindings: [UPSTREAM `src/lua_sdk/lib_system.cpp:195-203`]. Permission: `system` for `launch`, setting the name, and `reboot`; none for the rest [OURS].

| Lua | C | Returns | Blocks | Notes |
|---|---|---|---|---|
| `system.millis()` | `uint32_t badge_system_millis(void)` | ms since boot | no | C++: `badge::system::millis()` |
| `system.uptime()` | — (Lua only) | seconds, fractional | no | |
| `system.sleep(ms)` | `void badge_system_sleep(uint32_t ms)` | — | yes, `ms` capped at 2000 | native apps use this instead of `delay()` |
| `system.heap()` | `uint32_t badge_system_heap(void)` | free internal heap, bytes | no | |
| `system.psram()` | `uint32_t badge_system_psram(void)` | free PSRAM, bytes | no | |
| `system.lua_memory()` | — (Lua only) | `used, limit` bytes of the app's Lua heap | no | |
| `system.chip()` | — (Lua only) | `{model, revision, mhz, flash_bytes, psram_bytes}` | no | |
| `system.name()` | `const char *badge_system_name(void)` | device name | no | |
| `system.name(new)` | `badge_err_t badge_system_set_name(const char *name)` | Lua: the name. C: `BADGE_OK`, `denied` or `bad_arg` | no | **permission `system`**. 1–23 printable ASCII characters; Lua raises on an invalid name [UPSTREAM `lib_system.cpp:88-107`] |
| `system.reboot()` | `badge_err_t badge_system_reboot(void)` | does not return on success. C: `denied` without the permission | — | **permission `system`** |
| `system.exit()` | `void badge_system_exit(void)` | — | no | back to the launcher; deferred to the end of the frame. C++: `badge::system::exit()` |
| `system.launch(id)` | `badge_err_t badge_system_launch(const char *app_id)` | Lua: nothing. C: `BADGE_OK`, `denied` or `bad_arg` (no such app) | no | **permission `system`**. Hands over to another app; deferred. Lua raises `no such app: <id>` if it is not installed [UPSTREAM `lib_system.cpp:128-133`] |
| `system.apps()` | — (Lua only) | array of `{id, name, version, author, description, bytes}` | no | |
| `system.current_app()` | `const char *badge_app_id(void)` | this app's id | no | see [badge.app](#badgeapp) |
| `system.random_bytes([n])` | `void badge_system_random_bytes(uint8_t *out, size_t n)` | Lua: byte string | no | hardware RNG (`esp_random`). Lua `n` defaults to 32 and must be 1..256 |

A trimmed global `os` table is also available in Lua: `os.time`, `os.clock`, `os.date`, `os.difftime`. The badge has no real-time clock; `os.time()` is seconds since boot unless the system clock has been set [UPSTREAM `lib_system.cpp:205-325`].

Requests made with `launch` or `exit` from inside `on_stop` are dropped [UPSTREAM `lua_runtime.cpp:319-333`].

## badge.storage

Lua bindings: [UPSTREAM `src/lua_sdk/lib_storage.cpp:179-184, 261-263`]. Permission: none. Blocking: none (flash writes take milliseconds).

Paths are relative to the app's own directory `/apps/<id>/`: no leading `/`, no `..`, at most 96 characters [UPSTREAM `lua_runtime.cpp:404-410`]. In Lua a path that breaks the rule raises an error. Native apps go through the same resolver [OURS].

| Lua | C | Returns | Notes |
|---|---|---|---|
| `storage.read(path)` | `badge_err_t badge_storage_read(const char *path, uint8_t *buf, size_t cap, size_t *len)` | Lua: string, or `nil, reason` (`"not found"`, or a too-large message). C: `BADGE_OK` with the bytes read in `*len`, or `bad_arg`, `io`, `too_long` | Lua returns at most 64 KB per call |
| `storage.write(path, data)` | `badge_err_t badge_storage_write(const char *path, const uint8_t *data, size_t len)` | Lua: `true`, `false`, or `false, reason`. C: `BADGE_OK`, `bad_arg` or `io` | creates the parent directory |
| `storage.append(path, data)` | `badge_err_t badge_storage_append(const char *path, const uint8_t *data, size_t len)` | as `write` | |
| `storage.exists(path)` | `bool badge_storage_exists(const char *path)` | boolean | |
| `storage.size(path)` | `int32_t badge_storage_size(const char *path)` | Lua: bytes or `nil`. C: bytes or -1 | |
| `storage.remove(path)` | `bool badge_storage_remove(const char *path)` | boolean | |
| `storage.mkdir(path)` | `bool badge_storage_mkdir(const char *path)` | boolean | |
| `storage.list([subdir])` | — (Lua only) | sorted array of names, directories end with `/`, at most 64 entries; or `nil, reason` | |
| `storage.space()` | — (Lua only) | `used, total` bytes of the whole filesystem | |
| `storage.kv.get(key [, default])` | `bool badge_storage_kv_get(const char *key, char *out, size_t cap)` | Lua: string, the default, or `nil`. C: `false` when unset | |
| `storage.kv.set(key, value)` | `bool badge_storage_kv_set(const char *key, const char *value)` | boolean | strings only |
| `storage.kv.remove(key)` | `bool badge_storage_kv_remove(const char *key)` | boolean | |

`storage.kv` is stored in NVS namespace `luakv` and survives a filesystem reflash. The stored key is six hex digits of FNV-1a(app id), a colon, then the app's key, so the app's key is at most 8 characters; a longer key raises in Lua [UPSTREAM `lib_storage.cpp:186-225`]. Both runtimes use the same store and the same namespacing [OURS], so `tipjar` and `tipjar-native` do not share keys.

## badge.battery

Lua bindings: [UPSTREAM `src/lua_sdk/lib_sensors.cpp`]. Permission: none. Blocking: none.

| Lua | C | Returns | Notes |
|---|---|---|---|
| `battery.volts()` | `float badge_battery_volts(void)` | volts | |
| `battery.percent()` | `float badge_battery_percent(void)` | 0..100 | from a Li-Po curve |
| `battery.charging()` | `bool badge_battery_charging(void)` | boolean | inferred from voltage; the board has no charge-status line |

## badge.mic

Lua bindings: [UPSTREAM `src/lua_sdk/lib_sensors.cpp`]. Permission: none. Blocking: none. The runtime turns the microphones off when the app stops.

| Lua | C | Returns | Notes |
|---|---|---|---|
| `mic.enable([on])` | `bool badge_mic_enable(bool on)` | boolean | Lua `on` defaults to true |
| `mic.enabled()` | — (Lua only) | boolean | |
| `mic.level()` | `void badge_mic_level(float *left, float *right)` | Lua: `left, right`, 0..100, smoothed | |
| `mic.db()` | — (Lua only) | `left, right` in dBFS | |
| `mic.read([n])` | — (Lua only) | array of interleaved L/R 16-bit samples | `n` defaults to 256, capped at 1024 |

## badge.se050

Lua bindings: [UPSTREAM `src/lua_sdk/lib_sensors.cpp`]. Permission: none.

This module exposes the secure element's link test and random number generator only. It has no key or signing function; signatures come only from [badge.identity](#badgeidentity).

| Lua | C | Returns | Blocks | Notes |
|---|---|---|---|---|
| `se050.present()` | `bool badge_se050_present(void)` | boolean | no | |
| `se050.atr()` | — (Lua only) | hex string or `nil` | no | |
| `se050.test()` | — (Lua only) | boolean | about 20 ms | re-runs the link test |
| `se050.random(n)` | `bool badge_se050_random(uint8_t *out, size_t n)` | Lua: byte string, or `nil, reason`. C: `true`/`false` | one I²C exchange | `n` is 1..64. Never falls back to another source |
| `se050.random_available()` | — (Lua only) | boolean | one I²C exchange | asks for one real byte |

The secure-element code has not run on real hardware upstream or here [UNVERIFIED; fallback: use `system.random_bytes`].

## badge.wifi

Lua bindings: [UPSTREAM `src/lua_sdk/lib_net.cpp:202-211`]. Permission: `net` [OURS]. Blocking: none; joining is asynchronous.

| Lua | C | Returns | Notes |
|---|---|---|---|
| `wifi.connect(ssid [, password])` | — (Lua only) | boolean | starts joining; does not overwrite the saved network. Poll `connected()` |
| `wifi.connect_enterprise{ssid=, username=, password=, ca=, domain=, method=, identity=, phase2=}` | — (Lua only) | `true`, or `false, reason` | WPA2-Enterprise |
| `wifi.enterprise()` | — (Lua only) | boolean | |
| `wifi.disconnect()` | — (Lua only) | — | |
| `wifi.connected()` | `bool badge_wifi_connected(void)` | boolean | true for any route: Wi-Fi station, the badge's own hotspot, or a phone bridge [UPSTREAM `lib_net.cpp:124-128`] |
| `wifi.status()` | `const char *badge_wifi_status(void)` | `"connecting"`, `"connected"`, `"auth failed"`, `"bridged via phone"`, … | |
| `wifi.ip()` | — (Lua only) | dotted string | |
| `wifi.ssid()` | `const char *badge_wifi_ssid(void)` | string | `"phone-bridge"` when bridged |
| `wifi.rssi()` | `int badge_wifi_rssi(void)` | dBm | |
| `wifi.mac()` | — (Lua only) | lowercase MAC string | |
| `wifi.channel()` | `int badge_wifi_channel(void)` | channel number | |
| `wifi.scan()` | — (Lua only) | boolean | starts an asynchronous scan |
| `wifi.scanning()` | — (Lua only) | boolean | |
| `wifi.networks()` | — (Lua only) | array of `{ssid, rssi, encrypted}` | |
| `wifi.hotspot([password])` | — (Lua only) | boolean | SoftAP at 192.168.4.1 |

## badge.http

Lua bindings: [UPSTREAM `src/lua_sdk/lib_net.cpp:217-271`]. Permission: `net` [OURS]. Blocking: yes, up to the timeout.

| Lua | C | Returns |
|---|---|---|
| `http.get(url [, timeout_ms])` | `badge_err_t badge_http_get(const char *url, uint32_t timeout_ms, int *status, char *body, size_t cap, size_t *len)` | Lua: `status, body`, or `nil, message`. C: `BADGE_OK` with the HTTP status in `*status`, the body in `body` and its length in `*len`; or `denied`, `bad_arg`, `no_network`, `timeout`, `io`, `too_long` |
| `http.post(url, body [, content_type] [, timeout_ms])` | `badge_err_t badge_http_post(const char *url, const char *body_in, size_t body_len, const char *content_type, uint32_t timeout_ms, int *status, char *body, size_t cap, size_t *len)` | same |

- `timeout_ms` defaults to 5000 in Lua and is clamped to 100..10 000. `content_type` defaults to `"text/plain"`.
- With no route at all (no Wi-Fi, no phone bridge) Lua returns `nil, "wifi not connected"`.
- A non-200 status is not a failure: the call returns the status and the body. In C an HTTP status of 400 or above is still `BADGE_OK`; read `*status`.
- In C a body larger than `cap` is `too_long`; the first `cap−1` bytes are written and NUL-terminated. A URL that does not start with `http://` or `https://` is `bad_arg`.
- No custom request headers. GET and POST only.
- Over Wi-Fi the TLS certificate is **not validated** [UPSTREAM `src/net/net_route.cpp:34-40`]. Over a phone bridge the phone terminates TLS and the body is capped at 32 KB. Do not treat an `http.*` response as authenticated. The `rpc` and `attest` modules use their own client, which validates TLS when a CA is installed; see [transport trust](../identity/attestation.md#transport-trust).

## badge.espnow

Lua bindings: [UPSTREAM `src/lua_sdk/lib_espnow.cpp:161-172`]. Permission: `radio` [OURS]. Blocking: none.

Connectionless badge-to-badge frames. Presence beacons are sent by the OS and never reach apps. Frames are unencrypted and unauthenticated. Delivery is not confirmed: `true` means the radio accepted the frame.

| Lua | C | Returns | Notes |
|---|---|---|---|
| `espnow.enable([on] [, channel])` | `bool badge_espnow_enable(bool on)` | boolean | Lua `on` defaults to true; the C form has no channel argument |
| `espnow.enabled()` | `bool badge_espnow_enabled(void)` | boolean | |
| `espnow.channel()` | `int badge_espnow_channel(void)` | channel | follows the Wi-Fi channel when Wi-Fi is joined |
| `espnow.broadcast(data)` | `badge_err_t badge_espnow_broadcast(const uint8_t *data, size_t len)` | Lua: boolean. C: `BADGE_OK`, `denied`, `bad_arg`, `too_long` or `io` | at most 240 bytes; Lua raises above that, C returns `too_long` |
| `espnow.send(mac, data)` | `badge_err_t badge_espnow_send(const uint8_t mac[6], const uint8_t *data, size_t len)` | Lua: boolean. C: the same codes as `broadcast` | unicast; Lua raises on a malformed MAC |
| `espnow.peers()` | `size_t badge_espnow_peers(badge_peer_t *out, size_t max)` | Lua: array of `{mac, name, rssi, age_ms, packets}`. C: count written | **strongest RSSI first**; a peer quiet for 12 s drops off |
| `espnow.signal(mac)` | — (Lua only) | `rssi, age_ms`, or `nil` | |
| `espnow.beacon([on])` | — (Lua only) | boolean | whether this badge announces itself |
| `espnow.clear()` | — (Lua only) | — | forgets the peer table |

```c
typedef struct { uint8_t mac[6]; char name[24]; int8_t rssi; uint32_t age_ms; uint32_t packets; } badge_peer_t;
```

Lua constants: `espnow.MAX_PAYLOAD` (240), `espnow.MAX_PEERS` (20). Both are Lua only; a native app writes the literals 240 and 20.

Incoming application frames arrive as `on_espnow(mac, data, rssi)` in Lua and `on_espnow(const uint8_t mac[6], const uint8_t *data, size_t len, int rssi)` in C++. Payment-protocol frames (first bytes `H`, `P`) are handled by the wallet core first and are also delivered to the app; the authoritative state is what [badge.pay](#badgepay) reports, not what the app parses [OURS].

All badges must be on one Wi-Fi channel to hear each other; join one hotspot [UPSTREAM README "Radio notes"].

## badge.ble

Lua bindings: [UPSTREAM `src/lua_sdk/lib_ble.cpp:93-97`]. Permission: `radio` [OURS]. Blocking: none.

| Lua | C | Returns | Notes |
|---|---|---|---|
| `ble.enable([on] [, name])` | `bool badge_ble_enable(bool on)` | boolean | Lua `on` defaults to true, `name` to the device name |
| `ble.enabled()` | — (Lua only) | boolean | |
| `ble.connected()` | `bool badge_ble_connected(void)` | boolean | |
| `ble.send(line)` | `bool badge_ble_send(const char *line)` | boolean | a newline is appended if missing |
| `ble.listen([on])` | `void badge_ble_listen(bool on)` | Lua: boolean | claims the link; lines then arrive as `on_ble(line)`. Released automatically when the app stops |
| `ble.listening()` | — (Lua only) | boolean | |
| `ble.address()` | — (Lua only) | string or `nil` | |

## badge.identity

[OURS] Lua bindings in `src/lua_sdk/lib_wallet.cpp`, C in `src/app_host/badge_api.cpp`. Upstream has no identity binding for Lua.

Permission: `sign` for `sign`; none for the rest.

| Lua | C | Returns | Blocks | Notes |
|---|---|---|---|---|
| `identity.pubkey()` | `size_t badge_identity_pubkey_b58(char out[45])` | Lua: base58 string, or `nil` when the badge has no identity. C: characters written, 0 when there is no identity | no | the Solana address |
| `identity.pubkey_bytes()` | `bool badge_identity_pubkey(uint8_t out[32])` | Lua: 32-byte string, or `nil`. C: `true`/`false` | no | |
| `identity.badge_id()` | `size_t badge_identity_badge_id(char out[9])` | Lua: 8-character string. C: writes the 8 characters and a NUL and returns 8, or returns 0 if the identity is not ready | no | the first 8 base58 characters of the public key [UPSTREAM concept, `src/identity/identity.h`] |
| `identity.source()` | `uint8_t badge_identity_source(void)` | Lua: `"se050"`, `"software"` or `"none"`. C: 1, 2, 0 | no | where the key lives |
| `identity.decode(message)` | `badge_err_t badge_identity_decode(const uint8_t *message, size_t len, badge_decoded_t *out, const char **detail)` | Lua: table, or `nil, err, detail`. C: `badge_err_t`, result in `*out` | no | pure decode and local checks; no UI, no network. The approval screen never trusts it |
| `identity.sign(message [, hints])` | `badge_err_t badge_identity_sign(const uint8_t *message, size_t len, const badge_sign_hint_t *hint, uint8_t sig_out[64])` | Lua: 64-byte signature, or `nil, err, detail`. C: `badge_err_t`, signature in `sig_out` | **yes, until the user decides (≤ 60 s)** | **permission `sign`** |

`identity.decode` result:

| Lua field | C field (`badge_decoded_t`) | Meaning |
|---|---|---|
| `version`: `"legacy"` or `"v0"` | `uint8_t version`: `0xFF` legacy, `0` v0 | message format |
| `payer`: base58 | `uint8_t payer[32]` | fee payer and authority |
| `source`: base58 | `uint8_t source[32]` | source token account |
| `destination`: base58 | `uint8_t destination[32]` | destination token account |
| `mint`: base58 | `uint8_t mint[32]` | |
| `amount`: raw string | `uint64_t amount` | |
| `amount_ui`: string | `char amount_ui[24]` | for example `"10.00"` |
| `decimals`: integer | `uint8_t decimals` | from the instruction |
| `symbol`: string | `char symbol[8]` | `"HACK"`, or `""` when the mint is not the configured one |
| `mint_known`: boolean | `bool mint_known` | the mint is the configured one |
| `source_is_own`: boolean | `bool source_is_own` | source equals this badge's token account |
| `payer_is_self`: boolean | `bool payer_is_self` | the fee payer is this badge |

`identity.decode` succeeds whenever the decoder accepts the bytes. A failed local check is not an error: it is reported only through `mint_known`, `source_is_own` and `payer_is_self`, which are all false when the wallet is not ready. A message the decoder rejects gives `unknown_instruction` with the decoder reason as detail (Lua: third return value; C: `*detail`, which may be passed as `NULL`). A `NULL` `message` or `out` is `bad_arg`.

`identity.sign` arguments:

- `message` is a transaction **message** (the bytes that get signed), not a wire transaction [OURS: the first byte `0x01` is ambiguous between the two]. Build it with [`sol.transfer_message`](#badgesol) or receive it from elsewhere; either way the wallet core decodes it again from scratch.
- `hints` is optional and untrusted. A hint can lower the trust level shown on the approval screen; it can never raise it [OURS].

| Lua field | C field (`badge_sign_hint_t`) | Meaning |
|---|---|---|
| `recipient` (base58) | `const uint8_t *recipient` (32 bytes) | the owner wallet the app believes it is paying. The wallet derives that wallet's token account and uses the hint only if it equals the destination in the message |
| `claimed_name` (string) | `const char *claimed_name` | what the requester called itself |
| `claimed_amount` (raw string) | `const char *claimed_amount` | the amount the app displayed. If it differs from the decoded amount the screen shows `APP SAID <x> <SYM>` in red |
| `request_id` (16 hex characters) | `const uint8_t *request_id` (8 bytes) | binds this payment to a received payment request; the wallet then requires the request's payee and amount to match the message |

Every field may be omitted (Lua) or `NULL` (C); `hint` itself may be `NULL`.

What happens inside the call, in order: the re-entrancy guard (`busy`), permission check, wallet readiness, display and buttons present, rate limit, decode, policy checks, identity refresh, the approval screen, the signature, a self-check of the signature. The calling app is suspended for all of it and cannot draw, read buttons or dismiss the screen. See [policy checks](../wallet-core/signing-gate.md#policy-checks), [approval modal](../wallet-core/signing-gate.md#approval-modal) and [Screen A](../wallet-core/screens.md#screen-a) to [Screen E](../wallet-core/screens.md#screen-e).

Errors from `identity.sign`:

| Error | Screen shown first | Cause |
|---|---|---|
| `denied` | none | the app lacks `sign` |
| `not_ready` | none | identity failed, or the mint is not configured |
| `no_display` | none | framebuffer or buttons missing |
| `rate_limited` | none | cooldown after a non-approval; see [rate limits](../wallet-core/signing-gate.md#rate-limits) |
| `busy` | none | another prompt or session is active |
| `unknown_instruction` (+ detail) | blocked | the message is not exactly one SPL Token `TransferChecked` |
| `wrong_signer`, `unknown_mint`, `decimals`, `bad_source`, `over_limit`, `too_long` | blocked | policy checks failed |
| `blocked` | approval, red | red state and `block_red = 1`; returned after CANCEL or the timeout |
| `rejected` | approval | CANCEL pressed on a screen that could have been approved |
| `approval_timeout` | approval | no decision within the call's 60 s budget. One budget covers the identity check, the approval screen and the large-payment confirmation |
| `sign_failed` | approval, then blocked (headline `Signing failed`) | key backend failed, or the signature did not verify |

"Blocked" in the middle column is [Screen E](../wallet-core/screens.md#screen-e). `busy` is checked before everything else; with one app running at a time it is practically unreachable for `identity.sign`.

The signature is returned only after SELECT on the approval screen (press for green, hold 2 s for amber, and for amounts above `cap` a second hold on [Screen D](../wallet-core/screens.md#screen-d)). See [severity and gestures](../wallet-core/signing-gate.md#severity-and-gestures).

## badge.wallet

[OURS] Permission: none. Blocking: none.

| Lua | C | Returns | Notes |
|---|---|---|---|
| `wallet.info()` | `void badge_wallet_info(badge_wallet_info_t *out)` | Lua: table, below. C: fills `*out` | read-only view of the wallet configuration |
| `wallet.format(raw [, decimals])` | `size_t badge_wallet_format(uint64_t raw, char *out, size_t cap)` | Lua: string such as `"10.00"`. C: characters written, 0 if `cap` is too small | `decimals` defaults to the configured decimals; the C form always uses the configured value. No floating point is used |
| `wallet.parse(text [, decimals])` | `bool badge_wallet_parse(const char *text, uint64_t *raw)` | Lua: raw string, or `nil`. C: `true`/`false` | accepts `"12.5"`, `"12"`, `"0.05"` with at most `decimals` fraction digits |

`wallet.info()` fields. The Lua table has exactly these fourteen keys:

| Lua field | C field (`badge_wallet_info_t`) | Meaning |
|---|---|---|
| `ready`: boolean | `bool ready` | identity ready and mint configured |
| `pubkey`: base58 | `uint8_t pubkey[32]` | this badge's address |
| `source`: `"se050"`, `"software"` or `"none"` | `uint8_t source`: 1, 2, 0 | where the key lives |
| `mint`: base58 | `uint8_t mint[32]` | the HACK mint |
| `token_account`: base58 | `uint8_t token_account[32]` | this badge's token account, derived from `pubkey` and `mint` |
| `symbol`: string | `char symbol[8]` | `"HACK"` |
| `decimals`: integer | `uint8_t decimals` | 2 by default |
| `cap`: raw string | `uint64_t cap` | above this a second confirmation is required (default `"10000"`, 100.00 HACK) |
| `max`: raw string | `uint64_t max` | above this the wallet refuses (default `"100000"`, 1000.00 HACK) |
| `deadline_ms`: integer | `uint16_t deadline_ms` | presence deadline (default 400) |
| `block_red`: boolean | `bool block_red` | red states disable signing |
| `tls_pinned`: boolean | `bool tls_pinned` | a CA for the RPC host is installed |
| `clock_synced`: boolean | `bool clock_synced` | the system clock was set from the network |
| `dash_url`: string | `char dash_url[64]` | dashboard listener base URL, `""` when unset |

Configuration is set by the team, not by apps; see [config-limits-audit.md](../wallet-core/config-limits-audit.md).

`badge_wallet_info_t` is the app-facing subset of the wallet core's own `wallet_info_t` ([api.md](../wallet-core/api.md)); a native app never includes `wallet.h`.

## badge.sol

[OURS; the underlying encoders and derivations are host-tested in [`sol_tx.c`](../reference/code/sol_tx.c), [`sol_pda.c`](../reference/code/sol_pda.c), [`sol_b58.c`](../reference/code/sol_b58.c).] Permission: none. Blocking: none. Token-account derivation time on the badge is unmeasured [UNVERIFIED; fallback: cache the result per payee].

| Lua | C | Returns | Notes |
|---|---|---|---|
| `sol.transfer_message{to=, amount=, blockhash=}` | `size_t badge_sol_transfer_message(const uint8_t to_owner[32], uint64_t amount, const uint8_t blockhash[32], uint8_t *out, size_t cap)` | Lua: message bytes, or `nil, err`. C: length (214), 0 on failure | builds the 214-byte legacy message that moves `amount` from this badge's token account to the token account of `to` (an owner wallet, not a token account). `blockhash` comes from `rpc.blockhash()` |
| `sol.wire(message, signature)` | `size_t badge_sol_wire(const uint8_t *message, size_t len, const uint8_t sig[64], uint8_t *out, size_t cap)` | Lua: wire transaction bytes. C: length, 0 if `cap` is too small | `0x01` ‖ signature (64) ‖ message; 279 bytes for a 214-byte message |
| `sol.ata(owner [, mint])` | `bool badge_sol_ata(const uint8_t owner[32], uint8_t out[32])` | Lua: base58, or `nil, err`. C: `true`/`false` | the associated token account of `owner`. `mint` defaults to the configured mint; the C form always uses the configured mint |
| `sol.short(base58)` | `size_t badge_sol_short(const uint8_t key[32], char out[12])` | `"Gn2G..Ecxq"` | first 4 characters, `..`, last 4 |

The builder is a convenience in the untrusted path. It lives in firmware because Lua has no `u64`, SHA-256 or curve arithmetic, but its output is treated like any other bytes: the wallet core decodes the message from scratch before showing or signing it. See [transaction-building.md](../protocol/transaction-building.md).

A message buffer of 256 bytes and a wire buffer of 1 + 64 + 256 bytes are always large enough in C.

## badge.codec

[OURS] Permission: none. Blocking: none.

| Lua | C | Returns | Notes |
|---|---|---|---|
| `codec.b58encode(bytes)` | `size_t badge_codec_b58encode(const uint8_t *in, size_t len, char *out, size_t cap)` | Lua: string. C: characters written | |
| `codec.b58decode(str [, len])` | `bool badge_codec_b58decode(const char *in, uint8_t *out, size_t len)` | Lua: bytes, or `nil`. C: `true`/`false` | the decoded value must be exactly `len` bytes; Lua `len` defaults to 32 |
| `codec.b64encode(bytes)` | `size_t badge_codec_b64encode(const uint8_t *in, size_t len, char *out, size_t cap)` | Lua: string. C: characters written | |
| `codec.b64decode(str)` | `bool badge_codec_b64decode(const char *in, uint8_t *out, size_t cap, size_t *len)` | Lua: bytes, or `nil`. C: `true`/`false`, length in `*len` | |
| `codec.hex(bytes)` | `size_t badge_codec_hex(const uint8_t *in, size_t len, char *out, size_t cap)` | Lua: string of lowercase hex digits. C: `2 * len`, or 0 if `cap` is too small | |
| `codec.unhex(str)` | `bool badge_codec_unhex(const char *in, uint8_t *out, size_t cap, size_t *len)` | Lua: bytes, or `nil`. C: `true`/`false`, length in `*len` | |

`codec.b58encode(sig)` turns the 64 raw bytes that `identity.sign` returns into the base58 transaction signature used by `rpc.status`, `history.mark` and `pay.paid`.

## badge.json

[OURS] Lua only (a declared parity exception). A C++ app gets the `jsmn` tokenizer through the SDK header by defining `BADGE_SDK_WITH_JSMN` before including it; see [cpp-apps.md](cpp-apps.md#what-is-lua-only). Permission: none. Blocking: none.

| Lua | Returns | Notes |
|---|---|---|
| `json.decode(text)` | value, or `nil, err` | input at most 32 KB, nesting depth at most 16 |
| `json.encode(value)` | string | |
| `json.null` | the sentinel for JSON `null` | a light userdata; compare with `==` |

Number rule: an integer within ±2^31 becomes a Lua integer; **any other number becomes a string**, because 32-bit Lua cannot hold it. A lamport balance of `5000000000` decodes to the string `"5000000000"`; a price of `5.5` decodes to the string `"5.5"`.

JSON `null` decodes to the sentinel `badge.json.null` everywhere it can occur (object members, array elements, the top level) and encodes back to `null`. It is not `nil`, so a key whose value is `null` is present in the decoded table: test it with `value == badge.json.null`.

## badge.rpc

[OURS] `src/wallet/rpc.cpp`. Permission: `net`. Blocking: every function blocks for up to 4000 ms and extends the Lua deadline by 4500 ms.

Each function makes one JSON-RPC POST to the configured `rpc_url` (devnet by default). The client validates TLS when a CA named `rpc-ca` is installed, and falls back to the phone bridge when there is no Wi-Fi. A response longer than 8 KB is `too_long`. The timeout is `BADGE_RPC_TIMEOUT_MS` (4000) in C.

| Lua | C | Returns | JSON-RPC method |
|---|---|---|---|
| `rpc.balance([pubkey])` | `badge_err_t badge_rpc_balance(const uint8_t *pubkey_or_null, uint64_t *lamports)` | Lua: lamports string | `getBalance` |
| `rpc.token_balance([owner])` | `badge_err_t badge_rpc_token_balance(const uint8_t *owner_or_null, uint64_t *raw, uint8_t *decimals)` | Lua: `raw, decimals`. A token account that does not exist is `"0"` and the configured decimals (C: `BADGE_OK`, `*raw = 0`) | `getTokenAccountBalance` on the owner's token account for the configured mint |
| `rpc.blockhash()` | `badge_err_t badge_rpc_blockhash(uint8_t out[32])` | Lua: base58 string | `getLatestBlockhash` (confirmed) |
| `rpc.send(wire)` | `badge_err_t badge_rpc_send(const uint8_t *wire, size_t len, uint8_t sig_out[64], char *errmsg, size_t errcap)` | Lua: transaction signature (base58), or `nil, err, message`. C: signature in `sig_out`, error text in `errmsg` | `sendTransaction` (base64, preflight at confirmed, `maxRetries` 3) |
| `rpc.status(signature)` | `badge_err_t badge_rpc_status(const uint8_t sig[64], badge_tx_status_t *status)` | Lua: `"pending"`, `"processed"`, `"confirmed"`, `"finalized"` or `"failed"`. C: `BADGE_TX_PENDING` … `BADGE_TX_FAILED` | `getSignatureStatuses` |
| `rpc.token_owner(token_account)` | `badge_err_t badge_rpc_token_owner(const uint8_t token_account[32], uint8_t owner[32], uint8_t mint[32])` | Lua: `owner, mint` (base58), or `nil, err` | `getAccountInfo` (base64); owner is bytes 32..64 and mint bytes 0..32 of the token account |
| `rpc.call(method, params_json)` | `badge_err_t badge_rpc_call(const char *method, const char *params_json, char *out, size_t cap, size_t *len)` | Lua: the text of `result` as a JSON string, or `nil, err`. C: the same text in `out`, NUL-terminated, length in `*len` | generic; `params_json` is the JSON text of the `params` array |

- In Lua, `pubkey` and `owner` default to this badge. In C, `badge_rpc_balance` and `badge_rpc_token_balance` take `NULL` for "this badge"; no other function accepts `NULL` for a key.
- On failure every Lua function returns `nil, err`.
- A token account that does not exist is a balance of zero, not an error: the client turns the node's "could not find account" answer to `getTokenAccountBalance` into `"0"`. `no_account` comes only from `rpc.token_owner`, when `getAccountInfo` returns `value: null`.
- `rpc.send` does not classify the node's refusal. A preflight failure is `nil, "rpc", <message>`, where the third value is the first 96 characters of the node's `error.message` (C: written to `errmsg`). An app that wants to tell a missing destination account from an expired blockhash does so after the failed send; see [apps/pay.md](../apps/pay.md#error-states).
- A `timeout` or `io` from `rpc.send` does not mean the transaction was not sent: the node may have received it. Keep the transaction signature (`codec.b58encode(sig)`) and poll `rpc.status`.
- `rpc.status` takes the base58 transaction signature. `"confirmed"` and `"finalized"` both mean success.

Errors:

| Error | Cause |
|---|---|
| `denied` | the app lacks `net` |
| `not_ready` | identity or wallet configuration missing |
| `bad_arg` | a key or signature argument is malformed |
| `no_network` | no Wi-Fi and no bridge |
| `io` | HTTP or transport failure |
| `timeout` | no answer in 4000 ms |
| `rpc` | the node returned an `error` object |
| `parse` | the response did not have the expected shape; from `rpc.token_owner` also an account that is not a 165-byte SPL token account |
| `no_account` | `rpc.token_owner` only: the account does not exist |
| `too_long` | the response is longer than 8 KB; from `badge_rpc_call` also a result that does not fit `cap` |

RPC latency through a phone hotspot is unmeasured [UNVERIFIED; fallback: lengthen polling intervals, configure a second RPC URL].

## badge.attest

[OURS] `src/wallet/attest.cpp`. Permission: `net`.

Asks whether the on-chain registry (Solana Attestation Service, under this project's credential and schema) holds an attestation for a public key, and what name it carries. See [attestation decision](../identity/attestation.md#decision), [cache](../identity/attestation.md#cache) and [known names](../identity/attestation.md#known-names).

| Lua | C | Returns | Blocks |
|---|---|---|---|
| `attest.check(pubkey [, claimed_name [, force]])` | `badge_err_t badge_attest_check(const uint8_t subject[32], const char *claimed_name, bool force, badge_attest_t *out)` | Lua: table, or `nil, err`. C: `badge_err_t`, result in `*out` | only when it fetches: the cache entry is missing, older than `attest_ttl` (30 s by default), or `force` is true. Then up to 4000 ms |
| `attest.self([force])` | `badge_err_t badge_attest_self(bool force, badge_attest_t *out)` | Lua: table for this badge's own key, or `nil, err`. C: `badge_err_t`, result in `*out` | as `check` |
| `attest.cached(pubkey)` | `bool badge_attest_cached(const uint8_t subject[32], badge_attest_t *out)` | Lua: table, or `nil` when nothing is cached. C: `true`/`false` | never |

Result:

| Lua field | C field (`badge_attest_t`) | Meaning |
|---|---|---|
| `status` | `uint8_t status` | Lua: `"verified"`, `"unverified"`, `"mismatch"`, `"revoked"`, `"expired"`, `"unknown"`. C: a `badge_attest_status_t` value, 0..5 in the same order: `BADGE_ATTEST_VERIFIED`, `BADGE_ATTEST_UNVERIFIED`, `BADGE_ATTEST_MISMATCH`, `BADGE_ATTEST_REVOKED`, `BADGE_ATTEST_EXPIRED`, `BADGE_ATTEST_UNKNOWN` |
| `name` | `char name[33]` | the attested name, when the registry returned one |
| `expiry` | `int64_t expiry` | seconds since the Unix epoch, 0 = never. In Lua an integer; a value above 2147483647 is reported as 2147483647 |
| `age_ms` | `uint32_t age_ms` | age of the cache entry |
| `cached` | flag `BADGE_ATTEST_F_CACHED` (`0x01`) | the answer came from the cache |
| `flags` | `uint8_t flags` | bit mask: `0x01` cached, `0x02` RPC TLS not pinned (`BADGE_ATTEST_F_TLS_UNPINNED`), `0x04` checked through the phone bridge (`BADGE_ATTEST_F_VIA_BRIDGE`) |

Status meanings:

| Status | Meaning |
|---|---|
| `verified` | a valid attestation exists for this key |
| `unverified` | no attestation account, or a malformed one |
| `mismatch` | `claimed_name` conflicts with the attested name, or equals a name already seen as verified for a different key |
| `revoked` | this badge saw the key verified before and the account is now gone |
| `expired` | past expiry, and the clock is synced |
| `unknown` | could not check: offline, RPC error, or the route is the phone bridge. Never cached |

`unknown` is a result, not an error: a network failure returns a table with `status = "unknown"` (C: `BADGE_OK` with `BADGE_ATTEST_UNKNOWN`).

| Function | Errors |
|---|---|
| `attest.check`, `attest.self` | `denied` (no `net`), `bad_arg`, `not_ready` (registry credential or schema not configured) |
| `attest.cached` | `denied`; `nil` alone when nothing is cached (C: `false`) |

The numeric status values are those of the wallet core's own identity states, so the six names mean the same on the approval screen and in this module.

An app's `attest.check` is advisory. The signing gate runs its own check before the approval screen and shows its own result; nothing an app passes can make the screen say `verified`.

## badge.pay

[OURS] `src/wallet/pay_session.cpp`. Permission: `radio`; `request` and `receive` also need `sign`.

The payment protocol's frames are parsed, verified and answered in firmware before any app sees them. This module is the app's view of those tables. Protocol details: [messages](../protocol/payment-protocol.md#messages), [payee state machine](../protocol/payment-protocol.md#payee-state-machine), [payer state machine](../protocol/payment-protocol.md#payer-state-machine), [timing](../protocol/payment-protocol.md#timing), [rules](../protocol/payment-protocol.md#rules).

Payee side:

| Lua | C | Returns | Blocks | Notes |
|---|---|---|---|---|
| `pay.request(amount [, ttl_s])` | `badge_err_t badge_pay_request(uint64_t amount, uint16_t ttl_s, uint8_t id_out[8])` | Lua: request id (16 hex characters), or `nil, err` | **yes, on [Screen F](../wallet-core/screens.md#screen-f) until the user decides (≤ 60 s)** | **also needs `sign`**. After SELECT the firmware signs a payment request and broadcasts it every second until paid, cancelled or expired. `ttl_s` defaults to 120; a larger value is clamped to 120, and Screen F shows the clamped value. `amount` or `ttl_s` of 0 is `bad_arg`; an amount above `max` is `over_limit` |
| `pay.receive([ttl_s])` | `badge_err_t badge_pay_receive(uint16_t ttl_s)` | Lua: `true`, or `nil, err` | **yes, on [Screen G](../wallet-core/screens.md#screen-g)** | **also needs `sign`**. Receive mode: the badge answers presence challenges with request id zero. `ttl_s` defaults to 600; a larger value is clamped to 600; 0 is `bad_arg`. No payment is made by this |
| `pay.cancel()` | `void badge_pay_cancel(void)` | — | no | closes any session |
| `pay.status()` | `void badge_pay_status(badge_pay_status_t *out)` | table, below | no | |
| `pay.receipt()` | `badge_err_t badge_pay_receipt(void)` | Lua: `true`, or `nil, err` | no | stretch feature F19. Signs a receipt and sends it to the badge the PAID hint came from; the firmware kept that address in the session, so the call takes no argument. Only the app that opened the session may call it, once, while the session is in state `paid`; see [apps/receipts.md](../apps/receipts.md) |

`pay.status()` fields:

| Lua field | C field (`badge_pay_status_t`) | Meaning |
|---|---|---|
| `state` | `badge_pay_state_t state` | `"idle"`, `"open"`, `"receive"`, `"paid"`, `"expired"` (C: `BADGE_PAY_IDLE`, `_OPEN`, `_RECEIVE`, `_PAID`, `_EXPIRED`) |
| `id` | `uint8_t id[8]` | request id (all zero in receive mode) |
| `amount` | `uint64_t amount` | raw units |
| `remaining_ms` | `uint32_t remaining_ms` | time until the session expires |
| `proofs` | `uint8_t proofs` | presence challenges answered so far (at most 8 per session) |
| `payer` | `bool has_payer; uint8_t payer[32]` | public key of the last badge that challenged; `nil` in Lua when absent |
| `tx` | `bool has_tx; uint8_t tx_sig[64]` | transaction signature from the payer's PAID hint, base58 in Lua; `nil` in Lua when absent |

`state == "paid"` means a PAID hint arrived. It is a hint, not proof: the payee must confirm on chain (`rpc.status`, then `rpc.token_balance`) before treating the request as paid.

One session at a time; opening a second returns `busy`. The session closes when the app that opened it stops [OURS: a session must not outlive the screen that explained it]. While a session is open the payee app must not block its main loop: a 4 s HTTP call turns every incoming challenge into "not present". See [timing](../protocol/payment-protocol.md#timing).

Payer side:

| Lua | C | Returns | Notes |
|---|---|---|---|
| `pay.inbox()` | `size_t badge_pay_inbox(badge_pay_req_t *out, size_t max)` | Lua: array, strongest RSSI first. C: count written | only requests that parsed, are within their lifetime, are not in the replay ring and have `rssi >= rssi_min`. At most 4, one per payee key |
| `pay.dismiss(id)` | `void badge_pay_dismiss(const uint8_t id[8])` | — | moves a request to the replay ring; it is not listed again |
| `pay.hello(mac)` | `badge_err_t badge_pay_hello(const uint8_t mac[6])` | Lua: `true`, or `nil, err` | asks a peer for its public key |
| `pay.peer(mac)` | `bool badge_pay_peer(const uint8_t mac[6], badge_pay_peer_t *out)` | Lua: `{pubkey, name, age_ms}`, or `nil`. C: `true`/`false` | the last answer from that MAC, kept 60 s. The answer is unsigned: a key learned this way is unproven until a presence proof arrives |
| `pay.challenge(mac [, id])` | `badge_err_t badge_pay_challenge(const uint8_t mac[6], const uint8_t *id_or_null)` | Lua: `true`, or `nil, err` | sends a presence challenge with a fresh nonce. `id` omitted (C: `NULL`) = receive-mode challenge. At most 3 attempts for one request; a fourth returns `rate_limited` |
| `pay.presence(mac)` | `badge_presence_t badge_pay_presence(const uint8_t mac[6], uint32_t *elapsed_ms)` | Lua: `state, elapsed_ms`. C: the state, elapsed in `*elapsed_ms` | state is `"none"`, `"pending"`, `"present"`, `"late"`, `"bad_sig"` or `"timeout"` (C: `BADGE_PRESENCE_NONE` … `BADGE_PRESENCE_TIMEOUT`) |
| `pay.paid(mac, id, signature)` | `badge_err_t badge_pay_paid(const uint8_t mac[6], const uint8_t id[8], const uint8_t tx_sig[64])` | Lua: `true`, or `nil, err` | tells the payee which transaction to look up. `signature` is the base58 transaction signature |

`pay.inbox()` entry:

| Lua field | C field (`badge_pay_req_t`) | Meaning |
|---|---|---|
| `id` | `uint8_t id[8]` | request id |
| `payee` | `uint8_t payee[32]` | the key that signed the request |
| `name` | `char name[33]` | the name the payee claims. Unverified; use it only as a hint |
| `amount` | `uint64_t amount` | raw units |
| `mac` | `uint8_t mac[6]` | where the request came from |
| `rssi` | `int8_t rssi` | dBm |
| `age_ms` | `uint32_t age_ms` | since the last rebroadcast |
| `sig_ok` | `bool sig_ok` | the request's signature verified |

Presence states:

| State | Meaning |
|---|---|
| `none` | no challenge sent to this MAC |
| `pending` | challenge sent, waiting |
| `present` | valid proof within `deadline_ms` |
| `late` | valid proof after the deadline, within 1500 ms |
| `bad_sig` | proof with a wrong signature, wrong id, or a payee other than the request's |
| `timeout` | no proof within 1500 ms |

Constants: `pay.TIMEOUT_MS` (1500; C: `BADGE_PAY_TIMEOUT_MS`) and `pay.DEADLINE_MS` (the configured `deadline_ms`, 400 by default; in C read `deadline_ms` from `badge_wallet_info`).

Errors:

| Function | Errors |
|---|---|
| `pay.request` | `denied`, `not_ready`, `bad_arg`, `over_limit`, `busy`, `no_display`, `rate_limited`, `rejected`, `approval_timeout`, `sign_failed` |
| `pay.receive` | `denied`, `not_ready`, `bad_arg`, `busy`, `no_display`, `rate_limited`, `rejected`, `approval_timeout` |
| `pay.hello`, `pay.paid` | `denied`, `bad_arg`, `not_ready`, `io` (ESP-NOW off or the send was refused) |
| `pay.challenge` | the same, plus `rate_limited` after 3 attempts for one request |
| `pay.receipt` | `denied`, `no_session` (the session is not in state `paid`), `rate_limited` (already sent), `sign_failed`, `io` |
| `pay.inbox`, `pay.peer`, `pay.presence`, `pay.status`, `pay.dismiss`, `pay.cancel` | `denied` only |

For `pay.request` and `pay.receive`, `busy` means a session is already open or another wallet prompt is active.

Presence shown by an app is advisory. The signing gate looks the recipient up in the firmware's presence table itself when `identity.sign` is called.

The presence deadline and the RSSI threshold are unmeasured [UNVERIFIED; fallback: `deadline_ms` 800 with a secure-element key, 2500 without the fast Ed25519 backend; `rssi_min` -100 disables the filter].

## badge.history

[OURS] `src/wallet/history.cpp`. Permission: `wallet`. Blocking: none.

Payment history is a firmware-owned ring of 50 records in `/wallet/history.bin`. The wallet core itself appends an `out`/`signed` record for every transaction it signs, so an app cannot pay without leaving a record. Apps update the status and add incoming payments.

| Lua | C | Returns | Notes |
|---|---|---|---|
| `history.list([max])` | `size_t badge_history_list(badge_history_t *out, size_t max)` | Lua: array, newest first. C: count written | Lua `max` defaults to 20 |
| `history.mark(signature_b58, status)` | `badge_err_t badge_history_mark(const uint8_t sig[64], uint8_t status)` | Lua: `true`, or `nil, err` | sets the status of the record with that transaction signature |
| `history.add{dir="in", peer=, name=, amount=, sig=, verified=}` | `badge_err_t badge_history_add(const badge_history_t *entry)` | Lua: `true`, or `nil, err` | records an incoming payment. `this_boot` and `receipt` are set by firmware and ignored here |

Record:

| Lua field | C field (`badge_history_t`) | Meaning |
|---|---|---|
| `dir` | `uint8_t dir` | `"out"` (0) or `"in"` (1) |
| `peer` | `uint8_t peer[32]` | counterparty public key |
| `name` | `char name[33]` | counterparty name at the time |
| `amount` | `uint64_t amount` | raw units |
| `status` | `uint8_t status` | `"signed"` (0), `"submitted"` (1), `"confirmed"` (2), `"failed"` (3) |
| `sig` | `bool has_sig; uint8_t sig[64]` | transaction signature, base58 in Lua; `nil` in Lua when absent |
| `uptime_s` | `uint32_t uptime_s` | badge uptime when recorded; the badge has no real-time clock |
| `this_boot` | `bool this_boot` | the record was written since the last reboot. When false, `uptime_s` cannot be compared with the current uptime |
| `verified` | `bool verified` | the counterparty was verified at the time |
| `receipt` | `bool receipt` | stretch feature F19: a valid co-signed receipt was received for this payment |

"Time since" is the current uptime minus `uptime_s`, and only when `this_boot` is true; the History app prints `earlier` otherwise.

Errors:

| Function | Errors |
|---|---|
| `history.list` | `denied` |
| `history.mark`, `history.add` | `denied`, `bad_arg` (no record has that signature, or a field is malformed), `io` |

## badge.app

[OURS] Permission: none. Blocking: none.

| Lua | C | Returns | Notes |
|---|---|---|---|
| `app.id()` | `const char *badge_app_id(void)` | this app's id | |
| `app.kind()` | — (Lua only) | `"lua"` or `"native"` | the kind of the running app, so always `"lua"` where Lua can call it. A declared parity exception: a native app is native |
| `app.has(permission)` | `bool badge_app_has(uint32_t cap)` | boolean | Lua takes a permission name such as `"sign"`; C takes a `BADGE_CAP_*` bit |
| `app.api()` | `BADGE_API_VERSION` | `2` | the API version, equal to `badge.api_version` |
| `error(message)` (plain Lua) | `void badge_app_fail(const char *message)` | does not continue the app | stops the app and shows the error screen |
| — | `void *badge_alloc(size_t bytes)` | pointer, or `NULL` when over the cap | PSRAM, counted against the app's 1 MiB cap; every outstanding block is freed when the app stops |
| — | `void badge_free(void *p)` | — | |
| `badge.log(...)` | `void badge_log(const char *fmt, ...)` | — | see [Top level](#top-level) |

## The C ABI in full

`src/app_host/badge_api.h`, complete and verbatim from the repository copy at [`../reference/code/sdk-headers/app_host/badge_api.h`](../reference/code/sdk-headers/app_host/badge_api.h). Syntax-checked with `cc -std=c99 -fsyntax-only` and, through the SDK header, with `c++ -std=c++17 -fno-exceptions -fno-rtti -fsyntax-only`. Not linked, not run. [OURS]

```c
#ifndef BADGE_API_H
#define BADGE_API_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define BADGE_ABI_VERSION 1                   /* layout of badge_app_desc_t */
#define BADGE_API_VERSION 2                   /* == badge.api_version / badge.app.api() in Lua */

/* Conventions
   - Functions returning badge_err_t return BADGE_OK or one of the codes listed at the function or in the
     table "Error codes of the upstream-mirroring functions" at the end of this header.
   - A function that needs a permission the app lacks returns BADGE_ERR_DENIED if it returns badge_err_t.
     If it returns anything else it returns its empty value (false, 0, "", BADGE_PRESENCE_NONE, a zeroed
     struct with state BADGE_PAY_IDLE) and logs "[app] <id>: <function> needs permission <name>" once per
     launch. Check badge_app_has() first when the difference matters.
   - Public keys are 32 raw bytes, signatures 64 raw bytes, amounts uint64_t raw units.
   - Where a parameter is documented "NULL = this badge", NULL selects the badge's own public key. */
typedef int32_t badge_err_t;
enum { BADGE_OK = 0, BADGE_ERR_BAD_ARG = 1, BADGE_ERR_DENIED = 2, BADGE_ERR_NOT_READY = 3, BADGE_ERR_BUSY = 4,
       BADGE_ERR_RATE_LIMITED = 5, BADGE_ERR_NO_NETWORK = 6, BADGE_ERR_TIMEOUT = 7, BADGE_ERR_IO = 8,
       BADGE_ERR_RPC = 9, BADGE_ERR_PARSE = 10, BADGE_ERR_NO_ACCOUNT = 11, BADGE_ERR_TOO_LONG = 12,
       BADGE_ERR_NO_MEMORY = 13, BADGE_ERR_UNKNOWN_INSTRUCTION = 20, BADGE_ERR_WRONG_SIGNER = 21,
       BADGE_ERR_UNKNOWN_MINT = 22, BADGE_ERR_DECIMALS = 23, BADGE_ERR_BAD_SOURCE = 24, BADGE_ERR_OVER_LIMIT = 25,
       BADGE_ERR_BLOCKED = 26, BADGE_ERR_REJECTED = 27, BADGE_ERR_APPROVAL_TIMEOUT = 28,
       BADGE_ERR_SIGN_FAILED = 29, BADGE_ERR_NO_DISPLAY = 30, BADGE_ERR_NO_SESSION = 31 };
enum { BADGE_CAP_SIGN = 0x01, BADGE_CAP_NET = 0x02, BADGE_CAP_RADIO = 0x04, BADGE_CAP_WALLET = 0x08, BADGE_CAP_SYSTEM = 0x10 };
typedef enum { BADGE_KEY_UP = 0, BADGE_KEY_LEFT = 1, BADGE_KEY_RIGHT = 2, BADGE_KEY_DOWN = 3,
               BADGE_KEY_A = 4 /* SELECT */, BADGE_KEY_B = 5 /* CANCEL */ } badge_key_t;   /* == BTN_* in config.h */

/* ---- app descriptor: what a native app exports ---- */
typedef struct badge_app_desc {
  uint32_t abi;                               /* BADGE_ABI_VERSION */
  const char *id, *name, *version, *author, *description;
  uint32_t caps;                              /* BADGE_CAP_* */
  void (*on_start)(void);
  void (*on_update)(float dt);
  void (*on_draw)(void);
  void (*on_button)(badge_key_t key, bool pressed);
  void (*on_espnow)(const uint8_t mac[6], const uint8_t *data, size_t len, int rssi);
  void (*on_ble)(const char *line);
  void (*on_stop)(void);
} badge_app_desc_t;

/* ---- app ---- */
const char *badge_app_id(void);
bool        badge_app_has(uint32_t cap);
void        badge_app_fail(const char *message);          /* stop this app and show the error screen */
void       *badge_alloc(size_t bytes);                    /* PSRAM, counted against the 1 MiB app cap; NULL when over */
void        badge_free(void *p);
void        badge_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- gfx (colours are RGB565) ---- */
int      badge_gfx_width(void);
int      badge_gfx_height(void);
void     badge_gfx_clear(uint16_t c);
void     badge_gfx_pixel(int x, int y, uint16_t c);
void     badge_gfx_line(int x0, int y0, int x1, int y1, uint16_t c);
void     badge_gfx_rect(int x, int y, int w, int h, uint16_t c);
void     badge_gfx_fill_rect(int x, int y, int w, int h, uint16_t c);
void     badge_gfx_round_rect(int x, int y, int w, int h, int r, uint16_t c);
void     badge_gfx_fill_round_rect(int x, int y, int w, int h, int r, uint16_t c);
void     badge_gfx_circle(int x, int y, int r, uint16_t c);
void     badge_gfx_fill_circle(int x, int y, int r, uint16_t c);
void     badge_gfx_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c);
void     badge_gfx_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c);
void     badge_gfx_text(const char *s, int x, int y, uint16_t c, int size);
void     badge_gfx_text_center(const char *s, int cx, int y, uint16_t c, int size);
void     badge_gfx_text_right(const char *s, int rx, int y, uint16_t c, int size);
int      badge_gfx_text_width(const char *s, int size);
int      badge_gfx_text_height(int size);
uint16_t badge_gfx_color(uint8_t r, uint8_t g, uint8_t b);
uint16_t badge_gfx_hsv(float h, float s, float v);
uint16_t badge_gfx_gradient(float t);
bool     badge_gfx_image(const char *path, int x, int y, float scale);
bool     badge_gfx_image_size(const char *path, int *w, int *h);
uint8_t  badge_gfx_brightness(int set_or_minus1);
void     badge_gfx_flush(void);
#define BADGE_BLACK 0x0000
#define BADGE_WHITE 0xFFFF
#define BADGE_SOLANA_PURPLE 0x9A3F   /* rgb565(0x99,0x45,0xFF) */
#define BADGE_SOLANA_GREEN  0x1792   /* rgb565(0x14,0xF1,0x95) */
#define BADGE_RED    0xFA28          /* rgb565(0xFF,0x45,0x45) */
#define BADGE_ORANGE 0xFD84          /* rgb565(0xFF,0xB0,0x20) */
#define BADGE_MUTED  0x9495          /* rgb565(0x93,0x93,0xA8) */
#define BADGE_BG     0x0842          /* rgb565(0x0B,0x0B,0x12) */

/* ---- input ---- */
bool        badge_input_down(badge_key_t k);
bool        badge_input_pressed(badge_key_t k);
bool        badge_input_released(badge_key_t k);
bool        badge_input_repeated(badge_key_t k);
uint32_t    badge_input_held_ms(badge_key_t k);
bool        badge_input_any(void);
const char *badge_input_label(badge_key_t k);             /* "SELECT", "CANCEL", ... */
bool        badge_input_present(void);

/* ---- led ---- */
void    badge_led_set(int index, uint8_t r, uint8_t g, uint8_t b);
void    badge_led_all(uint8_t r, uint8_t g, uint8_t b);
void    badge_led_gradient(int index, float t, float intensity);
void    badge_led_show(void);
void    badge_led_off(void);
void    badge_led_pulse(uint8_t r, uint8_t g, uint8_t b, uint16_t ms);
uint8_t badge_led_brightness(int set_or_minus1);
void    badge_led_take(void);

/* ---- system ---- */
uint32_t    badge_system_millis(void);
void        badge_system_sleep(uint32_t ms);              /* <= 2000 */
uint32_t    badge_system_heap(void);
uint32_t    badge_system_psram(void);
const char *badge_system_name(void);
badge_err_t badge_system_set_name(const char *name);      /* CAP_SYSTEM; 1..23 printable ASCII */
void        badge_system_exit(void);                      /* deferred */
badge_err_t badge_system_launch(const char *app_id);      /* CAP_SYSTEM; deferred */
badge_err_t badge_system_reboot(void);                    /* CAP_SYSTEM; does not return on success */
void        badge_system_random_bytes(uint8_t *out, size_t n);

/* ---- storage (paths relative to /apps/<id>/) ---- */
badge_err_t badge_storage_read(const char *path, uint8_t *buf, size_t cap, size_t *len);
badge_err_t badge_storage_write(const char *path, const uint8_t *data, size_t len);
badge_err_t badge_storage_append(const char *path, const uint8_t *data, size_t len);
bool        badge_storage_exists(const char *path);
int32_t     badge_storage_size(const char *path);         /* -1 if missing */
bool        badge_storage_remove(const char *path);
bool        badge_storage_mkdir(const char *path);
bool        badge_storage_kv_get(const char *key, char *out, size_t cap);
bool        badge_storage_kv_set(const char *key, const char *value);
bool        badge_storage_kv_remove(const char *key);

/* ---- battery / mic / se050 ---- */
float badge_battery_volts(void);
float badge_battery_percent(void);
bool  badge_battery_charging(void);
bool  badge_mic_enable(bool on);
void  badge_mic_level(float *left, float *right);
bool  badge_se050_present(void);
bool  badge_se050_random(uint8_t *out, size_t n);         /* n <= 64 */

/* ---- wifi / http (CAP_NET) ---- */
bool        badge_wifi_connected(void);
const char *badge_wifi_status(void);
const char *badge_wifi_ssid(void);
int         badge_wifi_rssi(void);
int         badge_wifi_channel(void);
badge_err_t badge_http_get(const char *url, uint32_t timeout_ms, int *status, char *body, size_t cap, size_t *len);
badge_err_t badge_http_post(const char *url, const char *body_in, size_t body_len, const char *content_type,
                            uint32_t timeout_ms, int *status, char *body, size_t cap, size_t *len);

/* ---- espnow / ble (CAP_RADIO) ---- */
typedef struct { uint8_t mac[6]; char name[24]; int8_t rssi; uint32_t age_ms; uint32_t packets; } badge_peer_t;
bool        badge_espnow_enable(bool on);
bool        badge_espnow_enabled(void);
int         badge_espnow_channel(void);
badge_err_t badge_espnow_broadcast(const uint8_t *data, size_t len);            /* len <= 240 */
badge_err_t badge_espnow_send(const uint8_t mac[6], const uint8_t *data, size_t len);
size_t      badge_espnow_peers(badge_peer_t *out, size_t max);                  /* strongest RSSI first */
bool        badge_ble_enable(bool on);
bool        badge_ble_connected(void);
bool        badge_ble_send(const char *line);
void        badge_ble_listen(bool on);

/* ---- identity ---- */
typedef struct { const uint8_t *recipient; const char *claimed_name; const char *claimed_amount; const uint8_t *request_id; } badge_sign_hint_t;
bool        badge_identity_pubkey(uint8_t out[32]);
size_t      badge_identity_pubkey_b58(char out[45]);
size_t      badge_identity_badge_id(char out[9]);         /* first 8 base58 characters; returns 8, or 0 if not ready */
uint8_t     badge_identity_source(void);                  /* 0 none, 1 se050, 2 software */
typedef struct {                                          /* identity.decode(): preview only, never trusted by the wallet */
  uint8_t  version;                                       /* 0xFF legacy, 0 v0 */
  uint8_t  payer[32], source[32], destination[32], mint[32];
  uint64_t amount;
  uint8_t  decimals;
  char     amount_ui[24];                                 /* "10.00" */
  char     symbol[8];                                     /* "" when the mint is not the configured one */
  bool     mint_known, source_is_own, payer_is_self;
} badge_decoded_t;
/* BADGE_OK when the decoder accepts the bytes (local checks only in the three booleans);
   BADGE_ERR_UNKNOWN_INSTRUCTION with *detail = decoder reason ("ix_data", ...) otherwise. detail may be NULL. */
badge_err_t badge_identity_decode(const uint8_t *message, size_t len, badge_decoded_t *out, const char **detail);
badge_err_t badge_identity_sign(const uint8_t *message, size_t len, const badge_sign_hint_t *hint, uint8_t sig_out[64]);  /* CAP_SIGN */

/* ---- wallet ---- */
typedef struct {
  bool     ready;                                         /* identity ready and mint configured */
  uint8_t  pubkey[32];
  uint8_t  source;                                        /* 0 none, 1 se050, 2 software */
  uint8_t  mint[32], token_account[32];
  char     symbol[8];
  uint8_t  decimals;
  uint64_t cap, max;
  uint16_t deadline_ms;
  bool     block_red, tls_pinned, clock_synced;
  char     dash_url[64];                                  /* "" when unset */
} badge_wallet_info_t;
void        badge_wallet_info(badge_wallet_info_t *out);

/* ---- sol / codec ---- */
size_t      badge_sol_transfer_message(const uint8_t to_owner[32], uint64_t amount, const uint8_t blockhash[32], uint8_t *out, size_t cap);
size_t      badge_sol_wire(const uint8_t *message, size_t len, const uint8_t sig[64], uint8_t *out, size_t cap);
bool        badge_sol_ata(const uint8_t owner[32], uint8_t out[32]);
size_t      badge_sol_short(const uint8_t key[32], char out[12]);
size_t      badge_wallet_format(uint64_t raw, char *out, size_t cap);
bool        badge_wallet_parse(const char *text, uint64_t *raw);
size_t      badge_codec_b58encode(const uint8_t *in, size_t len, char *out, size_t cap);
bool        badge_codec_b58decode(const char *in, uint8_t *out, size_t len);
size_t      badge_codec_b64encode(const uint8_t *in, size_t len, char *out, size_t cap);
bool        badge_codec_b64decode(const char *in, uint8_t *out, size_t cap, size_t *len);
size_t      badge_codec_hex(const uint8_t *in, size_t len, char *out, size_t cap);       /* lowercase; returns 2*len, 0 if cap too small */
bool        badge_codec_unhex(const char *in, uint8_t *out, size_t cap, size_t *len);

/* ---- rpc (CAP_NET) ---- */
typedef enum { BADGE_TX_PENDING = 0, BADGE_TX_PROCESSED, BADGE_TX_CONFIRMED, BADGE_TX_FINALIZED, BADGE_TX_FAILED } badge_tx_status_t;
#define BADGE_RPC_TIMEOUT_MS 4000
/* All: denied, not_ready, no_network, timeout, io, rpc, parse, too_long (response over 8 KB). */
badge_err_t badge_rpc_balance(const uint8_t *pubkey_or_null, uint64_t *lamports);        /* NULL = this badge */
/* A token account that does not exist is BADGE_OK with *raw = 0 and the configured decimals. */
badge_err_t badge_rpc_token_balance(const uint8_t *owner_or_null, uint64_t *raw, uint8_t *decimals);   /* NULL = this badge */
badge_err_t badge_rpc_blockhash(uint8_t out[32]);
badge_err_t badge_rpc_send(const uint8_t *wire, size_t len, uint8_t sig_out[64], char *errmsg, size_t errcap);
badge_err_t badge_rpc_status(const uint8_t sig[64], badge_tx_status_t *status);
/* no_account when the account does not exist; parse when it is not a 165-byte SPL token account. */
badge_err_t badge_rpc_token_owner(const uint8_t token_account[32], uint8_t owner[32], uint8_t mint[32]);
/* Generic JSON-RPC. params_json is the JSON array text. Writes the text of "result" to out (NUL-terminated).
   too_long when the result does not fit cap. */
badge_err_t badge_rpc_call(const char *method, const char *params_json, char *out, size_t cap, size_t *len);

/* ---- attest (CAP_NET) ---- */
typedef enum { BADGE_ATTEST_VERIFIED = 0, BADGE_ATTEST_UNVERIFIED = 1, BADGE_ATTEST_MISMATCH = 2, BADGE_ATTEST_REVOKED = 3,
               BADGE_ATTEST_EXPIRED = 4, BADGE_ATTEST_UNKNOWN = 5 } badge_attest_status_t;   /* == wallet_identity_t */
typedef struct { uint8_t status;   /* badge_attest_status_t */
                 char name[33]; int64_t expiry; uint32_t age_ms; uint8_t flags; } badge_attest_t;
#define BADGE_ATTEST_F_CACHED 0x01
#define BADGE_ATTEST_F_TLS_UNPINNED 0x02
#define BADGE_ATTEST_F_VIA_BRIDGE 0x04
/* denied, bad_arg, not_ready (cred/schema unset). A network failure is BADGE_OK with status BADGE_ATTEST_UNKNOWN. */
badge_err_t badge_attest_check(const uint8_t subject[32], const char *claimed_name, bool force, badge_attest_t *out);
badge_err_t badge_attest_self(bool force, badge_attest_t *out);
bool        badge_attest_cached(const uint8_t subject[32], badge_attest_t *out);

/* ---- pay (CAP_RADIO; request/receive also CAP_SIGN) ---- */
typedef struct { uint8_t id[8]; uint8_t payee[32]; char name[33]; uint64_t amount; uint8_t mac[6]; int8_t rssi; uint32_t age_ms; bool sig_ok; } badge_pay_req_t;
typedef struct { uint8_t pubkey[32]; char name[33]; uint32_t age_ms; } badge_pay_peer_t;
typedef enum { BADGE_PAY_IDLE = 0, BADGE_PAY_OPEN, BADGE_PAY_RECEIVE, BADGE_PAY_PAID, BADGE_PAY_EXPIRED } badge_pay_state_t;
typedef enum { BADGE_PRESENCE_NONE = 0, BADGE_PRESENCE_PENDING, BADGE_PRESENCE_PRESENT, BADGE_PRESENCE_LATE,
               BADGE_PRESENCE_BAD_SIG, BADGE_PRESENCE_TIMEOUT } badge_presence_t;
typedef struct { badge_pay_state_t state; uint8_t id[8]; uint64_t amount; uint32_t remaining_ms; uint8_t proofs;
                 bool has_payer; uint8_t payer[32]; bool has_tx; uint8_t tx_sig[64]; } badge_pay_status_t;
#define BADGE_PAY_TIMEOUT_MS 1500             /* == badge.pay.TIMEOUT_MS; the deadline is badge_wallet_info().deadline_ms */
/* ttl_s above 120 is clamped to 120. bad_arg (amount or ttl_s 0), over_limit, busy, denied, not_ready,
   no_display, rate_limited, rejected, approval_timeout, sign_failed. */
badge_err_t badge_pay_request(uint64_t amount, uint16_t ttl_s, uint8_t id_out[8]);
/* ttl_s above 600 is clamped to 600. bad_arg, busy, denied, not_ready, no_display, rate_limited, rejected, approval_timeout. */
badge_err_t badge_pay_receive(uint16_t ttl_s);
/* F19 (stretch): sign and send the receipt to the badge that sent the PAID hint. Once per session.
   denied, no_session (session not in state paid), rate_limited (already sent), sign_failed, io. */
badge_err_t badge_pay_receipt(void);
void        badge_pay_cancel(void);
void        badge_pay_status(badge_pay_status_t *out);
size_t      badge_pay_inbox(badge_pay_req_t *out, size_t max);
void        badge_pay_dismiss(const uint8_t id[8]);
/* hello, challenge, paid: denied, bad_arg, not_ready, io (ESP-NOW off or send refused);
   challenge also rate_limited after 3 attempts for one request. */
badge_err_t badge_pay_hello(const uint8_t mac[6]);
bool        badge_pay_peer(const uint8_t mac[6], badge_pay_peer_t *out);
badge_err_t badge_pay_challenge(const uint8_t mac[6], const uint8_t *id_or_null);
badge_presence_t badge_pay_presence(const uint8_t mac[6], uint32_t *elapsed_ms);
badge_err_t badge_pay_paid(const uint8_t mac[6], const uint8_t id[8], const uint8_t tx_sig[64]);

/* ---- history (CAP_WALLET) ---- */
typedef struct { uint8_t dir; uint8_t status; bool verified; bool has_sig; uint8_t peer[32]; char name[33];
                 uint64_t amount; uint8_t sig[64]; uint32_t uptime_s;
                 bool this_boot;      /* false: written before the last reboot, uptime_s is not comparable with now */
                 bool receipt;        /* F19: a valid co-signed receipt was received for this payment */
               } badge_history_t;   /* dir 0 out, 1 in; status 0 signed 1 submitted 2 confirmed 3 failed */
size_t      badge_history_list(badge_history_t *out, size_t max);                /* newest first */
badge_err_t badge_history_mark(const uint8_t sig[64], uint8_t status);           /* denied, bad_arg (no such signature, bad status), io */
badge_err_t badge_history_add(const badge_history_t *entry);                     /* denied, bad_arg, io; this_boot and receipt are ignored */

/* Error codes of the upstream-mirroring functions
     badge_storage_read            bad_arg (path leaves the app directory), io (missing or unreadable), too_long (file larger than cap)
     badge_storage_write/append    bad_arg, io
     badge_http_get/post           denied, bad_arg (URL is not http:// or https://), no_network, timeout, io,
                                   too_long (body larger than cap; the first cap-1 bytes are written).
                                   An HTTP status of 400 or above is BADGE_OK; read *status.
     badge_espnow_broadcast/send   denied, bad_arg, too_long (len > 240), io (ESP-NOW off or the send was refused)
     badge_system_set_name         denied, bad_arg (not 1..23 printable ASCII)
     badge_system_launch           denied, bad_arg (no such app)
     badge_system_reboot           denied */

#ifdef __cplusplus
}
#endif
#endif
```

The `badge_app_desc_t` structure is what a native app exports; the `BADGE_APP` macro in the SDK header fills it. See [cpp-apps.md](cpp-apps.md#badge_app).

## Parity exceptions

The rule is that every function exists in both languages. These are the exceptions.

Declared Lua-only in version 1 of the C ABI [OURS: not needed by any shipped native app; add on demand]:

| Module | Lua-only |
|---|---|
| top level | `badge.version`, `print` (use `badge_log`) |
| `json` | the whole module (C++ apps get `jsmn` through the SDK header with `BADGE_SDK_WITH_JSMN`) |
| `gfx` | optional-colour defaults (C takes every colour explicitly); the colour constants not defined in `badge_api.h`: `SOLANA_TEAL`, `SOLANA_MAGENTA`, `PANEL`, `BORDER`, `YELLOW`, `CYAN`, `BLUE` |
| `input` | `keys()`, `COUNT` (in C the literal 6) |
| `led` | `COUNT` (in C the literal 2) |
| `system` | `apps()`, `chip()`, `uptime()`, `lua_memory()` |
| `storage` | `list()`, `space()` |
| `mic` | `read()`, `db()`, `enabled()` |
| `se050` | `atr()`, `test()`, `random_available()` |
| `wifi` | `connect`, `connect_enterprise`, `enterprise`, `disconnect`, `scan`, `scanning`, `networks`, `hotspot`, `ip`, `mac` |
| `espnow` | `signal()`, `beacon()`, `clear()`, `MAX_PAYLOAD` (in C the literal 240), `MAX_PEERS` (in C the literal 20) |
| `ble` | `address()`, `enabled()`, `listening()` |
| `app` | `kind()` (a native app is native) |

C-only, by nature: `badge_alloc`, `badge_free`, `badge_app_fail` (Lua has its own heap and `error()`), and the `badge_app_desc_t` descriptor.

Differences in shape, not in function:

| Lua | C |
|---|---|
| `system.name([new])` | `badge_system_name()` and `badge_system_set_name(name)` |
| `system.current_app()`, `app.id()` | `badge_app_id()` |
| `badge.api_version`, `app.api()` | the constant `BADGE_API_VERSION` |
| `identity.pubkey()` / `identity.pubkey_bytes()` | `badge_identity_pubkey_b58(out)` / `badge_identity_pubkey(out)` |
| `gfx.brightness([v])`, `led.brightness([v])` | one function with `set_or_minus1` |
| `espnow.enable([on] [, channel])`, `ble.enable([on] [, name])` | `badge_espnow_enable(on)`, `badge_ble_enable(on)` (no channel or name argument) |
| `sol.ata(owner [, mint])`, `wallet.format(raw [, decimals])`, `wallet.parse(text [, decimals])` | configured mint and decimals only |
| `rpc.balance([pubkey])`, `rpc.token_balance([owner])` | `NULL` for "this badge" |
| `attest.self([force])` | `badge_attest_self(force, out)` |
| `pay.TIMEOUT_MS` | `BADGE_PAY_TIMEOUT_MS` |
| `pay.DEADLINE_MS` | `deadline_ms` in the structure `badge_wallet_info(&info)` fills |

Every other Lua function has a C declaration with the same name, and every C function has a Lua form; the module tables above pair them.

## Requirements covered

- F1: `identity.pubkey()` and `identity.sign(message)` are exposed to Lua and to C/C++; signing is reachable only through the wallet core's approval screen ([badge.identity](#badgeidentity)).
- F4, F5, F6, F7: the calls the Home, Pay and Request apps are built from ([badge.rpc](#badgerpc), [badge.pay](#badgepay), [badge.sol](#badgesol), [badge.espnow](#badgeespnow), [badge.led](#badgeled)).
- F8, F9: the app-facing view of signed requests and presence checks ([badge.pay](#badgepay)).
- F10: `attest.check` ([badge.attest](#badgeattest)).
- F16: [badge.history](#badgehistory).
- NFR "sandbox fit": the blocking classes and budget extensions in [Conventions](#blocking-and-time-budgets).

## Open items

- [UNVERIFIED] Nothing in this document has run on a badge. The header was syntax-checked only. Fallback: none needed for the document; build and flash upstream first, then add the modules in order.
- [UNVERIFIED] RPC latency and connection reuse through a phone hotspot. Fallback: longer polling intervals, a second RPC URL.
- [UNVERIFIED] Presence deadline (400 ms default) and RSSI threshold (-75). Fallback: 800 ms with a secure-element key, 2500 ms without the fast Ed25519 backend; `rssi_min` -100.
- [UNVERIFIED] Token-account derivation time on the ESP32-S3. Fallback: cache per payee.
- [UNVERIFIED] The secure-element random functions upstream have not run on silicon. Fallback: `system.random_bytes`.
- [UNVERIFIED] The exact wording of the node's preflight error for an expired blockhash, which an app can only read from the message `rpc.send` returns. Fallback: match the substring `lockhash`, and otherwise show a generic failure line; see [apps/pay.md](../apps/pay.md#error-states).
