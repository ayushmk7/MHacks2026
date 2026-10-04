# Build, flash, provision

From an empty laptop to four provisioned badges. macOS is shown; Linux differs only in device names.

**Status.** WP00 was run on 2026-10-03: the toolchain below built unmodified upstream (1,871,707 bytes, about 3 min 40 s for a clean build) and flashed it to a badge at `/dev/cu.usbserial-10`. What that showed:

- USB auto-reset into the bootloader works; no button press was needed.
- **Upload must use 460800 baud.** At the default 921600 the CH340 link fails after the baud change ("Unable to verify flash chip connection"). Use the FQBN with `,UploadSpeed=460800` for `arduino-cli upload` (`scripts/build.sh` does).
- The badge boots to `[os] ready` and answers `PING` with `OK pong`. Opening the serial port does not reset the badge.
- On this badge the SE050 answers its ATR but refuses the applet select (`[se050] link/select failed (no ack on write)`), so the identity is a **software key** (`[id] 5vpmgLuC, software (878 ms)`). Key generation plus one sign and one verify took 878 ms in total, so TweetNaCl is well under a second per operation here.
- The log showed `[i2c] bus is held low ... recovering` and `[btn] TCA9534 stopped answering ... re-probing` shortly after boot; upstream re-probes by itself. Watch for it if buttons seem dead.
- The filesystem already held 6 upstream sample apps.
- Python packages cannot be installed system-wide on this Mac (PEP 668): use the venv at `<repo>/.venv` (`.venv/bin/python`), which has `pyserial` and `esptool`.

## Toolchain

```bash
brew install arduino-cli cmake
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32            # must be 3.x
arduino-cli lib install LovyanGFX
python3 -m venv .venv && .venv/bin/pip install pyserial esptool      # from the repository root
```

Record what built, in this table, the first time it works:

| Tool | Version | Date |
|---|---|---|
| `arduino-cli` | 1.5.1 | 2026-10-03 |
| `esp32:esp32` core | 3.3.12 | 2026-10-03 |
| LovyanGFX | 1.2.32 | 2026-10-03 |

Core 3.3.12 builds upstream cleanly.

The board settings are carried by one name, used in every command:

```bash
FQBN="esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom,CDCOnBoot=cdc"
UPLOAD_FQBN="$FQBN,UploadSpeed=460800"     # for arduino-cli upload
```

## The serial port

The badge appears as `/dev/cu.usbserial-*`. Only one program can hold it: close any serial monitor before flashing or running `vkdev.py`.

```bash
arduino-cli board list
```

If an upload cannot connect: hold `BOOT1`, tap `RST1`, release `BOOT1`, upload again. This is the one step that needs hands.

## Build upstream first (WP00)

Proves the toolchain, the cable and the badge with firmware known to boot, before any of our code exists.

```bash
git clone https://github.com/spacemandev-git/solana-defcon-badge-26 /tmp/upstream
git -C /tmp/upstream checkout 812b8c7aca5c366d18c0b040fafd2999f7204d84
cd /tmp/upstream/firmware/solana-os
arduino-cli compile --fqbn "$FQBN" .
arduino-cli upload  --fqbn "$UPLOAD_FQBN" -p /dev/cu.usbserial-10 .
arduino-cli monitor -p /dev/cu.usbserial-10 -c baudrate=115200
```

Expected in the log: the banner, `[lcd] ready 320x240`, an `[id]` line naming `secure element` or `software` (it ends with a time in ms), and `[os] ready`. Type `PING`; the badge answers `OK pong`. Record the `[id]` line: it says where this badge's key lives.

## The fork (WP01)

```bash
# from the repository root
cp -R /tmp/upstream/firmware/solana-os os
mv os/solana-os.ino os/os.ino        # an Arduino sketch's main file must be named after its folder
```

Then work package WP01 adds `src/vk/` and applies the hooks ([hooks](../architecture/upstream-hooks.md)). The upstream repository has no licence file (finding F9): keep the repository private or ask the author before publishing the fork.

## Build profiles

Two profiles; the only difference is the generated header `src/vk/vk_profile.h`.

| Profile | `VK_PROFILE_DEV` | Has | Flash on |
|---|---|---|---|
| `dev` | 1 | "DEV BUILD" banner, hold-to-sign on unverified and no-clock screens, serial test hooks (screen dump, button injection, pairing code) | development badges only |
| `release` | 0 | none of that | judge badges, the demo |

```bash
cd os
scripts/build.sh dev                                   # compile only
scripts/build.sh dev --upload /dev/cu.usbserial-10     # compile, pre-flash checks, upload
scripts/build.sh release --upload /dev/cu.usbserial-10
```

`scripts/build.sh <dev|release> [--upload <port>]` does, in order: write `src/vk/vk_profile.h`; run `scripts/preflash-check.sh <profile>`; `arduino-cli compile --fqbn "$FQBN" .`; if asked, `arduino-cli upload`. It prints the image size; record it in [testing](../testing/testing.md#measurements).

## Pre-flash checks

`scripts/preflash-check.sh <profile>` exits non-zero, and the build stops, if any check fails:

| # | Check | Meaning of a failure |
|---|---|---|
| 1 | hook ids found in upstream files equal the table in `UPSTREAM-HOOKS.md` | an untracked edit to an upstream file |
| 2 | `identity::sign`, `signBase64`, `se050_apdu::signEd25519`, `ed25519::sign`, `crypto_sign(` appear only in `src/identity/`, `src/hal/se050_apdu.cpp`, `src/vk/wallet/signer.cpp` | something can sign around the wallet core |
| 3 | `VK_SIGN_DOMAIN(` appears only in `src/vk/features/*/domain_*.cpp` | a signing domain defined outside a feature's domain file |
| 4 | no file under `src/native_apps/` or `src/vk/features/` includes anything from `src/identity/` (the badge's own key comes from `vk::wallet::publicKey()`) | a feature or native app reaching for the key |
| 5 | `test/host/run.sh` passes | the pure code changed behaviour |
| 6 | release only: `vk_profile.h` defines `VK_PROFILE_DEV 0` | a dev build about to go on a judge badge |

## Installing apps

Upstream's push writes only under `/apps/<id>/`, so the shared library is copied into each app at push time.

```bash
scripts/push-apps.sh --port /dev/cu.usbserial-10 dev        # every app, over USB serial
scripts/push-apps.sh --host 192.168.4.31 --token 123456 release   # over Wi-Fi, without the dev-only test apps
```

For each app folder under `apps/` the script: copies `lib/vk.lua` into a temporary copy of the folder; for `evilgame`, also copies `apps/game/*.lua` except `config.lua`; then pushes it, over serial with `vkdev.py push` (upstream's `AUTH`/`BEGIN`/`DATA`/`END` line protocol) or over Wi-Fi with upstream's `tools/badge-push.py --id <id>` (the tool otherwise takes the id from the folder name, which is a temporary one here). The `release` set leaves out the dev-only test apps `signtest`, `checktest`, `vktest` and `reqtest`; it **includes** `evilgame`, which the demo needs.

The pairing code is on the badge under Settings → Push. In the dev profile `vkdev.py` reads it itself (`VKPAIR`).

## Provisioning

After flashing and after `npm run devnet:setup` in `dashboard/` ([backend](../integration/backend.md)):

```bash
python3 os/scripts/vkdev.py --port /dev/cu.usbserial-10 provision \
    --env dashboard/.env --listener http://<laptop hotspot IP>:8788 \
    --wifi "<hotspot SSID>" "<password>" --cap 100.00 --max 1000.00
```

Details and the by-hand commands: [../platform/config.md](../platform/config.md#provisioning). The command ends by printing the badge's public key and key location.

## Four badges

For each badge, with its own port:

1. `scripts/build.sh release --upload <port>`
2. `vkdev.py --port <port> provision ...`
3. Put the printed public key and key location into `dashboard/server/config/badges.json` (`standIn: false`), restart the dashboard server.
4. `npm run devnet:setup` in `dashboard/` (funds each badge: SOL, token account, tokens).
5. `scripts/push-apps.sh --port <port> release`
6. Issue the merchant's attestation on the dashboard's Registry page.
7. Label the badge to match `badges.json`: Merchant, Impostor, Judge A, Judge B.

| Label | Port | Public key | Key location | Profile flashed |
|---|---|---|---|---|
| Merchant | | | | |
| Impostor | | | | |
| Judge A | | | | |
| Judge B | | | | |

Reflashing the application does not touch NVS, the SE050 or the filesystem: the key, the config, the installed apps, the history and the contacts all survive. `esptool.py erase_flash` destroys all of them, including a software key.

## SE050 fallback

If a badge's key is in the SE050 and signing a transfer fails (`[id] SE050 refused to sign`, or T-SE1 in [testing](../testing/testing.md#acceptance-tests) fails):

1. Set `VK_FORCE_SOFTWARE_KEY 1` in `src/vk/vk_build.h` and apply hook H18.
2. Rebuild and flash that badge.
3. On the badge: Settings → Identity → New identity. The new key is a software key.
4. Update `badges.json` with the new public key, re-run `devnet:setup`, re-issue the attestation.
5. The badge now reports `software` everywhere. Do not claim secure-element protection for it.

## Resetting

| To reset | Do | Loses |
|---|---|---|
| wallet config | `VKRESET` over USB, or Wallet → Reset, then confirm on the badge | config, consent |
| the device key | Settings → Identity → New identity | the old address, its funds and its attestation |
| everything | `esptool.py --chip esp32s3 erase_flash`, then flash and provision again | everything |
