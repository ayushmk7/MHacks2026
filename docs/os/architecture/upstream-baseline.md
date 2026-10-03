# Upstream baseline: what Solana OS provides

A digest of the firmware we build on, with the source location of every fact, so that nobody has to rediscover it by reading the whole upstream tree.

- Audience: firmware engineers new to the badge.
- Status: design, not yet built on hardware.
- Source read: Solana OS, directory `firmware/solana-os/` at commit `812b8c7aca5c366d18c0b040fafd2999f7204d84` (2026-08-07) of the [upstream repository](https://github.com/spacemandev-git/solana-defcon-badge-26/tree/812b8c7aca5c366d18c0b040fafd2999f7204d84/firmware/solana-os). Paths below are relative to that directory unless they start with `README.md (root)`, `pcb/`, `app/` or `broker/`, which are relative to the repository root.

Everything in this document is **[UPSTREAM]**: it was read in the upstream source at the path given. Two qualifications apply throughout:

- "(documented)" after a source means the fact is stated by upstream's README or comments and was not checked independently in code.
- **[UNVERIFIED]** marks things that only a badge can answer. No badge was flashed and `arduino-cli` was not installed when this was written, so every statement about on-device behaviour comes from reading source, not from running it.

What we change is in [Runtime and boot](runtime-and-boot.md). Terms are in the [glossary](../reference/glossary.md).

## Hardware

The v1 board.

| Item | Fact | Source |
|---|---|---|
| Module | ESP32-S3-WROOM-1-**N16R8**: 16 MB flash, 8 MB octal PSRAM | `README.md (root)` "Key parts: U4"; `partitions.csv:1`; `solana-os.ino:11-12` |
| CPU | Dual-core Xtensa LX7, 240 MHz (the boot banner prints `ESP.getCpuFreqMHz()`) | `solana-os.ino:61-64` (documented) |
| Display | ILI9341 over SPI, a 240×320 panel used in landscape as **320×240**, RGB565, 40 MHz write clock, PWM backlight on GPIO7 | `src/hal/display.cpp:13-66`; `src/config.h:25-31, 51-55` |
| Framebuffer | One 320×240×16 bpp sprite in PSRAM (150 KB). Everything draws into it; `display::flush()` blits when dirty. | `src/hal/display.h:1-36`; `src/hal/display.cpp:92-125` |
| Buttons | Six, behind a TCA9534 I²C expander at `0x20`, active low, interrupt on GPIO4. Silkscreen: UP, DOWN, LEFT, RIGHT, SELECT, CANCEL. | `src/config.h:67, 89-115`; `src/hal/buttons.cpp:225-247` |
| Button map | P0 UP, P1 LEFT, P2 RIGHT, P3 DOWN, P4 SELECT (`BTN_A`, Lua `"a"`), P5 CANCEL (`BTN_B`, Lua `"b"`). Debounce 25 ms, poll 15 ms, repeat after 420 ms then every 110 ms. | `src/config.h:91-115`; `README.md:139-157` |
| Force quit | Holding CANCEL for 1500 ms (`APP_ESCAPE_HOLD_MS`) stops the running app. Handled in the main loop. | `src/config.h:119`; `solana-os.ino:71-78` |
| LEDs | 2× WS2812B on GPIO2, driven by RMT, default brightness 72/255 | `src/config.h:41, 124-125`; `src/hal/leds.h:1-12` |
| Secure element | NXP **SE050C2** (`SE050C2HQ1_Z01SDZ`), I²C address `0x48`, shared bus SDA GPIO10 / SCL GPIO9 at 100 kHz, enable on GPIO8 (active high) | `pcb/v1/production/bom.csv:38`; `src/config.h:33-34, 43, 65-73` |
| Microphones | 2× SPH0641LM4H PDM, CLK GPIO47, DATA GPIO48, 16 kHz | `src/config.h:45-46, 130` |
| Battery | Li-Po, MCP73831 charger, 2k2:2k2 divider on GPIO1. There is no charge-status line; "charging" is inferred from voltage. | `README.md (root)` "Key parts"; `src/hal/power.h:1-6` |
| USB | USB-C. A CH340C USB-serial bridge (UART0, GPIO43/44) plus native USB behind mode switch `S1`. Logs go to both. | `README.md (root)` test-kit section; `src/badge_log.h:4-9` |
| Touch | GT911 footprint on the same I²C bus; unused, but must be released from reset at boot | `src/config.h:35-38` |
| Radios | 2.4 GHz Wi-Fi (station, SoftAP, WPA2-Personal and WPA2-Enterprise), BLE (NimBLE, Nordic UART Service), ESP-NOW. One radio, one channel. | `README.md:1248-1268`; `src/net/*` |
| I²C health | Bus recovery and scan helpers; a healthy board answers at `0x20` and `0x48` | `src/hal/badge_i2c.h`; `README.md (root)` "Working with a fresh board" |

[UNVERIFIED] Whether the SE050**C2** variant on these boards has Ed25519 enabled and accepts commands without an SCP03 secure channel. Upstream names both as the most likely failures (`src/hal/se050_apdu.cpp:21-33`). Consequences and fallback: [Keys and the SE050](../wallet-core/keys-and-se050.md).

## Firmware facts

| Item | Fact | Source |
|---|---|---|
| Framework | Arduino-ESP32 core `esp32:esp32` **3.x**, plus one library, `LovyanGFX`. It is an Arduino sketch, not an ESP-IDF project: no `CMakeLists.txt`, no PlatformIO, no IDF components. | `README.md:50-96`; `solana-os.ino` |
| Language | C++ for the firmware (namespaces, `String`, `std::function`); C for the vendored Lua and TweetNaCl | every `src/**/*.cpp` |
| Compiler flags | [UNVERIFIED] The exact `-std=`, exception and RTTI flags of the installed core. Our C++ SDK avoids depending on them. | — |
| Build | `arduino-cli compile --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom,CDCOnBoot=cdc" .` from `firmware/solana-os/` | `README.md:88-93` (documented) |
| Upload | `arduino-cli upload --fqbn <same> -p /dev/cu.usbserial-XXXX .` | `README.md:50-54` (documented) |
| Image size | About 1.75 MB flash and 72 KB static RAM per the README; the tracked build is 1,849,888 bytes | `README.md:95`; `app/src/lib/data/firmware.json` |
| Partition table | `nvs` 0x9000 +0x5000 (20 KB); `otadata` 0xe000 +0x2000; `app0` 0x10000 +0x330000 (3.19 MB); `app1` 0x340000 +0x330000; `spiffs` (used as LittleFS) 0x670000 +0x980000 (9.5 MB); `coredump` 0xFF0000 +0x10000 | `partitions.csv:11-17` |
| Flash parts | bootloader at `0x0`, partition table `0x8000`, `boot_app0.bin` `0xe000`, application `0x10000`, LittleFS image `0x670000` | `app/scripts/build-firmware.ts:226-246` |
| LittleFS image | `mklittlefs -c <dir> -b 4096 -p 256 -s <partition size>` built from `firmware/solana-os/apps/` | `app/scripts/build-firmware.ts:150-153` |
| OTA | Two OTA slots exist in the table; there is **no OTA update code** (no `Update.h`, no route) | `partitions.csv`; no match for `Update.` or `esp_ota` under `src/` |
| Logging | `badge_log`: UART0 + USB CDC + a 64-line × 120-character ring. The ring is shown on Settings → Console and at `GET /api/logs`. Lines are tagged `[tag] message`. | `src/badge_log.h` |
| Serial | 115200 baud. The serial console speaks the push line protocol, not free text. | `README.md:980-981, 1309-1310`; `solana-os.ino:87-102` |
| Tasks | The firmware creates no FreeRTOS task. Everything runs on the Arduino `loopTask`; the Wi-Fi and BLE stacks have their own tasks, whose callbacks only enqueue. | no match for `xTaskCreate` under `src/`; `src/net/espnow_mgr.cpp:40-53`; `README.md:1196-1199` |
| Loop task stack | The Arduino default, 8192 bytes; upstream does not override it | Arduino-ESP32 `cores/esp32/main.cpp`; no match for `LOOP_TASK_STACK` |
| Loop watchdog | Off by default in the Arduino core; upstream does not enable it | Arduino-ESP32 `cores/esp32/main.cpp`; no match for `esp_task_wdt` |

The upstream boot sequence and main-loop order are listed, with our changes marked, in [Runtime and boot](runtime-and-boot.md#boot-sequence).

## Source tree

```
firmware/solana-os/
  solana-os.ino            setup(), loop(), button routing, serial console
  partitions.csv
  src/config.h             pins, limits, tunables
  src/badge_log.{h,cpp}
  src/settings.{h,cpp}     NVS namespaces "sysconf" and "luakv"
  src/hal/                 badge_i2c, display, leds, buttons, power, mic, se050, se050_t1, se050_apdu
  src/identity/            identity, ed25519 (TweetNaCl wrapper), encoding (base58/base64 encode), tweetnacl.{c,h}
  src/net/                 wifi_mgr, cert_store, espnow_mgr, ble_mgr, ble_bridge, net_route, push_protocol, push_server, broker_client, broker_ca.h
  src/apps/app_store       LittleFS catalogue, app.ini
  src/lua_sdk/             lua_runtime, lua_bindings, lib_{gfx,input,led,system,storage,sensors,net,espnow,ble}.cpp
  src/ui/                  theme, boot, shell (launcher + settings + error screen)
  src/lua/                 vendored Lua 5.4.8
  apps/                    hello, dice, gallery, radar, whosnear, vumeter
  tools/badge-push.py      push client (HTTP or BLE)
  tools/fetch-broker-ca.ts
```

Source: `README.md:1132-1187`, confirmed against the repository's file list.

## Lua runtime and sandbox

| Item | Fact | Source |
|---|---|---|
| VM | Lua **5.4.8**, vendored, built with `LUA_32BITS 1` (integers are `int32_t`, numbers are `float`), `LUAI_MAXSTACK` 25000 | `src/lua/lua.h:19-21`; `src/lua/luaconf.h:125, 758`; `README.md:98-114` |
| Libraries | `_G`, `package`, `coroutine`, `table`, `string`, `math`, `utf8`. No `io`, `os` or `debug` (`liolib.c` and `loslib.c` are deleted). A curated global `os` offers `time`, `clock`, `date`, `difftime`. | `src/lua/linit.c`; `src/lua_sdk/lib_system.cpp:205-325` |
| Bytecode | Refused: `load`/`loadfile`/`dofile` accept text only, `string.dump` is disabled, the entry file is loaded with mode `"t"` | `README.md:109-110, 1234-1238`; `src/lua_sdk/lua_runtime.cpp:243-249` |
| One app at a time | One `lua_State` per app, created at launch and closed at stop | `src/lua_sdk/lua_runtime.h:1-15`; `src/lua_sdk/lua_runtime.cpp:208-317` |
| Memory | The allocator draws from PSRAM with a hard cap `LUA_HEAP_LIMIT_BYTES` = 1 MiB and falls back to internal RAM if PSRAM allocation fails | `src/config.h:192`; `src/lua_sdk/lua_runtime.cpp:66-88` |
| Time budget | A count hook every 20000 VM instructions checks a wall-clock deadline: **250 ms** per callback, **5000 ms** for the script top level and `on_start`. Error text: `app exceeded its time budget (stuck in a loop?)`. The deadline latches until the callback returns. | `src/config.h:195-196, 213`; `src/lua_sdk/lua_runtime.cpp:91-101, 125-154, 251-265` |
| Extending the budget | `runtime::extendDeadline(ms)`, capped at 12000 ms in total per callback (`LUA_CALLBACK_EXTENSION_CAP_MS`). Used by `system.sleep` (≤ 2000 + 50), `http.*` (timeout + 500 on Wi-Fi, + 2500 on the bridge), `se050.random` (120 + 60). | `src/config.h:210`; `src/lua_sdk/lua_runtime.cpp:198-206`; `src/lua_sdk/lib_system.cpp:42-50`; `src/net/net_route.cpp:133-139`; `src/lua_sdk/lib_sensors.cpp:145` |
| Lifecycle | Globals `on_start()`, `on_update(dt)`, `on_draw()`, `on_button(key, pressed)`, `on_espnow(mac, data, rssi)`, `on_ble(line)`, `on_stop()`; all optional. Frame order: radio events → `on_button` per edge → `on_update(dt)` → `on_draw()` → flush. | `src/lua_sdk/lua_runtime.h:17-28`; `README.md:349-374` |
| Errors | Any callback error stops the app. The message and traceback go to `runtime::lastError()`, the log, and the shell's error screen (SELECT retries). | `src/lua_sdk/lua_runtime.cpp:103-161`; `src/ui/shell.cpp:1250-1300, 1390-1398` |
| Launch and stop | `requestLaunch`/`requestStop` are deferred to `processRequests()` between frames. Requests made inside `on_stop` are dropped. | `src/lua_sdk/lua_runtime.cpp:319-350` |
| Stop cleanup | `on_stop`, clear the ESP-NOW receive handler, clear the BLE line handler, `ble_bridge::reset()`, `mic::disable()`, LEDs off, `lua_close` | `src/lua_sdk/lua_runtime.cpp:273-317` |
| File sandbox | `runtime::resolveAppPath()`: relative path, ≤ 96 characters, no leading `/`, no `..`; the result is under `/apps/<id>/` | `src/lua_sdk/lua_runtime.cpp:404-410` |
| `require` | `package.path` = `/littlefs/apps/<id>/?.lua;/littlefs/apps/<id>/?/init.lua;/littlefs/lib/?.lua`; `cpath` is empty | `src/lua_sdk/lua_runtime.cpp:163-177` |

Upstream states the limit of this sandbox itself: "These limits stop accidents … not a defence against a deliberately hostile app: it can still spin the CPU inside a single C binding, flood a radio, or fill the filesystem." (`README.md:1240-1244`)

### App packaging and delivery

- An app is a directory `/apps/<id>/` on LittleFS with `main.lua` and an optional `app.ini`. `id` matches `[a-z0-9._-]`, 1–32 characters. (`src/apps/app_store.h:11-13`; `README.md:333-334`)
- `app.ini` is `key=value` with `#` or `;` comments. Recognised keys: `name`, `version`, `author`, `description`, `entry`. Unknown keys are ignored. (`src/apps/app_store.cpp:48-52`)
- The launcher lists installed apps, then "Settings". SELECT runs, RIGHT deletes (with a confirm), CANCEL opens Settings. (`src/ui/shell.cpp:237-314`)
- Delivery: the HTTP API on port 80 (`POST /api/app?id=&path=&enc=&append=`), the BLE/serial line protocol (`BEGIN`/`DATA`/`END`), the web UI at `/`, or the app-store broker. Authentication is the 6-digit pairing code in `X-Badge-Token`; five failures lock out with back-off. One file is at most 96 KB. (`README.md:745-872`; `src/net/push_server.cpp:914-939`; `src/config.h:151`)
- `tools/badge-push.py` subcommands: `push <path> [--run]`, `list`, `stop`, `logs`, `status`, `run <id>`, `rm <id>`, `cert <pem> [--name <name>]` (without `--name` the certificate is stored under the lower-cased file name, `tools/badge-push.py:389`), `certs`, `rmcert <name>`, `join <ssid> [--password …]`, `defcon`. (`tools/badge-push.py:478-535`)
- Bundled examples: `apps/hello`, `apps/dice`, `apps/gallery`, `apps/radar`, `apps/whosnear`, `apps/vumeter`. `apps/whosnear/main.lua` is the best template for ESP-NOW messaging.

### Binding style

- One file per module with a `const luaL_Reg FUNCTIONS[]` table, an optional `const Field CONSTANTS[]`, and an opener `void openFoo(lua_State *L) { setTable(L, "foo", FUNCTIONS, CONSTANTS); }` declared in `lua_bindings.h` and called from `openBadge()`. (`src/lua_sdk/lua_bindings.cpp:42-86`; `README.md:1314-1348`)
- Conventions in every binding: `luaL_check*`/`luaL_opt*` for arguments; failure returns `nil, "reason"` (or `false, "reason"`); programmer errors raise with `luaL_error`. No heap-owning C++ object may be live across `luaL_error`, because it `longjmp`s. (`src/lua_sdk/lib_net.cpp:66-70`; `src/lua_sdk/lib_espnow.cpp:67-72`)
- Anything that draws calls `display::touch()`; anything that blocks calls `runtime::extendDeadline()` first; nothing touches the Lua state from another task. (`README.md:1339-1348`)

### Modules exposed to Lua

`badge` exposes `gfx`, `input`, `led`, `system`, `storage` (with `storage.kv`), `battery`, `mic`, `se050`, `wifi`, `http`, `espnow`, `ble`, plus `badge.version` (`"0.1.0"`), `badge.api_version` (1), `badge.device_name`, `badge.log(...)`, `badge.millis()`, `badge.sleep(ms)` and a global `print()` that goes to the log. (`src/lua_sdk/lua_bindings.cpp:65-73`) The function-by-function list is in the [API reference](../app-platform/api-reference.md).

Facts about these modules that shape our design:

- `badge.gfx.brightness([0..255])` **sets the backlight**, and `badge.gfx.flush()` **pushes the framebuffer immediately**. A Lua app can therefore draw anywhere and turn the backlight to 0. (`src/lua_sdk/lib_gfx.cpp:399-445`)
- `badge.http.get/post` are blocking, with a default timeout of 5000 ms clamped to 100..10000, and take no custom request headers. (`src/lua_sdk/lib_net.cpp:217-271`)
- `badge.espnow.peers()` returns peers **sorted strongest RSSI first**; `MAX_PAYLOAD` = 240, `MAX_PEERS` = 20. (`src/lua_sdk/lib_espnow.cpp:161-172`)
- `badge.wifi.connected()` is true for **any route**, including SoftAP mode and a phone bridge. (`src/lua_sdk/lib_net.cpp:202-211`)
- There is **no** JSON, base58, base64, SHA-256, big-integer or hex helper available to Lua.

## Identity

| Item | Fact | Source |
|---|---|---|
| Key type | One Ed25519 keypair per badge, created on first boot. The public key is the Solana address. Badge ID = first 8 base58 characters. | `src/identity/identity.h:1-31`; `README.md:1015-1024` |
| Sources | `identity::Source { None, SecureElement, Software }`; `sourceName()` returns `"secure element"`, `"software"` or `"none"` | `src/identity/identity.h:38-51`; `src/identity/identity.cpp:273-279` |
| Storage | NVS namespace **`badgeid`**: `src` (u8 source), `pub` (32 bytes), `seed` (32 bytes, **plaintext**, software source only) | `src/config.h:161`; `src/identity/identity.cpp:21-23, 82-98` |
| SE050 object | An Ed25519 key pair at object id `0xF0000001`, generated inside the part. The public key is read back and byte-reversed; a probe signature is verified in software before the key is trusted. | `src/config.h:158`; `src/identity/identity.cpp:103-144`; `src/hal/se050_apdu.cpp:283-349` |
| Fallback | If the SE050 path fails for any reason, a software key is created (with SE050 random bytes mixed into the seed if the part answers). Degenerate seeds are rejected. | `src/identity/identity.cpp:146-184, 230-236` |
| Reload | A stored SecureElement identity is kept even if the part does not answer this boot (signing then fails). If the part's live public key differs from the stored one, signing is refused. | `src/identity/identity.cpp:186-228, 287-308` |
| C++ API | `begin()`, `ready()`, `source()`, `sourceName()`, `publicKey()` (32 bytes or null), `publicKeyBase58()`, `badgeId()`, `sign(msg, len, out[64])`, `signBase64(...)`, `regenerate()`, `status()`, `base58Encode()`, `base64Encode()` | `src/identity/identity.h:44-84` |
| Sign limits | Software: message ≤ 4096 bytes, needs `malloc(len + 64)`. SE050: message ≤ **180** bytes (`MAX_SIGN_MESSAGE_BYTES`), budget 3000 ms. | `src/identity/ed25519.cpp:45, 69-98`; `src/hal/se050_apdu.h:69`; `src/hal/se050_t1.h` (`SLOW_BUDGET_MS`) |
| Verify | `ed25519::verify(msg, len, sig, pub)` exists (TweetNaCl `crypto_sign_open`) and is used only by the self-test | `src/identity/ed25519.h:53-58`; `src/identity/ed25519.cpp:100-127` |
| RNG | `esp_fill_random` / `esp_random`; true hardware entropy only while RF is on or `bootloader_random_enable()` is active. `se050::randomBytes()` is a separate source with no fallback. | `src/identity/ed25519.cpp:19-35`; `solana-os.ino:154-160, 213-217`; `src/hal/se050.h:30-50` |
| Regenerate | Settings → Identity → New identity calls `identity::regenerate()` after a confirm screen, then `broker::forget()` | `src/ui/shell.cpp:748-804` |
| Settings screen | Shows the badge ID (size 3), `key lives in` + `sourceName()` (green for the secure element, amber for software), `status()`, and the full base58 key wrapped at 32 characters | `src/ui/shell.cpp:699-746` |
| HTTP export | `GET /api/identity` (and `/api/v1/identity`), authenticated: `{"ready":bool,"badge_id":"…","pubkey":"…","source":"secure element"\|"software"\|"none","status":"…"}` | `src/net/push_server.cpp:772-782, 931-932` |
| Encoders | base58 encode and base64 encode only; no decoders in `identity/`. A base64 decoder exists privately in `push_protocol.cpp`. | `src/identity/encoding.cpp`; `src/net/push_protocol.cpp:327-360` |
| SHA-256 | `mbedtls/sha256.h` is available and used | `src/net/broker_client.cpp:7, 655-668` |
| JSON | No library. A hand-rolled span scanner in `broker_client.cpp` and ad-hoc parsing in `push_server.cpp`. | `src/net/broker_client.cpp:114-300` |

The SE050 transport and command set are described with the bring-up procedure in [Keys and the SE050](../wallet-core/keys-and-se050.md#se050-path-facts).

## Networking

| Item | Fact | Source |
|---|---|---|
| Wi-Fi provisioning | No on-device keyboard. Open networks join from Settings → Wi-Fi. Secured networks: start the badge hotspot (SSID = device name, password `solanabadge`, address 192.168.4.1), then use the web UI or `badge-push.py --host 192.168.4.1 --token <code> join "<ssid>" --password "<psk>"`. One saved network in NVS (`sysconf`: `ssid`, `pass`), auto-connect at boot. | `README.md:215-218, 50-61`; `src/settings.cpp:63-78`; `src/config.h:141`; `tools/badge-push.py:513-529` |
| Credentials at rest | NVS, unencrypted | `README.md:316-318` |
| `wifi_mgr::connected()` | True in station mode when associated, **and always true in access-point mode** | `src/net/wifi_mgr.cpp:324-327` |
| HTTP client | `net_route::request(method, url, body, contentType, timeoutMs, headers, n)` → `{ok, status, body, err, truncated}`. Wi-Fi first, then the phone bridge. GET and POST only. | `src/net/net_route.h:38-77`; `src/net/net_route.cpp:116-144` |
| TLS on Wi-Fi | `HTTPClient::begin(url)` with no CA, which makes Arduino's `HTTPClient` call `setInsecure()`: **certificates are not validated**. Pinned TLS exists only in `broker_client` (`WiFiClientSecure::setCACert` with a CA from `cert_store` or `broker_ca.h`). | `src/net/net_route.cpp:34-40`; `src/net/broker_client.cpp:332-357`; Arduino-ESP32 [`HTTPClient.cpp`](https://github.com/espressif/arduino-esp32/blob/master/libraries/HTTPClient/src/HTTPClient.cpp) |
| `cert_store` | PEM CA certificates in `/certs` on LittleFS, uploaded by name (`POST /api/certs?name=`, `CERTBEGIN`, `badge-push.py cert`) | `src/net/cert_store.h`; `README.md:786-788, 847-849` |
| Phone bridge | HTTP tunnelled over BLE NUS after `AUTH` + `BRIDGE ON`. The **phone terminates TLS**. Response cap 32 KB, one request in flight, dropped on disconnect or app exit. | `README.md:876-1011`; `BRIDGE_PROTOCOL.md` |
| Solana RPC helpers | None: no JSON-RPC, no transaction code, no base58 decode | no match for `rpc`, `blockhash` or `lamport` under `src/` |
| mDNS | `solana-badge.local` (`DEFAULT_HOSTNAME`) | `src/config.h:139`; `README.md:1290-1291` |
| Push server | Port 80, started only while `wifi_mgr::connected()`. State-changing routes need `X-Badge-Token` and pass a Host/Origin check. | `solana-os.ino:258-265`; `README.md:773-817` |

### ESP-NOW

Source: `src/net/espnow_mgr.{h,cpp}`.

- A frame is the magic `S B D G` (4 bytes) + a type byte + payload. Type `0x01` is a presence beacon (payload = device name); `0x02` is an application frame. The payload cap `ESPNOW_MAX_PAYLOAD` is **240** bytes (frame 245 ≤ 250).
- Frames are unencrypted and unauthenticated. Unicast peers are auto-registered (`ensureRegistered`) with `channel = 0` (the current channel) on the station interface.
- A beacon carrying `settings::deviceName()` (≤ 23 characters) is sent every 1000 ms. The peer table has 20 entries `{mac, name[24], rssi, lastSeenMs, packets}`; entries expire after 12 s and the table is cleared on a channel change.
- RSSI comes from `info->rx_ctrl->rssi` of the last frame heard from that MAC.
- Receive path: Wi-Fi-task callback → 8-entry ring (drops when full) → drained by `espnow_mgr::update()` on the main loop → `ReceiveHandler(mac, data, len, rssi)`. **No receive timestamp is recorded.**
- `send()` and `broadcast()` return whether `esp_now_send` accepted the frame. There is no send-status callback, so delivery is not confirmed.
- ESP-NOW follows the Wi-Fi channel. Joining an access point moves it; badges on different access points or channels cannot hear each other; the badge hotspot is pinned to the configured ESP-NOW channel. (`README.md:1248-1262`; `src/net/wifi_mgr.cpp:41, 270`)
- External fact: the ESP-NOW v1 payload limit is 250 bytes; v2 allows 1470 and v1 receivers truncate or drop longer frames ([Espressif ESP-NOW reference](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/network/esp_now.html)).

### Broker and BLE

- The app-store broker protocol (`broker/PROTOCOL.md`) carries badge registration (challenge → signed `solana-badge-register:<pubkey>:<nonce>` → bearer token), heartbeat and offer polling every 6 s, script download with a SHA-256 check, install results, a WebSocket ESP-NOW relay for the browser emulator and an HTTP proxy for it. It does **not** carry transactions or anything wallet-related. Default broker `https://broker.solanadefcon.com` (`src/config.h:171`).
- BLE is NimBLE with the Nordic UART Service `6e400001-b5a3-f393-e0a9-e50e24dcca9e` (RX `…0002` write, TX `…0003` notify). Its line protocol is shared with the serial console (`README.md:826-872`). BLE is off at boot by default (`src/settings.cpp:123`).

## UI

- All drawing targets `display::canvas()` (an `LGFX_Sprite`). Helpers: `display::text`, `textCentered`, `textRight`, `card`, `statusBar`. `STATUS_BAR_HEIGHT` is 22 px plus a 2 px gradient rule; the status bar shows the title on the left and `WiFi/wifi NOW <battery>%` on the right. (`src/hal/display.h:48-60`; `src/hal/display.cpp:160-222`)
- The font is LovyanGFX's default 6×8 bitmap font scaled by an integer. It is ASCII only. (`README.md:465-466`; `src/hal/display.cpp:160-185`)
- Palette (`src/ui/theme.h`): `BG 0x0B0B12`, `HEADER`, `PANEL`, `PANEL_2`, `BORDER`, `TEXT`, `MUTED`, `WHITE`, `PURPLE 0x9945FF`, `GREEN 0x14F195`, `OK = GREEN`, `WARN 0xFFB020`, `ERR 0xFF4545`, `ACCENT = PURPLE`.
- The shell (`src/ui/shell.cpp`) is a `Screen` enum with per-screen `drawX()`/`updateX()` functions, a shared `row()`, a `footer(hint)` bar of 18 px, and `ROW_HEIGHT` 22. The shell runs **only when no app is running** (`solana-os.ino:279-283`).
- Input model: `buttons::update()` computes per-tick edge masks; `pressed`, `released`, `repeated`, `down` and `heldMs` read them. Whoever is in the foreground reads them. There is no event queue. (`src/hal/buttons.h:1-9`)
- Precedent for a modal prompt: the app-store Offer screen. It ignores the button edges of the tick it was raised on so that an in-flight SELECT is not taken as consent (`src/ui/shell.cpp:69-73, 991-994, 1408-1415`). It is a shell screen, so it can never interrupt a running app.
- Precedent for blocking inside the main loop: `identity::regenerate()` called from the confirm screen (`src/ui/shell.cpp:785-790`), and the bridge wait loop inside a Lua binding, which pumps only what it needs and calls `delay(1)` (`src/net/net_route.cpp:71-86`).
- **No mechanism exists** for firmware to take the display and buttons away from a running Lua app. Our approval modal adds one; see [Approval modal](../wallet-core/signing-gate.md#approval-modal).

## Storage

| Store | Where | Used for | Source |
|---|---|---|---|
| NVS `sysconf` | 20 KB `nvs` partition | brightness, LED brightness, device name, Wi-Fi and EAP credentials, radio-at-boot flags, ESP-NOW channel, pairing code (`pair`), broker settings, `autostart` | `src/settings.cpp` |
| NVS `luakv` | same | per-app key/value (`badge.storage.kv`); the stored key is `<6 hex of FNV-1a(app id)>:<key>`, so app keys are ≤ 8 characters | `src/settings.cpp:19, 166-190`; `src/lua_sdk/lib_storage.cpp:186-225` |
| NVS `badgeid` | same | identity (`src`, `pub`, `seed`) | `src/identity/identity.cpp` |
| LittleFS `/apps/<id>/` | 9.5 MB `spiffs` partition | app files and app data | `src/apps/app_store.h` |
| LittleFS `/lib/` | same | shared Lua modules | `src/config.h:218` |
| LittleFS `/certs/` | same | CA certificates | `src/config.h:219` |

NVS key names are limited to 15 characters. The NVS partition is small (20 KB): large or frequently rewritten data belongs on LittleFS. Our additions (NVS namespace `wallet`, LittleFS directory `/wallet/`) are in [Config, limits and audit](../wallet-core/config-limits-audit.md).

## Findings that differ from the PRD

These are the places where the upstream code is not what the product requirements assumed. Each one changed the design.

| # | Finding | Source | Consequence for us |
|---|---|---|---|
| 1 | Upstream is an **Arduino sketch built with `arduino-cli`** on the Arduino-ESP32 3.x core, not an ESP-IDF project. | `solana-os.ino`; `README.md:50-96`; `app/scripts/build-firmware.ts:32` | On-device code is added as folders under `src/`. CMake applies only to host unit tests. |
| 2 | Upstream is **C++**, not C. Only the vendored Lua and TweetNaCl are C. | every `src/**/*.cpp` | "C wallet core" in the PRD means "native firmware below Lua". The pure-C modules (`sol_*.c`, `pay_proto.c`) are still C99 so they can be tested on a host. |
| 3 | **No Lua binding for identity exists.** Nothing exposes the public key or signing to Lua. | `src/lua_sdk/lua_bindings.cpp:65-73`; no match for `identity::` under `src/lua_sdk` | `identity.pubkey()` and `identity.sign()` (F1) are entirely ours. There is no existing Lua signing path to close. |
| 4 | The only caller of `identity::sign*` is the app-store broker client, which signs `"solana-badge-register:" + pubkey + ":" + nonce` **without any button press**. | `src/net/broker_client.cpp:508-513`; nonce check `isSafeNonce` at `:495-501` | A signing path outside the approval screen. It cannot produce a transaction signature (ASCII prefix), but it breaks "every signature requires a button press". We compile the broker out by default and route it through the gate when enabled. |
| 5 | Software Ed25519 is **TweetNaCl**, which upstream itself describes as "the slowest Ed25519 in existence … a signature costing a second of CPU"; the first-boot self-test "costs a second or two". | `src/identity/TWEETNACL-README:15-22`; `src/identity/identity.cpp:25-31` | A 250 ms proof deadline and a 2 s request-to-screen budget cannot be met with TweetNaCl sign + verify if those figures hold on the S3. We vendor Monocypher. Timing on hardware is [UNVERIFIED]. |
| 6 | The SE050 sign command is capped at **180 message bytes**. A legacy `transferChecked` message is **214 bytes** (v0: 216). | `src/hal/se050_apdu.h:69` | F17 cannot sign a payment without raising `MAX_SIGN_MESSAGE_BYTES`. The single-byte-Lc APDU form allows up to 242 message bytes (`src/hal/se050_apdu.cpp:360-367`). |
| 7 | The SE050 code "has never run against a real SE050". | `src/hal/se050_apdu.cpp:1-4`; `README.md:1054-1072` | Confirms the PRD risk. A fail-closed fallback to a software key exists (`src/identity/identity.cpp:230-236`). |
| 8 | Lua is built with `LUA_32BITS 1`: integers are `int32_t`, numbers are `float`. | `src/lua/luaconf.h:125`; `README.md:105` | A `u64` token amount or lamport balance cannot cross into Lua as a number; floats lose integer precision above 2^24. Amounts are strings. |
| 9 | **Upstream bug:** the ESP-NOW application-receive handler is installed once at boot and cleared when any app stops; nothing re-installs it. | install `solana-os.ino:134-137`; clear `src/lua_sdk/lua_runtime.cpp:290`; no other caller of `onReceive` | `on_espnow` works only for the first app launched after boot. Any multi-app flow (Home → Pay) silently loses ESP-NOW. Patched; see [Runtime and boot](runtime-and-boot.md#upstream-patches). |
| 10 | `badge.http` over Wi-Fi does **not validate TLS certificates**. | `src/net/net_route.cpp:34-40` | RPC answers fetched through `badge.http` are not authenticated against an on-path attacker on the hotspot. Our RPC client pins a CA when one is installed, following the broker client's pattern. |
| 11 | The badge has **no RTC**; `time()` is seconds since boot unless something sets the clock. On the WPA2-Enterprise path upstream seeds the clock from the firmware build date. | `README.md:293-304, 552-554`; `src/lua_sdk/lib_system.cpp:207-212`; `src/net/wifi_mgr.cpp:45-73` | Absolute expiry timestamps cannot be compared between badges, and an attestation's `expiry` cannot be checked without SNTP. A plausible `time()` does not prove a sync, so the wallet's `clock_synced` flag is set by the SNTP sync callback and not inferred from the time alone ([Attestation, Clock](../identity/attestation.md#clock)). |
| 12 | The built-in font is a 6×8 ASCII bitmap font scaled by an integer. | `README.md:465-466`; `src/hal/display.cpp:160-185` | The "✓" and "…" of the PRD mockup cannot be drawn as text. The tick is drawn with lines and elision is `..`. |
| 13 | The repository has **no licence file** and no licence statement in either README. | file list; no match for "license" in the READMEs | The terms are unstated. Vendored parts carry their own (Lua: MIT, `src/lua/LUA-README`; TweetNaCl: public domain, `src/identity/TWEETNACL-README`). |
| 14 | The push server already exposes the public key and key location: `GET /api/identity`. | `src/net/push_server.cpp:772-782, 931-932` | The dashboard can read each badge's key and key location with no new firmware. |

## Upstream issues

Known problems and TODOs in upstream that affect us.

- The ESP-NOW handler is lost after the first app exits (finding 9).
- The SE050 path has never run on silicon; SCP03 is not implemented; the variant may lack Ed25519. (`src/hal/se050_apdu.cpp:1-56`)
- The SE050 open sequence deliberately deviates from NXP's (it uses soft reset `0xCF`); swap to `0xC0` + `0xC7` if the open misbehaves. (`src/hal/se050_t1.h`, "ONE KNOWN DEVIATION")
- Broker offers have no publisher signature (`src/net/broker_client.cpp:38, 829`); anyone who knows a badge ID can raise an install prompt on it while the shell is in front (`README.md:1121-1126`).
- While the badge runs its own hotspot, `net_route` believes it has Wi-Fi and never uses the phone bridge. (`README.md:964-967`)
- The software seed is plaintext in NVS; flash encryption and secure boot are not enabled. (`README.md:1041-1048`)
- Pushing an app is "running code you trust". (`README.md:1240-1244`)
- The button mapping is per board revision; re-measure with the `[btn] P4 SELECT down` log line. (`README.md:159-164`)
- There is no UI or API that sets the autostart app: `settings::setAutostartApp` has no caller. (`solana-os.ino:226-232`)

## Requirements covered

None directly. This document is the factual base for F1–F3 (no identity binding, no modal mechanism upstream), F9 (no receive timestamp, TweetNaCl speed), F17 (SE050 state) and the non-functional requirements on sandbox fit and honesty.

## Open items

All [UNVERIFIED]; each is resolved by the first hardware session.

- Nothing has been compiled or flashed. Fallback: none; this is the first work package.
- Ed25519 time on the ESP32-S3 for TweetNaCl (about a second per upstream's own description). Fallback: Monocypher backend, longer `deadline_ms`.
- Whether the SE050C2 answers without SCP03, supports Ed25519, and returns keys and signatures in the byte order the code assumes. Fallback: software key, reported as such.
- Exact compiler flags of the installed Arduino core. Fallback: our code avoids exceptions, RTTI and anything above C++17/C99.
- ESP-NOW round-trip time and usable RSSI thresholds at table distance. Fallback: tune `deadline_ms` and `rssi_min` on the day.
- The upstream repository's licence terms. Fallback: credit upstream; ask the author before publishing a fork.
