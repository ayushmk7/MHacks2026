# Build, flash, provision

From an empty laptop to four provisioned badges. macOS is shown; Linux differs only in device names.

**Status.** WP00 was run on 2026-10-03: the toolchain below built unmodified upstream (1,871,707 bytes, about 3 min 40 s for a clean build) and flashed it to a badge at `/dev/cu.usbserial-10`. What that showed:

- USB auto-reset into the bootloader works; no button press was needed.
- **Upload must use 460800 baud.** At the default 921600 the CH340 link fails after the baud change ("Unable to verify flash chip connection"). Use the FQBN with `,UploadSpeed=460800` for `arduino-cli upload` (`scripts/build.sh` does).
- The badge boots to `[os] ready` and answers `PING` with `OK pong`. Opening the serial port does not reset the badge.
- On this badge the SE050 answers its ATR but refuses the applet select (`[se050] link/select failed (no ack on write)`), so the identity is a **software key** (`[id] 5vpmgLuC, software (878 ms)`). Key generation plus one sign and one verify took 878 ms in total, so TweetNaCl is well under a second per operation here.
- The log showed `[i2c] bus is held low ... recovering` and `[btn] TCA9534 stopped answering ... re-probing` shortly after boot. This is finding F17 ([baseline](../architecture/upstream-baseline.md#findings-that-shape-the-design)): upstream's recovery does not release a held clock line, and the buttons stay dead until power is removed once.
- The filesystem already held 6 upstream sample apps.
- Python packages cannot be installed system-wide on this Mac (PEP 668): use the venv at `<repo>/.venv` (`.venv/bin/python`), which has `pyserial` and `esptool`.

WP01 to WP03 were run on the same badge on 2026-10-03, with the fork (all hooks, the `src/vk/` skeleton, the dev tools):

- `scripts/build.sh dev --upload /dev/cu.usbserial-10` works as written below: pre-flash checks, compile, upload at 460800 baud with the same `--build-path`, hard reset. Image 1,882,539 bytes.
- `vkdev.py` opens the port without resetting the badge, and its `reset` (an RTS pulse) reboots it; `[os] ready` comes about 8 s after the pulse.
- The boot log has `[vk] registries: services=0 commands=8 lua=0 status=0 domains=0 routes=0 permissions=0 patterns=0 native=0 config=1` and `[id] 5vpmgLuC, software (1 ms)`. With hook H21 it has no `[se050]` line and no `[i2c] scanning bus` line.
- The I²C clock line was still held low at this flash (`[btn] bus not idle at first probe - recovering`, `[btn] TCA9534 init FAILED (will keep re-probing)`, then `[i2c] bus is held low (error 2: NACK on address) - recovering`). H21 does not release a bus that is already held: **remove power once** (USB out, battery off, a few seconds). After that the expected lines are `[btn] TCA9534 init ok` and no later `stopped answering`.

Batch 2 (WP10, WP11, WP12, WP20, WP22) was flashed to the same badge on 2026-10-03, after it had been power-cycled:

- Image 1,928,819 bytes. A build after a source change takes 65 to 85 s, the upload about 35 s, and `[os] ready` comes about 8 s after the reset.
- **The I²C bus is healthy**: `[btn] TCA9534 @0x20 ready, input=0x3F`, `[btn] TCA9534 init ok`, and in a 200 s log no `stopped answering` and no `bus is held low`. The heartbeat line reads `[os] up 30s  heap 192KB  psram 7922KB  batt 100%  btn=0 int=H  …` (`btn=` is the pressed-key mask in hex; `--` would mean the expander is not answering). Presses on the real keys appear as `[btn] P3 DOWN down (raw=0x37)`.
- The boot log has `[id] 5vpmgLuC, software (1 ms)`, `[vk] selfcheck ok` and `[vk] registries: services=4 commands=16 lua=0 status=1 domains=1 routes=0 permissions=0 patterns=5 native=0 config=7`.
- `VKINFO` on the unprovisioned badge: `OK time=none wifi=0 provisioned=0 profile=dev api=2 pubkey=5vpmgLuCfbV7Lp2hTNFz7w75ibhVkc56G1mR6weQedvj key=software selfcheck=1`.
- The device tests leave the badge provisioned with the test values of [testing](../testing/testing.md) (`rpc_url`, `approval_tmo_s` 10, `hold_ms` 1000). `VKRESET` and a hold on the badge returns it to unprovisioned.

**Design change, 2026-10-03.** The firmware is named BadgeOS and its user interface is its own shell ([shell](../ui/shell.md)); this lands in Batch 5. The log lines quoted above were recorded before it: from Batch 5 on the banner reads `BadgeOS 0.1.0`, the `[vk] registries:` line has `pages=` in place of `status=`, there is no `[boot] splash` line, and boot is about 4 s shorter (the two splash screens took about 2 s each).

Batch 5 (WP37, WP40 to WP45) and the close-out were flashed to the same badge on 2026-10-04:

- The banner reads `BadgeOS 0.1.0`; the boot log has no `[boot] splash` line; `[os] ready` comes about 4.5 s after the reset (it was about 8 s with the two splashes).
- The registries line is `[vk] registries: services=8 commands=17 lua=49 domains=5 routes=3 permissions=9 patterns=6 native=3 config=18 pages=14` in the dev profile (`commands=9` in the release profile: the eight dev commands are absent).
- The network names changed with this flash: hostname `badgeos`, hotspot password `badgeos-setup`, ESP-NOW magic `BDOS`. **A badge still on an older build no longer hears this one over ESP-NOW.**
- Upstream's six sample apps were deleted from this badge's filesystem (`AUTH`, `LIST`, `DEL <id>`); a badge flashed from upstream firmware still has them until that is done.
- Images: dev 1,991,895 bytes (59.6 % of the 3,342,336-byte slot), release 1,983,131 bytes (59.3 %). The release profile compiled at the first attempt and passes pre-flash check 6; on the badge `VKINFO` says `profile=release` and every dev command (`VKSTATE`, `VKBTN`, `VKSHOT`, `VKTIME`, `VKPAIR`, `VKNOTE`, `VKDEMOAPPROVE`, `VKPERF`) is answered by upstream's push protocol with `ERR not authorised - send AUTH <code>`, never `OK` (T-REL2). The badge was then flashed with the dev profile again.
- The I²C bus stayed healthy through every flash and the whole regression: `[btn] TCA9534 init ok`, heartbeat `btn=0 int=H`, no `stopped answering` and no `held low` line in any test log.
- `scripts/push-apps.sh --port … dev` takes about six minutes for the twelve app folders (each carries its own 55 KB copy of `vk.lua`). One run lost its push session part-way through two apps (`ERR not authorised` on a `DATA` line; pushing those two again worked). The cause was not found; if it happens, push the named apps again.

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

Then work package WP01 adds `src/vk/` and applies the hooks, and WP37 replaces upstream's shell, boot splash and names ([hooks and replaced files](../architecture/upstream-hooks.md)). Credit stays in the repository: `os/README.md` says "BadgeOS is built on Solana OS by spacemandev." The upstream repository has no licence file (finding F9): keep the repository private or ask the author before publishing the fork.

## Build profiles

Two profiles; the only difference is the generated header `src/vk/vk_profile.h`.

| Profile | `VK_PROFILE_DEV` | Has | Flash on |
|---|---|---|---|
| `dev` | 1 | "DEV BUILD" banner, hold-to-sign on unverified screens, serial test hooks (screen dump, button injection, pairing code) | development badges only |
| `release` | 0 | none of that | judge badges, the demo |

```bash
cd os
scripts/build.sh dev                                   # compile only
scripts/build.sh dev --upload /dev/cu.usbserial-10     # compile, pre-flash checks, upload
scripts/build.sh release --upload /dev/cu.usbserial-10
```

`scripts/build.sh <dev|release> [--upload <port>]` does, in order: write `src/vk/vk_profile.h`; run `scripts/preflash-check.sh <profile>`; `arduino-cli compile` with the sketch path and `--build-path` given under [Build directory](#build-directory); if asked, `arduino-cli upload` with `$UPLOAD_FQBN`. It prints the image size; record it in [testing](../testing/testing.md#measurements).

### Build directory

The sketch is compiled in place, from `os/`, with one build directory per profile inside it. Checked on 2026-10-03 with unmodified upstream in `os/` (the repository path contains a space):

```bash
FW="<repo>/os"                                   # absolute; always quoted, the path has a space
arduino-cli compile --fqbn "$FQBN" --build-path "$FW/build/<profile>" "$FW"
arduino-cli upload  --fqbn "$UPLOAD_FQBN" --build-path "$FW/build/<profile>" -p <port> "$FW"
```

- **Sketch path:** the absolute path of `os/` (`"$FW"`), not `.`, so the script works from any directory. Its main file is `os.ino`.
- **Build path:** `os/build/dev` for the dev profile and `os/build/release` for the release profile. Two directories, so switching profile never reuses the other profile's objects, and a release upload can never send a dev image. `arduino-cli` accepts a build path inside the sketch folder: it compiles only the sketch's top-level files and `src/`, so `build/` is not picked up.
- **No symlink is needed.** The space in the repository path breaks neither the compile nor the link; quoting the two paths is enough.
- **Upload** must use the FQBN with `,UploadSpeed=460800` (`$UPLOAD_FQBN`) and the same `--build-path`, so it sends the image that was just built. At the default 921600 baud the upload fails on this badge's CH340. Both lines were run as written in WP01 (2026-10-03): the upload sends `build/<profile>/os.ino.bin` and resets the badge.
- `build/` is ignored by git (upstream's `os/.gitignore` and the repository's `.gitignore` both list it). Never commit it.
- Measured in WP01, with `src/vk/` added: a build after source changes takes 65 to 85 s (most of the sketch is recompiled), a rebuild with nothing changed about 20 s including the host tests of check 5, and the upload 35 s (31 s of it writing 1.88 MB at 460800 baud). The line `Maximum is 16777216 bytes` in `arduino-cli`'s size summary is the flash size, not the application slot, which is 3,342,336 bytes.
- Measured in WP00, unmodified upstream: a first build into an empty build directory took 49 s (the ESP32 core itself was already in `arduino-cli`'s cache from WP00; with a cold cache expect about 4 minutes), an unchanged rebuild 14 s. Image: 1,871,707 bytes (11 % of the 16 MB flash), globals 76,984 bytes; identical to the WP00 build from the scratch path.

## Pre-flash checks

`scripts/preflash-check.sh <profile>` exits non-zero, and the build stops, if any check fails:

| # | Check | Meaning of a failure |
|---|---|---|
| 1 | hook ids found in upstream files equal the hook table in `UPSTREAM-HOOKS.md`; every path in its replaced-files table marked `deleted` does not exist, and every path marked `rewritten` or `edited` exists | an untracked edit to an upstream file; a replaced upstream file that came back (a merge from upstream restored `src/ui/shell.cpp`, the splash images or a sample app) |
| 2 | `identity::sign`, `signBase64`, `se050_apdu::signEd25519`, `ed25519::sign`, `crypto_sign(`, `crypto_ed25519_sign(`, `crypto_eddsa_sign(` appear only in `src/identity/`, `src/hal/se050_apdu.cpp`, `src/vk/wallet/signer.cpp` (the vendored Monocypher in `src/vk/wallet/vendor/`, which defines the last two, is not searched) | something can sign around the wallet core |
| 3 | `VK_SIGN_DOMAIN(` appears only in `src/vk/features/*/domain_*.cpp` | a signing domain defined outside a feature's domain file |
| 4 | no file under `src/native_apps/` or `src/vk/features/` includes anything from `src/identity/` (the badge's own key comes from `vk::wallet::publicKey()`) | a feature or native app reaching for the key |
| 5 | `test/host/run.sh` passes | the pure code changed behaviour |
| 6 | release only: `vk_profile.h` defines `VK_PROFILE_DEV 0` | a dev build about to go on a judge badge |
| 7 | `python3 scripts/check-names.py` prints nothing: no text a user or the network can see says "Solana" or "SKYRIZZ", apart from the blockchain names and the two upstream protocol texts listed under [Names that stay](../architecture/upstream-hooks.md#names-that-stay) | upstream's brand is about to appear on a screen, a web page or the network |

How the script reads the table:

- Check 1 runs the grep in [upstream-hooks](../architecture/upstream-hooks.md#checking-the-hooks). A row that names a range (H8: H8a–H8f) stands for those ids; a row whose purpose starts with `optional:` (H18) may be absent from the source. Its second half reads the replaced-files table of `UPSTREAM-HOOKS.md`: the first cell of a row holds one or more paths in backquotes, the second cell the kind.
- Check 7 is `scripts/check-names.py` (Python 3, standard library). It searches string literals only, with every comment removed first (`//` and `/* … */` in C and C++, `--` and `--[[ ]]` in Lua): the compiled sources (`os.ino`, `src/`, which includes the embedded web page), the Lua apps and library (`apps/`, `lib/`) with their `app.ini` files, the tools (`tools/*.py`, `scripts/*.py`), and `README.md` whole. Comments and identifiers are not searched, because no user sees them. `python3 scripts/check-names.py --list` prints every match with `kept` or `FAIL`. After a build, the same question is asked of the image: `strings -a build/<profile>/os.ino.bin | grep -iE 'solana|skyrizz' | sort -u` must list only the six names given in [upstream-hooks](../architecture/upstream-hooks.md#checking-the-hooks).
- Checks 2 and 3 scan `os.ino` and `src/` only (not `test/`), and ignore text after `//` on a line, so a comment may name these calls. Check 3 also skips the one line that defines the macro, `#define VK_SIGN_DOMAIN(` in `src/vk/wallet/signer.h`.
- Check 5 can be skipped with `VK_PREFLASH_SKIP_HOST_TESTS=1` when running the script by hand; `scripts/build.sh` never sets it.

## Installing apps

Upstream's push writes only under `/apps/<id>/`, so the shared library is copied into each app at push time.

```bash
scripts/push-apps.sh --port /dev/cu.usbserial-10 dev        # every app, over USB serial
scripts/push-apps.sh --host 192.168.4.31 --token 123456 release   # over Wi-Fi, without the dev-only test apps
```

For each app folder under `apps/` the script: copies `lib/vk.lua` into a temporary copy of the folder; for `evilgame`, also copies `apps/game/*.lua` except `config.lua`; then pushes it, over serial with `vkdev.py push` (upstream's `AUTH`/`BEGIN`/`DATA`/`END` line protocol) or over Wi-Fi with `tools/badge-push.py --id <id>` (the tool otherwise takes the id from the folder name, which is a temporary one here). `apps/` holds only BadgeOS's apps: upstream's six samples are deleted. The `release` set leaves out the dev-only test apps `signtest`, `checktest`, `vktest` and `reqtest`; it **includes** `evilgame`, which the demo needs.

The pairing code is on the badge under Settings → App push. In the dev profile `vkdev.py` reads it itself (`VKPAIR`). A release build has no `VKPAIR`, so over serial it needs the code too: `--token <code>` is accepted with `--port` and passed to `vkdev.py --code`. With `--host` the code may also come from the environment variable `BADGE_TOKEN`. `--dry-run` lists what would be pushed and pushes nothing. The script exits 0 when every app was pushed, 1 when any push failed, 2 for a usage error.

`vk.lua` (55 KB) goes into every app folder, whether or not the app requires it. `vkdev.py push` sends every file of a folder.

### Names on the network

| What | Value | Where it comes from |
|---|---|---|
| default hostname | `badgeos` (so `badge-push.py --host badgeos.local` where upstream's examples say `solana-badge.local`) | `DEFAULT_HOSTNAME`, hook H23 |
| the badge's own hotspot (Settings → Wi-Fi → Start hotspot), default password | `badgeos-setup` | `DEFAULT_AP_PASSWORD`, hook H23 |
| device name shown to other badges and over BLE | `badge-XXXX` | upstream's default, unchanged |
| ESP-NOW magic | `BDOS` | hook H23. Badges running upstream firmware are not heard |
| web page served on port 80 | titled `BadgeOS`, Receipt colours | `src/net/push_server.cpp`, a replaced file |
| app-store broker | none: the client is off until an address is set from the web page | `DEFAULT_BROKER_URL` is empty, hook H23 |

A badge that stored its own hostname or hotspot password under upstream firmware keeps the stored value; the table gives the defaults. Settings → App push shows the address to use.

## Provisioning

After flashing, from the repository root, on any laptop (the dashboard does not have to be set up on it):

```bash
# 1. check: prints every value, its source and the lines that would be sent; opens no serial port
python3 os/scripts/vkdev.py provision --dry-run \
    --listener http://<backend laptop hotspot IP>:8788 --wifi "<hotspot SSID>" "<password>" \
    --badge-id 1 --label "Merchant" --badges-json badges.provisioned.json

# 2. the same command with the port and without --dry-run, once per badge
python3 os/scripts/vkdev.py --port /dev/cu.usbserial-10 provision \
    --listener http://<backend laptop hotspot IP>:8788 --wifi "<hotspot SSID>" "<password>" \
    --badge-id 1 --label "Merchant" --badges-json badges.provisioned.json
```

Where the values come from:

| Values | Kind | Source |
|---|---|---|
| issuer public key, mint, decimals, symbol, cap, max, RPC URL | pinned devnet values (final), the same for every badge | `os/provision.public.env`, committed, public values only. A flag (`--issuer --mint --decimals --symbol --cap --max --rpc`) overrides a line |
| listener address, Wi-Fi SSID and password | per venue | `--listener` and `--wifi`, every run |
| badge id and label | per badge | `--badge-id`, `--label` |

The pinned values themselves are listed in [config](../platform/config.md#pinned-values). The tool no longer reads `dashboard/.env` by itself: it does only when the file is named with `--env dashboard/.env`, takes only the public names from it, and stops if it disagrees with `os/provision.public.env`. **The issuer's secret keypair (`dashboard/server/.keys/authority.json` on the backend laptop) never leaves that laptop and is never read by the tool**: the badge needs the issuer's public key, which is in `os/provision.public.env`, and `vkdev.py` has no code that opens a keypair file (`vkdev.py --selftest` checks this). Never copy that file, or the backend's `.env`, to the laptop that provisions.

The run ends with one line of JSON, ready to copy: the badge's entry for `dashboard/server/config/badges.json`. With `--badges-json <file>` the entry is also written into that file, so after the last badge there is one file to send to the backend owner.

Details, the checks made before a badge is touched, and the by-hand commands: [../platform/config.md](../platform/config.md#provisioning). `--autostart home` (upstream's autostart setting, `VKAUTOSTART`) still works: the Home app then starts at boot, and CANCEL in it returns to the launcher. Without it the badge boots to the launcher.

## Before a demo

Values nothing fills in automatically. Each must be set by hand before the demo, or the feature it belongs to fails quietly:

| What | Where | Until it is set |
|---|---|---|
| the shop's address: `REPLACE_WITH_SHOP_ADDRESS` | `os/apps/game/config.lua` **and** `os/apps/evilgame/config.lua`, field `shop.recipient` (the merchant badge's public key, base58) | every purchase in Game and Evil game ends with "the shop is not set up"; no approval opens |
| the attacker's token account: `REPLACE_WITH_ATTACKER_TOKEN_ACCOUNT` | `os/apps/evilgame/config.lua`, field `evil_recipient` (a **token account**, not a wallet address: it is passed to `vk.pay` as `destination`) | the Evil game's WRONG RECIPIENT demo cannot run |
| the issuer public key, the mint, decimals, symbol, cap, max and the RPC URL | pinned devnet values (final) in `os/provision.public.env`; `vkdev.py provision` reads them from there. Nothing to set by hand, but they must equal what the backend uses: on the backend laptop, `HACK_MINT`, `HACK_SYMBOL`, `HACK_DECIMALS` and `RPC_URL` in `dashboard/.env`, and the public half of `AUTHORITY_KEYPAIR`. The keypair file itself stays on that laptop | a badge provisioned with other values rejects every registry record (`issuer_key`) or pays in a token the backend does not watch (`tokens`) |
| the hotspot's SSID and password (per venue) | arguments of `vkdev.py provision --wifi "<SSID>" "<password>"` (or `VKWIFI`), per badge | the badge has no network: no clock (amber `CLOCK UNSYNCED`), no balance, no payment |
| the listener address (per venue) | `--listener http://<backend laptop hotspot IP>:8788` of the same command (config key `listener_url`) | no registry record: every payee is red `UNVERIFIED RECIPIENT` |
| the public keys of the four badges | the line of JSON each provisioning run ends with (or the file written with `--badges-json`) goes into `dashboard/server/config/badges.json` on the backend laptop, then `npm run devnet:setup` again to fund them | the badges hold no tokens |
| the repository link: config key `repo_url` | `VKSET repo_url https://github.com/ayushmk7/MHacks2026` on each badge (a plain key: no confirmation, and it can be set after provisioning). `VKRESET` erases it with the rest of the config, and the provisioning tool does not set it | Settings → About says `no link set` and Home shows its barcode instead of the QR code ([shell](../ui/shell.md#about)) |

After editing a `config.lua`, push the apps again (`scripts/push-apps.sh … release`). A badge provisioned by the device tests carries the **test** values (`rpc_url` `http://127.0.0.1:8899`, the test issuer key, `hold_ms` 1000, `approval_tmo_s` 10): reset it (`VKRESET`, hold SELECT) and provision it for real before a demo.

## Four badges

For each badge, with its own port:

1. `scripts/build.sh release --upload <port>`
2. `vkdev.py --port <port> provision --listener ... --wifi ... --badge-id <n> --label "<label>" --badges-json badges.provisioned.json` ([Provisioning](#provisioning))
3. Put the printed entry (public key, key location, `standIn: false`) into `dashboard/server/config/badges.json` on the backend laptop, in place of the stand-in entry with the same id, and restart the dashboard server. After the last badge, `badges.provisioned.json` holds all four entries.
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
