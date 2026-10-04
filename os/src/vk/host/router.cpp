// src/vk/host/router.cpp
// The ESP-NOW router (protocol/espnow.md, "Router"). It owns the one upstream receive handler for
// the life of the firmware (hooks H3 and H9; upstream finding F1) and decides, for every received
// app payload, whether a firmware route or the running app gets it.
#include "router.h"

#include "../../lua_sdk/lua_runtime.h"   // runtime::running, runtime::dispatchEspnow
#include "../../net/espnow_mgr.h"        // onReceive, send, lastRxMs (hook H13)
#include "../vk.h"                       // vk::modalActive
#include "../wallet/pure/vk_frames.h"    // vk_frame_type, VK_T_CHAL, VK_T_PROOF
#include "permissions.h"                 // vk::host::granted

namespace vk::host::router {
namespace {

// "Forward to the app": only when an app is running (Lua or native), the approval is not active,
// and the app was granted `espnow`. There is no queue of our own: otherwise the frame is dropped.
void forwardToApp(const uint8_t *mac, const uint8_t *data, size_t len, int8_t rssi) {
  if (!runtime::running()) return;
  if (vk::modalActive()) return;
  if (!vk::host::granted("espnow")) return;
  runtime::dispatchEspnow(mac, data, len, rssi);
}

// Called by espnow_mgr::update() on the loop task, once per queued app payload.
void onFrame(const uint8_t *mac, const uint8_t *data, size_t len, int8_t rssi) {
  const int type = vk_frame_type(data, len);   // -1: not a VK v1 frame, a plain app message
  if (type >= 0) {
    // Rule 1: a route that covers the type sees the frame first, timed from when the radio got it.
    const uint32_t rxMs = espnow_mgr::lastRxMs();
    for (EspnowRoute *route = firstRoute(); route; route = route->next()) {
      if (type < route->first || type > route->last || route->fn == nullptr) continue;
      if (route->fn(mac, data, len, rssi, rxMs)) return;
    }
    // Rule 2: CHAL and PROOF never reach an app, whether or not a route exists.
    if (type == VK_T_CHAL || type == VK_T_PROOF) return;
  }
  // Rule 3: everything no route consumed goes to the app.
  forwardToApp(mac, data, len, rssi);
}

}  // namespace

void install() {
  espnow_mgr::onReceive(onFrame);
}

bool send(const uint8_t *mac, const uint8_t *frame, size_t len) {
  if (frame == nullptr || len == 0) return false;
  // Upstream wraps the frame as an app payload, registers the peer and broadcasts for a null mac.
  // True means queued for the radio, not delivered. False: ESP-NOW is off, or len is over 240.
  return espnow_mgr::send(mac, frame, len);
}

}  // namespace vk::host::router
