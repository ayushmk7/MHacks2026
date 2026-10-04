# os/apps: the Lua apps

Every folder here is one Lua app. The folder name is the app id (`apps/pay/` is the app `pay`, installed on the badge as `/apps/pay/`). No script and no C++ file names an app: an app exists because its folder holds an `app.ini`, and where it is listed and how it is installed come from that file alone.

The full rules are in [docs/os/apps/apps.md](../../docs/os/apps/apps.md) (every app, and the rules every Lua app keeps), [docs/os/platform/app-host.md](../../docs/os/platform/app-host.md#manifest) (the manifest) and [docs/os/platform/lua-api.md](../../docs/os/platform/lua-api.md) (the Lua API).

## What an app folder holds

| File | What it is |
|---|---|
| `app.ini` | the manifest, `key=value` per line (below) |
| `main.lua` | the code. Draws only through `vk.ui` (the Receipt look); CANCEL always leads out |
| `config.lua` | every knob and every word the app shows, as one returned table. Deployment values (addresses, URLs, tokens) are not knobs: they are provisioned config keys read with `badge.wallet.config("<key>")` |

`lib/vk.lua` is not in the folder: `scripts/push-apps.sh` copies it in as `vk.lua` when it installs the app ([os/lib](../lib/README.md)).

## Manifest keys

Upstream's keys are `name`, `version`, `author`, `description` and `entry`. BadgeOS adds:

| Key | Example | Read by | Meaning |
|---|---|---|---|
| `permissions` | `permissions=sign,net,espnow` | firmware | what the app may use. `sign` and `request` ask the person once, on the first launch. No line: no permissions |
| `min_api` | `min_api=2` | firmware | the lowest `badge.api_version` the app runs on (BadgeOS is 2) |
| `category` | `category=games` | firmware | the launcher folder the app is listed in (`GAMES`); a folder exists because an app names it |
| `hidden` | `hidden=1` | firmware | not on the launcher or in Home's menu; still starts over serial (`RUN <id>`). Test fixtures |
| `count` | `count=notes` | firmware | the launcher shows the waiting-notification count on the app's cell (used by the native Inbox) |
| `profile` | `profile=dev` | `push-apps.sh` | installed by a `dev` push only; `release` leaves the app out |
| `include` | `include=game` | `push-apps.sh` | the files of `apps/game/` are pushed with this app, except its `config.lua`, its `app.ini` and any file this app has itself |

## Add or remove an app

```bash
cd os
scripts/new-app.sh my_app "My app" [--category games]   # makes apps/my_app/ from templates/lua_app/
scripts/push-apps.sh --port /dev/cu.usbserial-XX dev    # installs every app of the profile, the new one included
```

The id is `[a-z0-9._-]`, at most 32 characters. To remove an app, delete its folder, and on a badge that has it send `DEL <id>` or hold RIGHT on it in the launcher. A demo badge gets exactly the apps of this folder: flash with `scripts/fleet.sh`, never push an extra app by hand ([flash another badge](../../docs/os/guides/flash-another-badge.md#several-badges)).

## The apps

| Id | Name | Launcher | Installed by | What it does |
|---|---|---|---|---|
| `home` | Home | top level | dev, release | balance, address, key and clock at a glance, and a menu of the other apps |
| `pay` | Pay | top level | dev, release | lists the payment requests of badges nearby and pays one, after the firmware approval |
| `request` | Request | top level | dev, release | asks nearby badges to pay an amount, waits, and checks the payment on chain |
| `history` | History | top level | dev, release | every approval this badge answered, what it signed by itself, and the payments it received |
| `contacts` | Contacts | top level | dev, release | the people you have swapped cards with, and the swap itself |
| `dice` | Dice | GAMES | dev, release | roll one to five dice |
| `duel` | Duel | GAMES | dev, release | a two-badge reaction duel settled with a payment request |
| `game` | Game | GAMES | dev, release | dodge the falling blocks; the shop sells items for real tokens |
| `evilgame` | Evil game | hidden | dev | the Game with a dishonest shop (`include=game`, so the folder has only `app.ini` and `config.lua`): its screen lies, the firmware's approval cannot |
| `checktest` | Check test | hidden | dev | test fixture: runs a payment case and logs each step (`test/device/t_chk.py`, `t_sign.py`, `t_sto.py`, `t_wallet_core.py`) |
| `signtest` | Sign test | hidden | dev | test fixture: signs the transfer the laptop serves, after the firmware approval (`t_apr.py`, `t_sign_net.py`) |
| `reqtest` | Request test | hidden | dev | test fixture: opens a payment request and logs RESULT frames (`t_req.py`, `t_req_single.py`) |
| `vktest` | Library test | hidden | dev | test fixture: runs `lib/vk.lua` on the badge and logs each result (`t_vk.py`) |

The launcher also lists native apps (Inbox, and Self test in the TESTS folder), which live in [`src/native_apps/`](../src/native_apps/README.md). Each user app has a scripted device test, `test/device/t_app_<id>.py` ([os/test](../test/README.md)).
