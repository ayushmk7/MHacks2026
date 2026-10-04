# Upstream hooks

A copy of the two tables in `docs/os/architecture/upstream-hooks.md` ("Table" and "Replaced upstream
files"), which is the source: change that document first, then this copy. `scripts/preflash-check.sh`
(check 1) reads both tables and fails on any difference:

- it compares the hook ids tagged `// VK: H<n>` in upstream files with the hook table. A row whose
  purpose names a range of sites (H8: "H8a–H8f") stands for those ids; a row whose purpose starts
  with `optional:` may be absent from the source. H22 (the loop-task stack size, execution plan
  Risk 5) is reserved and has no row: the script accepts it when it is present;
- it reads the replaced-files table: every path whose kind is `deleted` must not exist, and every
  path whose kind is `rewritten` or `edited` must exist.

Upstream is commit `812b8c7`; its `solana-os.ino` is `os.ino` here. Ids are never reused: H14, H15
and H20 are retired, and no line in the source carries them.

## Table

| Id | File | Purpose |
|---|---|---|
| H1 | `os.ino` | include `src/vk/vk.h` |
| H2 | `os.ino` `setup()` | start BadgeOS after the Lua runtime |
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
| H16 | `src/config.h` | API version 2 |
| H17 | `src/hal/buttons.cpp` `update()` | injected buttons (dev profile only) |
| H18 | `src/identity/identity.cpp` | optional: never use the SE050 for a new key |
| H19 | `src/lua_sdk/lua_runtime.cpp` `callGlobal()` | no Lua callback runs while the approval is up |
| H21 | `src/hal/se050.cpp` `test()`, `src/hal/se050_t1.cpp` `begin()`, `src/hal/badge_i2c.cpp` `scan()` | provisional: nothing addresses the SE050 on the I²C bus; the badge behaves as if it had no secure element (button fix, finding F17) |
| H23 | `src/config.h`, `src/net/espnow_mgr.cpp`, `src/lua_sdk/lib_gfx.cpp`, `src/net/push_server.cpp`, `src/net/push_protocol.cpp` | BadgeOS names: OS name, hostname, hotspot password, broker URL, ESP-NOW magic; upstream's `SOLANA_*` Lua colour constants removed; the LED pulse when a push lands uses the theme's LED colour, not upstream's brand colours |
| H24 | `os.ino` `loop()` | the canvas is sent to the panel through `vk::flush()`, which counts the transfers |
| H25 | `src/apps/app_store.cpp` `refresh()` | the launcher's app catalogue is rebuilt after a rescan (two sites: the include and the call) |

## Replaced upstream files

Paths are relative to `os/`. Kinds: **deleted** (the file or folder does not exist in the fork),
**rewritten** (same path, BadgeOS's content), **edited** (upstream's file with text changes that
cannot carry a hook tag). A replaced file carries no hook tag.

| Path | Kind | What and why |
|---|---|---|
| `src/ui/shell.cpp` | deleted | upstream's launcher, settings screens, offer screens, error screen and delete confirmation. Replaced by `src/vk/shell/` (docs/os/ui/shell.md), which defines the same four functions. `src/ui/shell.h` is untouched and is the interface `os.ino` includes |
| `src/ui/boot.cpp` | rewritten | no splash images; `boot::progress()` draws the Receipt boot screen. `src/ui/boot.h` is untouched (its comment still describes upstream's splash) |
| `splash_images.h` | deleted | upstream's two splash images (the Solana logo and the SKYRIZZ credit) |
| `src/ui/theme.h` | edited | the palette constants hold Receipt-light values, so leftover upstream drawing and Lua's named colours match BadgeOS |
| `src/net/push_server.cpp` | edited | the embedded web page: title and heading `BadgeOS`, colours from the Receipt-light palette. A raw string cannot carry a hook tag. The only C++ change in the file is the tagged H23 LED line |
| `tools/badge-push.py` | edited | texts say BadgeOS; the example host is `badgeos.local` |
| `README.md` | rewritten | short: what BadgeOS is, the credit line "BadgeOS is built on Solana OS by spacemandev.", a pointer to `docs/os/`. Upstream's README is kept as `docs/os/reference/upstream-readme.md` |
| `apps/gallery`, `apps/hello`, `apps/radar`, `apps/vumeter`, `apps/whosnear` | deleted | upstream's sample apps. Upstream's `apps/dice` was deleted too; the `apps/dice` in the tree is BadgeOS's own app, written fresh (docs/os/apps/apps.md) |
