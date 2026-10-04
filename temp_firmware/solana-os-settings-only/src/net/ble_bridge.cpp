#include "ble_bridge.h"

#include <mbedtls/base64.h>

#include <utility>  // std::move

#include "../badge_log.h"
#include "ble_mgr.h"

namespace ble_bridge {
namespace {

// Where each spec frame is emitted or parsed:
//
//   %REQ   emit  sendRequestHead()        %RES   parse handleRes()
//   %URL   emit  sendChunked("%URL")      %DATA  parse handleData()
//   %CT    emit  sendChunked("%CT")       %END   parse handleEnd()
//   %HDR   emit  begin(), header loop     %ERR   parse handleErr()
//   %BODY  emit  sendChunkedSeq("%BODY")
//   %SEND  emit  begin(), last line

enum class Phase : uint8_t {
  Idle,       // nothing in flight
  Awaiting,   // %SEND written, waiting for %RES/%DATA/%END or %ERR
  Complete,   // %END seen, sResult holds the body
  Failed,     // %ERR / timeout / disconnect / protocol error
};

bool sEnabled = false;
Phase sPhase = Phase::Idle;

// Written from the BLE stack task, cleared on the main loop. A plain bool is
// enough: it is only ever set to true off-task and only ever cleared on-task.
volatile bool sLinkLost = false;

// Monotonic per connection, wraps at 65535, never 0 (0 means "no request").
uint16_t sNextId = 1;
uint16_t sActiveId = 0;

// Response assembly. The buffer is PSRAM (falling back to internal heap on a
// badge whose PSRAM did not train, same as broker_client::fetchScript).
uint8_t *sBuffer = nullptr;
size_t sCapacity = 0;
size_t sLength = 0;
uint32_t sNextSeq = 0;
int sStatus = 0;
bool sSawRes = false;
bool sTruncated = false;
String sContentType;

Result sResult;

void freeBuffer() {
  if (sBuffer != nullptr) {
    free(sBuffer);
    sBuffer = nullptr;
  }
  sCapacity = 0;
  sLength = 0;
}

// Clears everything about the in-flight request WITHOUT touching sResult, so a
// finished result survives until take().
void clearTransfer() {
  freeBuffer();
  sNextSeq = 0;
  sStatus = 0;
  sSawRes = false;
  sTruncated = false;
  sContentType = "";
}

void failNow(const String &message) {
  clearTransfer();
  sResult = Result();
  sResult.ok = false;
  sResult.err = message;
  sPhase = Phase::Failed;
  badge_log::tagf("bridge", "request %u failed: %s", (unsigned)sActiveId, message.c_str());
}

// -- base64 ------------------------------------------------------------------

String encodeBase64(const uint8_t *data, size_t length) {
  if (data == nullptr || length == 0) return String();
  const size_t capacity = 4 * ((length + 2) / 3) + 1;
  char *out = (char *)malloc(capacity);
  if (out == nullptr) return String();
  size_t written = 0;
  if (mbedtls_base64_encode((unsigned char *)out, capacity, &written, data, length) != 0) {
    free(out);
    return String();
  }
  out[written] = '\0';
  String encoded(out);
  free(out);
  return encoded;
}

// Decodes a whole token. Used for short values (error text, content type), so a
// transient malloc is fine; %DATA takes the streaming path in handleData().
bool decodeBase64(const String &encoded, String &out) {
  out = "";
  if (encoded.length() == 0) return true;
  // 3 bytes out per 4 in, rounded up, with slack for padding.
  const size_t capacity = (encoded.length() / 4 + 1) * 3 + 4;
  uint8_t *buffer = (uint8_t *)malloc(capacity);
  if (buffer == nullptr) return false;
  size_t decoded = 0;
  const int result = mbedtls_base64_decode(buffer, capacity, &decoded,
                                           (const unsigned char *)encoded.c_str(),
                                           encoded.length());
  if (result != 0) {
    free(buffer);
    return false;
  }
  const bool ok = out.concat((const char *)buffer, decoded);
  free(buffer);
  return ok;
}

// -- tokenising --------------------------------------------------------------

String nextToken(String &rest) {
  rest.trim();
  const int space = rest.indexOf(' ');
  if (space < 0) {
    const String token = rest;
    rest = "";
    return token;
  }
  const String token = rest.substring(0, space);
  rest = rest.substring(space + 1);
  return token;
}

// -- emitting ----------------------------------------------------------------

// Sends `payload` as one or more `<tag> <id> <b64>` lines. The payload is split
// into B64_CHUNK_BYTES pieces BEFORE encoding, which keeps every line at or
// under the 240-base64-character budget in the spec.
bool sendChunked(const char *tag, uint16_t id, const String &payload) {
  const size_t length = payload.length();
  if (length == 0) {
    return ble_mgr::sendLine(String(tag) + " " + String(id) + " ");
  }
  for (size_t offset = 0; offset < length; offset += B64_CHUNK_BYTES) {
    const size_t chunk = length - offset < B64_CHUNK_BYTES ? length - offset : B64_CHUNK_BYTES;
    const String encoded = encodeBase64((const uint8_t *)payload.c_str() + offset, chunk);
    if (encoded.length() == 0) return false;
    if (!ble_mgr::sendLine(String(tag) + " " + String(id) + " " + encoded)) return false;
  }
  return true;
}

// Same, but every line carries its sequence number: `<tag> <id> <seq> <b64>`.
bool sendChunkedSeq(const char *tag, uint16_t id, const String &payload) {
  const size_t length = payload.length();
  uint32_t seq = 0;
  for (size_t offset = 0; offset < length; offset += B64_CHUNK_BYTES) {
    const size_t chunk = length - offset < B64_CHUNK_BYTES ? length - offset : B64_CHUNK_BYTES;
    const String encoded = encodeBase64((const uint8_t *)payload.c_str() + offset, chunk);
    if (encoded.length() == 0) return false;
    if (!ble_mgr::sendLine(String(tag) + " " + String(id) + " " + String(seq) + " " + encoded)) {
      return false;
    }
    ++seq;
  }
  return true;
}

// -- parsing -----------------------------------------------------------------

// True when this frame belongs to the request we are actually waiting on. A
// late frame from a previous request (the phone finished a fetch we had already
// timed out on) mismatches and is dropped.
bool framePasses(const String &idToken) {
  if (sPhase != Phase::Awaiting) return false;
  const long id = idToken.toInt();
  return id > 0 && (uint16_t)id == sActiveId;
}

void handleRes(String &rest) {
  const String idToken = nextToken(rest);
  if (!framePasses(idToken)) return;
  if (sSawRes) return;  // duplicate %RES; the first one owns the buffer

  const int status = nextToken(rest).toInt();
  const long declared = nextToken(rest).toInt();
  const String ctToken = nextToken(rest);
  String flag = nextToken(rest);
  flag.toUpperCase();

  if (status <= 0 || declared < 0) {
    failNow("bridge protocol error (bad %RES)");
    return;
  }

  size_t capacity = (size_t)declared;
  if (capacity > RESP_CAP) capacity = RESP_CAP;

  if (capacity > 0) {
    sBuffer = (uint8_t *)ps_malloc(capacity);
    // A badge whose PSRAM did not train still has internal heap; try it rather
    // than failing the request outright.
    if (sBuffer == nullptr && capacity < ESP.getFreeHeap() / 2) {
      sBuffer = (uint8_t *)malloc(capacity);
    }
    if (sBuffer == nullptr) {
      failNow("out of memory");
      return;
    }
  }
  sCapacity = capacity;
  sLength = 0;
  sNextSeq = 0;
  sStatus = status;
  sSawRes = true;
  sTruncated = (flag == "TRUNC");
  if (!decodeBase64(ctToken, sContentType)) sContentType = "";
  if (sTruncated) {
    badge_log::tagf("bridge", "response truncated to %u bytes", (unsigned)capacity);
  }
}

void handleData(String &rest) {
  const String idToken = nextToken(rest);
  if (!framePasses(idToken)) return;
  if (!sSawRes) {
    failNow("bridge protocol error (%DATA before %RES)");
    return;
  }

  const String seqToken = nextToken(rest);
  const long seq = seqToken.toInt();
  if (seq < 0 || (uint32_t)seq != sNextSeq) {
    failNow("bridge sequence error");
    return;
  }
  ++sNextSeq;

  const String payload = nextToken(rest);
  if (payload.length() == 0) return;

  const size_t room = sCapacity - sLength;
  if (room == 0) {
    // Past RESP_CAP already: the phone is streaming more than it promised. Keep
    // what we have and mark it.
    sTruncated = true;
    return;
  }

  size_t decoded = 0;
  const int result = mbedtls_base64_decode(sBuffer + sLength, room, &decoded,
                                           (const unsigned char *)payload.c_str(),
                                           payload.length());
  if (result == 0) {
    sLength += decoded;
    return;
  }
  if (result != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    failNow("bridge protocol error (bad base64)");
    return;
  }

  // The chunk straddles the cap. Decode it somewhere else and keep the prefix
  // that fits, so the body is exactly RESP_CAP bytes rather than short by a
  // chunk.
  const size_t capacity = (payload.length() / 4 + 1) * 3 + 4;
  uint8_t *scratch = (uint8_t *)malloc(capacity);
  if (scratch == nullptr) {
    sTruncated = true;
    sLength = sCapacity;
    return;
  }
  size_t got = 0;
  if (mbedtls_base64_decode(scratch, capacity, &got, (const unsigned char *)payload.c_str(),
                            payload.length()) != 0) {
    free(scratch);
    failNow("bridge protocol error (bad base64)");
    return;
  }
  const size_t keep = got < room ? got : room;
  memcpy(sBuffer + sLength, scratch, keep);
  free(scratch);
  sLength += keep;
  sTruncated = true;
}

void handleEnd(String &rest) {
  const String idToken = nextToken(rest);
  if (!framePasses(idToken)) return;
  if (!sSawRes) {
    failNow("bridge protocol error (%END before %RES)");
    return;
  }

  sResult = Result();
  sResult.ok = true;
  sResult.status = sStatus;
  sResult.truncated = sTruncated;
  sResult.contentType = sContentType;
  if (sLength > 0) {
    // One copy out of PSRAM into the String the caller gets. The PSRAM buffer is
    // released immediately afterwards so the two never both sit at full size.
    if (!sResult.body.reserve(sLength) || !sResult.body.concat((const char *)sBuffer, sLength)) {
      clearTransfer();
      sResult = Result();
      sResult.err = "out of memory";
      sPhase = Phase::Failed;
      return;
    }
  }
  clearTransfer();
  sPhase = Phase::Complete;
}

void handleErr(String &rest) {
  const String idToken = nextToken(rest);
  if (!framePasses(idToken)) return;
  String message;
  if (!decodeBase64(nextToken(rest), message) || message.length() == 0) {
    message = "bridge error";
  }
  failNow(message);
}

// Teardown for a link that went away, run on the main loop.
void serviceLinkLoss() {
  sLinkLost = false;
  sEnabled = false;
  if (sPhase == Phase::Awaiting) {
    failNow("bridge disconnected");
  } else {
    clearTransfer();
  }
}

}  // namespace

// ---------------------------------------------------------------------------

void setEnabled(bool on) {
  // Settle a disconnect the BLE task flagged but the main loop has not serviced
  // yet. Without this, a phone that drops and reconnects inside one loop tick
  // could get "OK bridge ready" and then have the deferred teardown turn the
  // claim straight back off underneath it.
  if (sLinkLost) serviceLinkLoss();
  if (on == sEnabled) return;
  sEnabled = on;
  if (!on) {
    if (sPhase == Phase::Awaiting) {
      failNow("bridge turned off");
    } else {
      clearTransfer();
    }
  }
  badge_log::tagf("bridge", on ? "on - HTTP tunnels through the phone (phone terminates TLS)"
                               : "off");
}

bool enabled() { return sEnabled; }

bool ready() { return sEnabled && ble_mgr::connected() && !sLinkLost; }

uint16_t begin(const String &method, const String &url, const String &body,
               const String &contentType, uint32_t timeoutMs, const Header *headers,
               size_t headerCount) {
  if (!ready()) return 0;
  if (sPhase == Phase::Awaiting) return 0;  // one in flight at a time, by design

  clearTransfer();
  sResult = Result();

  const uint16_t id = sNextId;
  // Wraps at 65535 and skips 0, which is reserved for "no request".
  sNextId = (sNextId == 65535) ? 1 : (uint16_t)(sNextId + 1);
  sActiveId = id;
  sPhase = Phase::Awaiting;

  const String head = "%REQ " + String(id) + " " + method + " " + String(timeoutMs) + " " +
                      String((unsigned)body.length());
  bool ok = ble_mgr::sendLine(head);
  if (ok) ok = sendChunked("%URL", id, url);
  if (ok && contentType.length() > 0) ok = sendChunked("%CT", id, contentType);
  for (size_t i = 0; ok && i < headerCount; ++i) {
    const String name = encodeBase64((const uint8_t *)headers[i].name.c_str(),
                                     headers[i].name.length());
    const String value = encodeBase64((const uint8_t *)headers[i].value.c_str(),
                                      headers[i].value.length());
    if (name.length() == 0) {
      ok = false;
      break;
    }
    ok = ble_mgr::sendLine("%HDR " + String(id) + " " + name + " " + value);
  }
  if (ok && body.length() > 0) ok = sendChunkedSeq("%BODY", id, body);
  if (ok) ok = ble_mgr::sendLine("%SEND " + String(id));

  if (!ok) {
    // Nothing was ever handed to the phone, so there is no result to report -
    // just go back to idle and let the caller decide what to say.
    badge_log::tagf("bridge", "request %u could not be sent", (unsigned)id);
    clearTransfer();
    sResult = Result();
    sPhase = Phase::Idle;
    sActiveId = 0;
    return 0;
  }
  return id;
}

void pump() {
  if (sLinkLost) {
    serviceLinkLoss();
    return;
  }
  // Drains ONLY '%' frames. App and push-protocol lines stay in their own queue,
  // in order, so nothing here re-enters runtime::dispatchBle() - which would
  // call armDeadline() and wipe the extension budget of the very Lua callback
  // that is blocked waiting for us.
  ble_mgr::pumpBridge();
  if (sLinkLost) serviceLinkLoss();
}

bool done(uint16_t id) {
  if (sActiveId != id) return true;
  return sPhase == Phase::Complete || sPhase == Phase::Failed;
}

const Result &result() { return sResult; }

Result take() {
  Result out = std::move(sResult);
  sResult = Result();
  clearTransfer();
  sPhase = Phase::Idle;
  sActiveId = 0;
  return out;
}

void abort(const char *reason) {
  if (sPhase != Phase::Awaiting) return;
  failNow(String(reason));
}

void update() {
  if (sLinkLost) serviceLinkLoss();
}

void reset() {
  if (sLinkLost) sLinkLost = false;
  if (sPhase == Phase::Awaiting) {
    failNow("bridge closed");
  } else {
    clearTransfer();
  }
  sResult = Result();
  sPhase = Phase::Idle;
  sActiveId = 0;
  if (sEnabled) {
    sEnabled = false;
    badge_log::tagf("bridge", "off");
  }
}

void linkLost() {
  // Deliberately does no work beyond two scalar writes: this runs on the BLE
  // stack task, and freeing the response buffer here could pull it out from
  // under a main-loop request that is mid-assembly.
  if (!sEnabled && sPhase == Phase::Idle) return;
  sLinkLost = true;
}

void onLine(const String &line) {
  if (!sEnabled) return;

  String rest = line;
  rest.trim();
  if (rest.length() == 0) return;

  const String frame = nextToken(rest);
  if (frame == "%RES") {
    handleRes(rest);
  } else if (frame == "%DATA") {
    handleData(rest);
  } else if (frame == "%END") {
    handleEnd(rest);
  } else if (frame == "%ERR") {
    handleErr(rest);
  }
  // Anything else is a frame from a future protocol version. Dropped silently -
  // it is not app traffic and the phone gets no reply channel here.
}

}  // namespace ble_bridge
