# Upstream hooks

A copy of the table in `docs/os/architecture/upstream-hooks.md`, which is the source: change that
document first, then this copy. `scripts/preflash-check.sh` (check 1) compares the hook ids tagged
`// VK: H<n>` in upstream files with this table and fails on any difference. A row whose purpose
starts with `optional:` may be absent from the source.

Upstream is commit `812b8c7`; its `solana-os.ino` is `os.ino` here.

| Id | File | Purpose |
|---|---|---|
| H1 | `os.ino` | include `src/vk/vk.h` |
| H2 | `os.ino` `setup()` | start Badge OS after the Lua runtime |
| H3 | `os.ino` `startRadios()` | ESP-NOW frames go to the router |
| H4 | `os.ino` `loop()` | the approval pauses apps and the shell |
| H5 | `os.ino` `loop()` | run services every pass |
| H6 | `os.ino` `pumpSerialConsole()` | our serial commands, USB only |
| H7 | `src/lua_sdk/lua_bindings.cpp` `openBadge()` | add our Lua functions; drop modules the app was not granted |
| H8 | `src/lua_sdk/lua_runtime.cpp` | pre-launch check and native apps (six sites, H8a–H8f) |
| H9 | `src/lua_sdk/lua_runtime.cpp` `stop()` | do not clear the ESP-NOW handler |
| H10 | `src/net/broker_client.cpp` `submitRegistration()` | store registration signs through the wallet core |
| H11 | `src/apps/app_store.cpp` | native apps appear in the app list (four sites) |
| H12 | `src/hal/se050_apdu.h` | SE050 message limit 180 → 242 |
| H13 | `src/net/espnow_mgr.cpp`, `.h` | receive timestamp |
| H14 | `src/hal/display.cpp` `statusBar()` | status items |
| H15 | `src/ui/boot.cpp` `progress()` | Receipt boot screen and LED boot bar |
| H16 | `src/config.h` | API version 2 |
| H17 | `src/hal/buttons.cpp` `update()` | injected buttons (dev profile only) |
| H18 | `src/identity/identity.cpp` | optional: never use the SE050 for a new key |
| H19 | `src/lua_sdk/lua_runtime.cpp` `callGlobal()` | no Lua callback runs while the approval is up |
| H20 | `src/ui/shell.cpp` `update()` | the shell repaints when Badge OS asks |
| H21 | `src/hal/se050.cpp` `test()`, `src/hal/se050_t1.cpp` `begin()`, `src/hal/badge_i2c.cpp` `scan()` | provisional: nothing addresses the SE050 on the I²C bus; the badge behaves as if it had no secure element (button fix, finding F17) |

