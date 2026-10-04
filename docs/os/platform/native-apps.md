# Native C++ apps

How to write an app in C++ that is compiled into the firmware, and what that does and does not mean for trust. Files: `src/vk/sdk/badge_sdk.hpp`, `src/vk/host/native.{h,cpp}`, `src/native_apps/<id>/`.

## When to use one

| Use Lua when | Use native when |
|---|---|
| the app should be installable without reflashing | the app is part of the OS itself (Inbox, Wallet settings) |
| it comes from anyone outside the team | it needs speed Lua cannot give (a game with heavy per-pixel work) |
| it handles money and you want the sandbox | it needs an upstream C++ interface Lua does not expose |

## Trust

A native app is firmware. It runs with no memory cap, no time budget and no permission enforcement on upstream interfaces, in the same address space as the wallet core. Therefore:

- every native app is reviewed like any other firmware change;
- the product's claim about untrusted apps is a claim about **Lua** apps;
- a native app still cannot sign except through `vk::wallet::begin` (the pre-flash check finds any other use of the key, see [signing](../wallet/signing.md#the-key-gate)), and `begin` still checks the app's declared permissions and still shows the approval.

## The smallest app

One file. No list to edit anywhere.

```cpp
// src/native_apps/hello_native/hello_native.cpp
#include "../../vk/sdk/badge_sdk.hpp"

class HelloNative final : public badge::App {
 public:
  void on_draw() override {
    auto &c = display::canvas();
    c.fillScreen(theme::BG);
    display::textCentered("gm from C++", display::width() / 2, 100, theme::GREEN, 2);
    display::touch();
  }
  void on_button(uint8_t key, bool pressed) override {
    if (key == BTN_B && pressed) badge::exit();     // CANCEL
  }
};

BADGE_APP(HelloNative, "hello_native", "Hello (C++)", "1.0.0", "");
```

Build and flash; it appears in the launcher after the Lua apps (native apps are listed in id order). The id belongs to the firmware: a pushed Lua folder with the same id is ignored, and the native app is the one that launches ([app host](app-host.md#permissions)).

## The SDK header

```cpp
// src/vk/sdk/badge_sdk.hpp
#pragma once
#include <Arduino.h>
#include "../../config.h"
#include "../../hal/display.h"
#include "../../hal/buttons.h"
#include "../../hal/leds.h"
#include "../../net/espnow_mgr.h"
#include "../../net/net_route.h"
#include "../../settings.h"
#include "../../ui/theme.h"
#include "../core/config.h"
#include "../core/clock.h"
#include "../core/registry.h"
#include "../host/notify.h"
#include "../host/router.h"
#include "../wallet/signer.h"
#include "../wallet/approval.h"

namespace badge {

class App {
 public:
  virtual ~App() {}
  virtual void on_start() {}
  virtual void on_update(float dt) { (void)dt; }
  virtual void on_draw() {}
  virtual void on_button(uint8_t key, bool pressed) { (void)key; (void)pressed; }
  virtual void on_espnow(const uint8_t mac[6], const uint8_t *data, size_t len, int8_t rssi) {
    (void)mac; (void)data; (void)len; (void)rssi;
  }
  virtual void on_stop() {}
};

void exit();     // asks the host to stop this app at the end of the frame (runtime::requestStop)

struct NativeApp : vk::Registered<NativeApp> {
  const char *id, *name, *version, *permissions;
  App *(*create)();
  NativeApp(const char *i, const char *n, const char *v, const char *p, App *(*c)())
      : id(i), name(n), version(v), permissions(p), create(c) {}
};

}  // namespace badge

// id: [a-z0-9._-], unique among native and Lua apps. permissions: comma-separated, as in app.ini.
#define BADGE_APP(Class, id, name, version, permissions)                                   \
  static badge::App *badge_create_##Class() { return new Class(); }                       \
  static badge::NativeApp badge_app_##Class(id, name, version, permissions, badge_create_##Class)
```

The app object is created with `new` when the app starts and deleted when it stops, so every launch begins with fresh state, like a Lua app.

## Rules for native code

1. **Never block.** `on_update` and `on_draw` together should take under 20 ms. There is no watchdog; a native app that loops forever freezes the badge, including the force-quit. One network request or one signature per frame is the upper bound for blocking work.
2. **Draw only into `display::canvas()`** and call `display::touch()`. Do not call `display::flush()` or touch the panel.
3. **Read buttons through `on_button`** or `buttons::down()`. CANCEL must always lead out of the app.
4. **No signing except `vk::wallet::begin`.** No includes from `src/identity/`.
5. **No exceptions, no RTTI** (the Arduino core builds without them). `new` can return memory from the internal heap; for anything over a few kilobytes use `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`.
6. **Release what you take** in `on_stop` or the destructor: LEDs (`leds::off()`), the microphone, any ESP-NOW state.
7. **Declare permissions truthfully** in `BADGE_APP`. They are shown in the Wallet app and enforced at the wallet API, from the constructor to the destructor (`on_stop` included). A name no feature registered refuses the launch with `unknown permission: <name>`, as it does in `app.ini`.
8. **Redraw after a pause.** A native app gets no callback when an approval closes over it, and the approval's picture is still on the canvas. An app that draws only when something changed must also redraw when `on_update`'s `dt` is large (the first frame after the pause; the Wallet app uses 0.25 s).
9. Our own state (config, stores, notifications) is reached through the `vk::` headers the SDK includes, never by opening `/vk/` files directly, and never by including a feature's header: a native app that needs the balance uses `vk::wallet::tokenInfoLookup`, and must work when it is null.

## Paying from a native app

Same flow as Lua, with the C++ API:

```cpp
vk::wallet::Ctx ctx;                         // attach record / request bytes if you have them
auto r = vk::wallet::begin("solana", msg, msgLen, ctx, "mygame");
// r == VK_OK: the approval is open; this app is paused. On a later frame:
uint8_t sig[64]; vk::wallet::Reason why;
switch (vk::wallet::poll(sig, why)) {
  case vk::wallet::Poll::PENDING: break;
  case vk::wallet::Poll::SIGNED:  /* submit */ break;
  case vk::wallet::Poll::FAILED:  /* show vk::wallet::reasonName(why) */ break;
  case vk::wallet::Poll::IDLE:    break;
}
```

The message is built with `sol_tx_build_transfer` ([solana-payments](../wallet/solana-payments.md#builder)).

## Removing a native app

Delete its folder and reflash. Nothing else references it.

## Tests

Device: T-APP5 (a native app launches, draws, exits with CANCEL, and relaunches with fresh state) and T-APP6 (a native app without `sign` in its permissions gets `denied` from `begin`, in `on_start` and in `on_stop`; run with the temporary app `zz_denytest`, which is not kept in the tree), in [../testing/testing.md](../testing/testing.md#acceptance-tests).
