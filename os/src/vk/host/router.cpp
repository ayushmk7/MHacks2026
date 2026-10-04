// ESP-NOW router, WP01 stub: installs exactly the handler upstream installed at hook H3's site, so
// frames reach the running app as before. Routes are registered but not consulted until WP22.
#include "router.h"

#include "../../lua_sdk/lua_runtime.h"
#include "../../net/espnow_mgr.h"

namespace vk::host::router {

void install() {
  espnow_mgr::onReceive([](const uint8_t *m, const uint8_t *d, size_t n, int8_t r) {
    if (runtime::running()) runtime::dispatchEspnow(m, d, n, r);
  });
}

bool send(const uint8_t *mac, const uint8_t *frame, size_t len) {
  return espnow_mgr::send(mac, frame, len);   // upstream: a null mac broadcasts
}

}  // namespace vk::host::router
