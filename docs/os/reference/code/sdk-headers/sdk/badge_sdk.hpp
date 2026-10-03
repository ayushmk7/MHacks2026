// src/sdk/badge_sdk.hpp (essentials; the wrappers for every badge_* function follow the same pattern)
#pragma once
#include <new>
#include "../app_host/badge_api.h"

// JSON for native apps: define BADGE_SDK_WITH_JSMN before including this header to get jsmn (v1.1.0,
// vendored at src/wallet/vendor/jsmn.h) with static linkage. Native apps never include the vendor path themselves.
#ifdef BADGE_SDK_WITH_JSMN
#define JSMN_STATIC
#include "../wallet/vendor/jsmn.h"
#endif

namespace badge {

class App {
 public:
  virtual ~App() = default;
  virtual void on_start() {}
  virtual void on_update(float /*dt*/) {}
  virtual void on_draw() {}
  virtual void on_button(badge_key_t /*key*/, bool /*pressed*/) {}
  virtual void on_espnow(const uint8_t /*mac*/[6], const uint8_t * /*data*/, size_t /*len*/, int /*rssi*/) {}
  virtual void on_ble(const char * /*line*/) {}
  virtual void on_stop() {}
};

namespace gfx    { inline void clear(uint16_t c = BADGE_BG) { badge_gfx_clear(c); }
                   inline void text_center(const char *s, int cx, int y, uint16_t c = BADGE_WHITE, int size = 1) { badge_gfx_text_center(s, cx, y, c, size); } }
namespace system { inline uint32_t millis() { return badge_system_millis(); }
                   inline void exit() { badge_system_exit(); } }
// ... one inline wrapper per function in badge_api.h, same names without the prefix ...

}  // namespace badge

// Defines the descriptor `BADGE_APP_DESC_<Class>` with C linkage. The object is built with placement new at
// launch and destroyed at stop, so every launch starts from a fresh state.
#define BADGE_APP(Class, ID, NAME, VERSION, CAPS)                                                    \
  namespace {                                                                                        \
  alignas(Class) unsigned char badge_storage_##Class[sizeof(Class)];                                 \
  Class *badge_self_##Class = nullptr;                                                               \
  }                                                                                                  \
  extern "C" const badge_app_desc_t BADGE_APP_DESC_##Class = {                                       \
      BADGE_ABI_VERSION, ID, NAME, VERSION, "", "", (CAPS),                                          \
      [] { badge_self_##Class = new (badge_storage_##Class) Class(); badge_self_##Class->on_start(); }, \
      [](float dt) { badge_self_##Class->on_update(dt); },                                           \
      [] { badge_self_##Class->on_draw(); },                                                         \
      [](badge_key_t k, bool p) { badge_self_##Class->on_button(k, p); },                            \
      [](const uint8_t mac[6], const uint8_t *d, size_t n, int rssi) { badge_self_##Class->on_espnow(mac, d, n, rssi); }, \
      [](const char *line) { badge_self_##Class->on_ble(line); },                                    \
      [] { badge_self_##Class->on_stop(); badge_self_##Class->~Class(); badge_self_##Class = nullptr; }}
