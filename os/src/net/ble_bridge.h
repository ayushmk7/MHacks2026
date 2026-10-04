/*
  HTTP over BLE - the badge half of the phone bridge.

  A badge with no Wi-Fi still has a phone standing next to it, and that phone
  has internet. The phone (a Web Bluetooth page) connects to the same Nordic
  UART Service the push protocol uses, says `BRIDGE ON`, and from then on the
  badge can hand it an HTTP request to run and get the response back. See
  BRIDGE_PROTOCOL.md for the wire format and README.md#phone-bridge for the
  operator-facing story.

  Frames, all on their own newline-terminated line, all opaque values base64:

    badge -> phone   %REQ <id> <method> <timeoutMs> <bodyLen>
                     %URL  <id> <b64 url>            (repeatable, in order)
                     %CT   <id> <b64 content-type>   (optional)
                     %HDR  <id> <b64 name> <b64 value>   (extension, see below)
                     %BODY <id> <seq> <b64 chunk>
                     %SEND <id>
    phone -> badge   %RES  <id> <status> <totalLen> <b64 ct> [TRUNC]
                     %DATA <id> <seq> <b64 chunk>
                     %END  <id>
                     %ERR  <id> <b64 message>

  SECURITY - READ THIS BEFORE USING IT FOR ANYTHING THAT MATTERS.
  The phone terminates TLS. A request that goes out over the bridge is decrypted
  and re-encrypted by the phone, so:
    - the badge's pinned-CA guarantee for the broker (broker_ca.h) does NOT hold
      over the bridge; the phone and anything it trusts can read and rewrite the
      tunnelled traffic;
    - "https://" in a bridged URL says nothing about what the badge received.
  Only bridge to a phone you own. The bridge is off by default, requires an
  authenticated push session to turn on, and drops on disconnect / app exit.

  Threading: everything except linkLost() must be called from the main loop.
  onLine() is called by ble_mgr while draining the bridge receive queue.
*/
#pragma once

#include <Arduino.h>

namespace ble_bridge {

// Largest response body the badge will accept, decoded. Anything beyond this is
// dropped and the result is flagged truncated. Sized against PSRAM, not against
// what a phone can fetch.
constexpr size_t RESP_CAP = 32768;

// Decoded bytes per outbound base64 payload line. A multiple of 3, so every
// chunk but the last encodes to exactly 240 unpadded base64 characters - which
// means the phone gets the same bytes whether it decodes each chunk on its own
// or concatenates them all and decodes once. Both readings of the spec work.
constexpr size_t B64_CHUNK_BYTES = 180;

// One extra request header, carried as a %HDR frame.
//
// EXTENSION beyond BRIDGE_PROTOCOL.md v1, which has no header frame at all.
// broker_client needs `Authorization: Bearer ...` and cannot work over the
// bridge without it. A phone that does not implement %HDR should ignore the
// unknown frame; the request then goes out unauthenticated and the broker
// answers 401, which surfaces as a normal broker error rather than a hang.
struct Header {
  String name;
  String value;
};

// Outcome of one bridged request.
struct Result {
  bool ok = false;
  int status = 0;
  String body;
  String contentType;
  String err;
  bool truncated = false;  // response was longer than RESP_CAP
};

// -- Claim -------------------------------------------------------------------
// Turned on by `BRIDGE ON` over an authenticated push session; dropped by
// `BRIDGE OFF`, BLE disconnect, ble.listen(false) and app exit.
void setEnabled(bool on);
bool enabled();

// True when the bridge is enabled AND a central is connected, i.e. a request
// stands a chance. net_route::available() is the thing callers usually want.
bool ready();

// -- Request lifecycle -------------------------------------------------------
// Emits %REQ..%SEND and returns the request id (always non-zero), or 0 if the
// bridge is not ready, another request is in flight, or the link died mid-send.
uint16_t begin(const String &method, const String &url, const String &body,
               const String &contentType, uint32_t timeoutMs, const Header *headers = nullptr,
               size_t headerCount = 0);

// Drains queued bridge frames into the state machine. Safe to call in a tight
// wait loop; it never dispatches app or push-protocol lines, so it cannot
// re-enter the Lua runtime (and cannot reset the caller's deadline extension).
void pump();

// True once `id` has a result - or was never the in-flight request.
bool done(uint16_t id);

// Peek at the finished result.
const Result &result();

// Take the finished result (moving the body out) and return to idle. This is
// the normal way to end a request; result() is only for inspection.
Result take();

// Ends an in-flight request with `reason` as the error. Used for the caller's
// own timeout.
void abort(const char *reason);

// -- Housekeeping ------------------------------------------------------------
// Main-loop tick: services a link loss noticed on the BLE task.
void update();

// Full teardown: drops the claim, fails anything in flight, frees the response
// buffer. Main loop only.
void reset();

// Safe from any task (in particular the NimBLE host task): records that the
// link is gone and lets the main loop do the teardown.
void linkLost();

// One '%' frame from the link. Called by ble_mgr on the main loop.
void onLine(const String &line);

}  // namespace ble_bridge
