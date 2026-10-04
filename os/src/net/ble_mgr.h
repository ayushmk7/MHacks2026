/*
  BLE transport - a Nordic UART Service (NUS), which every BLE terminal app and
  Web Bluetooth page already knows how to talk to.

    Service  6e400001-b5a3-f393-e0a9-e50e24dcca9e
    RX       6e400002-... write / write-no-response   host -> badge
    TX       6e400003-... notify                      badge -> host

  Traffic is newline-delimited text. Where a line goes depends on whether a Lua
  app has claimed the link:

    - no handler installed (i.e. the launcher is up) -> push_protocol, so a
      phone can install and launch apps over BLE
    - handler installed by a running app             -> straight to the app

  That rule is what keeps "push an app over BLE" working without an app being
  able to be silently talked over.

  One exception sits above both: any line whose first character is '%' is an
  HTTP-over-BLE bridge frame (see net/ble_bridge.h and the "Phone bridge"
  section of README.md). Those are pulled out of the stream before the app /
  push-protocol decision is made, so a phone can keep tunnelling the badge's
  HTTP traffic while an app owns the link - and an app never sees them.
*/
#pragma once

#include <Arduino.h>
#include <functional>

namespace ble_mgr {

// Called with one complete line, newline stripped.
using LineHandler = std::function<void(const String &)>;

// First character of a bridge frame. Lines starting with this never reach an
// app's on_ble() or push_protocol.
constexpr char BRIDGE_PREFIX = '%';

bool begin(const String &deviceName);
void end();
bool enabled();
bool connected();

// Drains both receive queues: bridge frames first (in order), then app / push
// lines. Called once per main-loop tick.
void update();

// Drains ONLY bridge frames, handing them to ble_bridge::onLine(). App and
// push-protocol lines stay queued, in arrival order, for the next update().
//
// This is what a blocking binding calls while it waits for a bridged HTTP
// response: loop() (and therefore update()) is not running during that wait, so
// something has to pump the link - but dispatching an app line from here would
// re-enter runtime::dispatchBle() -> armDeadline() and silently reset the
// callback's extension budget out from under the very call that is waiting.
void pumpBridge();

// Negotiated ATT MTU for the current link, or 23 when nothing has been
// negotiated (or nothing is connected).
uint16_t mtu();

// Splits into notifications sized from the negotiated MTU (20 bytes when the
// central never negotiated); a newline is appended if the text does not already
// end with one.
bool send(const String &text);
bool sendLine(const String &text);

// Installs the app-level handler. Clearing it hands the link back to
// push_protocol.
void onLine(LineHandler handler);
void clearLineHandler();
bool hasLineHandler();

String address();

}  // namespace ble_mgr
