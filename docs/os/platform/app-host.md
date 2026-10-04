# App host

What Badge OS adds around apps: the manifest keys, permissions and first-run consent, the API version, the native runtime, notifications, and how Lua functions are registered. Files: `src/vk/host/`.

Upstream already provides the Lua runtime, the sandbox (1 MB heap, 250 ms per callback), the launcher, app push and the app store ([baseline](../architecture/upstream-baseline.md)). None of that is rewritten.

## Two kinds of app

| | Lua app | Native app |
|---|---|---|
| Lives in | `/apps/<id>/` on the badge's filesystem (source in `os/apps/<id>/`) | `src/native_apps/<id>/`, compiled into the firmware |
| Installed by | push over Wi-Fi, USB or BLE; the app store | reflashing |
| Sandbox | memory cap, time budget, permissions | none; trusted code |
| API | `badge.*` tables ([Lua API](lua-api.md)) | upstream C++ headers + `vk::` API through `badge_sdk.hpp` ([native apps](native-apps.md)) |
| Manifest | `app.ini` | the `BADGE_APP(...)` line |
| Consent prompt | yes, for sensitive permissions | no |

Both appear in the same launcher list (hook H11), are launched and stopped the same way, receive the same callbacks, and are paused the same way by the approval.

| Callback | Lua global | Native `badge::App` method |
|---|---|---|
| once after load | `on_start()` | `on_start()` |
| every frame | `on_update(dt)` then `on_draw()` | `on_update(float dt)` then `on_draw()` |
| button edge | `on_button(key, pressed)` | `on_button(uint8_t key, bool pressed)` |
| ESP-NOW frame | `on_espnow(mac, data, rssi)` | `on_espnow(const uint8_t mac[6], const uint8_t *data, size_t len, int8_t rssi)` |
| BLE line | `on_ble(line)` | not available (a native app that needs BLE uses upstream's `ble_mgr` directly and clears its handler in `on_stop`) |
| before stop | `on_stop()` | `on_stop()` |

Holding CANCEL for 1.5 s force-quits any app (upstream `APP_ESCAPE_HOLD_MS`).

## Manifest

`app.ini`, `key=value` per line. Upstream keys: `name`, `version`, `author`, `description`, `entry`. Badge OS adds two, read by our own parser (upstream's `Info` struct is not changed):

| Key | Form | Default | Meaning |
|---|---|---|---|
| `permissions` | comma-separated permission names, no spaces | (empty) | what the app may use |
| `min_api` | integer | 1 | lowest `badge.api_version` the app works with |

```ini
name=Pay
version=1.0.0
author=team
description=Pay a nearby badge
permissions=sign,net,espnow
min_api=2
```

```cpp
// src/vk/host/manifest.h
namespace vk::host::manifest {
struct Extra { String permissions; uint32_t min_api = 1; };
bool load(const String &appId, Extra &out);     // reads /apps/<id>/app.ini via app_store::readFile
}
```

## Permissions

A permission is a registered name. Everything an app can do beyond drawing, input, LEDs and reading the badge's public identity is behind one.

```cpp
// src/vk/host/permissions.h
namespace vk::host {
struct Permission : Registered<Permission> {
  const char *name;             // as written in app.ini
  const char *label;            // shown to the user: "request payments"
  bool consent;                 // true: the user must approve on first launch
  const char *upstream_tables;  // comma-separated upstream badge.<table> names this gates, or nullptr
  Permission(const char *n, const char *l, bool c, const char *t) : name(n), label(l), consent(c), upstream_tables(t) {}
};
#define VK_PERMISSION(ident, name, label, consent, upstream_tables) \
  static vk::host::Permission vk_permission_##ident(name, label, consent, upstream_tables)

bool granted(const char *permission);                  // for the active app; true when no app is active (firmware callers)
bool preLaunch(const String &appId, String &error);    // hook H8a
}
```

| Permission | Label | Consent | Gates | Registered in |
|---|---|---|---|---|
| `sign` | request payments | **yes** | `wallet.begin*`, `poll`, `check_record`, `build_transfer`, `wire_tx`, `requests`, `challenge`, `presence` | `host/permissions.cpp` |
| `request` | ask others to pay this badge | **yes** | `wallet.request_*` | `features/requests` |
| `contacts` | read and add contacts | no | `wallet.contact_*`, `wallet.contacts` | `features/contacts` |
| `history` | read payment history | no | `wallet.history` | `features/history` |
| `net` | use the network | no | upstream `badge.wifi`, `badge.http` | `host/permissions.cpp` |
| `espnow` | talk to nearby badges | no | upstream `badge.espnow`; delivery of `on_espnow` | `host/permissions.cpp` |
| `ble` | use Bluetooth | no | upstream `badge.ble` | `host/permissions.cpp` |
| `mic` | use the microphone | no | upstream `badge.mic` | `host/permissions.cpp` |
| `storage` | store files | no | upstream `badge.storage` | `host/permissions.cpp` |

Always available without a permission: `badge.gfx`, `badge.input`, `badge.led`, `badge.system`, `badge.battery`, `badge.se050`, `badge.codec`, and the identity functions of `badge.wallet` (`pubkey`, `address`, `key_location`, `time_ok`, `provisioned`, `tokens`, `config`, `balance`, `token_account`).

How it is enforced:

1. `preLaunch(appId)` reads the manifest. An unknown permission name refuses the launch with `unknown permission: <name>` (a typo must not silently grant nothing). It also refuses an id longer than 32 characters and a Lua folder whose id equals a native app's id. The granted set goes into a **pending** slot. Upstream's `launch()` then stops the old app (which clears the **active** slot), and only then builds the new Lua state: `vk::lua::open` (or `native::start`) promotes pending to active. `granted()` reads the active slot and never asks upstream which app is current (upstream sets that after the bindings are opened). A `preLaunch` that is not followed by a launch just leaves a pending slot that the next `preLaunch` overwrites.
2. `vk::lua::open(L)` (hook H7) builds the `badge` table for that app: a registered Lua function is installed only if its permission is granted; otherwise a stub is installed that raises `permission '<name>' not granted (add it to permissions= in app.ini)`. An upstream module table that is not granted is replaced by a table whose every access raises the same message.
3. The router delivers `on_espnow` only with `espnow` ([protocol](../protocol/espnow.md#router)).
4. The wallet core checks `granted(domain->permission)` again inside `begin()`; that check also covers native apps.

An app with no `permissions=` line gets none. Upstream's sample apps get a `permissions=` line added: `radar`: `espnow`; `whosnear`: `espnow,net`; `vumeter`: `mic,storage`; `gallery`: `storage`. Apps installed from upstream's app store arrive with an `app.ini` the firmware generates (name, version, author, description, entry only), so **a store app has no permissions**: it can draw, read buttons and drive the LEDs, and nothing else. That is the intended default for code from strangers.

`vk::lua::open` also removes the Lua globals `loadfile` and `dofile`: upstream leaves them in, and they can open any path on the filesystem as a Lua chunk, which leaks whether a file exists (finding F13).

### Consent

A permission marked "consent" needs the user's approval the first time an app that requests it is launched, and again whenever the app's permission list changes.

- Store: `/vk/consent.bin`, up to 32 entries of `app_id[33]` + `hash u32` (FNV-1a of the sorted permission list); format in [stores](../wallet/stores.md#consent). The oldest entry is replaced when full. It registers a `VK_ON_RESET` listener that erases it.
- `preLaunch` finds no matching entry → raises a confirmation ([approval](../wallet/approval.md)): title `Allow app`, headline `NEW PERMISSIONS`, big = the app's name, one line per consent permission (`May` / the label), amber, hold. It returns false with an empty error, so no error screen is shown. Upstream still runs its "app stopped" branch for a failed launch: the launcher cursor returns to the first row and any open push session is reset.
- Approved → the entry is saved and the app is launched with `runtime::requestLaunch`. Rejected → nothing happens.

This covers every install path (push, serial, BLE, store) with no change to any of them. Native apps skip consent: they were reviewed and compiled in.

## Lifecycle events

```cpp
// src/vk/host/lifecycle.h
namespace vk::host {
struct AppStopListener : Registered<AppStopListener> {
  void (*fn)(const char *appId);                 // appId may be "" (upstream calls stop() with no app running)
  explicit AppStopListener(void (*f)(const char *)) : fn(f) {}
};
#define VK_ON_APP_STOP(ident, fn) static vk::host::AppStopListener vk_on_app_stop_##ident(fn)

void onAppStopping(const String &appId);         // hook H8b: calls every listener
bool luaPaused();                                // hook H19: true while the approval is active
}
```

Listeners: the approval engine (drops an approval or result owned by that app), the permissions module (clears the active grant slot), the requests feature (closes that app's open requests). A feature that holds anything on behalf of an app registers one.

## API version

`badge.api_version` is 2 (hook H16). `preLaunch` refuses an app whose `min_api` is higher with `needs a newer Badge OS (API <n>)`. Bump `VK_API_VERSION` and H16 together when a Lua function is added; never change the meaning of an existing function.

## Lua function registry

```cpp
// src/vk/host/lua_registry.h
namespace vk::lua {
struct LuaFunction : Registered<LuaFunction> {
  const char *module;        // "wallet" -> badge.wallet
  const char *name;          // "begin_solana"
  const char *permission;    // nullptr = always available
  lua_CFunction fn;
  LuaFunction(const char *m, const char *n, const char *p, lua_CFunction f) : module(m), name(n), permission(p), fn(f) {}
};
#define VK_LUA_FUNCTION(ident, module, name, permission, fn) \
  static vk::lua::LuaFunction vk_lua_##ident(module, name, permission, fn)

void open(lua_State *L);       // hook H7; `badge` is on top of the stack and stays there
}
```

A feature adds a Lua function with one line next to its implementation; the module table is created on first use. Conventions for every binding: validate argument types with `luaL_check*`; return `nil, "<reason>"` for refusals ([reasons](../reference/reasons.md)); call `runtime::extendDeadline(ms)` before anything that can take longer than a few milliseconds.

## Native runtime

```cpp
// src/vk/host/native.h
namespace vk::host::native {
size_t count();
bool infoAt(size_t index, app_store::Info &out);       // for hook H11
bool infoById(const String &id, app_store::Info &out);
bool exists(const String &id);
bool start(const String &id);     // constructs the app object and calls on_start()
void stop();                      // on_stop(), then destroys the object
bool active();
void update(float dt);            // on_update(dt), on_draw()
void button(uint8_t key, bool pressed);
void espnow(const uint8_t *mac, const uint8_t *data, size_t length, int8_t rssi);
const char *permissions();        // of the active app; "" when none
}
```

Details, the SDK and the rules for native code: [native-apps.md](native-apps.md).

## Notifications

A small inbox for things that happen while the relevant app is not open.

```cpp
// src/vk/host/notify.h
namespace vk::host::notify {
struct Note { char title[24]; char body[40]; char app_id[33]; uint32_t at_ms; };
void post(const char *title, const char *body, const char *app_id);   // identical title+body within 10 s is ignored
size_t count();
const Note *at(size_t index);     // newest first
void remove(size_t index);
void clear();
}
```

- Eight notes, in RAM, oldest dropped. Nothing is persisted.
- Shown by: the `inbox` status item (`[n]` in the bar), the `notify` LED pattern while a note is waiting and no app is running, and the **Inbox** native app, which lists the notes; SELECT launches `app_id`, RIGHT dismisses.
- Posted by firmware features only (a payment request seen, a contact saved). Apps cannot post.

## System apps

Badge OS's own screens are native apps, so upstream's shell is not edited and each screen can be removed by deleting its folder:

| App id | Launcher name | What it shows |
|---|---|---|
| `inbox` | Inbox | notifications |
| `wallet_settings` | Wallet | provisioning state, key location, public key, token table with caps, clock source, every config key (read-only), "Reset wallet config" (→ `config::requestReset()`), build profile |
| `hello_native` | Hello (C++) | the smallest native app; proof the runtime works |

## Adding and removing

| To | Do |
|---|---|
| add a Lua app | create `apps/<id>/app.ini` and `main.lua`, push it ([extending](../guides/extending.md#add-a-lua-app)) |
| add a native app | create `src/native_apps/<id>/<id>.cpp` with one `BADGE_APP(...)` line, reflash ([extending](../guides/extending.md#add-a-native-app)) |
| add a permission | one `VK_PERMISSION(...)` line; name it in the `VK_LUA_FUNCTION` lines it gates |
| add a Lua function | one `VK_LUA_FUNCTION(...)` line |
| remove an app | delete its folder (and `DEL <id>` on badges that have it) |

## Tests

Host: `test_manifest` (parser), `test_consent` (hash, store round trip with an in-memory file). Device: T-APP1 to T-APP7 in [../testing/testing.md](../testing/testing.md#acceptance-tests).
