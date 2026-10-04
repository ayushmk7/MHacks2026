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
  const char *launcher = "";   // the launcher keys of app.ini, ';' between them: "category=games"
  NativeApp(const char *i, const char *n, const char *v, const char *p, App *(*c)())
      : id(i), name(n), version(v), permissions(p), create(c) {}
  // What BADGE_APP uses: `launcher` is the optional last argument.
  NativeApp(App *(*c)(), const char *i, const char *n, const char *v, const char *p, const char *l = "")
      : id(i), name(n), version(v), permissions(p), create(c), launcher(l != nullptr ? l : "") {}
};

}  // namespace badge

// id: [a-z0-9._-], unique among native and Lua apps. permissions: comma-separated, as in app.ini.
// An optional last argument holds the launcher keys an app.ini would hold, with ';' between them
// (app-host.md, "Manifest"):
//   BADGE_APP(SelfTest, "selftest", "Self test", "1.0.0", "", "category=tests");
// A native app compiled only in the dev profile wraps its own file in #if VK_PROFILE_DEV.
#define BADGE_APP(Class, id, name, version, ...)                                           \
  static badge::App *badge_create_##Class() { return new Class(); }                       \
  static badge::NativeApp badge_app_##Class(badge_create_##Class, id, name, version, __VA_ARGS__)
