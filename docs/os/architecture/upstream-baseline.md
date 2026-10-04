# Upstream baseline

What Solana OS already provides, with the exact names our code calls. Every fact here was read from the upstream source at commit `812b8c7` of <https://github.com/spacemandev-git/solana-defcon-badge-26>, folder `firmware/solana-os/`. Since WP01 (2026-10-03) the fork built from this source runs on a badge: it boots, the launcher and settings screens work, and upstream's `hello` sample app is pushed over serial, runs and exits. Facts below that were only read, not exercised, stay as read.

**What the fork no longer has of upstream.** BadgeOS keeps upstream's runtime, radios, push, store client and device key, and replaces its user interface: `src/ui/shell.cpp` is deleted (BadgeOS's shell is `src/vk/shell/`, [shell](../ui/shell.md)), `src/ui/boot.cpp` is rewritten without the splash images, `src/ui/theme.h` holds Receipt-light values, the six sample apps are deleted, and the names and network identifiers are BadgeOS's (hook H23). The full list is in [upstream-hooks.md](upstream-hooks.md#replaced-upstream-files). The rows below say where a fact describes upstream only. Upstream's own README (Lua API, app format, push protocol) is kept as [../reference/upstream-readme.md](../reference/upstream-readme.md), because `os/README.md` is now a short BadgeOS one.

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
| `src/config.h` | `SOLANA_OS_API_VERSION` (1), `BTN_UP/LEFT/RIGHT/DOWN/A/B` (0,1,2,3,4,5), `BUTTON_COUNT`, `APP_ESCAPE_HOLD_MS` (1500), `RGB_LED_COUNT` (2), `LCD_WIDTH/HEIGHT`, `ESPNOW_MAX_PAYLOAD` (240), `LUA_CALLBACK_BUDGET_MS` (250), `LUA_CALLBACK_EXTENSION_CAP_MS` (12000), `FS_ROOT` (`"/littlefs"`), `APPS_DIR`, `LIB_DIR`, `APP_MANIFEST` (`"app.ini"`). `SOLANA_OS_NAME`, `SOLANA_OS_VERSION` (`"0.1.0"`): the name and version the banner, the push protocol's `INFO` reply, `/api/status` and Settings → Device info print. In the fork (hook H23): `SOLANA_OS_NAME` is `"BadgeOS"`, `DEFAULT_HOSTNAME` is `"badgeos"` (upstream `"solana-badge"`), `DEFAULT_AP_PASSWORD` is `"badgeos-setup"` (upstream `"solanabadge"`), `DEFAULT_BROKER_URL` is empty (upstream's DEF CON broker). The `SPLASH_*` constants are unused |
| `src/hal/display.h` | `display::canvas()` (an `LGFX_Sprite&`), `width()`, `height()`, `touch()`, `flush()`, `invalidate()`, `text()`, `textCentered()`, `textRight()`, `card()`, `setBrightness()`, `STATUS_BAR_HEIGHT` (22). Nothing paints the panel directly; `flush()` at the end of `loop()` pushes the canvas. `statusBar(title)` still exists but nothing calls it in the fork: BadgeOS's screens draw their header with `receipt::header` ([ui](../ui/ui.md#the-receipt-kit)) |
| `src/hal/buttons.h` | `buttons::down(key)`, `pressed(key)`, `released(key)`, `repeated(key)`, `heldMs(key)`, `downMask()`. Edge masks are valid for the current loop pass |
| `src/hal/leds.h` | `leds::set(i,r,g,b)`, `setAll`, `off()`, `show()`, `pulse(r,g,b,ms)`, `playIdle()`, `stopAnimation()`, `animating()`, `update()`, `setBrightness()`. `playBoot()` exists but is no longer called (the boot shows the LED boot bar instead) |
| `src/identity/identity.h` | `identity::ready()`, `source()` (`Source::SecureElement` / `Software` / `None`), `sourceName()`, `publicKey()` (32 bytes), `publicKeyBase58()`, `badgeId()`, `sign(msg, len, out64)`, `signBase64(...)`, `base58Encode`, `base64Encode` |
| `src/identity/ed25519.h` | `ed25519::verify(msg, len, sig64, pub32)` (TweetNaCl). Upstream notes about one second per operation on this chip (`identity.cpp:25-27`, `TWEETNACL-README`). The software signing path accepts up to 4096 bytes and blocks for that long |
| `src/hal/se050_apdu.h` | `MAX_SIGN_MESSAGE_BYTES = 180`. The SE050 signing path refuses longer messages |
| `src/lua_sdk/lua_runtime.h` | `runtime::running()`, `currentApp()`, `requestLaunch(id)`, `requestStop()`, `update()`, `dispatchButton()`, `dispatchEspnow()`, `dispatchBle()`, `state()`, `extendDeadline(ms)`, `lastError()` |
| `src/lua_sdk/lua_bindings.h` | `bindings::openBadge(L)`, `setTable(L, name, functions, constants)`. Module openers: `openGfx`, `openInput`, `openLed`, `openSystem`, `openStorage`, `openSensors` (`battery`, `mic`, `se050`), `openNet` (`wifi`, `http`), `openEspnow`, `openBle` |
| `src/apps/app_store.h` | `app_store::count()`, `at(i, Info&)`, `byId`, `exists(id)`, `readFile(id, path, out)`, `directory(id)`, `isValidId(id)`, `refresh()`, `removeApp(id)`, `usedBytes()`, `totalBytes()`. `Info` has `id, name, version, author, description, entry, sizeBytes`. App ids match `[a-z0-9._-]` |
| `src/net/espnow_mgr.h` | `send(mac, data, len)` (true means queued, not delivered; peers are registered automatically), `broadcast(data, len)`, `onReceive(handler)`, `clearReceiveHandler()`, `peerByMac`, `macToString`, `macFromString`, `enabled()` |
| `src/net/net_route.h` | `net_route::request(method, url, body, contentType, timeoutMs, headers, headerCount)` → `Response{ok, status, body, err}`; `available()`, `wifiUp()`. Only the method string `"POST"` sends a body; anything else is a GET. HTTPS is not pinned to any CA. Connect and read each get `timeoutMs`, so budget twice the timeout |
| `src/net/wifi_mgr.h` | `wifi_mgr::connected()` (also true in hotspot mode), `mode()` (`Mode::Off`, `Station`, `AccessPoint`), `connect(ssid, password, save)` (what `JOINWIFI` calls; with `save` true it stores the network) |
| `src/net/ble_mgr.h` | the line handler Lua's `badge.ble.listen()` installs; `ble_mgr::clearLineHandler()` |
| `src/net/push_protocol.h` | `handleLine(line, reply)`: the line protocol shared by USB serial and BLE (`PING`, `AUTH`, `LIST`, `BEGIN`, `DATA`, `END`, `DEL`, `RUN`, `STOP`, `SETWIFI`, `JOINWIFI`, ...) |
| `src/net/push_server.cpp` | HTTP API on port 80: `/api/status`, `/api/apps`, `/api/app`, `/api/run`, `/api/stop`, `/api/wifi`, `/api/identity`, `/api/logs`, `/api/reboot`. Header `X-Badge-Token: <pairing code>` |
| `src/net/broker_client.cpp` | App-store client; off in the fork until a broker URL is set, because the default URL is empty (hook H23). The shell uses `broker::enabled()`, `setEnabled()`, `url()`, `state()`, `stateText()`, `registered()`, `lastError()`, `forget()`, `hasOffer()`, `offer()`, `accept()`, `decline()`, `installing()`, `installProgress()`, `installedCount()`, `failedCount()`, `lastResult()`, `clearResult()`. Signs `"solana-badge-register:" + pubkey_b58 + ":" + nonce` with `identity::signBase64` (hook H10) |
| `src/settings.h` | `settings::deviceName()`, `pairingCode()`, `brightness()`, `setAutostartApp(id)`, `autostartApp()`, `wifiSsid()`, `kvGet/kvSet`. NVS namespaces `sysconf` and `luakv` (plus `badgeid` for the identity) share one 20 KB partition |
| `src/ui/shell.h` | The interface `os.ino` calls: `shell::begin()`, `update()`, `onAppStopped()`, `showError(const String &)`. Untouched |
| `src/ui/shell.cpp` | **Deleted in the fork**; replaced by `src/vk/shell/` ([shell](../ui/shell.md)), which implements the four functions of `shell.h`. Upstream's version: launcher and settings screens; the launcher lists `app_store` entries then "Settings"; SELECT runs, RIGHT deletes, CANCEL opens settings; it redraws only when its private `sDirty` flag is set. Every upstream call its screens made is listed, screen by screen, in [shell.md](../ui/shell.md#settings-pages) |
| `src/ui/boot.cpp` | **Rewritten in the fork** (no splash; [the file](upstream-hooks.md#srcuibootcpp-complete)). Same interface and call sites: `boot::run()` once, then `boot::progress(step, detail, percent)` from `setup()` as each stage **begins**, with 20, 45, 55, 65, 85, 100 (and 75 from hook H2) |
| `src/ui/theme.h` | `theme::BG`, `HEADER`, `PANEL`, `BORDER`, `WHITE`, `TEXT`, `MUTED`, `GREEN`, `PURPLE`, `WARN`, `ERR`, `rgb565(r,g,b)`. **Edited in the fork**: the names stay and the values are Receipt-light ([table](upstream-hooks.md#srcuithemeh-the-values)). BadgeOS code does not use them; it uses `vk::ui::theme::color()` |
| `src/badge_log.h` | `badge_log::tagf(tag, fmt, ...)`, `println`, `printf` |
| `tools/badge-push.py` | Pushes an app folder over HTTP or BLE. Its texts say BadgeOS in the fork |
| `apps/` | Upstream shipped six sample apps (`dice`, `gallery`, `hello`, `radar`, `vumeter`, `whosnear`). **Deleted in the fork**; the dev-only fixtures (`nativetest` and the hidden Lua test apps) take their place in tests |

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

Upstream wraps every frame as a 4-byte magic, a type byte (`0x01` beacon, `0x02` app), then up to 240 bytes of payload. Beacons fill the peer table. App payloads are queued by the Wi-Fi task (a ring of 8 slots, 7 usable; when full the newest frame is dropped silently) and drained by `espnow_mgr::update()` on the main loop, which calls the one receive handler. Our frames ([../protocol/espnow.md](../protocol/espnow.md)) are the payload of a type `0x02` frame. The magic is `'S','B','D','G'` in upstream and `'B','D','O','S'` in the fork (hook H23), so a BadgeOS badge and an upstream badge ignore each other's frames and beacons.

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
| F12 | Upstream's shell repaints only when its own dirty flag is set | `shell.cpp:1442-1469` | Settled without a hook: BadgeOS's shell consumes `vk::ui::consumeShellRepaint()` itself and watches the values it shows ([shell](../ui/shell.md#framework)). Hook H20 is retired |
| F13 | Lua's `loadfile` and `dofile` can open any path as a chunk, leaking whether a file exists | `src/lua/linit.c` | `vk::lua::open` removes both globals |
| F14 | `leds::update()` redraws every 16 ms while an upstream animation is set, and the idle animation restarts whenever the launcher returns | `leds.cpp:296-360` | Our LED service calls `leds::stopAnimation()` on every frame it draws |
| F15 | A BLE line can run a Lua callback at any time, and queued launch/stop requests are applied outside the app/shell branch | `ble_mgr.cpp:275`, `solana-os.ino:294` | Hooks H19 and H4 |
| F16 | A launch that fails still resets the push session and, in upstream's shell, the launcher cursor | `solana-os.ino:296-303` | Documented; the serial tool re-authenticates. BadgeOS's launcher keeps its cursor |
| F17 | A Solana OS session can leave the SE050 holding the I²C clock line low until power is removed; the button expander is then unreadable under any firmware. Other facts found on the way: `0x4A` on the bus is a TSC2007 touch controller neither firmware knows; the SE050's real enable pin is hard-wired on, so GPIO8 cannot disable it; upstream's bus recovery only handles a stuck SDA, not a stuck SCL; `badge_i2c::readReg` labels every failure "NACK on address" whatever the real error | measured on the badge, 2026-10-03 | Hook H21 (provisional) keeps the SE050 off the bus; a power cycle clears an existing hold. **First evidence for H21 (WP12 flash, 2026-10-03):** after power was removed once, the bus was healthy under the fork: `[btn] TCA9534 @0x20 ready, input=0x3F`, `[btn] TCA9534 init ok`, no `stopped answering` and no `bus is held low` in a 200 s log, the heartbeat read `btn=0 int=H` (the expander answers; `--` would mean it does not) at 30, 60, 90, 120, 150 and 180 s, and a person's presses on the real keys were logged (`[btn] P3 DOWN down`, `P0 UP`, `P4 SELECT`) and launched an app. It stayed healthy through three flashes and every reset of the Batch 2 device tests (run twice), and after a Lua app called `badge.se050.test()`, `random()` and `random_available()` (all refused by H21 without touching the bus). This shows the fork does not re-create the hold; it does not show which SE050 operation caused it (open item U13), because the fault was not provoked again |
| F11 | The serial console and BLE share one line protocol | `push_protocol.h` | Our serial commands are intercepted in `pumpSerialConsole()` (H6), so they exist on USB only |
