# Flash BadgeOS on another badge

How to put BadgeOS on a Solana DEF CON badge that is not the development badge: a badge still running the stock firmware, or a teammate's badge. For the toolchain details, the build profiles and provisioning in depth, see [build, flash, provision](build-flash-provision.md); this page is the short path, start to finish.

**Status of this procedure.** Every step here has been run on one badge (the development badge, a CH340 serial port, a software key). No second badge has been flashed yet, so anything that depends on what a stock badge already holds (its stored key, its installed apps) is described from the code, not from a run. Those places are marked "not yet verified on a second badge".

## What flashing changes and what it keeps

The badge's flash holds four things. Flashing the firmware replaces only the first.

| Part | Flashing BadgeOS | Notes |
|---|---|---|
| Firmware (bootloader, partition table, application) | replaced | the partition table is the same as the stock firmware's |
| Settings and the software key (NVS, at `0x9000`) | kept | a badge keeps its identity and address across flashes |
| Installed apps and wallet files (LittleFS) | kept | apps the stock firmware installed are still there afterwards |
| Secure element (SE050) | never touched | BadgeOS does not address it at all ([hook H21](../architecture/upstream-hooks.md)) |

Two consequences for a badge coming from the stock firmware (not yet verified on a second badge):

- **Old apps.** The stock firmware's sample apps stay on the filesystem and will be listed on the launcher. Delete each one on the badge (select it on the launcher and hold RIGHT until the delete confirmation opens), or erase the whole flash first ([A clean start](#a-clean-start)).
- **The key.** BadgeOS signs with a software key and never talks to the secure element. If the stock firmware created the badge's key inside the secure element, BadgeOS cannot sign with it. After flashing, open Settings → Identity: if "key lives in" does not say `software`, create a new identity there (the badge gets a new address) or erase the flash first.

## What you need

- The badge, a USB-C data cable, and the badge's power switch within reach.
- macOS or Linux with the repository checked out on branch `badge-os`.
- Either the full toolchain ([Toolchain](build-flash-provision.md#toolchain)), or only Python and `esptool` plus the four binary files from someone who has the toolchain ([Without the toolchain](#without-the-toolchain)).

```bash
# from the repository root, once
python3 -m venv .venv && .venv/bin/pip install pyserial esptool
```

## 1. Find the port

Plug the badge in and switch it on.

```bash
arduino-cli board list        # or: ls /dev/cu.usbserial-*
```

The badge is `/dev/cu.usbserial-<something>` (`/dev/ttyUSB<n>` on Linux). With several badges plugged in, each has its own port: unplug one and list again to see which is which. Only one program can hold a port: close every serial monitor first.

## 2. Flash

Pick a profile. `release` is what goes on any badge that is not a development badge: it has no test commands (no button injection, no screen dump). `dev` is for a badge you run the test scripts against.

### With the toolchain

```bash
cd os
scripts/build.sh release --upload /dev/cu.usbserial-XX
```

The script runs the pre-flash checks, builds into `build/release/` and uploads at 460800 baud (the default 921600 fails on the badge's serial chip). The first build takes several minutes; later ones are incremental.

### Without the toolchain

Someone with the toolchain runs `scripts/build.sh release` once and hands over these files from `os/build/release/`:

```
os.ino.bootloader.bin   os.ino.partitions.bin   boot_app0.bin   os.ino.bin   flash_args
```

Then, in the folder that holds them:

```bash
/path/to/repo/.venv/bin/esptool.py --chip esp32s3 --port /dev/cu.usbserial-XX --baud 460800 \
    write_flash @flash_args
```

`flash_args` carries the flash mode and the four offsets (`0x0`, `0x8000`, `0xe000`, `0x10000`).

**Do not flash `os.ino.merged.bin`.** It is one image from offset 0 with padding between the parts, and the padding covers the settings area at `0x9000`: flashing it erases the badge's key and configuration.

### If the upload cannot connect

Hold `BOOT1`, tap `RST1`, release `BOOT1`, and run the command again.

## 3. Power-cycle the badge once

After flashing, remove all power: **unplug USB, switch the badge off, wait a few seconds, switch it on, plug USB back in.**

This is not optional. A reset over serial (which every flash ends with) can leave the badge's I²C clock line held low, and then no button works under any firmware until power is removed ([the log of that fault](../../logs/BADGE-BUTTONS-I2C.md)). The first boot after a full power cycle is the one to judge.

The badge should show the BadgeOS boot screen and then the launcher (title `MENU`). Press a direction key: the selection must move.

## 4. Install the apps

The firmware carries the shell, the settings and the native apps. The Lua apps (Pay, Request, History, Contacts, the games, …) are files on the badge's filesystem and are pushed separately:

```bash
cd os
scripts/push-apps.sh --port /dev/cu.usbserial-XX --token <code> release
```

`<code>` is the pairing code the badge shows under Settings → App push (a release build does not hand it out over serial). On a `dev` build, leave `--token` out and use `dev` as the last word to install the test apps too.

Over Wi-Fi instead, with the badge's address and code from the same page:

```bash
scripts/push-apps.sh --host <badge address> --token <code> release
```

## 5. Provision the wallet

A badge that has never been provisioned shows `SETUP NEEDED` on the launcher. The wallet values (the issuer's public key, the token, the RPC and listener addresses) come from the laptop, once per badge:

```bash
# from the repository root; add --dry-run first to see exactly what will be sent (no port needed)
.venv/bin/python os/scripts/vkdev.py --port /dev/cu.usbserial-XX provision \
    --listener http://<backend laptop address on the hotspot>:8788 \
    --wifi "<SSID>" "<password>" \
    --badge-id <n> --label "<label>" \
    --badges-json badges.provisioned.json
```

The pinned public values are read from `os/provision.public.env`; nothing secret is needed on this laptop ([Provisioning](build-flash-provision.md#provisioning), [config](../platform/config.md)). The last line printed is the badge's entry for the backend's `badges.json`; `--badges-json` also collects the entries of every badge you provision into one file to send to the backend owner.

Wi-Fi can also be set on the badge itself: Settings → Wi-Fi → Scan, pick the network, type the password with the on-screen keyboard ([text entry](../ui/text-entry.md)).

A badge that was provisioned before (for example with test values) refuses new secure values until it is reset: Settings → Wallet → Reset, confirmed with a hold on the badge, then provision again.

## 6. Check it

On the badge:

1. Launcher → `TESTS` → `RUN ALL`. Every automatic row should read `OK`; rows that need a person, the network or a second badge read `--`. A `FAIL` names the part ([apps: Self test](../apps/apps.md#self-test)).
2. Settings → Identity: the address, and "key lives in".
3. Settings → About: the version; the QR code opens the repository.
4. The header's top-right shows the time once the badge has joined Wi-Fi, and the measured battery.

From the laptop (any profile):

```bash
.venv/bin/python os/scripts/vkdev.py --port /dev/cu.usbserial-XX info     # prints VKINFO: version, profile, address, provisioned, wifi
```

## Several badges

Each badge needs steps 2 to 6 with its own port, its own `--badge-id` and label, and its own power cycle:

```bash
cd os
for port in /dev/cu.usbserial-10 /dev/cu.usbserial-20; do
  scripts/build.sh release --upload "$port"   # the first pass compiles; later ones reuse the build and only run the checks and upload
done
```

Then power-cycle each badge, push the apps and provision each one. Keep a table of which label is on which badge ([Four badges](build-flash-provision.md#four-badges)); the backend owner needs each badge's public key in `badges.json` and funds it with `npm run devnet:setup`.

## A clean start

To remove everything the stock firmware left (its apps, its settings, a software key):

```bash
.venv/bin/esptool.py --chip esp32s3 --port /dev/cu.usbserial-XX erase_flash
```

then flash, power-cycle, push the apps and provision. **This destroys the badge's software key**: the badge gets a new address at its next boot, and anything the old address held is lost. Do not do it to a badge whose address is already funded or registered unless that is what you want.

## Going back to the stock firmware

Flash the stock firmware's own build the same way (its repository's instructions, same board settings). Settings and installed files survive in both directions; the wallet files BadgeOS wrote under `/vk/` are simply ignored by the stock firmware.

## When something is wrong

| What you see | Cause | Do |
|---|---|---|
| Upload stops with "Unable to verify flash chip connection" | baud rate too high for the serial chip | use `scripts/build.sh` (it uploads at 460800), or `--baud 460800` with `esptool` |
| Upload cannot connect at all | the chip is not in download mode, or the port is held | close serial monitors; hold `BOOT1`, tap `RST1`, release `BOOT1`, retry |
| Screen stays black right after a flash | the flash is still running, or the badge has not restarted | wait for "Hard resetting", then power-cycle |
| The launcher shows, no button does anything | I²C clock line held low after a warm reset | full power cycle: unplug USB, switch off, wait, switch on, plug in |
| `SETUP NEEDED` on the launcher | not provisioned | step 5 |
| The launcher lists only a few apps | the Lua apps are not installed | step 4 |
| The launcher lists apps you do not know | left by the previous firmware | delete them on the badge (hold RIGHT on the app), or [a clean start](#a-clean-start) |
| `push-apps.sh` stops with `ERR not authorised` | the pairing code is missing or wrong (release build), or the session was dropped | pass `--token <code>` from Settings → App push; run the command again for the apps it lists as failed |
| Every payment is red `UNVERIFIED RECIPIENT` | the badge cannot fetch the payee's registry record | check Wi-Fi, `listener_url`, and that the backend is running on the hotspot |
| The time is missing from the header | no Wi-Fi yet, so no time sync | join Wi-Fi (Settings → Wi-Fi) |
