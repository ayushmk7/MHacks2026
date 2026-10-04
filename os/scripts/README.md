# os/scripts: the laptop tools

Everything run on the laptop to build, flash, install apps on, provision and test a badge. Every script works from any directory and with a space in the repository path. The Python tools need the repository's venv (`python3 -m venv .venv && .venv/bin/pip install pyserial esptool` at the repository root); `fixtures.py` and some device tests also need `cryptography`.

| Script | Usage | What it does |
|---|---|---|
| `build.sh` | `scripts/build.sh <dev\|release> [--upload <port>] [--keep-apps]` | writes `src/vk/vk_profile.h`, runs the pre-flash checks, compiles into `build/<profile>/`; with `--upload` flashes at 460800 baud and then writes an image of every app of the profile to the filesystem partition (`--keep-apps` leaves the filesystem alone) |
| `fleet.sh` | `scripts/fleet.sh <dev\|release> <port> [<port> ...]` | builds once, flashes firmware and apps to every port in parallel, and (dev builds) fails if any two launchers differ. The way to flash the demo badges |
| `push-apps.sh` | `scripts/push-apps.sh --port <port> [--token <code>] [--dry-run] dev\|release`<br>`scripts/push-apps.sh --host <ip> --token <code> [--dry-run] dev\|release`<br>`scripts/push-apps.sh --image <file> dev\|release` | installs every app of `apps/` (copying `lib/vk.lua` into each), over USB serial or Wi-Fi, or writes a LittleFS image of them. `profile=` and `include=` in each `app.ini` decide what goes |
| `push_serial.py` | `push_serial.py --port <port> [--code <code>] <id>=<folder> ...` | the USB push that `push-apps.sh --port` runs: one session, the port held exclusively, re-authenticates on `ERR not authorised` |
| `vkdev.py` | `vkdev.py --port <port> <command>`; `vkdev.py --selftest` | the serial tool: `info`, `cmd`, `wait-ready`, `reset`, `state`, `btn`, `shot`, `push`, `run`, `stop`, `tidy`, `menu`, `monitor`, `test`, `provision` (values from `os/provision.public.env`; `--dry-run` opens no port) |
| `preflash-check.sh` | `scripts/preflash-check.sh <dev\|release>` | the seven pre-flash checks (hook table, where signing may be called, where domains may be defined, no `src/identity/` includes from features or native apps, host tests, release profile, names); `build.sh` runs it and stops on a failure |
| `check-names.py` | `scripts/check-names.py [--list]` | pre-flash check 7: no string literal a user or the network can see names the upstream OS brand; `--list` prints every match with its verdict |
| `new-app.sh` | `scripts/new-app.sh <id> "<Name>" [--native] [--category <name>]` | makes a working Lua app in `apps/<id>/` or a native app in `src/native_apps/<id>/` from `templates/` |

Typical sequence for the demo badges:

```bash
cd os
scripts/fleet.sh release /dev/cu.usbserial-10 /dev/cu.usbserial-210 /dev/cu.usbserial-310 /dev/cu.usbserial-410
# power-cycle every badge once (USB out, switch off, switch on, USB in), then per badge:
../.venv/bin/python scripts/vkdev.py --port /dev/cu.usbserial-10 provision \
    --listener http://<backend laptop>:8788 --wifi "<SSID>" "<password>" --badge-id 1 --label "<label>"
```

Full guides: [flash another badge](../../docs/os/guides/flash-another-badge.md), [build, flash, provision](../../docs/os/guides/build-flash-provision.md), [testing](../../docs/os/testing/testing.md#the-serial-tool).

## os/tools

`os/tools/` holds two tools that come from upstream: `badge-push.py` (push apps over Wi-Fi or BLE; `push-apps.sh --host` uses it; its texts were edited to say BadgeOS, see [upstream-hooks](../../docs/os/architecture/upstream-hooks.md#replaced-upstream-files)) and `fetch-broker-ca.ts` (regenerates the app-store broker's pinned CA header, `src/net/broker_ca.h`).
