# Upstream hooks

Every change BadgeOS makes to a Solana OS file. This list is complete: if a change to an upstream file is not here, it must not exist.

BadgeOS is a fork: the runtime, the radios, app push, the store client and the device key are upstream's code, and the user interface (boot screen, launcher, settings, dialogs) is BadgeOS's own ([shell](../ui/shell.md)). So an upstream file is in exactly one of three states:

1. **untouched**: byte for byte upstream's text;
2. **hooked**: upstream's text plus tagged hook lines from the [table](#table);
3. **replaced**: deleted, rewritten or edited as a whole, and listed in [Replaced upstream files](#replaced-upstream-files).

## Rules

1. A hook is the smallest edit that hands control to code under `src/vk/`, or that changes one upstream value. Logic never goes in an upstream file.
2. Every changed or added line of a hook ends with `// VK: H<n>`.
3. A change that cannot be a few tagged lines (a file BadgeOS deletes, rewrites, or whose text cannot carry a tag) is a row in [Replaced upstream files](#replaced-upstream-files). A replaced file carries no hook tag.
4. `os/UPSTREAM-HOOKS.md` is a copy of the two tables in this file (hooks, replaced files). `scripts/preflash-check.sh` (check 1) does two things and fails on any difference:
   - it compares the hook ids found by `grep -rn "// VK: H" os.ino src | grep -v "^src/vk/"` with the hook table. The exclusion is anchored to the start of the line, which is the file path: the H1 line itself contains the text `src/vk/` and an unanchored exclusion would drop it. A table row whose purpose names a range of sites (H8: "H8a–H8f") stands for those ids; a row whose purpose starts with `optional:` (H18) may be absent from the source;
   - it reads the replaced-files table: every path whose kind is `deleted` must not exist, and every path whose kind is `rewritten` or `edited` must exist.
5. A new hook needs a new id here first; a new replaced file needs a row here first. Prefer a registry ([overview](overview.md#6-self-registration)) over either: most additions need none. Ids are never reused: H14, H15 and H20 are retired ([Retired hooks](#retired-hooks)), and H22 is reserved for the loop-task stack size (execution plan, Risk 5).
6. Hooks leave upstream's behaviour unchanged unless this file says otherwise. Two WP01 stubs are not empty: `router::install()` must install a handler that forwards to `runtime::dispatchEspnow`, and `signStoreRegistration()` must forward to the signer. Four hooks change behaviour on purpose: H9 (fixes finding F1), H12 (a larger SE050 limit), H16 (API version 2) and H23 (BadgeOS's names and network identifiers).

Line numbers are for upstream commit `812b8c7`. Upstream's `solana-os.ino` is `os.ino` in the fork; that rename is the one change that is neither a tagged line nor a row in the replaced-files table.

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
| H23 | `src/config.h`, `src/net/espnow_mgr.cpp`, `src/lua_sdk/lib_gfx.cpp` | BadgeOS names: OS name, hostname, hotspot password, broker URL, ESP-NOW magic; upstream's `SOLANA_*` Lua colour constants removed |

## The edits

### H1 — include

After the last upstream `#include` in `os.ino`:

```cpp
#include "src/vk/vk.h"  // VK: H1
```

### H2 — boot

In `setup()`, directly after `runtime::begin();`:

```cpp
  boot::progress("Wallet", "config, keys, stores", 75);  // VK: H2
  vk::begin();                                           // VK: H2
```

`vk::begin()` never fails the boot. If the config is missing the badge runs unprovisioned ([config](../platform/config.md#provisioning)).

### H3 — ESP-NOW handler

In `startRadios()`, replace the whole `espnow_mgr::onReceive(...)` statement (the lambda that calls `runtime::dispatchEspnow`) with:

```cpp
  vk::host::router::install();  // VK: H3
```

The router forwards to `runtime::dispatchEspnow` itself ([protocol](../protocol/espnow.md#router)).

### H4 — modal

In `loop()`, replace

```cpp
  routeButtons();

  if (runtime::running()) {
    runtime::update();
  } else {
    shell::update();
  }
```

with

```cpp
  if (vk::modalActive()) {     // VK: H4
    vk::modalUpdate();         // VK: H4
  } else {                     // VK: H4
    routeButtons();
    if (runtime::running()) {
      runtime::update();
    } else {
      shell::update();
    }
  }                            // VK: H4
```

In the same function, a few lines below, change the `processRequests` line so that no app is launched or stopped behind the approval:

```cpp
  const bool lifecycleRan = vk::modalActive() ? false : runtime::processRequests();  // VK: H4
```

Requests queued during the approval (a pushed `RUN` or `STOP`, `badge.system.launch`) are applied on the first pass after it closes. `shell::update()` here is BadgeOS's shell (`src/vk/shell/shell.cpp`); upstream's `src/ui/shell.h` still declares it ([shell](../ui/shell.md#framework)). While the approval is up the shell neither reads buttons nor draws; the approval engine calls `vk::ui::requestShellRepaint()` when it closes and the shell redraws on its next pass.

### H5 — services

In `loop()`, directly after `broker::update();`:

```cpp
  vk::update();  // VK: H5
```

### H6 — serial commands

In `pumpSerialConsole()`, replace the `push_protocol::handleLine(sSerialLine, [](const String &reply) {...});` call with:

```cpp
        const push_protocol::Reply reply = [](const String &text) { badge_log::println(text.c_str()); };  // VK: H6
        if (!vk::serial::handleLine(sSerialLine, reply)) push_protocol::handleLine(sSerialLine, reply);   // VK: H6
```

Our commands therefore exist on USB serial only, never on BLE or HTTP (finding F11).

### H7 — Lua bindings

In `openBadge()`, directly after `openBle(L);`, and add `#include "../vk/host/lua_registry.h"` (also tagged):

```cpp
  vk::lua::open(L);  // VK: H7
```

`vk::lua::open` runs with the `badge` table on top of the stack. It adds every registered Lua function the app being launched is permitted to call, replaces the upstream module tables the app was not granted with tables that raise a clear error, and removes the globals `loadfile` and `dofile` (finding F13) ([app host](../platform/app-host.md#permissions)).

### H8 — pre-launch and native apps

All in `src/lua_sdk/lua_runtime.cpp`. Add `#include "../vk/host/native.h"` and `#include "../vk/host/permissions.h"`, tagged `// VK: H8a`: the source has no plain `H8` id, only H8a to H8f.

H8a, first lines of `launch()`, before the `app_store::exists` check:

```cpp
  { String vkError; if (!vk::host::preLaunch(appId, vkError)) {                                          // VK: H8a
      if (!running()) sLastError = vkError; else badge_log::tagf("lua", "%s", vkError.c_str());          // VK: H8a
      return false; } }                                                                                  // VK: H8a
  if (vk::host::native::exists(appId)) {                                                                // VK: H8a
    stop(); sLastError = ""; sCurrentApp = appId; sLastApp = appId; sLastFrameAt = millis();             // VK: H8a
    if (vk::host::native::start(appId)) return true;                                                     // VK: H8a
    sCurrentApp = ""; sLastError = "native app failed to start"; return false;                           // VK: H8a
  }                                                                                                      // VK: H8a
```

H8b, first lines of `stop()`:

```cpp
  vk::host::onAppStopping(sCurrentApp);  // VK: H8b
  if (vk::host::native::active()) {      // VK: H8b
    sTeardownActive = true; vk::host::native::stop(); sTeardownActive = false;                          // VK: H8b
    ble_mgr::clearLineHandler(); ble_bridge::reset(); mic::disable(); leds::stopAnimation(); leds::off();  // VK: H8b
    sCurrentApp = ""; return;            // VK: H8b
  }                                      // VK: H8b
```

H8c, `running()`:

```cpp
bool running() { return sState != nullptr || vk::host::native::active(); }  // VK: H8c
```

H8d, first lines of `update()`:

```cpp
  if (vk::host::native::active()) {                                    // VK: H8d
    const uint32_t vkNow = millis();                                   // VK: H8d
    vk::host::native::update((vkNow - sLastFrameAt) / 1000.0f);        // VK: H8d
    sLastFrameAt = vkNow; return true;                                 // VK: H8d
  }                                                                    // VK: H8d
```

H8e, H8f, first line of `dispatchButton`, `dispatchEspnow`:

```cpp
  if (vk::host::native::active()) { vk::host::native::button(key, pressed); return; }            // VK: H8e
  if (vk::host::native::active()) { vk::host::native::espnow(mac, data, length, rssi); return; } // VK: H8f
```

`dispatchBle` is not hooked: its only caller is the handler that Lua's `badge.ble.listen()` installs, so it never runs for a native app. Native apps have no `on_ble` callback.

When a launch is refused while another app is running, the error is logged instead of stored: otherwise it would be shown later, when the running app exits normally, as if that app had failed.

A launch that returns false still makes upstream's loop run its "app stopped" branch: the push session is reset (a serial or BLE pusher must `AUTH` again), the app list is rescanned (`shell::onAppStopped()` calls `app_store::refresh()`), the shell returns to the launcher, which keeps its cursor (clamped to the new app count), and the idle LED animation restarts.

### H9 — keep the ESP-NOW handler

In `stop()`, replace `espnow_mgr::clearReceiveHandler();` with:

```cpp
  // VK: H9 the router owns the ESP-NOW handler for the life of the firmware (upstream finding F1)
```

`ble_mgr::clearLineHandler();` on the next line stays.

### H10 — store registration

In `submitRegistration()` (line 513), replace `identity::signBase64(message)`:

```cpp
  const String signature = vk::wallet::signStoreRegistration(message);  // VK: H10
```

Add `#include "../vk/wallet/signer.h"  // VK: H10` to the file. It returns an empty string when the `store_reg` feature is absent or the text is not exactly a registration line, which the existing code already treats as "identity refused to sign".

### H11 — native apps in the app list

In `src/apps/app_store.cpp`, with `#include "../vk/host/native.h"` (tagged). Native apps are listed after the Lua apps.

```cpp
// count():   return <existing value> + vk::host::native::count();                               // VK: H11
// at():      if (index >= <existing count>) return vk::host::native::infoAt(index - <existing count>, out);  // VK: H11
// byId():    if (vk::host::native::infoById(id, out)) return true;                              // VK: H11  (first line)
// exists():  if (vk::host::native::exists(id)) return true;                                     // VK: H11  (first line)
```

`<existing count>` is upstream's `sCount`. `exists()` in upstream checks the filesystem, not the cached list. `infoAt` and `infoById` must fill every field of `Info` (`sizeBytes = 0`, `entry = ""`): the struct has no initialisers.

`removeApp()` is not hooked: it finds no folder for a native id and returns false. The launcher never offers delete for a native app ([shell](../ui/shell.md#launcher)), so that path is reached only by a push. Known consequences, accepted: a pushed `DEL <native id>` stops that app if it is running and then answers "delete failed"; pushing a Lua app whose id equals a native id would create a second list entry, so `preLaunch` refuses to launch a Lua folder whose id is also a native id and logs it.

### H12 — SE050 message limit

`src/hal/se050_apdu.h`:

```cpp
constexpr size_t MAX_SIGN_MESSAGE_BYTES = 242;  // VK: H12 (was 180). 242 + 12 bytes of TLV = 0xFE, the largest single-byte Lc
```

[UNVERIFIED] on a real SE050; see [signing](../wallet/signing.md#key). The specific risk: a 242-byte message makes a 259-byte APDU, which is three chained T=1 blocks, and upstream's chaining path has never run on a part.

### H13 — receive timestamp

`src/net/espnow_mgr.cpp`: add `uint32_t rxMs;` to `QueuedPacket`; in `onDataReceived` next to `slot.rssi = rssi;` add `slot.rxMs = millis();`; in `update()` directly before the `sHandler(...)` call add `sLastRxMs = packet.rxMs;`; add `uint32_t sLastRxMs = 0;` and `uint32_t lastRxMs() { return sLastRxMs; }`. Declare `uint32_t lastRxMs();` in the header. All tagged `// VK: H13`.

The router reads `espnow_mgr::lastRxMs()` to time presence proofs from the moment the radio received the frame, not from when the loop got round to it.

### H16 — API version

`src/config.h`:

```cpp
#define SOLANA_OS_API_VERSION 2  // VK: H16
```

### H17 — injected buttons

At the top of `buttons.cpp`:

```cpp
#include "../vk/vk_build.h"                     // VK: H17
#if VK_TEST_HOOKS                                // VK: H17
#include "../vk/features/devtools/devtools.h"   // VK: H17
#endif                                           // VK: H17
```

In `buttons::update()`, directly before the loop that maintains `sDownSince`:

```cpp
#if VK_TEST_HOOKS                                                           // VK: H17
  vk_dev_apply_injected_buttons(&sDownMask, &sPressedMask, &sReleasedMask); // VK: H17
#endif                                                                      // VK: H17
```

`extern "C" void vk_dev_apply_injected_buttons(uint8_t *down, uint8_t *pressed, uint8_t *released);` is declared in `src/vk/features/devtools/devtools.h`. In the release profile the hook compiles to nothing. Upstream clears the pressed and released masks on every pass and can overwrite the down mask on any hardware change, so the injector runs on every pass and, for the keys it is holding, forces the bit on in `*down`, clears it in `*released`, and sets it in `*pressed` only on the pass the injected press begins.

### H18 — force a software key (optional)

First line of `createOnSecureElement()` in `src/identity/identity.cpp`, with `#include "../vk/vk_build.h"`:

```cpp
  if (VK_FORCE_SOFTWARE_KEY) return false;  // VK: H18
```

This affects only the creation of a new identity. A badge that already holds an SE050 identity keeps it until Settings → Identity → New identity is used (the page exists in BadgeOS's shell and calls the same `identity::regenerate()`: [shell](../ui/shell.md#settings-pages)).

### H19 — no Lua while the approval is up

In `callGlobal()` in `src/lua_sdk/lua_runtime.cpp`, directly after `if (sState == nullptr) return true;`, with `#include "../vk/host/lifecycle.h"` (tagged):

```cpp
  if (vk::host::luaPaused()) { lua_pop(sState, argCount); return true; }  // VK: H19
```

`luaPaused()` is true while the approval is active. H4 already stops the per-frame callbacks; this closes every other way into Lua: a BLE line arriving for an app that called `badge.ble.listen()`, a frame, `on_stop` when a pusher deletes the running app. A skipped callback is dropped, not queued.

### H21 — SE050 quarantine (provisional)

Finding F17: on the development badge, a Solana OS session leaves an I²C slave holding SCL low. The hold survives resets and reflashing and clears only when power is removed; while it lasts the button expander cannot be read under any firmware, which is why the buttons "work in the test kit but not in Solana OS". By elimination the slave is the SE050 (it stretches the clock, has no usable reset on this board, and is the device addressed just before the failure). The two operations only Solana OS performs on it are the boot-time bus scan's zero-length probe of `0x48` and the applet-select write. Which of them latches the part is **not yet proven**: that needs a power cycle and a second deliberate failure.

Until it is proven, the fork does not address the SE050 at all. Every I²C transfer to `0x48` (`SE050_ADDR`) in upstream starts in one of three functions, and each returns its "absent" result first when `VK_SE050_QUARANTINE` is 1. Each of the three files also gets `#include "../vk/vk_build.h"  // VK: H21` after its own includes.

In `se050::test()` in `src/hal/se050.cpp` (the soft reset and ATR read), as the first line:

```cpp
  if (VK_SE050_QUARANTINE) return false;  // VK: H21 (never address 0x48; present() stays false)
```

In `se050_t1::begin()` in `src/hal/se050_t1.cpp` (the T=1 link: soft reset and applet select), as the first line:

```cpp
  if (VK_SE050_QUARANTINE) { sError = "quarantined (H21)"; return false; }  // VK: H21
```

In `badge_i2c::scan()` in `src/hal/badge_i2c.cpp` (a zero-length probe of every address, `0x48` included), as the first line:

```cpp
  if (VK_SE050_QUARANTINE) return;  // VK: H21 (the scan probes every address, 0x48 included)
```

`VK_SE050_QUARANTINE` is defined in `src/vk/vk_build.h` and is 1. Set it to 0 only on a badge whose SE050 is known to work. The first version of this hook (WP01) was two guards in `setup()` in `os.ino`; they covered only the boot path and are gone, so `os.ino` has upstream's text at those two lines again.

Why these three are enough (read from the source, and checked by the grep below):

- `se050_t1.cpp` has the only other code that transfers to `SE050_ADDR`: `writeBlock()` and `readBlock()`. They are reached only through `exchange()` (called by `softReset()`, called by `begin()`) and through `transceive()`, which runs `if (!sReady && !begin()) return false;` first. `sReady` becomes true only inside `begin()` after its first line, so with the guard it is never true and no block is ever written or read.
- `se050_apdu.cpp` has no I²C call of its own: `begin()` calls `se050_t1::begin()`, and every command goes through `se050_t1::transceive()`. `se050::randomBytes()` does the same.
- `badge_i2c::readReg()` and `writeReg()` take the address from the caller; the only caller is `buttons.cpp`, with `TCA9534_ADDR`.

```bash
cd os
grep -rn "SE050_ADDR" os.ino src            # config.h (the constant), se050.cpp, se050_t1.cpp, badge_i2c.cpp only
grep -rln "Wire\.beginTransmission\|Wire\.requestFrom" os.ino src   # the same three .cpp files
```

What each caller now sees, all of which upstream already handles as "no secure element":

| Path | Result with the quarantine |
|---|---|
| `setup()`: `se050::test()`, `badge_i2c::scan()` | no `[se050]` line and no `[i2c] scanning bus` line in the boot log |
| `identity::create()` (a badge with no stored identity, or Settings → Identity → New identity) | `se050_apdu::begin()` fails, the log has `[se050] link/select failed (quarantined (H21))` and `[id] SE050 unavailable (select)`, and the key is made in software; `se050::randomBytes()` fails, so the seed uses the chip's own random source only |
| `identity::load()` and `identity::sign()` with an identity stored in the SE050 | upstream keeps the stored identity (`[id] SE050 did not answer; identity present but cannot sign this boot`) and every signature fails. Do not set the flag on a badge whose key is in its SE050 |
| Settings → Device info, SELECT | `badge_i2c::retry()` and `buttons::retry()` still run; the SE050 test and the scan do nothing; the row reads `SE050 … no answer` |
| Lua `badge.se050.test()`, `random()`, `random_available()` | `false`, `nil, "quarantined (H21)"`, `false` |

This hook does not release a bus that is already held: **the badge must be power-cycled once** (USB unplugged, battery off, a few seconds) after flashing a build that contains it. It is provisional: when the trigger is identified, replace it with the narrowest change that avoids that one operation and update this section.

### H23 — BadgeOS names

Nothing a user can see and no network identifier says "Solana" or "SKYRIZZ" ([shell](../ui/shell.md#what-was-removed)). The values that are single lines in upstream files are changed in place and tagged.

`src/config.h` (four lines; the macro and constant names are upstream's identifiers, invisible to users, and stay):

```cpp
#define SOLANA_OS_NAME     "BadgeOS"  // VK: H23 (was "Solana OS")
constexpr char     DEFAULT_HOSTNAME[]   = "badgeos";  // VK: H23 (was "solana-badge")
constexpr char     DEFAULT_AP_PASSWORD[] = "badgeos-setup";  // VK: H23 (was "solanabadge")
#define DEFAULT_BROKER_URL ""  // VK: H23 (was upstream's DEF CON broker): the store client is off until an address is set from the web UI
```

`SOLANA_OS_NAME` is what the serial banner, the push protocol's `INFO` reply, `/api/status` and Settings → Device info print. With an empty broker URL upstream's store client stays compiled in and idle: `broker::update()` returns at `url().length() == 0`, so it never registers and never polls; Settings → App store shows `off` ([shell](../ui/shell.md#settings-pages)).

`src/net/espnow_mgr.cpp`:

```cpp
constexpr uint8_t MAGIC[4] = {'B', 'D', 'O', 'S'};  // VK: H23 (was SBDG)
```

`src/lua_sdk/lib_gfx.cpp`: the four entries `{"SOLANA_PURPLE", …}`, `{"SOLANA_GREEN", …}`, `{"SOLANA_TEAL", …}` and `{"SOLANA_MAGENTA", …}` of the colour-constant table are deleted and replaced by one line:

```cpp
    // VK: H23 upstream's SOLANA_* colour constants are removed; apps use badge.theme.color
```

No `gfx.PAPER` or `gfx.INK` is added: the theme ([ui](../ui/ui.md#theme)) is the only source of BadgeOS's colours, and it changes at run time, which a constant cannot.

Consequences, accepted:

- A badge running BadgeOS and a badge running upstream Solana OS no longer hear each other's ESP-NOW frames or beacons (different magic). All badges of one deployment must run BadgeOS.
- The default hostname and the default hotspot password changed. A badge whose settings already store other values keeps them; the device name shown to other badges (ESP-NOW beacon, BLE) is upstream's `badge-XXXX` default and is not changed.
- A Lua app that uses `gfx.SOLANA_PURPLE` and its three siblings reads `nil`. Upstream's sample apps, which did, are deleted ([Replaced upstream files](#replaced-upstream-files)).

## Retired hooks

Three hooks of the first design are gone. Their ids are not reused, and no line in the source carries them.

| Id | Was | Why it is gone |
|---|---|---|
| H14 | `src/hal/display.cpp` `statusBar()`: draw BadgeOS's status items in upstream's bar | The status-item registry is removed: every BadgeOS header shows only the time and the battery ([ui](../ui/ui.md#header)). `display.cpp` is upstream's text again. `display::statusBar()` is dead code that nothing calls |
| H15 | `src/ui/boot.cpp` `progress()`: draw the Receipt boot screen instead of upstream's | `boot.cpp` is a replaced file now: there is nothing of upstream's left in it to hook |
| H20 | `src/ui/shell.cpp` `update()`: repaint when BadgeOS asks | `shell.cpp` is deleted. BadgeOS's shell calls `vk::ui::consumeShellRepaint()` itself (`src/vk/ui/repaint.h`) |

## Replaced upstream files

Paths are relative to `os/`. Kinds: **deleted** (the file or folder does not exist in the fork), **rewritten** (same path, BadgeOS's content), **edited** (upstream's file with text changes that cannot carry a hook tag). `os/UPSTREAM-HOOKS.md` copies this table and pre-flash check 1 reads it (rule 4).

| Path | Kind | What and why |
|---|---|---|
| `src/ui/shell.cpp` | deleted | upstream's launcher, settings screens, offer screens, error screen and delete confirmation. Replaced by `src/vk/shell/` ([shell](../ui/shell.md)), which defines the same four functions. `src/ui/shell.h` is untouched and is the interface `os.ino` includes |
| `src/ui/boot.cpp` | rewritten | no splash images; `boot::progress()` draws the Receipt boot screen. The complete file is below. `src/ui/boot.h` is untouched (its comment still describes upstream's splash) |
| `splash_images.h` | deleted | upstream's two splash images (the Solana logo and the SKYRIZZ credit) |
| `src/ui/theme.h` | edited | the palette constants hold Receipt-light values, so leftover upstream drawing and Lua's named colours match BadgeOS (table below) |
| `src/net/push_server.cpp` | edited | the embedded web page only: title and heading `BadgeOS`, colours from the Receipt-light palette. No C++ logic changes. A raw string cannot carry a hook tag |
| `tools/badge-push.py` | edited | texts say BadgeOS; the example host is `badgeos.local` |
| `README.md` | rewritten | short: what BadgeOS is, the credit line "BadgeOS is built on Solana OS by spacemandev.", a pointer to `docs/os/`. Upstream's README is kept as [`docs/os/reference/upstream-readme.md`](../reference/upstream-readme.md) |
| `apps/dice`, `apps/gallery`, `apps/hello`, `apps/radar`, `apps/vumeter`, `apps/whosnear` | deleted | upstream's six sample apps |

### `src/ui/boot.cpp`, complete

```cpp
// BadgeOS boot sequence: no splash. Each stage of setup() draws the Receipt boot screen
// (docs/os/ui/shell.md, "Boot"). This file replaces upstream's boot.cpp
// (docs/os/architecture/upstream-hooks.md, "Replaced upstream files").
#include "boot.h"

#include "../hal/display.h"
#include "../settings.h"
#include "../vk/ui/leds.h"

namespace boot {

void progress(const char *step, const char *detail, uint8_t percent) {
  vk::ui::bootScreen(step, detail, percent);
}

void run() {
  display::setBrightness(settings::brightness());
  progress("", "power on", 0);
}

}  // namespace boot
```

`vk::ui::bootScreen` (declared in `src/vk/ui/leds.h`, defined in `src/vk/ui/boot_screen.cpp`) stores the percentage for the LED boot bar ([ui](../ui/ui.md#boot-bar)), draws the screen, flushes the display and advances the LED bar by one frame. `os.ino` is not changed: `setup()` still calls `boot::run()` once and `boot::progress()` as each stage begins (20, 45, 55, 65, 75 from hook H2, 85, 100). Upstream's `leds::playBoot()` is no longer called by anything, and `config.h`'s `SPLASH_*` constants are unused.

### `src/ui/theme.h`, the values

Only the values change; every name stays, so upstream code that uses them compiles unchanged.

| Constant | New value | Receipt token it equals |
|---|---|---|
| `BG`, `HEADER`, `PANEL` | `#F3EFE4` | `PAPER` |
| `PANEL_2` | `#E4DFD0` | none: paper, one step darker |
| `BORDER` | `#8A8474` | `FAINT` |
| `TEXT` | `#1B1A17` | `INK` |
| `MUTED` | `#6D6759` | `SUB` |
| `PURPLE` (and `ACCENT`) | `#1B1A17` | `INK`: upstream's accent becomes ink |
| `GREEN` (and `OK`), `TEAL` | `#17804F` | `STAMP_OK` |
| `MAGENTA` | `#C8321E` | `STAMP_BAD` |
| `WARN` | `#B56A00` | `STAMP_WARN` |
| `ERR` | `#C8321E` | `STAMP_BAD` |
| `WHITE`, `BLACK` | unchanged | Lua exposes them as colour names; they keep their literal meaning |
| `BRAND_PURPLE_RGB`, `BRAND_MAGENTA_RGB` | `0x1B1A17` | `INK`: the start of the ramp |
| `BRAND_GREEN_RGB`, `BRAND_TEAL_RGB` | `0xFFE2AA` | `LED`: the end of the ramp |

`theme::gradient()` interpolates from `BRAND_PURPLE_RGB` to `BRAND_GREEN_RGB`, so Lua's `gfx.gradient` and `led.gradient` now run from ink to the LED colour. These constants are compile-time and do not follow the dark theme. BadgeOS's own screens never use them: they draw with `vk::ui::theme::color()`. What still uses them is the first fill of the panel in `display::begin()`, and Lua's named colours in `lib_gfx.cpp` (`gfx.GREEN`, `gfx.TEXT`, …).

### Names that stay

The word "solana" remains in the source in two kinds of place, and neither is the OS brand.

- **Upstream identifiers and protocol texts that no user sees:** the macro names `SOLANA_OS_NAME`, `SOLANA_OS_VERSION` and `SOLANA_OS_API_VERSION`; the store registration text `solana-badge-register:` (upstream's broker protocol, signed only if a broker URL is configured, through signing domain `store-reg`); the identity self-test text `solana-badge identity self test` (signed and verified locally, never shown or sent); upstream's comments. The default device name `badge-XXXX` never said Solana and stays.
- **The Solana blockchain:** the feature `solana_pay`, the signing domain `solana`, the Lua function `wallet.begin_solana`, the pure files `sol_*.c` and `sol.h`, and `rpc_url` examples such as `https://api.devnet.solana.com`. That is the payment network the badge pays on, not the name of the OS.

The credit to upstream is in the repository (`os/README.md`, [README](../README.md)) and never on the device.

## Checking the hooks

```bash
cd os
grep -rn "// VK: H" os.ino src | grep -v "^src/vk/" | sed -E 's/.*VK: (H[0-9]+[a-z]?).*/\1/' \
  | sort -u | sort -t H -k 2n | tr '\n' ' '
```

Expected output: `H1 H2 H3 H4 H5 H6 H7 H8a H8b H8c H8d H8e H8f H9 H10 H11 H12 H13 H16 H17 H19 H21 H23`, plus `H18` if used (and `H22` if Risk 5's fallback was applied).

The replaced files (pre-flash check 1, second half):

```bash
cd os
for p in src/ui/shell.cpp splash_images.h apps/dice apps/gallery apps/hello apps/radar apps/vumeter apps/whosnear; do
  [ -e "$p" ] && echo "still exists: $p"; done
for p in src/ui/boot.cpp src/ui/theme.h src/net/push_server.cpp tools/badge-push.py README.md; do
  [ -e "$p" ] || echo "missing: $p"; done
```

Expected output: nothing. The script takes the paths from the table in `UPSTREAM-HOOKS.md`, not from a list of its own.

The names (pre-flash check 7): no line of code or user-visible text names upstream's brand.

```bash
cd os
grep -rniE 'solana|skyrizz' os.ino src tools/badge-push.py README.md \
  | grep -v -e '^src/lua/' -e '^src/vk/features/solana_pay/' -e '^src/vk/wallet/pure/sol' \
  | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|/\*|\*)' \
  | grep -vE 'SOLANA_OS_(NAME|VERSION|API_VERSION)|solana_pay|begin_solana|"solana"|solana-badge-register:|solana-badge identity self test|Solana OS by spacemandev'
```

Expected output: nothing. The second filter drops the folders that are about the blockchain, the third drops whole-line comments, and the fourth drops the names listed under [Names that stay](#names-that-stay). A line of code with a trailing comment that names Solana is printed: reword the comment if the file is ours, or add the file's wording to this section if it is upstream's.
