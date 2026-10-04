# BadgeOS

Firmware for the ESP32-S3 badge. BadgeOS runs apps written in Lua or C++, and it is a wallet:
the badge holds one signing key, and every payment is approved by its owner on a screen that
belongs to the firmware, which apps cannot draw over or skip.

What is in it:

- a shell in the Receipt design: boot screen, launcher, settings and dialogs;
- a wallet core, the only code that can sign with the badge key, and an approval screen;
- checks the firmware makes itself before a payment: who is being paid, that they asked for
  exactly this, and that they are here now;
- an app platform: Lua apps and native C++ apps, permissions with first-run consent, an
  ESP-NOW router, notifications;
- apps: Home, Pay, Request, History, Contacts, Dice, a game with a shop, a duel with a stake.

BadgeOS is built on Solana OS by spacemandev.

## What is in this folder

| Path | What it is |
|---|---|
| `os.ino`, `partitions.csv`, `src/` | the Arduino sketch. `src/vk/` is BadgeOS ([map](src/vk/README.md)), `src/native_apps/` the native apps ([list](src/native_apps/README.md)); every other folder under `src/` is upstream, changed only through tagged hooks |
| [`apps/`](apps/README.md) | the Lua apps, one folder each, and their manifest keys |
| [`lib/`](lib/README.md) | `vk.lua`, the shared Lua library copied into every app |
| [`scripts/`](scripts/README.md) | build, flash (one badge or a fleet), push apps, provision, serial tool, pre-flash checks, new app |
| [`test/`](test/README.md) | host tests (laptop) and device tests (badge over USB) |
| [`templates/`](templates/README.md) | what `scripts/new-app.sh` copies for a new Lua or native app |
| `tools/` | upstream's push tool and broker-CA script ([scripts](scripts/README.md#ostools)) |
| `provision.public.env` | the pinned public provisioning values (issuer key, HACK mint, limits, RPC); no secrets |
| `UPSTREAM-HOOKS.md` | copy of the hook and replaced-files tables, read by `scripts/preflash-check.sh` |
| `BRIDGE_PROTOCOL.md` | upstream's wire protocol for the HTTP-over-BLE bridge (a phone gives the badge internet over Bluetooth) |
| `build/` | build output, one folder per profile; not in git |

## Quick start

```bash
cd os
scripts/build.sh dev --upload /dev/cu.usbserial-XX        # one badge, dev profile, firmware and apps
scripts/fleet.sh release <port> <port> <port> <port>       # several badges, identical launchers
test/host/run.sh                                           # host tests, no badge
```

Then power-cycle each badge once and provision it with `scripts/vkdev.py provision`
([flash another badge](../docs/os/guides/flash-another-badge.md)).

## Documentation

Everything is specified in [`docs/os/`](../docs/os/README.md): start with its README.

- Flash a badge, the short path: [`docs/os/guides/flash-another-badge.md`](../docs/os/guides/flash-another-badge.md)
- Build, flash and provision in depth: [`docs/os/guides/build-flash-provision.md`](../docs/os/guides/build-flash-provision.md)
- Every change made to an upstream file: [`docs/os/architecture/upstream-hooks.md`](../docs/os/architecture/upstream-hooks.md)
  (copied in [`UPSTREAM-HOOKS.md`](UPSTREAM-HOOKS.md))
- The Lua API BadgeOS adds: [`docs/os/platform/lua-api.md`](../docs/os/platform/lua-api.md)
- Naming and documentation conventions: [`docs/os/reference/conventions.md`](../docs/os/reference/conventions.md)
- Upstream's README, kept as the reference for the upstream Lua API and the app format:
  [`docs/os/reference/upstream-readme.md`](../docs/os/reference/upstream-readme.md)
