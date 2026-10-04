# Upstream baseline

What Solana OS already provides, with the exact names our code calls. Every fact here was read from the upstream source at commit `812b8c7` of <https://github.com/spacemandev-git/solana-defcon-badge-26>, folder `firmware/solana-os/`. Nothing here has been run on a badge by us.

Use this instead of guessing an upstream function name. If something you need is not listed, read the upstream file named in the table and add it here.

## Hardware

| Part | Value |
|---|---|
| MCU | ESP32-S3-WROOM-1-N16R8: 16 MB flash, 8 MB OPI PSRAM |
| Display | 2.8" ILI9341, 320×240 landscape, drawn through one RGB565 sprite in PSRAM |
| Buttons | 6, behind a TCA9534 I²C expander at `0x20` |
| LEDs | **2** × WS2812B (`RGB_LED_COUNT = 2`), default brightness 72 |
| Secure element | NXP SE050 at I²C `0x48` |
| Microphone | PDM |
| USB | CH340 USB-serial bridge (USB id `1a86:7523`), appears as `/dev/cu.usbserial-*` on macOS |

## Build

- Arduino sketch built with `arduino-cli` on the Arduino-ESP32 3.x core, with the LovyanGFX library. Not ESP-IDF, not PlatformIO.
- FQBN: `esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom,CDCOnBoot=cdc`
- Everything under the sketch's `src/` is compiled recursively.
- Image about 1.75 MB in a 3.2 MB application slot (`0x330000`). LittleFS volume 9.5 MB at `0x670000`.

## Files and the functions we use

| File | What we use |
|---|---|
| `solana-os.ino` (in our fork: `os.ino`) | `setup()`, `loop()`, `routeButtons()`, `pumpSerialConsole()`, `startRadios()`. Hook sites H1–H6 |
| `src/config.h` | `SOLANA_OS_API_VERSION` (1), `BTN_UP/LEFT/RIGHT/DOWN/A/B` (0,1,2,3,4,5), `BUTTON_COUNT`, `APP_ESCAPE_HOLD_MS` (1500), `RGB_LED_COUNT` (2), `LCD_WIDTH/HEIGHT`, `ESPNOW_MAX_PAYLOAD` (240), `LUA_CALLBACK_BUDGET_MS` (250), `LUA_CALLBACK_EXTENSION_CAP_MS` (12000), `FS_ROOT` (`"/littlefs"`), `APPS_DIR`, `LIB_DIR`, `APP_MANIFEST` (`"app.ini"`) |
| `src/hal/display.h` | `display::canvas()` (an `LGFX_Sprite&`), `width()`, `height()`, `touch()`, `flush()`, `invalidate()`, `text()`, `textCentered()`, `textRight()`, `card()`, `statusBar(title)`, `STATUS_BAR_HEIGHT` (22). Nothing paints the panel directly; `flush()` at the end of `loop()` pushes the canvas |
| `src/hal/buttons.h` | `buttons::down(key)`, `pressed(key)`, `released(key)`, `repeated(key)`, `heldMs(key)`, `downMask()`. Edge masks are valid for the current loop pass |
| `src/hal/leds.h` | `leds::set(i,r,g,b)`, `setAll`, `off()`, `show()`, `pulse(r,g,b,ms)`, `playBoot()`, `playIdle()`, `stopAnimation()`, `animating()`, `update()` |
| `src/identity/identity.h` | `identity::ready()`, `source()` (`Source::SecureElement` / `Software` / `None`), `sourceName()`, `publicKey()` (32 bytes), `publicKeyBase58()`, `badgeId()`, `sign(msg, len, out64)`, `signBase64(...)`, `base58Encode`, `base64Encode` |
| `src/identity/ed25519.h` | `ed25519::verify(msg, len, sig64, pub32)` (TweetNaCl). Upstream notes about one second per operation on this chip (`identity.cpp:25-27`, `TWEETNACL-README`). The software signing path accepts up to 4096 bytes and blocks for that long |
| `src/hal/se050_apdu.h` | `MAX_SIGN_MESSAGE_BYTES = 180`. The SE050 signing path refuses longer messages |
| `src/lua_sdk/lua_runtime.h` | `runtime::running()`, `currentApp()`, `requestLaunch(id)`, `requestStop()`, `update()`, `dispatchButton()`, `dispatchEspnow()`, `dispatchBle()`, `state()`, `extendDeadline(ms)`, `lastError()` |
| `src/lua_sdk/lua_bindings.h` | `bindings::openBadge(L)`, `setTable(L, name, functions, constants)`. Module openers: `openGfx`, `openInput`, `openLed`, `openSystem`, `openStorage`, `openSensors` (`battery`, `mic`, `se050`), `openNet` (`wifi`, `http`), `openEspnow`, `openBle` |
| `src/apps/app_store.h` | `app_store::count()`, `at(i, Info&)`, `byId`, `exists(id)`, `readFile(id, path, out)`, `directory(id)`, `isValidId(id)`. `Info` has `id, name, version, author, description, entry, sizeBytes`. App ids match `[a-z0-9._-]` |
| `src/net/espnow_mgr.h` | `send(mac, data, len)` (true means queued, not delivered; peers are registered automatically), `broadcast(data, len)`, `onReceive(handler)`, `clearReceiveHandler()`, `peerByMac`, `macToString`, `macFromString`, `enabled()` |
| `src/net/net_route.h` | `net_route::request(method, url, body, contentType, timeoutMs, headers, headerCount)` → `Response{ok, status, body, err}`; `available()`, `wifiUp()`. Only the method string `"POST"` sends a body; anything else is a GET. HTTPS is not pinned to any CA. Connect and read each get `timeoutMs`, so budget twice the timeout |
| `src/net/wifi_mgr.h` | `wifi_mgr::connected()` (also true in hotspot mode), `mode()` (`Mode::Off`, `Station`, `AccessPoint`), `connect(ssid, password, save)` (what `JOINWIFI` calls; with `save` true it stores the network) |
| `src/net/ble_mgr.h` | the line handler Lua's `badge.ble.listen()` installs; `ble_mgr::clearLineHandler()` |
| `src/net/push_protocol.h` | `handleLine(line, reply)`: the line protocol shared by USB serial and BLE (`PING`, `AUTH`, `LIST`, `BEGIN`, `DATA`, `END`, `DEL`, `RUN`, `STOP`, `SETWIFI`, `JOINWIFI`, ...) |
| `src/net/push_server.cpp` | HTTP API on port 80: `/api/status`, `/api/apps`, `/api/app`, `/api/run`, `/api/stop`, `/api/wifi`, `/api/identity`, `/api/logs`, `/api/reboot`. Header `X-Badge-Token: <pairing code>` |
| `src/net/broker_client.cpp` | App-store client. Signs `"solana-badge-register:" + pubkey_b58 + ":" + nonce` with `identity::signBase64` (hook H10) |
| `src/settings.h` | `settings::deviceName()`, `pairingCode()`, `brightness()`, `setAutostartApp(id)`, `autostartApp()`, `wifiSsid()`, `kvGet/kvSet`. NVS namespaces `sysconf` and `luakv` (plus `badgeid` for the identity) share one 20 KB partition |
| `src/ui/shell.cpp` | Launcher and settings screens. Launcher lists `app_store` entries then "Settings". SELECT runs, RIGHT deletes, CANCEL opens settings. It redraws only when its private `sDirty` flag is set (input, a change in the app count or battery percent) |
| `src/ui/boot.cpp` | `boot::progress(step, detail, percent)`; called from `setup()` as each stage **begins**, with 20, 45, 55, 65, 85, 100 |
| `src/ui/theme.h` | `theme::BG`, `HEADER`, `PANEL`, `BORDER`, `WHITE`, `TEXT`, `MUTED`, `GREEN`, `PURPLE`, `WARN`, `ERR`, `rgb565(r,g,b)` |
| `src/badge_log.h` | `badge_log::tagf(tag, fmt, ...)`, `println`, `printf` |
| `tools/badge-push.py` | Pushes an app folder over HTTP or BLE |

## Lua app model

- One app at a time, one fresh `lua_State` per launch. Heap capped at 1 MB from PSRAM.
- Entry points are globals: `on_start()`, `on_update(dt)`, `on_draw()`, `on_button(key, pressed)`, `on_espnow(mac, data, rssi)`, `on_ble(line)`, `on_stop()`.
- Each callback has 250 ms (`on_start` and the script's top level get 5000 ms). A blocking binding may extend it with `runtime::extendDeadline(ms)`, up to 12 s per callback in total.
- `require` searches the app's folder, then `/lib/?.lua`.
- Lua is built with 32-bit integers and `float` numbers (`src/lua/luaconf.h`). A token amount does not fit in a Lua number.
- `badge.http.get(url [, content_type [, timeout_ms]])` and `badge.http.post(url, body [, content_type [, timeout_ms]])` return `status, body` or `nil, message`. Timeout 100–10000 ms, default 5000.
- `badge.gfx.flush()` exists and pushes the canvas to the panel from inside a callback. `badge.gfx.brightness(n)` sets the backlight, including to zero.
- `badge.system.apps()`, `badge.system.launch(id)`, `badge.system.exit()` and `badge.system.current_app()` exist.
- The standard globals `loadfile`, `dofile` and `require` are present and open any absolute path as a text chunk.

## ESP-NOW framing

Upstream wraps every frame as `'S','B','D','G'` (4 bytes), a type byte (`0x01` beacon, `0x02` app), then up to 240 bytes of payload. Beacons fill the peer table. App payloads are queued by the Wi-Fi task (a ring of 8 slots, 7 usable; when full the newest frame is dropped silently) and drained by `espnow_mgr::update()` on the main loop, which calls the one receive handler. Our frames ([../protocol/espnow.md](../protocol/espnow.md)) are the payload of a type `0x02` frame.

## Findings that shape the design

| # | Finding | Where | What we do |
|---|---|---|---|
| F1 | After the first app stops, no app ever receives ESP-NOW again: `runtime::stop()` calls `espnow_mgr::clearReceiveHandler()`, and the handler is installed only once, in `startRadios()` | `lua_runtime.cpp:290`, `solana-os.ino:134` | Hook H9 removes the clear; the router owns the handler (H3) |
| F2 | The SE050 path refuses messages over 180 bytes; a token transfer message is 214 | `se050_apdu.h:69` | Hook H12 raises it to 242; [signing](../wallet/signing.md#key) covers what still does not fit |
| F3 | The clock is seeded from the build date only on the WPA2-Enterprise join path; on an ordinary hotspot it reads 1970 plus uptime until something sets it. Either way the raw system time says nothing about whether it is right | `wifi_mgr.cpp:48,200` | The clock service keeps its own source state and never trusts the raw time |
| F4 | The store client signs with the badge key without a button press | `broker_client.cpp:513` | Hook H10 routes it through signing domain `store-reg` |
| F5 | `identity::sign` is public and callable from anywhere in the firmware | `identity.h` | Pre-flash check: the only caller is `src/vk/wallet/signer.cpp` |
| F6 | The SE050 code has never run on a real part (upstream's own note) | `se050_apdu.cpp` header | Work package WP50, with the software key as fallback |
| F7 | TweetNaCl is slow on this chip (about a second per operation, upstream's note) | `identity.cpp:25` | Measured in WP51; Monocypher backend is the fallback |
| F8 | Lua can push to the panel mid-callback | `lib_gfx.cpp` `flush` | The approval pauses all app code |
| F9 | The upstream repository has no licence file | repository root | Credit upstream; ask the author before publishing the fork |
| F10 | App files can only be written under `/apps/<id>/`; ids are 1–32 characters of `[a-z0-9._-]` and cannot contain `/` | `app_store.h`, `app_store.cpp:177` | Our stores under `/vk/` cannot be written by push or by Lua storage |
| F12 | The shell repaints only when its own dirty flag is set | `shell.cpp:1442-1469` | Hook H20 lets Badge OS request a repaint |
| F13 | Lua's `loadfile` and `dofile` can open any path as a chunk, leaking whether a file exists | `src/lua/linit.c` | `vk::lua::open` removes both globals |
| F14 | `leds::update()` redraws every 16 ms while an upstream animation is set, and the idle animation restarts whenever the launcher returns | `leds.cpp:296-360` | Our LED service calls `leds::stopAnimation()` on every frame it draws |
| F15 | A BLE line can run a Lua callback at any time, and queued launch/stop requests are applied outside the app/shell branch | `ble_mgr.cpp:275`, `solana-os.ino:294` | Hooks H19 and H4 |
| F16 | A launch that fails still resets the push session and the launcher cursor | `solana-os.ino:296-303` | Documented; the serial tool re-authenticates |
| F11 | The serial console and BLE share one line protocol | `push_protocol.h` | Our serial commands are intercepted in `pumpSerialConsole()` (H6), so they exist on USB only |
