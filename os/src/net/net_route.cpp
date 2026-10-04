#include "net_route.h"

#include <HTTPClient.h>

#include <utility>  // std::move

#include "../badge_log.h"
#include "../lua_sdk/lua_runtime.h"
#include "ble_bridge.h"
#include "wifi_mgr.h"

namespace net_route {
namespace {

// How much longer than the request's own timeout the badge waits for the phone
// before giving up: the phone has to run the fetch AND stream the answer back
// over BLE, and the BLE half is not covered by the HTTP timeout the phone was
// handed.
constexpr uint32_t BRIDGE_SLACK_MS = 2500;

// Slack on the Wi-Fi route, unchanged from what badge.http always granted.
constexpr uint32_t WIFI_SLACK_MS = 500;

bool usableUrl(const String &url) {
  return url.startsWith("http://") || url.startsWith("https://");
}

Response requestOverWifi(const char *method, const String &url, const String &body,
                         const String &contentType, uint32_t timeoutMs, const Header *headers,
                         size_t headerCount) {
  Response out;
  const bool hasBody = (strcmp(method, "POST") == 0);

  HTTPClient http;
  http.setTimeout(timeoutMs);
  http.setConnectTimeout(timeoutMs);
  if (!http.begin(url)) {
    out.err = "bad url";
    return out;
  }
  if (hasBody) http.addHeader("Content-Type", contentType);
  for (size_t i = 0; i < headerCount; ++i) http.addHeader(headers[i].name, headers[i].value);

  const int status =
      hasBody ? http.POST((uint8_t *)body.c_str(), body.length()) : http.GET();
  if (status <= 0) {
    out.err = HTTPClient::errorToString(status);
    http.end();
    return out;
  }

  out.body = http.getString();
  http.end();
  out.ok = true;
  out.status = status;
  return out;
}

Response requestOverBridge(const char *method, const String &url, const String &body,
                           const String &contentType, uint32_t timeoutMs, const Header *headers,
                           size_t headerCount) {
  Response out;

  const uint16_t id =
      ble_bridge::begin(String(method), url, body, contentType, timeoutMs, headers, headerCount);
  if (id == 0) {
    out.err = "bridge unavailable";
    return out;
  }

  // THE DEADLOCK THIS LOOP EXISTS TO AVOID: while a Lua binding blocks, loop()
  // is not running, so ble_mgr::update() is not running either and the phone's
  // reply would sit in the receive queue forever. The wait therefore pumps the
  // link itself - but only the bridge lane. Draining an app line here would
  // reach runtime::dispatchBle() -> armDeadline(), which resets sExtensionGranted
  // and hands the very callback we are blocking inside a fresh 12-second budget
  // (or, worse, re-enters Lua from underneath a running Lua call).
  const uint32_t deadline = millis() + timeoutMs + BRIDGE_SLACK_MS;
  while (!ble_bridge::done(id)) {
    if ((int32_t)(millis() - deadline) >= 0) {
      ble_bridge::abort("bridge timeout");
      break;
    }
    ble_bridge::pump();
    delay(1);  // yield to the NimBLE host task; also feeds the task watchdog
  }

  ble_bridge::Result got = ble_bridge::take();
  out.ok = got.ok;
  out.status = got.status;
  out.body = std::move(got.body);
  out.err = got.err;
  out.truncated = got.truncated;
  if (!out.ok && out.err.length() == 0) out.err = "bridge failed";
  return out;
}

}  // namespace

bool wifiUp() { return wifi_mgr::connected(); }

bool bridged() { return !wifi_mgr::connected() && ble_bridge::ready(); }

bool available() { return wifi_mgr::connected() || ble_bridge::ready(); }

const char *statusText() {
  if (bridged()) return "bridged via phone";
  return wifi_mgr::statusText();
}

String ssid() {
  if (bridged()) return String("phone-bridge");
  return wifi_mgr::ssid();
}

Response request(const char *method, const String &url, const String &body,
                 const String &contentType, uint32_t timeoutMs, const Header *headers,
                 size_t headerCount) {
  Response out;

  if (!available()) {
    out.err = "no network";
    return out;
  }
  if (!usableUrl(url)) {
    // Checked up front so both routes answer the same way. HTTPClient would
    // have said this itself; the bridge would have handed the phone a URL its
    // SSRF guard then rejected with a much vaguer message.
    out.err = "bad url";
    return out;
  }

  if (wifi_mgr::connected()) {
    runtime::extendDeadline(timeoutMs + WIFI_SLACK_MS);
    return requestOverWifi(method, url, body, contentType, timeoutMs, headers, headerCount);
  }

  runtime::extendDeadline(timeoutMs + BRIDGE_SLACK_MS);
  out = requestOverBridge(method, url, body, contentType, timeoutMs, headers, headerCount);
  if (out.truncated) {
    badge_log::tagf("bridge", "body truncated at %u bytes", (unsigned)ble_bridge::RESP_CAP);
  }
  return out;
}

}  // namespace net_route
