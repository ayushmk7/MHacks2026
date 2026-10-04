# Naming and documentation conventions

How things are named and documented in `os/` and `docs/os/`. These rules are read off the code as it is, not invented for it: where most of the code does one thing, that is the rule, and the places that do otherwise are listed under [Known deviations](#known-deviations). Follow the rules for anything new. Do not rename existing names to fit them (see the deviations section for why).

The architecture overview keeps the conventions about values and words (buttons, amounts, public keys, log tags, reason codes, the product name): [overview, section 11](../architecture/overview.md#11-conventions). This page covers names of files, identifiers, keys and commands, and how folders and files are documented.

## Files and folders

| What | Rule | Example |
|---|---|---|
| C and C++ sources | `snake_case`, `.c` / `.cpp` with a header of the same name, `.h` | `src/vk/core/wifi_net.cpp`, `wifi_net.h` |
| BadgeOS code | under `os/src/vk/<layer>/`: `core`, `wallet`, `host`, `ui`, `shell`, `features`, `sdk` | `src/vk/host/router.cpp` |
| a feature | one folder `src/vk/features/<feature>/`; deleting it removes the feature | `features/history/` |
| a signing domain | `features/<feature>/domain_<name>.cpp`, the only place `VK_SIGN_DOMAIN(` may appear (pre-flash check 3) | `features/requests/domain_pay_req.cpp` |
| Lua bindings of a part | `lua_<part>.cpp` in that part's folder | `features/history/lua_history.cpp`, `ui/lua_theme.cpp` |
| a settings page | `src/vk/shell/pages/page_<id>.cpp`, `<id>` being the page id it registers | `pages/page_wifi.cpp` registers `wifi` |
| pure C (no Arduino, host-tested) | C99, `vk_<module>.{c,h}` under `src/vk/wallet/pure/`; outside it, `<module>_core.{c,h}` next to the C++ that uses it | `wallet/pure/vk_frames.c`, `ui/keyboard_core.c` |
| vendored code | `vendor/` with a `README` stating the version, source and checksums | `wallet/vendor/` |
| a native app | `src/native_apps/<id>/<id>.cpp`, `<id>` in `[a-z0-9_]` | `native_apps/inbox/inbox.cpp` |
| a Lua app | `os/apps/<id>/` with `app.ini`, `main.lua`, `config.lua`; `<id>` lower case, `[a-z0-9._-]`, at most 32 characters | `os/apps/pay/` |
| a host test | `os/test/host/test_<module>.{c,cpp,lua}`, printing `all <module> tests passed` | `test_frames.c` |
| a device test | `os/test/device/t_<area>.py`; a user app's test is `t_app_<id>.py`; suffix `_2` needs two badges, `_net` the network, `_single` is the one-badge half of a two-badge test | `t_app_pay.py`, `t_pay_2.py`, `t_req_single.py` |
| a screenshot | `os/test/device/shots/<screen or case>_<theme>.png` | `dice_receipt-dark.png` |
| a laptop script | `os/scripts/`, lower-case kebab case, a verb first when it does one thing; a usage comment at the top that the script also prints on a bad call | `push-apps.sh`, `check-names.py`, `build.sh` |
| a document | lower-case kebab case `.md`, in the `docs/os/` folder of its area | `docs/os/guides/flash-another-badge.md` |
| a folder README | `README.md`, upper case | `os/apps/README.md` |

## Identifiers

| What | Rule | Example |
|---|---|---|
| C++ namespaces | `vk::<layer>` or `vk::<layer>::<module>`, lower case; a feature's namespace is `vk::<feature>` | `vk::host::router`, `vk::ui::theme`, `vk::history` |
| namespace aliases in a `.cpp` | the module's own name | `namespace receipt = vk::ui::receipt;` |
| C++ functions and methods | `lowerCamelCase` | `requestReset()`, `serviceBegin()` |
| C++ types | `PascalCase` | `struct Launcher`, `class Screen` |
| constants and enum values | `UPPER_SNAKE_CASE` | `REFRESH_MS`, `F_SECURE` |
| macros | `VK_` + `UPPER_SNAKE_CASE`; include guards of pure C headers `VK_<FILE>_H` | `VK_PROFILE_DEV`, `VK_FRAMES_H` |
| registration macros | `VK_<THING>(ident, "name", ...)`: one line in the file that owns the item; `ident` is the name with `-` turned into `_` | `VK_CONFIG_KEY(sleep_s, "sleep_s", ...)`, `VK_SIGN_DOMAIN(pay_req, "pay-req", ...)` |
| pure C functions and types | `vk_<module>_<verb>` and `vk_<thing>_t` | `vk_frame_type()`, `vk_token_t` |
| Lua C functions | `l_<name>` (file-local) | `l_keep_awake` |
| native app classes | `PascalCase` of the id, in the `BADGE_APP` line | `BADGE_APP(SelfTest, "selftest", ...)` |
| Lua (apps and `lib/vk.lua`) | `snake_case` for functions, fields and locals; modules are tables on `vk` | `vk.reason_text`, `vk.pay.start` |

The prefix `vk` in paths, namespaces and macros and the Lua table `badge` are identifiers, not the product name, and are not renamed ([overview, section 11](../architecture/overview.md#11-conventions)). A top-level namespace other than `vk` is upstream's and is implemented, not owned, by BadgeOS (for example `namespace shell` in `shell/shell.cpp` is upstream's `src/ui/shell.h` interface).

## Keys, commands and tags

| What | Rule | Example |
|---|---|---|
| config keys | `snake_case`, with the unit as a suffix: `_s`, `_ms`, `_pct`, `_min`, `_url` | `sleep_s`, `presence_ms`, `batt_low_pct`, `utc_offset_min`, `rpc_url` |
| `config.lua` knobs | `snake_case`, same unit suffixes | `refresh_ms`, `signal_dbm`, `led_ms` |
| manifest keys | `snake_case` (upstream's and ours) | `min_api`, `category` |
| permission names | one lower-case word | `sign`, `espnow`, `history` |
| settings page ids, service names, LED patterns, info fields | `snake_case` | `wifi`, `screen_power`, `approve_green` |
| signing domains and theme names | kebab case | `pay-req`, `pay-proof`, `receipt-light` |
| USB serial commands | `VK` + one upper-case word, no separator; arguments separated by spaces, fields inside one argument by `\|` | `VKSET <key> <value>`, `VKWIFI <ssid>\|<password>` |
| upstream serial commands | unchanged upstream names, no prefix | `PING`, `RUN`, `STOP`, `DEL`, `AUTH` |
| hook tags in upstream files | `// VK: H<n>` at the end of the changed line, a letter for one of several sites of the same hook (`H8a` to `H8f`), with `(was ...)` when a value changed. Ids are never reused | `// VK: H30 (was 190: ...)` |
| C++ log tags | `badge_log::tagf("<tag>", ...)`, a short lower-case tag | `"vk"`, `"req"`, `"bal"` |
| Lua app log lines | `badge.log("<CODE> <event> ...")`: the app's code in capitals, the event in lower case. Tests match on these lines | `PAY list 0`, `REQ open <id>`, `CON swap on` |

## Commit messages

One subject line naming the area, a colon, and what changed in plain words, no trailing period; the hook id in brackets when an upstream file is touched; a work-package commit starts with its id. A body when the why is not obvious. The last line is the co-author trailer when an agent wrote the change.

```
Settings: Wallet and Inbox return to Settings (same row) when closed, not to the launcher
Full backlight by default (H30)
WP10: batch 2, config, signer, approval engine and screen, clock, router (WP10 WP11 WP12 WP20 WP22)
```

## How things are documented

**Every folder that holds something a person runs, adds to or reads has a `README.md`.** It is one or two screens and says, from what is on disk:

1. what the folder is for, in one or two sentences;
2. what is in it (a table: file or subfolder, one line each);
3. how to run it or how to add to it (the exact command);
4. where the full specification is (a link into `docs/os/`).

A README never restates a specification; it points to it. When the folder changes, its README changes in the same commit.

**`docs/os/` is the single source of truth for the firmware.** Each item has one owning document (the table in [docs/os/README.md](../README.md#documents)); other documents link to it. A change to code that a document specifies updates the document in the same commit. Status and dates are written as "As of `<date>`" with what was seen and on which badge, and anything not seen on hardware is marked `[UNVERIFIED]` with its fallback.

**Every source file starts with a header comment**: what the file is, and the document and section that specify it, in brackets (`(app-host.md, "Manifest")`). Many files put the file's path on the first line (`// src/vk/core/config.h`); new files should. A file that is removable says so ("Deleting this file removes the page and nothing else"). C++ uses `//`; pure C headers use one `/* ... */` block. Lua files start with `-- <App>: <what it does> (docs/os/apps/apps.md, "<App>")`.

**Comments say why, not what.** A comment explains a constraint, a hardware fact, a finding (`finding F17`), or the hook it belongs to. A literal that comes from a specification names it. A trailing comment on an `#include` says which names the file needs from it (`#include "router.h"   // vk::host::router::install`).

**Words.** The product is BadgeOS, one word; headers on the device read `BADGEOS`. No user-visible text names the upstream OS (`scripts/check-names.py`); READMEs may credit it.

## Known deviations

Places where existing names break the rules above. **Not renamed: the build and four flashed badges depend on them.** Source file names are in the build and the pre-flash checks, app ids are the folder names on every badge's filesystem and the names the device tests run, and serial commands, config keys and domain names are read by `vkdev.py`, the tests and the backend. Renaming any of them would break a working build or a flashed badge; each stays until there is time to rename it with every caller in one change.

Files and folders:

- `os/src/vk/wallet/pure/sol.h`, `sol_b58.c`, `sol_sha256.c`, `sol_tx.c`: pure C without the `vk_` prefix (they began as the reference code in [`code/`](code/), which keeps the same names).
- `os/src/vk/core/wifi_net.{h,cpp}` holds the namespace `vk::wifi`, and `os/src/vk/host/lua_registry.{h,cpp}` holds `vk::lua`: file and namespace names differ.
- `os/src/vk/features/selftest_sign/` holds `domain_selftest.cpp`: the folder and the domain differ in name. `os/src/vk/features/devtools/demo_approve.cpp` registers a serial command, not a domain or a Lua binding, with no prefix.
- `os/src/vk/sdk/badge_sdk.hpp` is the only `.hpp` header; every other header is `.h`.
- Lua app ids run words together (`checktest`, `reqtest`, `signtest`, `vktest`, `evilgame`) while the native app id `wallet_settings` uses `_`, and the native `selftest` and `nativetest` run words together.
- Device tests: `t_con.py`, `t_req.py` and `t_hook.py` need two badges but have no `_2`; `t_wallet_app.py` and `t_selftest.py` test apps but are not `t_app_<id>.py`; `t_pages1.py`, `t_pages2.py`, `t_rel1.py` number instead of naming.
- Screenshots: some name a test case instead of a screen and carry no theme (`chk_T-CHK2.png`, `apr4_approval.png`, `harness_r2-probe.png`).
- `os/scripts/push_serial.py` uses `_` and `os/scripts/vkdev.py` has no separator, unlike `push-apps.sh`, `check-names.py`, `preflash-check.sh`.
- Upper-case documents: `os/UPSTREAM-HOOKS.md` (a copy that `preflash-check.sh` reads by that name), `os/BRIDGE_PROTOCOL.md` (upstream's), `docs/logs/BADGE-BUTTONS-I2C.md` (linked from the guides, the harness and the source).
- Outside BadgeOS, owned by teammates and left as they are: `docs/dashboard/` uses upper-case names (`API.md`, `RUNBOOK.md`), and `docs/specs/` mixes `00-Interfaces.md`, `P1-a-firmware-wallet-core.md`, `Prd-verified-payment-key.md`, `CHANGELOG-routing.md`.

Identifiers and keys:

- The alias for `vk::ui::theme` is `th` in some files and `tk` in others (`shell/pages/page_restart.cpp`, `page_push.cpp`, `page_theme.cpp`, `page_wifi.cpp`, `page_espnow.cpp`, `page_bluetooth.cpp`, `page_display.cpp`); the receipt kit is aliased `rc` in `native_apps/selftest/` and `receipt` elsewhere. `page_wifi.cpp` aliases `vk::wifi` as `net`.
- A few constants use `kName` instead of `UPPER_SNAKE_CASE` (`page_restart.cpp`, `page_leds.cpp`, `page_display.cpp`, `domain_selftest.cpp`, `lua_wallet.cpp`, `signer.cpp`).
- Config keys `tokens`, `day_limit`, `display_name`, `issuer_key`, `shop_address` carry no unit suffix (they have no unit, which is fine); `wifi_autojoin` and `awake_usb` are flags with no `_on` / `is_` marker.
- Signing domain ids and theme names are kebab case while every other registered name is snake case; the macro idents map `-` to `_`.
- Lua log codes: user apps use their name (`PAY`, `REQ`, `DICE`), fixtures use two letters (`CT` checktest, `ST` signtest, `RT` reqtest, `VT` vktest), and Contacts uses `CON`.

Documentation:

- About half of `os/src/vk/` puts the path on the first line of its header comment; the rest start with the purpose. `os/src/vk/core/registry.h` and `os/src/vk/host/lifecycle.cpp` have no header comment.
- "Badge OS" in two words survives in comments of about a dozen files (`host/manifest.{h,cpp}`, `wallet/pure/vk_frames.{h,c}`, `wallet/pure/vk_reason.h`, `test/host/run.sh`, `test/host/vectors.mjs`) and in the design mockup.
- Commit subjects `update`, `push latest`, `apps` (early history) name no area.
