/*
  One place that answers "how does this badge reach the internet right now, and
  please make this request over it".

  Two routes, tried in that order:

    1. real Wi-Fi   - HTTPClient, exactly the behaviour badge.http always had
    2. phone bridge - ble_bridge, tunnelled over the Nordic UART link

  Before this existed every HTTP call site rolled its own HTTPClient and gated
  itself on wifi_mgr::connected(); adding a second transport meant touching all
  of them. Now they ask net_route.

  SECURITY. The two routes do NOT have the same guarantees:

    - Over Wi-Fi, TLS terminates on the badge. `https://` means what it says.
    - Over the bridge, TLS terminates on the PHONE. The badge sends a URL and
      gets bytes back; it cannot see, let alone verify, the certificate. The
      phone (and anything with control over it) can read and rewrite every
      request and response.

  broker_client keeps its own pinned-CA HTTPClient for the Wi-Fi route for
  exactly this reason, and only falls back to net_route when the only route left
  is the bridge - at which point the pinning guarantee is gone and the code says
  so. See README.md#phone-bridge.
*/
#pragma once

#include <Arduino.h>

#include "ble_bridge.h"

namespace net_route {

// One extra request header. Over Wi-Fi these become HTTPClient::addHeader();
// over the bridge they become %HDR frames (a documented extension to the v1
// bridge protocol - see ble_bridge.h).
using Header = ble_bridge::Header;

struct Response {
  bool ok = false;
  int status = 0;
  String body;
  String err;
  // Set when the bridge cut the body at ble_bridge::RESP_CAP. `body` still
  // holds the prefix; whether that is usable is the caller's call.
  bool truncated = false;
};

// True when the badge is on a real Wi-Fi network (or its own SoftAP - see the
// note in wifi_mgr::connected()).
bool wifiUp();

// True when there is no Wi-Fi but a phone is bridging.
bool bridged();

// True when either route exists.
bool available();

// For the UI and for badge.wifi: the real Wi-Fi status text, or "bridged via
// phone" when the bridge is the only route.
const char *statusText();

// The real SSID, or "phone-bridge" when bridged.
String ssid();

// Blocks for up to roughly `timeoutMs` (plus the bridge's own slack). Returns
// ok=false with `err` set on any transport failure; `err` is "bad url" for a
// URL neither route can use, so callers keep the string badge.http always
// returned.
//
// Extends the Lua callback deadline by what it intends to spend, so a blocking
// call is not mistaken for a runaway loop. Harmless off the Lua thread:
// runtime::extendDeadline() is a no-op when no callback is armed.
Response request(const char *method, const String &url, const String &body,
                 const String &contentType, uint32_t timeoutMs, const Header *headers = nullptr,
                 size_t headerCount = 0);

}  // namespace net_route
