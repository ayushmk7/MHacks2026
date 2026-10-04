# Badge HTTP-over-BLE Bridge — wire protocol v1

The phone (webapp/PWA, Web Bluetooth central) gives a real hardware badge internet by
tunnelling the badge's HTTP requests over the existing Nordic UART Service (NUS).
This spec is the single source of truth for BOTH the firmware (badge side) and the
webapp (phone side).

## Transport & framing
- Same NUS as today: RX char (phone→badge, write-no-response), TX char (badge→phone, notify).
- Newline-delimited ASCII lines, exactly like the push line protocol. One frame per line.
- **All bridge frames start with `%`** so they are unambiguously distinguishable from:
  - push-protocol commands / `OK`/`ERR`/`+` replies, and
  - arbitrary app `on_ble` text (an app that called `ble.listen()` NEVER sees `%`-prefixed
    lines — `ble_mgr` dispatch intercepts them before the app/push handler).
- All opaque values (URL, body, content-type, error text, response bytes) are **base64**
  (standard alphabet, padded). Numbers are decimal ASCII. Tokens are space-separated.
- `<id>` is a small decimal request id chosen by the badge, monotonic per connection,
  wraps at 65535. At most **one** in-flight request id at a time (the badge blocks in the
  Lua callback), but the framing carries the id so a late/duplicate frame from a prior
  request is dropped by mismatch.

## Handshake (phone-initiated, over the existing command protocol)
1. Phone connects to NUS, sends `AUTH <pairingCode>\n` → expects `OK ...` (existing flow,
   existing lockout rules). If `pushRequiresPairing()` is false, AUTH still returns OK.
2. Phone sends `BRIDGE ON\n` → badge replies `OK bridge ready` and sets
   `ble_bridge::ready() = true`. `BRIDGE OFF\n` → `OK bridge off`. `BRIDGE ?` → `OK bridge on|off`.
   `BRIDGE ON` requires auth (same gate as other commands); pre-auth → `ERR unauthorized`.
3. While ready, the badge may emit `%REQ` frames at any time (async, badge-initiated).
   Disconnect, `BRIDGE OFF`, or `ble.listen(false)`/app stop clears ready.

## Request (badge → phone)
```
%REQ  <id> <method> <timeoutMs> <bodyLen>      ; method = GET|POST ; bodyLen = decoded bytes (0 for GET)
%URL  <id> <b64 url>                           ; may be split across multiple %URL lines in order; concatenate b64 BEFORE decode
%CT   <id> <b64 content-type>                  ; optional, POST only
%BODY <id> <seq> <b64 chunk>                   ; seq from 0, in order; only if bodyLen>0
%SEND <id>                                     ; terminator — phone begins the fetch now
```
`<b64 ...>` payload lines carry ≤ **240 base64 chars** each (≤180 decoded bytes) to stay
within one improved-MTU notify comfortably.

## Response (phone → badge)
```
%RES  <id> <status> <totalLen> <b64 content-type>   ; status = HTTP status (>0). totalLen = decoded body bytes (≤ RESP_CAP)
%DATA <id> <seq> <b64 chunk>                          ; seq from 0, in order
%END  <id>                                            ; all chunks sent
%ERR  <id> <b64 message>                              ; terminal failure instead of %RES/%END
```
- **`RESP_CAP` = 32768 decoded bytes.** If the real response is larger, the phone sends
  `%RES` with `totalLen` = min(actual, RESP_CAP), streams only the first RESP_CAP bytes,
  then `%END`, AND sets content-type note; the phone SHOULD also send
  `%ERR <id> <b64 "response truncated at 32768 bytes">` is NOT used for truncation — instead
  truncation is signalled by `%RES` carrying an extra trailing token `TRUNC`:
  `%RES <id> <status> <totalLen> <b64 ct> TRUNC`. Badge returns the truncated body normally
  (apps that care can check `Content-Length`), and logs a truncation notice.
- `%DATA` chunk payload ≤ 240 base64 chars, same as request.

## Badge-side semantics (net_route + Lua)
- New `net_route::request(method,url,body,ct,timeoutMs)` → `{ok,status,body,err}`.
  - If `wifi_mgr::connected()` → existing `HTTPClient` path (unchanged behaviour incl. TLS).
  - Else if `ble_bridge::ready()` → BLE bridge path below.
  - Else → `{ok=false, err="no network"}` (badge.http maps to `nil,"wifi not connected"`
    for source-compat — keep the exact legacy string when neither route exists).
- `badge.http` (lib_net) and `broker_client` both call `net_route::request` (repoint the two
  existing call sites; broker over-bridge inherits RESP_CAP — a >32KB script fetch fails with
  a clear error, documented; registration/inbox/small scripts are fine).
- Blocking pattern (avoids the main-loop-drain deadlock the exploration flagged):
  ```
  runtime::extendDeadline(min(timeoutMs+slack, remaining to 12s cap));
  ble_bridge::begin(id, req...);              // emits %REQ..%SEND via improved-MTU send
  while (!ble_bridge::done(id) && millis() < deadline) {
      ble_bridge::pump();                     // drains ONLY %-frames from the NUS RX ring;
                                              // NON-% lines are left queued (no reentrant
                                              // dispatchBle/armDeadline — extension budget intact)
      delay(1);                               // yield to NimBLE/Wi-Fi tasks
  }
  ```
- `ble_bridge::pump()` = a filtered variant of `ble_mgr::update()` that pops queue entries,
  routes `%`-prefixed lines to the bridge state machine, and re-holds any non-`%` line for
  normal dispatch after the request completes. RESP buffer in **PSRAM** (`ps_malloc`, 32KB cap).
- Timeout / disconnect mid-response → `{ok=false, err="bridge timeout"|"bridge disconnected"}`.
- **wifi.connected() (Lua) returns true when bridged**, `wifi.ssid()`→"phone-bridge",
  `wifi.status()`→a new "bridged via phone" string, so existing apps that gate on
  `wifi.connected()` before `http` just work. Documented as an intentional convenience lie
  (mirrors the SoftAP lie already in `wifi_mgr::connected()`).

## Throughput fix (prerequisite, touches shared BLE send)
- `ble_mgr::send()` currently 20B/notify + delay(4) (~5KB/s, blocks UI). Change to use the
  negotiated MTU: chunk = min(negotiatedMtu-3, remaining), drop delay to a minimal yield
  (`delay(1)` only every N chunks, or rely on notify backpressure). Safe for push transfers:
  `badge-push.py` reassembles by `\n` split, chunk-size agnostic. Keep a 20B floor if MTU
  negotiation hasn't completed.

## Phone-side semantics (webapp)
- Web Bluetooth: `requestDevice({filters:[{services:[NUS]}], optionalServices:[NUS]})`,
  get RX (writeWithoutResponse) + TX (notifications). Reassemble TX notifications by `\n`.
- On `%REQ..%SEND`: decode, run `fetch(url,{method,headers,body})` on the phone's own network,
  read up to RESP_CAP bytes, stream back `%RES/%DATA*/%END` (or `%ERR`).
- **SSRF-lite guard** (browser can't resolve DNS): reject URL literals to loopback/private
  ranges and non-http(s) schemes by hostname; surface a user toggle "allow local addresses"
  (default off). Document residual DNS-rebind risk. The badge is the user's own device, but a
  hostile installed Lua app could probe the phone's LAN — hence the guard.
- Content-type header from `%CT`; forward it. Never forward the phone's cookies/credentials
  (`credentials:'omit'`), fixed `User-Agent` note not possible in browser fetch (UA is fixed
  by the browser) — that's fine.

## Compatibility
- Reference client `badge-push.py` never sends `BRIDGE ON`, never sees `%` frames — unaffected.
- Improved MTU send only speeds it up.
- Emulator test harness: a TS "fake badge" implements the BADGE side of this protocol over an
  in-process transport (same interface as the Web Bluetooth transport) so the phone-side tunnel
  + fetch state machine is verified end-to-end in `bun test` without hardware.
