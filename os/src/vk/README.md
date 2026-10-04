# os/src/vk: the BadgeOS layer

Everything BadgeOS adds to the upstream firmware lives under this folder. The rest of `os/src/` is upstream (Lua runtime, hardware layer, networking, identity), changed only through the tagged hooks of [upstream-hooks.md](../../../docs/os/architecture/upstream-hooks.md). The file-by-file map is section 4 of the [architecture overview](../../../docs/os/architecture/overview.md#4-source-tree); this page is the short version.

| Path | What it is | Specified in |
|---|---|---|
| `vk.h`, `vk.cpp` | the entry points `os.ino` calls through hooks H2, H4, H5 and H24: `vk::begin()`, `vk::update()`, the modal check | [overview](../../../docs/os/architecture/overview.md#5-main-loop) |
| `vk_build.h` | the compile-time switches (profile, Ed25519 backend, test hooks) | [overview](../../../docs/os/architecture/overview.md#10-compile-time-switches) |
| `vk_profile.h` | written by `scripts/build.sh` for the profile (`VK_PROFILE_DEV` 1 or 0); not in git | [build guide](../../../docs/os/guides/build-flash-provision.md#build-profiles) |
| `core/` | the self-registration primitive, services, the config store, the clock, USB serial commands, file I/O, saved Wi-Fi networks | [overview](../../../docs/os/architecture/overview.md#6-self-registration), [config](../../../docs/os/platform/config.md) |
| `wallet/` | the wallet core: the only caller of the badge key (`signer.cpp`), the signing-domain table, the approval engine, Ed25519 verify, `badge.wallet` for Lua. `wallet/pure/` is plain C99 with no Arduino, host-tested; `wallet/vendor/` is Monocypher | [signing](../../../docs/os/wallet/signing.md), [approval](../../../docs/os/wallet/approval.md), [solana-payments](../../../docs/os/wallet/solana-payments.md) |
| `host/` | the app platform: manifest, permissions and first-run consent, the Lua function registry, the native-app runtime, the ESP-NOW router, notifications, the launcher's app catalogue | [app host](../../../docs/os/platform/app-host.md) |
| `ui/` | the theme (Receipt light and dark), the receipt drawing kit, the approval screen, the boot screen, the on-screen keyboard, LED patterns, screen power and battery; `badge.theme`, `badge.receipt`, `badge.screen` for Lua | [ui](../../../docs/os/ui/ui.md), [text entry](../../../docs/os/ui/text-entry.md) |
| `shell/` | the shell: the screen stack, the launcher, the Settings list, dialogs, and one `pages/page_<id>.cpp` per settings page | [shell](../../../docs/os/ui/shell.md) |
| `features/` | one folder per feature, each removable by deleting it: `solana_pay`, `requests` (requests and presence), `contacts`, `history`, `balance`, `store_reg`, `selftest_sign`, `devtools` (dev profile only) | [overview](../../../docs/os/architecture/overview.md#8-features-and-what-they-need), [extending](../../../docs/os/guides/extending.md#add-a-whole-feature) |
| `sdk/` | `badge_sdk.hpp`, the one header a native app includes, with `BADGE_APP` | [native apps](../../../docs/os/platform/native-apps.md#the-sdk-header) |

## Rules that hold here

- Every extensible list registers itself with one macro line in the file that owns the item (`VK_SERVICE`, `VK_CONFIG_KEY`, `VK_SERIAL_COMMAND`, `VK_LUA_FUNCTION`, `VK_PERMISSION`, `VK_SIGN_DOMAIN`, `VK_ESPNOW_ROUTE`, `VK_SETTINGS_PAGE`, `VK_LED_PATTERN`, `VK_THEME`, ...). There is no central table to edit.
- `identity::sign` is called only from `wallet/signer.cpp`; `VK_SIGN_DOMAIN(` appears only in `features/*/domain_*.cpp`; nothing under `features/` includes `src/identity/`. `scripts/preflash-check.sh` enforces all three.
- Names, file layout and comment style: [conventions](../../../docs/os/reference/conventions.md).
