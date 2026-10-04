# Extending BadgeOS

BadgeOS is built so that adding a thing is **one new file or one new line in the feature that owns it**, and removing a thing is **deleting that file or folder**. There is no central table to edit, no switch statement to extend, no list to keep in sync. This guide is the recipe for each kind of addition and removal.

It works because every extensible list is a self-registering registry ([overview](../architecture/overview.md#6-self-registration)), every trusted mechanism (signing, the approval, permissions, routing) is generic and driven by data, and every deployment value is provisioned config rather than code.

## Quick reference

| I want to add… | I write… | Files I touch | Reflash? |
|---|---|---|---|
| a Lua app | a folder with `app.ini`, `main.lua`, `config.lua` | only the new folder | no (push it) |
| a native app | one `.cpp` with `BADGE_APP(...)` | only the new folder | yes |
| a setting | one `VK_CONFIG_KEY(...)` line | the file that uses it | yes |
| a payment token | one more entry in the `tokens` config value | none | no (`VKSET`) |
| a Lua function | one `VK_LUA_FUNCTION(...)` line | the feature's file | yes |
| a permission | one `VK_PERMISSION(...)` line | the feature's file | yes |
| a kind of signature | one `domain_<name>.cpp` with `VK_SIGN_DOMAIN(...)` | only the new file | yes |
| something to approve on screen | a function that fills an `ApprovalRequest` | your own file | yes |
| a frame type between badges (app) | Lua pack/unpack with `vk.app_frame` | your app | no |
| a frame type handled by firmware | codec functions + one `VK_ESPNOW_ROUTE(...)` line | `vk_frames`, the feature's file | yes |
| a background job | one `VK_SERVICE(...)` line | the feature's file | yes |
| a USB serial command | one `VK_SERIAL_COMMAND(...)` line | the feature's file | yes |
| an LED animation | one `VK_LED_PATTERN(...)` line | any file | yes |
| a settings page | one `pages/page_<id>.cpp` with `VK_SETTINGS_PAGE(...)` | only the new file | yes |
| a reaction to every approval | one `VK_ON_APPROVAL(...)` line | the feature's file | yes |
| a reaction to an app stopping, or to a wallet reset | one `VK_ON_APP_STOP(...)` or `VK_ON_RESET(...)` line | the feature's file | yes |
| a field in `VKINFO` | one `VK_INFO_FIELD(...)` line | the feature's file | yes |
| a whole feature | a folder under `src/vk/features/` | only the new folder | yes |

| I want to remove… | I do… |
|---|---|
| a Lua app | delete `apps/<id>/`; `DEL <id>` on badges that have it |
| a native app | delete `src/native_apps/<id>/` |
| a feature | delete `src/vk/features/<name>/` and the apps that need it ([dependency table](../architecture/overview.md#8-features-and-what-they-need)) |
| a signing domain, route, command, pattern, setting | delete its one line or file |
| a settings page | delete `src/vk/shell/pages/page_<id>.cpp` |
| a token | remove its entry from `tokens` (confirmed on the badge) |

After any addition or removal: `scripts/build.sh dev` runs the pre-flash checks and the host tests; if they pass, nothing else needs to change.

## Add a Lua app

1. `apps/<id>/app.ini`:
   ```ini
   name=My app
   version=1.0.0
   permissions=net
   min_api=2
   ```
2. `apps/<id>/config.lua`: `return { ... }` with every value someone might want to change.
3. `apps/<id>/main.lua`: define the callbacks you need (`on_start`, `on_update`, `on_draw`, `on_button`, `on_espnow`, `on_stop`). `local vk = require("vk")` for JSON, RPC, payments.
4. `scripts/push-apps.sh --port <port> dev` (or `vkdev.py push apps/<id>`).

It appears in the launcher's grid. If it requests `sign` or `request`, the user is asked once on first launch. Reference: [Lua API](../platform/lua-api.md), [app rules](../apps/apps.md#rules-for-every-lua-app). To take a payment, copy the [smallest paying app](../platform/lua-api.md#the-smallest-paying-app).

## Add a native app

1. Create `src/native_apps/<id>/<id>.cpp`: a class derived from `badge::App` and one `BADGE_APP(Class, "<id>", "<Name>", "1.0.0", "<permissions>")` line. Start from `hello_native`.
2. `scripts/build.sh dev --upload <port>`.

Rules for native code: [native apps](../platform/native-apps.md#rules-for-native-code).

## Add a config key

In the file that uses the value:

```cpp
VK_CONFIG_KEY(my_period, "my_period_ms", vk::config::Type::U32, "500", vk::config::F_NONE, 100, 5000, "how often X happens");
const uint32_t period = vk::config::u32("my_period_ms");
```

Names are at most 15 characters. Mark it `F_SECURE` only if changing it could weaken an approval; `F_REQUIRED` only if the badge cannot work without it. Add the row to [config](../platform/config.md#keys).

## Add a payment token

No code. On each badge:

```
VKSET tokens <mint1>:2:HACK:100.00:1000.00,<mint2>:6:USDC:50.00:500.00
```

The badge shows a confirmation (it is a secure key); hold SELECT. The decoder, the approval, the caps and `wallet.tokens()` all read the table.

## Add a Lua function

```cpp
static int l_double(lua_State *L) { lua_pushinteger(L, 2 * luaL_checkinteger(L, 1)); return 1; }
VK_LUA_FUNCTION(my_double, "mymodule", "double", nullptr, l_double);    // badge.mymodule.double(n)
```

The fourth argument is the permission name, or `nullptr` for none. The module table is created automatically. Document it in [Lua API](../platform/lua-api.md).

## Add a permission

```cpp
VK_PERMISSION(camera, "camera", "use the camera", false, nullptr);
```

Arguments: identifier, name as written in `app.ini`, label shown to the user, whether first-run consent is needed, and the upstream `badge.<table>` names it gates (or `nullptr`). Then name it in the `VK_LUA_FUNCTION` lines it protects. Add the row to [app host](../platform/app-host.md#permissions).

## Add a signing domain

Use this when the badge must sign a new kind of thing (a login challenge, an event stamp).

1. Choose a name and a prefix: 2–15 characters of `[a-z-]` then `:`, not a prefix of any existing one ([rules](../wallet/signing.md#self-check)).
2. Create `src/vk/features/<feature>/domain_<name>.cpp`:
   ```cpp
   #include "../../wallet/signer.h"
   #include "../../wallet/approval.h"
   using namespace vk::wallet;

   static Reason decodeLogin(const uint8_t *bytes, size_t len, const Ctx &, ApprovalRequest &out) {
     // validate the exact structure of `bytes`; refuse anything else
     if (len < 8 || len > 96) return VK_UNDECODABLE;
     strlcpy(out.title, "Log in", sizeof out.title);
     strlcpy(out.headline, "SIGN IN REQUEST", sizeof out.headline);
     // ... fill big, sub, lines from the decoded bytes only
     out.severity = Severity::AMBER;
     out.select = SelectRule::HOLD;
     return VK_OK;
   }
   VK_SIGN_DOMAIN(login, "login", "login:", true, "sign", 96, decodeLogin, nullptr);
   ```
3. Apps call `wallet.begin("login", bytes)` and `wallet.poll()`. No new Lua function is needed for a button domain.
4. Add the row to the [domain table](../wallet/signing.md#domain-table) and a case to `test/host/test_domains.cpp`.

For an **auto** domain (no button), the last two arguments are `nullptr, validateFn`, the permission is `nullptr`, and the bytes must be built by firmware, never passed in by an app: add a Lua function that takes high-level arguments and builds the bytes itself.

Nothing in the signer, the approval engine or the screen is edited.

## Add something to approve on screen

Any firmware code can ask the user to confirm something without writing UI:

```cpp
vk::wallet::ApprovalRequest r{};
strlcpy(r.title, "Join event", sizeof r.title);
strlcpy(r.headline, "CONFIRM", sizeof r.headline);
strlcpy(r.big, "MHacks 2026", sizeof r.big);
r.severity = vk::wallet::Severity::AMBER;
r.select = vk::wallet::SelectRule::PRESS;
vk::wallet::approval::confirm(r, [](bool ok, void *) { /* act on ok */ }, nullptr);
```

## Add an ESP-NOW frame type

**For an app** (types 64–255): claim a free block in the [type registry](../protocol/espnow.md#type-registry), then in Lua:

```lua
badge.espnow.broadcast(vk.app_frame(72, payload))            -- send
function on_espnow(mac, data, rssi)
  local body = vk.app_body(data, 72)                         -- nil unless it is a type-72 frame
end
```

**For firmware** (types 1–63): add the struct and its `build`/`parse` pair to `src/vk/wallet/pure/vk_frames.{h,c}`, a round-trip test to `test/host/test_frames.c`, and in the feature:

```cpp
static bool onMyFrame(const uint8_t mac[6], const uint8_t *frame, size_t len, int8_t rssi, uint32_t rx_ms) {
  // parse, act; return true to consume, false to also deliver to the running app
  return true;
}
VK_ESPNOW_ROUTE(myframe, 20, 20, onMyFrame);
```

## Add a background job

```cpp
static void myBegin() { /* once at boot */ }
static void myUpdate() { /* every loop; return within a few milliseconds */ }
VK_SERVICE(my_job, myBegin, myUpdate);
```

## Add a USB serial command

```cpp
static void cmdUptime(const String &args, const vk::serial::Reply &reply) {
  reply("OK " + String(millis() / 1000));
}
VK_SERIAL_COMMAND(uptime, "VKUPTIME", cmdUptime, "seconds since boot");
```

Names start with `VK`. Commands exist on USB only. A command that should exist only in dev builds goes in `features/devtools/`.

## Add an LED pattern

See [ui](../ui/ui.md#adding-a-pattern). One line. (There are no status items: the header shows the time and the battery only.)

## Add a settings page

One file, `src/vk/shell/pages/page_<id>.cpp`, that includes `../page.h`, draws with the receipt kit and the page helpers, and ends in one registration line:

```cpp
VK_SETTINGS_PAGE(mypage, "mypage", 95, "My page", value, enter, update, draw, 0);   // a row that opens a screen
VK_SETTINGS_ACTION(mytoggle, "mytoggle", 96, "My toggle", value, action);          // a row that acts in place
```

The arguments, the helper functions, the layout constants and a complete example are in [shell](../ui/shell.md#add-a-settings-page). The `order` decides where the row sits in the Settings list; the id is the screen name device tests see in `VKSTATE.screen`. Add the row to the [page table](../ui/shell.md#settings-page-registry). Removing a page is deleting its file. The launcher, the dialogs and the boot screen are not registries: they are the shell's own files.

## React to every approval

```cpp
static void onApproval(const vk::wallet::ApprovalOutcome &o) {
  if (o.approved) { /* count it, light something, queue a report */ }
}
VK_ON_APPROVAL(my_listener, onApproval);
```

## Add a whole feature

1. Create `src/vk/features/<name>/`.
2. Put everything the feature needs inside it: domain file, routes, services, config keys, Lua functions, permission, store.
3. Depend only on `src/vk/core/`, `src/vk/wallet/`, `src/vk/host/`, `src/vk/ui/` headers, never on another feature's headers. If two features must talk, add a small function-pointer interface to the wallet core or host, as `presenceLookup` does ([checks](../wallet/checks.md#presence-lookup)).
4. Add a row to the [feature table](../architecture/overview.md#8-features-and-what-they-need) and write the feature's section in the relevant document.

## Remove a feature

1. Delete `src/vk/features/<name>/`.
2. Delete or adjust the apps that need it (the feature table says which behaviour disappears).
3. `scripts/build.sh dev`. If it builds and the checks pass, the removal is complete: there is no leftover table entry, because there was never a table.

## What is deliberately not extensible

| Fixed | Why |
|---|---|
| the approval screen's layout and severity colours | a customisable approval could be made to lie |
| the single signing path in `signer.cpp` | one place to review |
| the rule that an upstream file is untouched, hooked by marked lines, or listed as replaced | keeps the fork mergeable and the difference from upstream countable |
| the header's right side (time and battery only) and the names on screen | one look, one name: BadgeOS |
| the decoder's strictness | the badge signs only what it can show |

Changing any of these is a design change: update the owning document first, then the code and its tests.

## Needing an upstream change

If none of the registries can do what you need and an upstream file must change, add a hook: give it the next id in [upstream-hooks.md](../architecture/upstream-hooks.md) (retired ids H14, H15, H20 are never reused; H22 is reserved), keep it to a marked line that calls into `src/vk/`, and update the expected list in the pre-flash check. If a whole upstream file must go or be rewritten, add it to [Replaced upstream files](../architecture/upstream-hooks.md#replaced-upstream-files) first.
