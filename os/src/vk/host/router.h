// The ESP-NOW router: owns the one upstream receive handler (espnow.md, "Router").
#pragma once

#include <Arduino.h>

#include "../core/registry.h"

namespace vk::host::router {
// Return true to consume the frame; false to let it continue to the running app.
using Handler = bool (*)(const uint8_t mac[6], const uint8_t *frame, size_t len, int8_t rssi, uint32_t rx_ms);

struct EspnowRoute : Registered<EspnowRoute> {
  uint8_t first, last;      // inclusive type range
  const char *name;
  Handler fn;
  EspnowRoute(uint8_t f, uint8_t l, const char *n, Handler h) : first(f), last(l), name(n), fn(h) {}
};
#define VK_ESPNOW_ROUTE(ident, first, last, handler) \
  static vk::host::router::EspnowRoute vk_route_##ident(first, last, #ident, handler)

// The field `first` hides the registry's static first(), so `EspnowRoute::first()` does not compile.
// Iterate with: for (auto *r = firstRoute(); r; r = r->next())
inline EspnowRoute *firstRoute() { return Registered<EspnowRoute>::first(); }

void install();                                                     // hook H3
bool send(const uint8_t *mac, const uint8_t *frame, size_t len);    // mac nullptr = broadcast
}
