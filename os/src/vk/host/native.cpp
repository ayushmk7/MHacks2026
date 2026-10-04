// Native runtime, WP01 stub: reports no native apps and is never active, so hooks H8 and H11 leave
// upstream's launcher and runtime unchanged. The runtime arrives in WP31.
#include "native.h"

#include "../../lua_sdk/lua_runtime.h"
#include "../sdk/badge_sdk.hpp"

namespace vk::host::native {

size_t count() { return 0; }

bool infoAt(size_t index, app_store::Info &out) {
  (void)index; (void)out;
  return false;
}

bool infoById(const String &id, app_store::Info &out) {
  (void)id; (void)out;
  return false;
}

bool exists(const String &id) {
  (void)id;
  return false;
}

bool start(const String &id) {
  (void)id;
  return false;
}

void stop() {}
bool active() { return false; }
void update(float dt) { (void)dt; }
void button(uint8_t key, bool pressed) { (void)key; (void)pressed; }

void espnow(const uint8_t *mac, const uint8_t *data, size_t length, int8_t rssi) {
  (void)mac; (void)data; (void)length; (void)rssi;
}

const char *permissions() { return ""; }

}  // namespace vk::host::native

// Declared in sdk/badge_sdk.hpp: asks the host to stop the running app at the end of the frame.
void badge::exit() { runtime::requestStop(); }
