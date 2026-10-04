# os/src/native_apps: the native C++ apps

A native app is compiled into the firmware. Each one is a folder named after its id, holding one `.cpp` (several, for the Self test) that includes the SDK header [`../vk/sdk/badge_sdk.hpp`](../vk/sdk/badge_sdk.hpp) (plus BadgeOS headers such as the receipt kit as needed, never `src/identity/`) and ends with a `BADGE_APP(...)` line. That line registers the app, so the build compiles it and the launcher lists it with no other edit; deleting the folder and reflashing removes it.

| Folder | Id | Launcher | What it is |
|---|---|---|---|
| `inbox/` | `inbox` | top level, with the waiting-notification count (`count=notes`) | the notification inbox; also opened by Settings → Inbox |
| `wallet_settings/` | `wallet_settings` | hidden; Settings → Wallet | read-only pages of what the badge is provisioned with, and the wallet reset |
| `selftest/` | `selftest` | TESTS folder (`category=tests`) | the on-badge checklist (hardware, stores, wallet, radio, network); one file per suite, `suite_<name>.cpp` |
| `nativetest/` | `nativetest` | hidden; dev profile only (`#if VK_PROFILE_DEV`) | a fixture the device tests launch and stop (`test/device/t_native.py`) |

The launcher keys of a native app are the last argument of its `BADGE_APP` line, `;` between keys, with the same meaning as in a Lua app's `app.ini` ([os/apps](../../apps/README.md#manifest-keys)).

## Add one

```bash
cd os
scripts/new-app.sh my_app "My app" --native [--category games]   # src/native_apps/my_app/my_app.cpp from templates/native_app/
scripts/build.sh dev --upload /dev/cu.usbserial-XX
```

A native id is `[a-z0-9_]` because it also names the C++ class. Rules for native code (never block, draw only with the receipt kit, sign only through `vk::wallet::begin`, never include `src/identity/`): [native apps](../../../docs/os/platform/native-apps.md#rules-for-native-code). Each app is described in [apps.md](../../../docs/os/apps/apps.md).
