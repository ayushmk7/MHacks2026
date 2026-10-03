# Build and flash

From an empty laptop to four badges running the OS: toolchain, building upstream unmodified, creating the fork, compile-time switches, the checks before every flash, and identity reset.

Audience: firmware engineers.

Status: design, not yet built on hardware.

**Read this first.** No command in this guide has been executed. `arduino-cli` and `esptool` were not installed on the machine where this was designed, and no badge has been flashed. The commands are copied from the upstream manual (`firmware/solana-os/README.md` at commit `812b8c7` of the [upstream repository](https://github.com/spacemandev-git/solana-defcon-badge-26/tree/812b8c7aca5c366d18c0b040fafd2999f7204d84/firmware/solana-os)) and from the design. Expect to correct details the first time through, and record the corrections here. [UNVERIFIED: everything on hardware; fallback: work package WP0 exists to find out before any of our code is written]

One observation exists. On 2026-10-03 a badge was connected to the design laptop and enumerated as a WCH CH340 USB-serial bridge (USB id `1a86:7523`) at `/dev/cu.usbserial-10`, which matches the upstream description of the CH340C port. The serial port was held by another program, so no command was sent to the badge and nothing was read from it.

The firmware is an Arduino sketch built with `arduino-cli` on the Arduino-ESP32 3.x core [UPSTREAM `README.md:50-96`]. It is not an ESP-IDF or PlatformIO project. CMake is used only for the host unit tests [OURS: the pure-C modules are testable without a badge].

## Toolchain

macOS is shown. Linux differs only in paths and in the serial device names.

1. Install the tools.

   ```bash
   brew install arduino-cli
   arduino-cli config init
   arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   arduino-cli core update-index
   arduino-cli core install esp32:esp32          # must be 3.x; record the exact version that builds
   arduino-cli lib install LovyanGFX
   python3 -m pip install esptool bleak          # esptool for erase/partial flashing, bleak only for BLE push
   ```

2. Check the core version and write it down.

   ```bash
   arduino-cli core list
   ```

   The `esp32:esp32` line must show a 3.x version [UPSTREAM `README.md:67-82`]. [UNVERIFIED: which 3.x version builds upstream cleanly. The latest release on 2026-10-03 was 3.3.12. Fallback: if the newest core fails to build, install an earlier 3.x with `arduino-cli core install esp32:esp32@<version>` and pin that version in the table below.]

   | Tool | Version that built | Date | Who |
   |---|---|---|---|
   | `arduino-cli` | | | |
   | `esp32:esp32` core | | | |
   | `LovyanGFX` | | | |
   | `esptool` | | | |

3. For the host tests you also need a C compiler and CMake 3.16 or newer (`brew install cmake`). See [../testing/host-tests.md](../testing/host-tests.md).

Board settings the build depends on [UPSTREAM `README.md:67-82`]:

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| Flash | 16 MB |
| PSRAM | OPI PSRAM, required (the 150 KB framebuffer and the Lua heap live there) |
| Partition scheme | Custom (`partitions.csv`) |
| USB CDC on boot | Enabled |

All of them are carried by one fully qualified board name, used in every command below:

```bash
FQBN="esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom,CDCOnBoot=cdc"
```

## Build upstream first

This is work package WP0. Do it before touching any of our code: it proves the toolchain, the cable, the badge and the hotspot with firmware that is known to boot.

1. Optional, for a board that has never been powered: flash the upstream test kit and check that the I²C bus answers at `0x20` (button expander) and `0x48` (SE050) [UPSTREAM root `README.md`, "Working with a fresh board"].

   ```bash
   esptool.py --chip esp32s3 write_flash 0x0 firmware/testkit/Badge-testkit-full.bin
   ```

2. Clone upstream and build it unmodified.

   ```bash
   git clone --depth 1 https://github.com/spacemandev-git/solana-defcon-badge-26 /tmp/upstream
   cd /tmp/upstream/firmware/solana-os
   FQBN="esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom,CDCOnBoot=cdc"
   arduino-cli compile --fqbn "$FQBN" .
   ```

   These documents were written against commit `812b8c7`. Check what you cloned with `git -C /tmp/upstream rev-parse --short HEAD`. If upstream has moved on, fetch the documented commit instead, because every upstream line number quoted in this documentation refers to it:

   ```bash
   git -C /tmp/upstream fetch --depth 1 origin 812b8c7aca5c366d18c0b040fafd2999f7204d84
   git -C /tmp/upstream checkout FETCH_HEAD
   ```

   Expected: the compile finishes and reports the image size. Upstream states roughly 1.75 MB of flash and 72 KB of static RAM; the build upstream tracks is 1,849,888 bytes, in an application slot of 3,342,336 bytes [UPSTREAM `README.md:95`, `partitions.csv`].

3. Find the serial port and upload.

   ```bash
   arduino-cli board list                                   # find /dev/cu.usbserial-XXXX (CH340C, USB id 1a86:7523) or /dev/cu.usbmodemXXXX (native USB, S1 switched)
   arduino-cli upload --fqbn "$FQBN" -p /dev/cu.usbserial-XXXX .
   ```

   If the upload cannot connect: hold `BOOT1`, tap `RST1`, release `BOOT1`, then upload again [UPSTREAM root `README.md`]. If the port is busy, another program holds it; close that program's serial monitor first.

4. Open the serial monitor.

   ```bash
   arduino-cli monitor -p /dev/cu.usbserial-XXXX -c baudrate=115200
   ```

   Expected lines, among others [UPSTREAM log formats in `solana-os.ino:59-64, 234`, `src/hal/display.cpp:110`, `src/identity/identity.cpp:265`]:

   ```
   #  <os name> <os version>
   #  built <date> <time>
   #  chip <model> rev<n>  <n> MHz  flash 16MB  psram <n>KB
   [lcd] ready 320x240, <n> KB framebuffer in PSRAM
   [id] <badgeId>, secure element (<n> ms)
   [os] ready
   ```

   The `[id]` line reads `software` instead of `secure element` when the SE050 path fell back; in that case it is preceded by `[id] SE050 refused at <stage>, sw <xxxx>` and `[id] falling back to a software key`. See [troubleshooting](troubleshooting.md#se050-status-words). Either outcome is a pass for WP0. Record which one each badge reports.

   The serial console speaks the push line protocol, not free text [UPSTREAM `README.md:826-872`]. Type `PING`; the badge answers `OK pong`.

5. On the badge, the launcher is empty on a fresh badge. That is expected [UPSTREAM `README.md:118-134`]. Open Settings → Identity and note the badge ID and whether the key lives in the secure element or in software.

6. Put the badge on the phone hotspot ([configure.md](configure.md#wi-fi)), then read its identity over HTTP. The pairing code and the IP address are on Settings → Push.

   ```bash
   BADGE=192.168.x.y; CODE=123456
   curl -s -H "X-Badge-Token: $CODE" "http://$BADGE/api/identity"
   # {"ready":true,"badge_id":"<8 chars>","pubkey":"<base58>","source":"software","status":"<text>"}
   ```

   [UPSTREAM `src/net/push_server.cpp:772-782`]. Our build adds `"key_location"` and `"token_account"` to this response (patch P11).

7. Flash a second badge the same way, join the same hotspot, and open Settings → ESP-NOW on both. Each must list the other [UPSTREAM radar screen]. Both badges must be on the same Wi-Fi channel, which joining one hotspot guarantees [UPSTREAM `README.md:1248-1262`].

WP0 is done when the log lines above were seen, `/api/identity` answers, and two badges see each other on the radar.

## Our tree

The fork does not exist in this repository yet. It is created in work package WP1 as a copy of upstream's `firmware/solana-os/`.

1. Copy upstream into the repository. Run from the repository root.

   ```bash
   mkdir -p firmware
   cp -R /tmp/upstream/firmware/solana-os firmware/solana-os
   ```

2. Add the host-tested reference code and the host tests. The files are copied verbatim [OURS, host-tested].

   ```bash
   mkdir -p firmware/solana-os/src/wallet/vendor firmware/solana-os/test/host
   cp docs/os/reference/code/{sol.h,sol_b58.c,sol_curve.c,sol_pda.c,sol_tx.c,sol_sha256.c,pay_proto.h,pay_proto.c,attest_parse.c} \
      firmware/solana-os/src/wallet/
   cp docs/os/reference/code/{vectors.mjs,vectors-to-h.mjs,vectors.json,vectors.h,test_sol.c,test_pay.c,test_attest.c} \
      firmware/solana-os/test/host/
   ```

   Add the headers that fix the interfaces between modules. They are listings, syntax-checked as C99 and C++17 and never linked; the `.cpp` files that implement them are written in the work packages [OURS].

   ```bash
   H=docs/os/reference/code/sdk-headers
   mkdir -p firmware/solana-os/src/{app_host,sdk}
   cp $H/wallet/{wallet.h,pay_session.h,attest.h,wallet_crypto.h,wallet_internal.h,audit.h,history.h} \
      firmware/solana-os/src/wallet/
   cp $H/identity/identity_private.h firmware/solana-os/src/identity/
   cp $H/app_host/badge_api.h        firmware/solana-os/src/app_host/
   cp $H/sdk/badge_sdk.hpp           firmware/solana-os/src/sdk/
   ```

   `sdk-headers/wallet/sol.h` and `sdk-headers/wallet/pay_proto.h` are byte-identical copies of the two headers already copied above; they exist only so the listings resolve their includes. `sdk-headers/native_apps/registry.cpp` and `tipjar/tipjar.cpp` are not copied here: `arduino-cli` compiles every `.cpp` under `src/`, and they link only once the native runtime exists (work package WP12).

   `test/host/` is outside `src/`, so `arduino-cli` does not compile it [OURS]. `arduino-cli` compiles everything under `src/` recursively [UPSTREAM build layout]. [UNVERIFIED: that the reference C compiles unchanged under the Arduino core. It is plain C99; fallback: fix warnings as they appear.]

3. Vendor the third-party sources into `src/wallet/vendor/` [OURS]:

   | Files | From |
   |---|---|
   | `monocypher.c`, `monocypher.h`, `monocypher-ed25519.c`, `monocypher-ed25519.h` | Monocypher 4.0.2 (`monocypher.{c,h}` and `optional/monocypher-ed25519.{c,h}` of the release; BSD-2-Clause OR CC0; <https://monocypher.org>) |
   | `jsmn.h` | jsmn v1.1.0 (tag `v1.1.0` of <https://github.com/zserge/jsmn>, single file `jsmn.h`, MIT) |

   In firmware `.cpp` files, write `#define JSMN_STATIC` before `#include "vendor/jsmn.h"`, so each file gets its own static copy of the tokenizer. Native apps do not include the vendor path; they get jsmn through the SDK with `#define BADGE_SDK_WITH_JSMN` before including `badge_sdk.hpp` ([C++ apps](../app-platform/cpp-apps.md)).

4. Apply the upstream patches P1–P14 and add the new directories. The patch list with code is in [../architecture/runtime-and-boot.md](../architecture/runtime-and-boot.md); the order in which to apply them is the order of the work packages in [../roadmap/implementation-plan.md](../roadmap/implementation-plan.md). What gets added:

   | Path | Contents |
   |---|---|
   | `src/wallet/` | wallet core: gate and policy (`wallet.cpp`), modal screens (`wallet_ui.cpp`), config (`wallet_config.cpp`, `wallet_defaults.h`), `wallet_crypto.cpp`, `pay_session.cpp`, `attest.cpp`, `rpc.cpp`, `audit.cpp`, `history.cpp`, plus the reference C (step 2), the headers (step 2) and `vendor/` |
   | `src/identity/identity_private.h` | the only declaration of the signing function (patch P4) |
   | `src/app_host/` | `badge_api.h`, `badge_api.cpp`, `app_host.{h,cpp}`, `native_runtime.{h,cpp}` |
   | `src/sdk/badge_sdk.hpp` | the C++ SDK, header only |
   | `src/native_apps/` | `registry.cpp` and one directory per compiled-in app (listings in `docs/os/reference/code/sdk-headers/native_apps/`) |
   | `src/lua_sdk/lib_wallet.cpp` | Lua bindings for all our modules |
   | `apps/` | Lua apps `home`, `pay`, `request`, `history`, `checkout`, `tipjar` |
   | `test/host/` | host tests and vectors |
   | `local_config.h`, `local_config.example.h` | next to `solana-os.ino`; the first is gitignored and holds the first-boot Wi-Fi credentials, the second is committed and shows the two `#define`s (next section) |

5. Build and upload with the same commands as upstream, from the fork.

   ```bash
   cd firmware/solana-os
   FQBN="esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom,CDCOnBoot=cdc"
   arduino-cli compile --fqbn "$FQBN" .
   arduino-cli upload --fqbn "$FQBN" -p /dev/cu.usbserial-XXXX .
   arduino-cli monitor -p /dev/cu.usbserial-XXXX -c baudrate=115200
   ```

   Expected, in addition to the upstream lines: a boot progress step titled `Wallet` with the text `config, token account`, and a `[wallet] ata <n>ms` log line when the badge derives its own token account [OURS]. The wallet start is never fatal: if it fails, the badge boots and signing returns `not_ready` [OURS].

   Image size: our additions are estimated at about 120 KB on top of upstream, which leaves about 1.4 MB free in the 3,342,336-byte slot. [UNVERIFIED estimate; the headroom is the fallback. Record the size the compile reports in [../testing/measurements.md](../testing/measurements.md#other-quantities-to-record).]

   Loop task stack: the fork raises it to 16 KB with `SET_LOOP_TASK_STACK_SIZE(16 * 1024);` in `solana-os.ino` (patch P7). [UNVERIFIED: the size actually needed with TLS, Lua and Ed25519 on one stack; fallback 24 KB. Measure with [M6](../testing/measurements.md#m6).]

## Compile-time switches

All are in `src/wallet/wallet_defaults.h` unless noted [OURS].

| Macro | Default | Meaning |
|---|---|---|
| `WALLET_ED25519_BACKEND` | `1` | 1 Monocypher, 0 TweetNaCl |
| `WALLET_ENABLE_BROKER` | `0` | upstream app store client |
| `WALLET_FORCE_SOFTWARE_KEY` | `0` | skip the SE050 key path (patch P14) |
| `PAY_ENABLE_PRESENCE` | `1` | CHAL/PROOF |
| `PAY_VERIFY_REQ` | `1` | verify REQ signatures |
| `PAY_MEASURE` | `0` | 1 lifts the PROOF count limits, for measurement M1 only |
| `WALLET_DEFAULT_RPC_URL`, `_MINT`, `_CRED`, `_SCHEMA`, `_DASH_URL` | devnet URL, empty, empty, empty, empty | first-boot config |
| `WALLET_DEFAULT_WIFI_SSID`, `WALLET_DEFAULT_WIFI_PASS` (in gitignored `local_config.h`; defaults at the top of `solana-os.ino`) | empty | first-boot Wi-Fi so four badges need no per-badge provisioning |

When to change one:

| Situation | Switch | Consequence |
|---|---|---|
| Monocypher does not build, or is not fast enough | `WALLET_ED25519_BACKEND 0` | TweetNaCl, about a second per operation per upstream; set `deadline_ms` to 2500 and expect amber more often |
| The SE050 path fails on any badge ([T-SE1](../testing/acceptance.md#t-se1)) | `WALLET_FORCE_SOFTWARE_KEY 1` | every badge reports `software`; see [Resetting identity](#resetting-identity) |
| The nonce handshake is not finished | `PAY_ENABLE_PRESENCE 0` | no CHAL is sent; presence is "not checked" (amber) everywhere; the replay demo is dropped |
| REQ signatures are not finished | `PAY_VERIFY_REQ 0` | requests are listed unverified; screens are amber at best |
| Measuring the PROOF round trip ([M1](../testing/measurements.md#m1)) | `PAY_MEASURE 1` | the per-session cap of 8 PROOFs, the cap of 3 per payer MAC and the payer's cap of 3 attempts are lifted; the 200 ms minimum gap stays. Never use this build for the demo |
| You need the upstream app store | `WALLET_ENABLE_BROKER 1` | the broker registration signature is routed through the wallet core (patch P8, which is not scheduled in any work package and is required before this switch is ever set to 1); off by default because it signs without a button press upstream and can raise install prompts during judging |

Baking the demo configuration into the image saves configuring four badges one by one:

- Wallet defaults: once the dashboard's `npm run devnet:setup` has created the mint and the registry, put the values from `curl -s http://127.0.0.1:8787/api/status` (`token.mint`, `registry.credential`, `registry.schema`) and the laptop's listener URL into `WALLET_DEFAULT_MINT`, `WALLET_DEFAULT_CRED`, `WALLET_DEFAULT_SCHEMA`, `WALLET_DEFAULT_DASH_URL`. They apply on first boot only; later changes go through [configure.md](configure.md#wallet-config).
- Wi-Fi: create `firmware/solana-os/local_config.h`, next to `solana-os.ino`, with the phone hotspot credentials. Never commit it; the committed `local_config.example.h` shows the two lines.

  ```c
  #define WALLET_DEFAULT_WIFI_SSID "<phone hotspot SSID>"
  #define WALLET_DEFAULT_WIFI_PASS "<password>"
  ```

  The file is optional. At the top of `solana-os.ino`, before the other includes, the fork has:

  ```cpp
  #if __has_include("local_config.h")
  #include "local_config.h"          // gitignored; defines the two macros below
  #endif
  #ifndef WALLET_DEFAULT_WIFI_SSID
  #define WALLET_DEFAULT_WIFI_SSID ""
  #endif
  #ifndef WALLET_DEFAULT_WIFI_PASS
  #define WALLET_DEFAULT_WIFI_PASS ""
  #endif
  ```

  When `local_config.h` is absent the build succeeds with empty defaults and the first-boot Wi-Fi step is skipped. [UNVERIFIED: `__has_include` under the installed core's compiler; fallback: commit an empty `local_config.h` and drop the `#if`]

  On first boot, if no Wi-Fi is saved and `WALLET_DEFAULT_WIFI_SSID` is not empty, the badge saves it and connects; if `autostart` is unset it is set to `home` [OURS].

## Pre-flash checks

Run these before every flash of the fork. The first two enforce the key gate described in [../wallet-core/signing-gate.md](../wallet-core/signing-gate.md#key-gate); the third runs the host tests.

```bash
cd firmware/solana-os
grep -rn "identity_private.h" src | grep -v "src/wallet/wallet.cpp\|src/identity/"                              # must print nothing
grep -rn "signGated\|se050_apdu::signEd25519\|crypto_ed25519_sign\|crypto_sign(" src \
  | grep -v "src/identity/\|src/wallet/\|src/hal/se050_apdu"                                                     # must print nothing
cmake -S test/host -B /tmp/badge-host && cmake --build /tmp/badge-host && ctest --test-dir /tmp/badge-host --output-on-failure
```

| Check | Pass when | What a failure means |
|---|---|---|
| First `grep` | prints nothing | a file other than `wallet.cpp` or the identity module includes `identity_private.h`, so something outside the gate can name the signing function |
| Second `grep` | prints nothing | a signing primitive is called from outside `src/identity/`, `src/wallet/` or the SE050 driver |
| `ctest` | all three tests pass (`test_sol`, `test_pay`, `test_attest`) | the decoder, the derivations, the frame codec or the attestation parser changed behaviour; do not flash |

These rules stop mistakes, not malice: native code shares one address space [OURS; see [../security/security-model.md](../security/security-model.md)].

## Flashing four badges

1. Flash all four with the same image (Wi-Fi and wallet defaults baked in). Repeat the upload command with each badge's port.
2. For each badge: read `/api/identity` and fill `dashboard/server/config/badges.json` (`pubkey`, `keyLocation`, `standIn: false`). With our build the response carries the value to paste:

   ```bash
   BADGE=192.168.x.y; CODE=123456
   curl -s -H "X-Badge-Token: $CODE" "http://$BADGE/api/identity"
   # {"ready":true,"badge_id":"Gn2GQYmc","pubkey":"Gn2GQYmcQkatE2594ov2ELKkYWZHwARG7FiAoPWSEcxq","source":"software","status":"software key","key_location":"software","token_account":"2awXKo3YDig8kVtqRkpFhf7zf8LZaNj2TAQnGztr6wrr"}
   ```

   The example is the dashboard's stand-in merchant key; its token account is the one for the test-vector mint, and a real badge prints its own. `keyLocation` is `"se050"` or `"software"`, exactly the `key_location` value. Leave `tokenAccount` as `null`: the badge always uses its associated token account [OURS]. Restart the dashboard server after editing the file. The dashboard side of this step is in [BADGE-GAPS.md](../../dashboard/BADGE-GAPS.md).
3. On the laptop, inside `dashboard/`: `npm run devnet:setup` (0.05 SOL, token account, 1000 HACK per badge), then issue "MHacks Merch" to the merchant badge on the Registry page.
4. On each badge: Home shows the balances; Settings → Wallet shows the mint and credential.
5. Label the badges physically (Merchant, Impostor, Judge A, Judge B) to match `badges.json`.
6. Seed known names on each judge badge ([pre-event checklist, item 10](pre-event-checklist.md#10-seed-known-names-on-each-judge-badge)).

Record sheet (fill in; do not commit pairing codes):

| Label | Badge ID | Public key | Key location | Token account | Port |
|---|---|---|---|---|---|
| Merchant | | | | | |
| Impostor | | | | | |
| Judge A | | | | | |
| Judge B | | | | | |

Identity survives reflashing the application: NVS and the SE050 are untouched by `arduino-cli upload` [UPSTREAM partition layout]. A reflash therefore needs no change to `badges.json`, no new funding and no new attestation. The LittleFS volume (installed apps, `/wallet/known.bin`, the audit log, history) is untouched as well, unless a filesystem image is written at `0x670000`; that replaces the whole volume and erases the known-names store, so the judge badges must be seeded again.

## Resetting identity

To give one badge a new key:

1. On the badge: Settings → Identity → New identity. It asks first [UPSTREAM `src/ui/shell.cpp:748-804`].
2. Read the new key from `/api/identity` and update that badge's slot in `badges.json`. Restart the dashboard server.
3. Re-fund it: `npm run devnet:setup` inside `dashboard/`.
4. Re-issue its attestation on the Registry page if it had one. Revoke the attestation of the old key.

`esptool.py erase_flash` also destroys a software key, along with the Wi-Fi credentials, the pairing code, the wallet config and every app. Use it only when you mean to start a badge from nothing.

If the SE050 path fails on a badge (the on-device test [T-SE1](../testing/acceptance.md#t-se1) fails, or signing a transfer fails):

1. Build with `WALLET_FORCE_SOFTWARE_KEY 1` (patch P14: `identity::create()` skips the secure element and creates a software key).
2. Flash.
3. On that badge: Settings → Identity → New identity, to create a software key.
4. Update `badges.json`, re-fund and re-issue for the new key, as above.
5. The badge then reports `software` wherever it reports the key. Drop the secure-element line from the pitch.

A software key is a 32-byte seed stored in plaintext in NVS. Anyone with the badge and a USB cable can read it; flash encryption and secure boot are not enabled [UPSTREAM `README.md:1041-1048`]. Details and the bring-up procedure are in [../wallet-core/keys-and-se050.md](../wallet-core/keys-and-se050.md).

## Requirements covered

No requirement is implemented by this guide. It is the precondition for all of them, and it carries the fallback switches for F8, F9 and F17.

## Open items

- [UNVERIFIED] No command here has been run; `arduino-cli` was not installed where this was designed. Resolved by WP0.
- [UNVERIFIED] Which Arduino-ESP32 3.x version builds upstream cleanly. Pin it in the table under [Toolchain](#toolchain).
- [UNVERIFIED] Arduino core compiler flags (`-std`, exceptions, RTTI) and whether `sodium.h` is present. The SDK rules avoid depending on them; Monocypher is vendored.
- [UNVERIFIED] The reference C compiles unchanged under the Arduino core. Fallback: fix warnings as they appear.
- [UNVERIFIED] Flash size increase of about 120 KB. 1.4 MB of headroom exists.
- [UNVERIFIED] Loop-task stack of 16 KB is enough. Fallback 24 KB; measure with [M6](../testing/measurements.md#m6).
- [UNVERIFIED] SE050 signing of a 214-byte message (patch P6, T=1 chaining). Fallback: `WALLET_FORCE_SOFTWARE_KEY 1`.
- [UNVERIFIED] `__has_include("local_config.h")` under the installed core's compiler. Fallback: commit an empty `local_config.h` and drop the `#if`.
- [UNVERIFIED] The header listings under `sdk-headers/` compile under the Arduino core. They pass `-std=c99` and `-std=c++17` syntax checks on the host.
