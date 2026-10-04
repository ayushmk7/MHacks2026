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
