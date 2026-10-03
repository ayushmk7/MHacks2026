# Writing a Lua app

Purpose: how to write, push and debug a Lua app for the badge, including one that asks the wallet for a signature.

Audience: Lua authors. No knowledge of the firmware is assumed. Read [overview.md](overview.md) first for the rules both runtimes share.

Status: design, not yet built on hardware. The Lua runtime, the sandbox and the push tool exist upstream and were read in source. Every `badge.identity`, `badge.wallet`, `badge.sol`, `badge.codec`, `badge.json`, `badge.rpc`, `badge.attest`, `badge.pay`, `badge.history` and `badge.app` call below is specified and not yet implemented. The Lua listings in this document were syntax-checked on a development machine with `luac -p` (Lua 5.5.1; the badge runs Lua 5.4.8); none has run on a badge.

Upstream means Solana OS, `firmware/solana-os/` at commit `812b8c7` of the [upstream repository](https://github.com/spacemandev-git/solana-defcon-badge-26/tree/812b8c7/firmware/solana-os). Upstream paths below are relative to that directory.

## The smallest app

An app is a directory. Its name is the app id: `[a-z0-9._-]`, 1 to 32 characters.

```
apps/gm/
  main.lua
```

```lua
-- apps/gm/main.lua
function on_draw()
  local g = badge.gfx
  g.clear()
  g.text_center("gm", g.width() // 2, 100, g.SOLANA_GREEN, 3)
end

function on_button(key, pressed)
  if key == "b" and pressed then badge.system.exit() end
end
```

[UPSTREAM README "The smallest app"]

Push it and run it (the pairing code is on Settings → Push):

```sh
cd firmware/solana-os
tools/badge-push.py --host <badge-ip> --token 123456 push apps/gm --run
```

What the listing shows:

- Entry points are global functions. All are optional: `on_start()`, `on_update(dt)`, `on_draw()`, `on_button(key, pressed)`, `on_espnow(mac, data, rssi)`, `on_ble(line)`, `on_stop()`. The script's top level runs once before `on_start`.
- Each frame runs in this order: radio events, `on_button` per edge, `on_update(dt)`, `on_draw()`, then the runtime pushes the framebuffer. You never call a "present" function.
- `key` is one of `"up"`, `"down"`, `"left"`, `"right"`, `"a"`, `"b"`. On the board `"a"` is the key marked SELECT and `"b"` is the key marked CANCEL. Write SELECT and CANCEL in on-screen hints; `badge.input.label(key)` returns the silkscreen word.
- CANCEL should always go back or exit. Holding CANCEL for 1.5 s force-quits any app regardless of what it does.
- The screen is 320×240. The font is ASCII only, 6×8 px at size 1; size 2 is 12×16, size 3 is 18×24.

Every function is in [api-reference.md](api-reference.md).

## app.ini with permissions

`app.ini` sits next to `main.lua`. Every key is optional.

```ini
# apps/balance/app.ini
name=Balance
version=1.0.0
author=you
description=Shows this badge's HACK balance
permissions=net
min_api=2
```

| Key | Meaning |
|---|---|
| `name`, `version`, `author`, `description`, `entry` | as upstream; `entry` defaults to `main.lua` [UPSTREAM `src/apps/app_store.cpp:48-52`] |
| `permissions` | comma-separated: `sign`, `net`, `radio`, `wallet`, `system`. Absent means `net,radio,system` [OURS] |
| `min_api` | the app is not launched on firmware whose `badge.api_version` is lower. Use `2` for anything that calls the wallet modules [OURS] |

Which permission a call needs:

| Permission | Calls |
|---|---|
| `sign` | `identity.sign`, `pay.request`, `pay.receive` |
| `net` | `wifi.*`, `http.*`, `rpc.*`, `attest.*` |
| `radio` | `espnow.*`, `ble.*`, `pay.*` |
| `wallet` | `history.*` |
| `system` | `system.launch`, `system.name(new)`, `system.reboot` |

Everything else needs none. Ask for the smallest set: the launcher marks every app that holds `sign` with `$`, and the approval screen names the app that asked.

Leaving `permissions` out gives `net,radio,system` and **not** `sign`: an app that never mentions permissions cannot ask for a signature.

A missing permission shows up in two ways. Wallet modules return `nil, "denied"`. Upstream modules (`wifi`, `http`, `espnow`, `ble`, and the guarded `system` calls) raise a Lua error that names the permission, which stops the app.

### A first wallet app: read the balance

```lua
-- apps/balance/main.lua    (permissions=net)
local g = badge.gfx
local me = badge.identity.pubkey()            -- base58 string, or nil if the badge has no identity
local line = "SELECT to read balance"

function on_button(key, pressed)
  if not pressed then return end
  if key == "b" then badge.system.exit() return end
  if key ~= "a" then return end
  local raw, err = badge.rpc.token_balance()  -- blocks up to 4 s; raw is a decimal STRING
  if raw then
    line = badge.wallet.format(raw) .. " " .. badge.wallet.info().symbol
  else
    line = "error: " .. err
  end
end

function on_draw()
  g.clear()
  g.text_center(me and badge.sol.short(me) or "no identity", 160, 60, g.SOLANA_GREEN, 3)
  g.text_center(line, 160, 120, g.WHITE, 2)
  g.text_center("SELECT balance   CANCEL exit", 160, 226, g.MUTED, 1)
end
```

The network call is made from `on_button`, once per press, not from `on_update` every frame.

### Asking for a signature

```ini
# apps/signtest/app.ini
name=Sign test
permissions=sign,net
min_api=2
```

```lua
-- apps/signtest/main.lua    (permissions=sign,net)
local g = badge.gfx
local TO = "Gn2GQYmcQkatE2594ov2ELKkYWZHwARG7FiAoPWSEcxq"   -- example key: the stand-in merchant badge
local AMOUNT = "1000"                                        -- raw units: 10.00 HACK at 2 decimals
local out = "SELECT to pay 10.00 HACK"

function on_button(key, pressed)
  if not pressed then return end
  if key == "b" then badge.system.exit() return end
  if key ~= "a" then return end

  local bh, e1 = badge.rpc.blockhash()
  if not bh then out = "rpc: " .. e1 return end

  local msg, e2 = badge.sol.transfer_message{ to = TO, amount = AMOUNT, blockhash = bh }
  if not msg then out = "build: " .. e2 return end

  -- Blocks here on the wallet's approval screen until SELECT, CANCEL or 60 s.
  local sig, e3, detail = badge.identity.sign(msg, { recipient = TO, claimed_amount = AMOUNT })
  if not sig then
    out = "not signed: " .. e3 .. (detail and (" (" .. detail .. ")") or "")
    return
  end

  local txid, e4, message = badge.rpc.send(badge.sol.wire(msg, sig))
  out = txid and ("sent " .. txid:sub(1, 8)) or ("send: " .. e4 .. " " .. (message or ""))
end

function on_draw()
  g.clear()
  g.text_center(out, 160, 110, g.WHITE, 1)
  g.text_center("SELECT pay   CANCEL exit", 160, 226, g.MUTED, 1)
end
```

What happens at `badge.identity.sign`:

1. The app stops executing. It cannot draw, read buttons, change the backlight or dismiss anything until the call returns.
2. The wallet core decodes the message bytes itself and draws its own screen from them: amount, token, recipient, identity and presence. The hints the app passed can make the screen more suspicious (`APP SAID …`), never less.
3. SELECT on that screen signs. CANCEL returns `nil, "rejected"`. No decision in 60 s returns `nil, "approval_timeout"`.
4. Without `permissions=sign` the call returns `nil, "denied"` and no screen appears.

The app in this listing passes no payment request id and performs no presence check, so the best the screen can show is amber with `presence not checked`; signing then takes a 2 s hold of SELECT. [Severity and gestures](../wallet-core/signing-gate.md#severity-and-gestures) explains the colours; [examples.md](examples.md) walks through a complete payment app.

A Lua app can draw something that looks like the approval screen before calling `identity.sign`. It gains nothing: the real screen follows, is redrawn in full every frame, and only its SELECT signs.

### Multiple files

`require` works. `package.path` is `/littlefs/apps/<id>/?.lua;/littlefs/apps/<id>/?/init.lua;/littlefs/lib/?.lua`, so `require("dec")` finds `dec.lua` next to `main.lua` [UPSTREAM `src/lua_sdk/lua_runtime.cpp:163-177`]. There is no `package.cpath`; no native code can be loaded.

## Sandbox limits

[UPSTREAM README "The sandbox", `src/lua_sdk/lua_runtime.cpp`, `src/lua/linit.c`] unless tagged otherwise.

| Limit | Value |
|---|---|
| VM | Lua 5.4.8, one fresh state per app, closed at stop |
| Libraries | `_G`, `package`, `coroutine`, `table`, `string`, `math`, `utf8`. No `io`, no `debug`. `os` is trimmed to `time`, `clock`, `date`, `difftime` |
| Bytecode | refused: `load`, `loadfile`, `dofile` are text-only; `string.dump` is disabled |
| Memory | 1 MiB Lua heap. Running out is an ordinary Lua error (`not enough memory`). `badge.system.lua_memory()` returns used and limit |
| Time | 250 ms per callback; 5000 ms for the top level plus `on_start`. Past it: `app exceeded its time budget (stuck in a loop?)` and the app stops |
| Blocking calls | `http.*`, `rpc.*`, `attest.check`, `system.sleep` extend the budget by their timeout, at most 12 s in total per callback |
| Wallet screens | `identity.sign`, `pay.request`, `pay.receive` pause the budget for as long as the user takes, at most 60 s [OURS] |
| Files | `badge.storage` is confined to `/apps/<id>/`; relative paths of at most 96 characters, no `..` |
| Key/value | `badge.storage.kv`, keys of at most 8 characters, string values, namespaced per app |
| Numbers | 32-bit integers and 32-bit floats; see the next section |
| Clock | no real-time clock: `os.time()` is seconds since boot unless the network has set the clock |
| Apps at once | one |

What the sandbox is not: upstream states that these limits stop accidents, not a hostile app. A hostile Lua app can still spin inside a binding, flood a radio or fill the filesystem. What it cannot do on this firmware is reach the key, sign without the approval screen, or draw over that screen [OURS].

### Working inside the time budget

A payment needs several network calls (identity check, blockhash, send, status). Each may take up to 4 s. Do not chain more than two in one callback. Run one step per frame instead:

```lua
-- One blocking step per frame. Each step returns true to continue, or nil, err to stop.
local steps, at, failed = {}, 0, nil

local function run(list) steps, at, failed = list, 1, nil end

function on_update(dt)
  local step = steps[at]
  if not step then return end
  local ok, err = step()
  if ok then at = at + 1 else steps, failed = {}, err end
end

function on_button(key, pressed)
  if key == "a" and pressed then
    run({
      function() return badge.rpc.blockhash() end,
      function() return badge.rpc.balance() end,
    })
  elseif key == "b" and pressed then
    badge.system.exit()
  end
end

function on_draw()
  badge.gfx.clear()
  badge.gfx.text_center(failed or ("step " .. at), 160, 110, badge.gfx.WHITE, 2)
end
```

Between two steps the runtime draws a frame, so the screen can show progress. The Pay app is built this way; see [apps/pay.md](../apps/pay.md).

## The 32-bit number rule

The badge's Lua is built with 32-bit integers and 32-bit floats [UPSTREAM `src/lua/luaconf.h:125`]. Consequences:

| Fact | Consequence |
|---|---|
| Integers are `int32`: −2 147 483 648 to 2 147 483 647 | `2147483647 + 1` wraps to a negative number |
| Floats have a 24-bit mantissa | integers above 16 777 216 are not exact as floats |
| Token amounts and lamport balances are `u64` on chain | they do not fit a Lua number |

So the rule of the whole API is: **amounts cross as decimal strings of raw base units** [OURS].

| Quantity | Example | In Lua |
|---|---|---|
| 10.00 HACK at 2 decimals | raw 1000 | `"1000"` |
| 500.00 HACK | raw 50000 | `"50000"` |
| 0.05 SOL | 50 000 000 lamports | `"50000000"` |
| 5 SOL | 5 000 000 000 lamports | `"5000000000"` (does not fit an integer) |

Use the firmware to convert, never floating point:

```lua
badge.wallet.format("1000")            --> "10.00"      raw string -> display string, configured decimals
badge.wallet.format("50000000", 9)     --> "0.050000000" lamports -> SOL
badge.wallet.parse("10")               --> "1000"       display string -> raw string
badge.wallet.parse("0.05")             --> "5"
badge.wallet.parse("abc")              --> nil
```

Rules for app code:

1. Keep an amount as the string you received. Pass it on as a string.
2. Never call `tonumber` on a balance, and never multiply an amount by `100` in Lua.
3. `badge.json.decode` follows the same rule: an integer within ±2^31 becomes a Lua integer, and any other number becomes a string. JSON `null` becomes the sentinel `badge.json.null`, not `nil`; compare with `==`.
4. Small amounts that you step with the D-pad (a picker bounded by the wallet's cap) may be Lua integers while you edit them. Convert with `tostring` before passing them to the API.
5. To compare or add two amounts, use string arithmetic.

A helper for rule 5, to copy next to `main.lua` as `dec.lua`:

```lua
-- dec.lua : comparison and addition of non-negative decimal strings (raw amounts, lamports)
local dec = {}

local function strip(s)
  s = s:gsub("^0+", "")
  return s == "" and "0" or s
end

function dec.cmp(a, b)                 -- returns -1, 0 or 1
  a, b = strip(a), strip(b)
  if #a ~= #b then return #a < #b and -1 or 1 end
  if a == b then return 0 end
  return a < b and -1 or 1
end

function dec.add(a, b)
  local out, carry = {}, 0
  local i, j = #a, #b
  while i >= 1 or j >= 1 or carry > 0 do
    local d = carry
    if i >= 1 then d = d + a:byte(i) - 48 end
    if j >= 1 then d = d + b:byte(j) - 48 end
    carry = d // 10
    out[#out + 1] = string.char(48 + d % 10)
    i, j = i - 1, j - 1
  end
  return strip(string.reverse(table.concat(out)))
end

function dec.toint(s)                  -- a Lua integer when the value is at most 9 digits, else nil
  s = strip(s)
  if #s > 9 then return nil end
  return math.tointeger(tonumber(s))
end

return dec
```

```lua
local dec = require("dec")
dec.cmp("100000", "99999")             --> 1
dec.add("4294967296", "1000")          --> "4294968296"
dec.toint("10000")                     --> 10000
dec.toint("5000000000")                --> nil
```

`dec.lua` was run on the development machine against these four cases and against random pairs checked with 64-bit arithmetic; it has not run on a badge.

Public keys, signatures and request ids are strings too: base58 for keys and transaction signatures, 16 hex characters for a request id, and raw byte strings for messages and for the 64-byte signature `identity.sign` returns. Lua strings are byte-safe, so `#msg` is the byte length.

A transaction signature is base58 everywhere it is used as an id: the result of `rpc.send`, the argument of `rpc.status`, `history.mark` and `pay.paid`, and the fields `pay.status().tx` and `history.list()[i].sig`. The one exception is the value `identity.sign` returns, which is the raw 64 bytes that `sol.wire` needs; `badge.codec.b58encode(sig)` converts it. [OURS]

## Pushing with badge-push.py

The tool is `firmware/solana-os/tools/badge-push.py` [UPSTREAM]. The badge and the laptop must be on the same network (the phone hotspot), or the laptop joins the badge's own hotspot.

Find two things on the badge under Settings → Push: its IP address and its six-digit pairing code.

```sh
cd firmware/solana-os
export BADGE_TOKEN=123456                                   # instead of --token on every call

tools/badge-push.py --host <badge-ip> push apps/tipjar --run   # send the directory, then launch it
tools/badge-push.py --host <badge-ip> list                     # installed apps
tools/badge-push.py --host <badge-ip> run tipjar               # launch
tools/badge-push.py --host <badge-ip> stop                     # back to the launcher
tools/badge-push.py --host <badge-ip> rm tipjar                # uninstall
tools/badge-push.py --host <badge-ip> logs                     # the log ring
tools/badge-push.py --host <badge-ip> status
```

- `push <dir>` sends every `.lua`, `.ini`, `.txt`, `.json`, `.csv`, `.png`, `.jpg` in the directory, recursively, skipping dotfiles. One file may be at most 96 KB [UPSTREAM README "Pushing apps", `src/config.h:151`].
- `--host solana-badge.local` works where mDNS does; use the IP otherwise.
- Over BLE: `tools/badge-push.py --ble badge-4F2A --token 123456 push apps/tipjar --run` (needs `pip install bleak`; BLE is off at boot by default).
- Without the tool: `curl -H "X-Badge-Token: 123456" --data-binary @main.lua "http://<badge-ip>/api/app?id=tipjar&path=main.lua"`, then `curl -H "X-Badge-Token: 123456" -X POST "http://<badge-ip>/api/run?id=tipjar"` [UPSTREAM README "HTTP API"].
- Five wrong pairing codes lock the write path out with a growing backoff [UPSTREAM].
- Pushing an app whose id belongs to a compiled-in native app is refused with HTTP 409 [OURS].
- While a wallet screen is up the push server is not serviced: `stop`, `run` and `logs` take effect after the user has decided [OURS]. A remote command cannot remove an approval prompt.

To ship apps inside the firmware image instead of pushing them, see [configure.md](../guides/configure.md).

## Reading logs

`badge.log(...)` and `print(...)` write a line tagged `[app]`. Firmware lines carry their own tags.

| Where | How |
|---|---|
| Laptop, over Wi-Fi | `tools/badge-push.py --host <badge-ip> --token 123456 logs`, or `GET /api/logs` |
| On the badge | Settings → Console |
| USB serial | 115200 baud, for example `arduino-cli monitor -p /dev/cu.usbserial-XXXX -c baudrate=115200` |

The ring holds the last 64 lines of 120 characters [UPSTREAM `src/badge_log.h`].

| Tag | Source |
|---|---|
| `app` | your `badge.log` and `print` lines [UPSTREAM] |
| `wallet` | gate decisions and audit lines [OURS] |
| `pay` | payment-protocol frames, sessions, timings [OURS] |
| `attest` | identity checks [OURS] |
| `rpc` | one line per RPC call with its duration [OURS] |

When an app dies, the message and a Lua traceback are also shown on the badge's error screen; SELECT retries the app [UPSTREAM `src/ui/shell.cpp:1250-1300`].

Testing without a badge: upstream ships a browser emulator of the Lua SDK. It does not know the wallet modules. To exercise an app's screens there, stub `badge.identity`, `badge.pay`, `badge.rpc`, `badge.attest` and `badge.sol` in a Lua file of your own. This is optional and not provided [OURS].

## Common errors

| Symptom | Likely cause | Fix |
|---|---|---|
| `identity.sign` returns `nil, "denied"` | `app.ini` lacks `permissions=sign` | add it, push again |
| The app stops with a Lua error naming a permission | an upstream module (`http`, `espnow`, …) was called without `net` or `radio` | add the permission |
| `identity.sign` returns `nil, "not_ready"` | the HACK mint is not configured, or the badge has no identity | see [configure.md](../guides/configure.md); check the `[id]` line at boot |
| Blocked screen "Unknown instruction", app gets `nil, "unknown_instruction", <detail>` | the message is not exactly one SPL Token `TransferChecked`; for example a wire transaction was passed instead of a message | pass the message bytes; build with `sol.transfer_message` |
| Blocked screen "Not your token account" | wrong mint configured, or the message was built for another source | compare `badge.sol.ata(badge.identity.pubkey())` with the dashboard's token account |
| `rate_limited` | a second request within 2 s of a rejection, or three non-approvals in 60 s | wait; do not retry in a loop |
| `busy` | another prompt or payment session is active | finish or cancel it |
| `app exceeded its time budget (stuck in a loop?)` | a loop that does not return, or more than 12 s of blocking calls in one callback | one step per frame; see [Working inside the time budget](#working-inside-the-time-budget) |
| `not enough memory` with plenty of free RAM | the app's own 1 MiB cap | check `badge.system.lua_memory()` |
| `nil, "wifi not connected"` from `http.*`; `nil, "no_network"` from `rpc.*` | no Wi-Fi and no phone bridge | Settings → Wi-Fi |
| An amount is wrong by a few units, or negative | it went through a Lua number | keep amounts as strings; see [The 32-bit number rule](#the-32-bit-number-rule) |
| `on_espnow` never fires in the second app launched after boot | an upstream bug that this firmware patches | make sure the patched firmware is flashed; see [runtime-and-boot.md](../architecture/runtime-and-boot.md) |
| Other badges do not appear in `espnow.peers()` | badges on different Wi-Fi channels | join every badge to the same hotspot |
| Launcher is empty after pushing | the app id has characters outside `[a-z0-9._-]` | rename the directory |
| `kv: key '…' is too long` | key/value keys are at most 8 characters | shorten the key |
| The badge is stuck in an app | — | hold CANCEL for 1.5 s |

More symptoms, including firmware-side ones, are in [troubleshooting.md](../guides/troubleshooting.md). Error strings are listed in [error-codes.md](../reference/error-codes.md#badge_err_t).

## Requirements covered

- F1: `identity.pubkey()` and `identity.sign(message)` from Lua, and what the caller observes while the approval screen is up.
- NFR "sandbox fit": how signing and network calls interact with the 250 ms callback budget.
- NFR "judge UX": SELECT and CANCEL naming, CANCEL always exits.
- Acceptance checks T-F1 and T-F1b in [acceptance.md](../testing/acceptance.md) use an app like `signtest` above.

## Open items

- [UNVERIFIED] No listing here has run on a badge; they were syntax-checked on a host with a newer Lua (5.5.1 against the badge's 5.4.8). Fallback: fix on first push; the error screen shows the line.
- [UNVERIFIED] RPC call time through a phone hotspot, which decides how many calls fit a callback. Fallback: one call per frame.
- [UNVERIFIED] Whether the phone hotspot lets the laptop reach the badge (client isolation), which the push tool needs. Fallback: join the badge's own hotspot for pushing, or use USB serial.
