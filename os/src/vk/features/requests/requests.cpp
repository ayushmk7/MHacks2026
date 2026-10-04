// Payment requests: the payee's active requests and the payer's request cache
// (protocol/espnow.md, "Payment requests (payee side)", "Payee: answering CHAL", "Request cache").
//
// The first part of this file, compiled only for the badge, fills `hooks` with the real clock,
// signer, verifier, radio, config store and inbox, and registers what the feature adds: six config
// keys, the permission `request`, the routes for REQ and CHAL, the service and the app-stop
// listener. The rest is the table logic. It touches nothing outside this file except through
// `hooks` and the pure frame codec, so test/host/test_requests.cpp drives it with fakes.
#include "requests.h"

#include <string.h>

#ifndef VK_HOST_TEST
#include <Arduino.h>

#include "../../../badge_log.h"
#include "../../../lua_sdk/lua_runtime.h"   // runtime::running, runtime::currentApp
#include "../../core/clock.h"
#include "../../core/config.h"
#include "../../core/service.h"
#include "../../host/lifecycle.h"
#include "../../host/notify.h"
#include "../../host/permissions.h"
#include "../../host/router.h"
#include "../../wallet/crypto.h"
#include "../../wallet/signer.h"
#define VK_REQ_LOG(...) badge_log::tagf("req", __VA_ARGS__)
#else
#define VK_REQ_LOG(...) ((void)0)
#endif

namespace vk::requests {

// The longest notification body that is shown whole: notify::Note::body is 40 bytes.
constexpr size_t NOTE_BODY_CHARS = 39;

// ---------------------------------------------------------------------------
// Firmware glue: the real hooks and the registrations
// ---------------------------------------------------------------------------

#ifndef VK_HOST_TEST
namespace {

static_assert(sizeof(vk::host::notify::Note::body) == NOTE_BODY_CHARS + 1, "notify::Note::body changed size");

uint32_t fwNowMs() { return (uint32_t)millis(); }

bool fwAppRunning(const char *app_id) {
  return runtime::running() && app_id != nullptr && runtime::currentApp() == app_id;
}

// A Solana request names a token of the table by its symbol; a bank request is in cents.
int fwDecimals(uint8_t rail, const char *currency) {
  if (rail == VK_RAIL_BANK) return 2;
  if (currency == nullptr) return -1;
  vk_token_t tokens[VK_MAX_TOKENS];
  const size_t count = vk::config::tokens(tokens);
  for (size_t i = 0; i < count && i < VK_MAX_TOKENS; ++i) {
    if (strcmp(tokens[i].symbol, currency) == 0) return tokens[i].decimals;
  }
  return -1;
}

bool fwPayAppRunning() {
  return runtime::running() && runtime::currentApp() == vk::config::text("pay_app");
}

void fwNotify(const char *title, const char *body) {
  const String payApp = vk::config::text("pay_app");
  vk::host::notify::post(title, body, payApp.c_str());
}

}  // namespace

// Filled by constant initialisation, so the hooks are in place before any service, route or
// signing-domain validator can run, whatever the order of the registries.
Hooks hooks = {
    fwNowMs,
    vk::clock::ok,
    vk::clock::now,
    vk::config::provisioned,
    vk::wallet::publicKey,
    vk::config::u32,
    vk::wallet::randomBytes,
    vk::wallet::signAuto,
    vk_verify_c,
    vk::host::router::send,
    fwAppRunning,
    fwDecimals,
    fwPayAppRunning,
    fwNotify,
};

namespace {

bool routeReq(const uint8_t mac[6], const uint8_t *frame, size_t len, int8_t rssi, uint32_t rx_ms) {
  return onReq(mac, frame, len, rssi, rx_ms);
}

bool routeChal(const uint8_t mac[6], const uint8_t *frame, size_t len, int8_t rssi, uint32_t rx_ms) {
  (void)rssi; (void)rx_ms;
  return onChal(mac, frame, len);
}

void appStopping(const char *appId) { closeForApp(appId); }

VK_SERVICE(requests, nullptr, update);
VK_ESPNOW_ROUTE(pay_req, VK_T_REQ, VK_T_REQ, routeReq);
VK_ESPNOW_ROUTE(pay_chal, VK_T_CHAL, VK_T_CHAL, routeChal);
VK_ON_APP_STOP(requests, appStopping);

VK_PERMISSION(request, "request", "ask others to pay this badge", true, nullptr);

// platform/config.md, Keys. Only presence_ms is secure: raising it weakens the presence check.
VK_CONFIG_KEY(presence_ms, "presence_ms", vk::config::Type::U32, "1500", vk::config::F_SECURE, 50, 5000,
              "CHAL to PROOF deadline, ms");
VK_CONFIG_KEY(req_ttl_s, "req_ttl_s", vk::config::Type::U32, "60", vk::config::F_NONE, 10, 600,
              "lifetime of a payment request, s");
VK_CONFIG_KEY(req_period_ms, "req_period_ms", vk::config::Type::U32, "1000", vk::config::F_NONE, 250, 5000,
              "REQ rebroadcast period, ms");
VK_CONFIG_KEY(req_max_proofs, "req_max_proofs", vk::config::Type::U32, "8", vk::config::F_NONE, 1, 64,
              "presence proofs answered per request");
VK_CONFIG_KEY(req_gap_ms, "req_gap_ms", vk::config::Type::U32, "200", vk::config::F_NONE, 0, 5000,
              "minimum time between presence proofs, ms");
VK_CONFIG_KEY(pay_app, "pay_app", vk::config::Type::STR, "pay", vk::config::F_NONE, 1, 24,
              "app opened from a payment-request notification");

}  // namespace
#else
Hooks hooks = {};
#endif

// ---------------------------------------------------------------------------
// Hook wrappers: a null hook means "not available"
// ---------------------------------------------------------------------------

namespace {

uint32_t nowMs() { return hooks.nowMs ? hooks.nowMs() : 0; }
bool clockOk() { return hooks.clockOk && hooks.clockOk(); }
uint32_t unixNow() { return hooks.unixNow ? hooks.unixNow() : 0; }
uint32_t configU32(const char *key) { return hooks.configU32 ? hooks.configU32(key) : 0; }
const uint8_t *ownKey() { return hooks.ownKey ? hooks.ownKey() : nullptr; }

bool sameId(const uint8_t a[8], const uint8_t b[8]) { return memcmp(a, b, 8) == 0; }

// ---------------------------------------------------------------------------
// Payee: the active requests
// ---------------------------------------------------------------------------

struct Active {
  bool used;
  uint8_t req_id[8];
  uint8_t frame[VK_REQ_MAX_LEN];
  size_t frame_len;
  uint32_t expiry;              // unix seconds
  char app_id[APP_ID_MAX + 1];
  uint32_t proofs;              // PROOFs signed for it
  bool proof_tried;             // last_proof_ms is meaningful
  uint32_t last_proof_ms;       // when the last signature for it finished
  bool sent;                    // broadcast at least once
  uint32_t last_sent_ms;
};
Active sActive[MAX_ACTIVE];

// The last few requests that closed, so request_status can still answer "closed" or "expired".
constexpr size_t MAX_CLOSED = 4;
struct ClosedEntry {
  bool used;
  uint8_t req_id[8];
  State state;
  uint32_t proofs;
  uint32_t order;               // higher = closed later
};
ClosedEntry sClosed[MAX_CLOSED];
uint32_t sClosedOrder = 0;

Active *findActive(const uint8_t req_id[8]) {
  for (Active &a : sActive) {
    if (a.used && sameId(a.req_id, req_id)) return &a;
  }
  return nullptr;
}

void closeSlot(Active &a, State state) {
  ClosedEntry *target = &sClosed[0];
  for (ClosedEntry &c : sClosed) {
    if (!c.used) { target = &c; break; }
    if (c.order < target->order) target = &c;
  }
  target->used = true;
  memcpy(target->req_id, a.req_id, 8);
  target->state = state;
  target->proofs = a.proofs;
  target->order = ++sClosedOrder;
  memset(&a, 0, sizeof a);
}

// "An active request closes ... at its expiry."
void expireDue() {
  if (!clockOk()) return;
  const uint32_t now = unixNow();
  for (Active &a : sActive) {
    if (a.used && now >= a.expiry) closeSlot(a, State::Expired);
  }
}

bool printableText(const char *text, size_t minLen, size_t maxLen, size_t &lenOut) {
  if (text == nullptr) return false;
  size_t n = 0;
  for (; text[n] != '\0'; ++n) {
    if (n >= maxLen) return false;
    const unsigned char c = (unsigned char)text[n];
    if (c < 0x20 || c > 0x7E) return false;
  }
  if (n < minLen) return false;
  lenOut = n;
  return true;
}

}  // namespace

size_t activeCount() {
  size_t n = 0;
  for (const Active &a : sActive) n += a.used ? 1 : 0;
  return n;
}

bool isActive(const uint8_t req_id[8]) { return req_id != nullptr && findActive(req_id) != nullptr; }

vk::wallet::Reason canOpen() {
  if (!(hooks.provisioned && hooks.provisioned())) return VK_NOT_PROVISIONED;
  if (!clockOk()) return VK_NO_TIME;       // the expiry is a real time
  expireDue();
  if (activeCount() >= MAX_ACTIVE) return VK_BUSY;
  return VK_OK;
}

vk::wallet::Reason openRequest(const OpenArgs &args, Opened &out) {
  memset(&out, 0, sizeof out);
  const vk::wallet::Reason refused = canOpen();
  if (refused != VK_OK) return refused;

  // ---- arguments
  size_t currencyLen = 0, nameLen = 0, appLen = 0;
  if (args.rail != VK_RAIL_SOLANA && args.rail != VK_RAIL_BANK) return VK_BAD_ARG;
  if (args.amount == 0) return VK_BAD_ARG;
  if (!printableText(args.currency, 1, 4, currencyLen)) return VK_BAD_ARG;
  if (!printableText(args.name, 1, VK_NAME_MAX, nameLen)) return VK_BAD_ARG;
  const char *appId = args.app_id ? args.app_id : "";
  if (!printableText(appId, 0, APP_ID_MAX, appLen)) return VK_BAD_ARG;
  const uint32_t ttl = args.ttl_s ? args.ttl_s : configU32("req_ttl_s");
  const uint32_t now = unixNow();
  if (ttl == 0 || ttl > UINT32_MAX - now) return VK_BAD_ARG;

  const uint8_t *own = ownKey();
  if (own == nullptr || hooks.randomBytes == nullptr || hooks.signAuto == nullptr) return VK_SIGN_FAILED;

  // ---- the frame
  vk_req_t req;
  memset(&req, 0, sizeof req);
  req.rail = args.rail;
  memcpy(req.payee_pubkey, own, 32);
  req.amount = args.amount;
  memcpy(req.currency, args.currency, currencyLen);
  req.expiry = now + ttl;
  req.name_len = (uint8_t)nameLen;
  memcpy(req.name, args.name, nameLen);
  // A fresh id. One this badge is still using is drawn again (a 1 in 2^64 event, or a broken RNG).
  for (int attempt = 0; attempt < 4; ++attempt) {
    hooks.randomBytes(req.req_id, sizeof req.req_id);
    if (findActive(req.req_id) == nullptr) break;
  }
  if (findActive(req.req_id) != nullptr) return VK_SIGN_FAILED;

  // Built once with a zero signature to get the bytes to sign, then the signature is put in place.
  uint8_t frame[VK_REQ_MAX_LEN];
  const size_t frameLen = vk_req_build(&req, frame, sizeof frame);
  if (frameLen == 0) return VK_BAD_ARG;
  const size_t signedLen = frameLen - 64;
  uint8_t sig[64];
  if (hooks.signAuto("pay-req", frame, signedLen, sig) != VK_OK) return VK_SIGN_FAILED;
  memcpy(frame + signedLen, sig, 64);

  // ---- store it. canOpen() found a free slot.
  for (Active &a : sActive) {
    if (a.used) continue;
    memset(&a, 0, sizeof a);
    a.used = true;
    memcpy(a.req_id, req.req_id, 8);
    memcpy(a.frame, frame, frameLen);
    a.frame_len = frameLen;
    a.expiry = req.expiry;
    memcpy(a.app_id, appId, appLen);
    a.app_id[appLen] = '\0';

    memcpy(out.req_id, req.req_id, 8);
    memcpy(out.frame, frame, frameLen);
    out.frame_len = frameLen;
    out.expiry = req.expiry;
    return VK_OK;
  }
  return VK_BUSY;
}

bool closeRequest(const uint8_t req_id[8]) {
  if (req_id == nullptr) return false;
  expireDue();                       // one that ran out is "expired", not "closed"
  Active *a = findActive(req_id);
  if (a == nullptr) return false;
  closeSlot(*a, State::Closed);
  return true;
}

void closeForApp(const char *app_id) {
  if (app_id == nullptr) app_id = "";
  for (Active &a : sActive) {
    if (a.used && strcmp(a.app_id, app_id) == 0) closeSlot(a, State::Closed);
  }
}

bool requestStatus(const uint8_t req_id[8], State &state, uint32_t &proofs) {
  if (req_id == nullptr) return false;
  expireDue();
  if (const Active *a = findActive(req_id)) {
    state = State::Open;
    proofs = a->proofs;
    return true;
  }
  const ClosedEntry *found = nullptr;
  for (const ClosedEntry &c : sClosed) {
    if (c.used && sameId(c.req_id, req_id) && (found == nullptr || c.order > found->order)) found = &c;
  }
  if (found == nullptr) return false;
  state = found->state;
  proofs = found->proofs;
  return true;
}

const char *stateName(State state) {
  switch (state) {
    case State::Open:    return "open";
    case State::Closed:  return "closed";
    case State::Expired: return "expired";
  }
  return "closed";
}

// ---------------------------------------------------------------------------
// Payee: answering CHAL
// ---------------------------------------------------------------------------

bool onChal(const uint8_t mac[6], const uint8_t *frame, size_t len) {
  vk_chal_t chal;
  if (mac == nullptr || vk_chal_parse(frame, len, &chal) != 0) return true;

  // 1. The request must be one of ours, and still open.
  expireDue();
  Active *a = findActive(chal.req_id);
  if (a == nullptr) return true;

  // 2. Rate limits per request: a total, and a gap since the last signature finished.
  if (a->proofs >= configU32("req_max_proofs")) return true;
  if (a->proof_tried && (uint32_t)(nowMs() - a->last_proof_ms) < configU32("req_gap_ms")) return true;

  // 3. Sign req_id ‖ nonce ‖ payer_pubkey and answer the challenger. The loop is blocked for this
  //    one signature; that is the latency the payer measures.
  uint8_t bytes[VK_PROOF_SIGNED_LEN];
  vk_proof_signed_bytes(chal.req_id, chal.nonce, chal.payer_pubkey, bytes);
  vk_proof_t proof;
  memcpy(proof.req_id, chal.req_id, 8);
  const vk::wallet::Reason signedOk =
      hooks.signAuto ? hooks.signAuto("pay-proof", bytes, sizeof bytes, proof.sig) : VK_SIGN_FAILED;
  a->proof_tried = true;
  a->last_proof_ms = nowMs();
  if (signedOk != VK_OK) {
    VK_REQ_LOG("proof not signed: %s", vk_reason_name(signedOk));
    return true;
  }
  a->proofs++;

  uint8_t out[VK_PROOF_LEN];
  const size_t n = vk_proof_build(&proof, out, sizeof out);
  if (n == 0 || hooks.sendFrame == nullptr || !hooks.sendFrame(mac, out, n)) VK_REQ_LOG("proof not sent");
  return true;
}

// ---------------------------------------------------------------------------
// The signing domains' validators
// ---------------------------------------------------------------------------

bool validReqBytes(const uint8_t *bytes, size_t len, const uint8_t *own_key) {
  if (bytes == nullptr || own_key == nullptr) return false;
  if (len < VK_REQ_MIN_LEN - 64 || len > VK_REQ_SIGNED_MAX) return false;
  // The codec parses whole frames, so the bytes are completed with a zero signature.
  uint8_t frame[VK_REQ_MAX_LEN];
  memset(frame, 0, sizeof frame);
  memcpy(frame, bytes, len);
  vk_req_t req;
  if (vk_req_parse(frame, len + 64, &req) != 0) return false;
  if (req.signed_len != len) return false;
  return memcmp(req.payee_pubkey, own_key, 32) == 0;
}

bool validProofBytes(const uint8_t *bytes, size_t len) {
  if (bytes == nullptr || len != VK_PROOF_SIGNED_LEN) return false;
  expireDue();
  return findActive(bytes) != nullptr;     // the first 8 bytes are the req_id
}

// ---------------------------------------------------------------------------
// Payer: the request cache
// ---------------------------------------------------------------------------

namespace {

struct CacheSlot {
  bool used;
  Cached entry;
};
CacheSlot sCache[MAX_CACHED];

// "An entry is dropped at its expiry or 30 s after it was last heard."
void pruneCache() {
  const bool timed = clockOk();
  const uint32_t unixS = timed ? unixNow() : 0;
  const uint32_t ms = nowMs();
  for (CacheSlot &slot : sCache) {
    if (!slot.used) continue;
    const bool expired = timed && unixS >= slot.entry.req.expiry;
    const bool silent = (uint32_t)(ms - slot.entry.heard_ms) >= CACHE_SILENCE_MS;
    if (expired || silent) slot.used = false;
  }
}

// "<name> <amount> <currency>", with the name shortened so the amount is never cut off.
void postNotification(const vk_req_t &req) {
  if (hooks.notify == nullptr) return;
  if (hooks.payAppRunning && hooks.payAppRunning()) return;

  char amount[24];
  if (formatAmount(req.rail, req.currency, req.amount, amount, sizeof amount) == 0) amount[0] = '\0';
  const size_t tail = 1 + strlen(amount) + 1 + strlen(req.currency);      // " <amount> <currency>"
  size_t nameLen = req.name_len;
  if (tail >= NOTE_BODY_CHARS) nameLen = 0;
  else if (nameLen > NOTE_BODY_CHARS - tail) nameLen = NOTE_BODY_CHARS - tail;

  char body[VK_NAME_MAX + 1 + sizeof amount + 1 + sizeof req.currency + 1];
  size_t at = 0;
  memcpy(body, req.name, nameLen);
  at += nameLen;
  if (nameLen) body[at++] = ' ';
  memcpy(body + at, amount, strlen(amount));
  at += strlen(amount);
  body[at++] = ' ';
  memcpy(body + at, req.currency, strlen(req.currency));
  at += strlen(req.currency);
  body[at] = '\0';
  hooks.notify("Payment request", body);
}

}  // namespace

size_t formatAmount(uint8_t rail, const char *currency, uint64_t raw, char *out, size_t cap) {
  if (out == nullptr || cap == 0) return 0;
  const int known = hooks.decimals ? hooks.decimals(rail, currency) : -1;
  const uint8_t decimals = (known >= 0 && known <= 19) ? (uint8_t)known : 0;
  const size_t n = sol_format_amount(raw, decimals, out, cap);
  if (n == 0) out[0] = '\0';
  return n;
}

bool onReq(const uint8_t mac[6], const uint8_t *frame, size_t len, int8_t rssi, uint32_t rx_ms) {
  vk_req_t req;
  if (mac == nullptr || vk_req_parse(frame, len, &req) != 0) return false;
  if (clockOk() && unixNow() >= req.expiry) return false;      // already over: never cached
  pruneCache();

  CacheSlot *slot = nullptr;
  for (CacheSlot &s : sCache) {
    if (s.used && sameId(s.entry.req.req_id, req.req_id)) { slot = &s; break; }
  }
  const bool isNew = slot == nullptr;
  if (isNew) {
    // A free slot, else the one that has been silent longest.
    const uint32_t ms = nowMs();
    for (CacheSlot &s : sCache) {
      if (!s.used) { slot = &s; break; }
      if (slot == nullptr || (uint32_t)(ms - s.entry.heard_ms) > (uint32_t)(ms - slot->entry.heard_ms)) slot = &s;
    }
  }

  slot->used = true;
  memcpy(slot->entry.frame, frame, len);
  slot->entry.frame_len = len;
  slot->entry.req = req;
  memcpy(slot->entry.mac, mac, 6);
  slot->entry.rssi = rssi;
  slot->entry.heard_ms = rx_ms;

  if (isNew) postNotification(req);
  return false;                       // a running app gets the frame too
}

size_t cacheCount() {
  pruneCache();
  size_t n = 0;
  for (const CacheSlot &s : sCache) n += s.used ? 1 : 0;
  return n;
}

const Cached *cacheAt(size_t index) {
  for (const CacheSlot &s : sCache) {
    if (!s.used) continue;
    if (index == 0) return &s.entry;
    --index;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// The service
// ---------------------------------------------------------------------------

void update() {
  if (activeCount() != 0) {
    expireDue();
    // The app-stop listener closes an app's requests; this also catches one opened while the app
    // was already stopping (upstream runs the listeners before the app's on_stop).
    if (hooks.appRunning) {
      for (Active &a : sActive) {
        if (a.used && !hooks.appRunning(a.app_id)) closeSlot(a, State::Closed);
      }
    }
    if (activeCount() != 0) {
      const uint32_t period = configU32("req_period_ms");
      const uint32_t ms = nowMs();
      for (Active &a : sActive) {
        if (!a.used) continue;
        if (a.sent && (uint32_t)(ms - a.last_sent_ms) < period) continue;
        a.sent = true;
        a.last_sent_ms = ms;
        if (hooks.sendFrame) hooks.sendFrame(nullptr, a.frame, a.frame_len);   // the same bytes every time
      }
    }
  }
  pruneCache();
}

void reset() {
  memset(sActive, 0, sizeof sActive);
  memset(sClosed, 0, sizeof sClosed);
  sClosedOrder = 0;
  memset(sCache, 0, sizeof sCache);
}

}  // namespace vk::requests
