// Payment requests (protocol/espnow.md, "Payment requests (payee side)", "Payee: answering CHAL",
// "Request cache (payer side)"). Private to features/requests.
//
// Two tables live here:
//   - the active requests this badge has opened (payee side): built and signed once, rebroadcast
//     by the feature's service, and the only requests a CHAL is answered for;
//   - the cache of requests heard from other badges (payer side), which wallet.requests() lists.
// The presence slots (payer side of CHAL and PROOF) are in presence.h.
//
// Everything outside this feature is reached through `hooks`: time, the clock, random bytes, the
// signer, the verifier, the radio, the config store, the notification inbox. On the badge the
// hooks are filled when the firmware starts; the host suite test_requests fills them with fakes.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../../wallet/pure/vk_checks.h"   // vk_presence_t, vk_frames.h
#include "../../wallet/reason.h"

namespace vk::requests {

constexpr size_t MAX_ACTIVE = 2;                 // "At most 2 are active"
constexpr size_t MAX_CACHED = 8;                 // "the last 8 distinct requests seen"
constexpr uint32_t CACHE_SILENCE_MS = 30000;     // dropped 30 s after it was last heard
constexpr size_t APP_ID_MAX = 32;                // longest app id (app-host.md, Permissions)

// ---- limits against strangers in radio range (protocol/espnow.md, "Limits") ---------------------
// Payee: a CHAL is answered only within all of these, and config `req_max_proofs` (answers per
// challenger MAC per request) and `req_gap_ms` (from the end of one answer to the start of the
// next, whatever the request).
constexpr size_t CHAL_QUEUE_LEN = 4;             // CHALs waiting for an answer, all requests together
constexpr size_t CHALLENGERS_PER_REQUEST = 4;    // challenger MACs counted per request; the least recent is forgotten
constexpr size_t ANSWERED_NONCES = 8;            // nonces answered per request, remembered: a repeat is a replay
constexpr uint32_t ANSWER_BURST = 3;             // answers that may follow each other only `req_gap_ms` apart...
constexpr uint32_t ANSWER_REFILL_MS = 1000;      // ...after which one more per this, sustained
// Payer: the request cache.
constexpr uint32_t REQ_PERIOD_MAX_MS = 5000;     // the top of config `req_period_ms`'s range
constexpr uint32_t CACHE_MAC_HOLD_MS = 2 * REQ_PERIOD_MAX_MS;   // an entry's MAC changes only after this silence
constexpr uint32_t CACHE_VERIFY_GAP_MS = 100;    // at most one REQ signature check per this

// What the limits refused, since boot or reset(). For the log and the host tests.
struct Stats {
  uint32_t chal_replayed;     // a nonce already answered for that request
  uint32_t chal_over_budget;  // the challenger MAC had its `req_max_proofs`
  uint32_t chal_displaced;    // pushed out of a full queue by a newer CHAL
  uint32_t chal_stale;        // waited longer than `presence_ms`, or its request closed
  uint32_t req_bad_sig;       // a REQ whose own signature does not verify: not cached
  uint32_t req_deferred;      // a new REQ heard inside CACHE_VERIFY_GAP_MS: left for its next broadcast
  uint32_t req_conflict;      // same req_id and payee key as a cached entry, other bytes: ignored
};
const Stats &stats();

// ---- hooks ------------------------------------------------------------------------------------
// A null hook is treated as "not available": no time, not provisioned, no key, sign fails,
// verification fails, nothing is sent, nothing is posted.
struct Hooks {
  uint32_t (*nowMs)();                                   // millis()
  bool (*clockOk)();                                     // vk::clock::ok()
  uint32_t (*unixNow)();                                 // vk::clock::now()
  bool (*provisioned)();                                 // vk::config::provisioned()
  const uint8_t *(*ownKey)();                            // vk::wallet::publicKey(): 32 bytes or null
  uint32_t (*configU32)(const char *key);                // vk::config::u32()
  void (*randomBytes)(uint8_t *out, size_t len);         // vk::wallet::randomBytes()
  // vk::wallet::signAuto(): the domain's validator runs, then the one signing path.
  vk::wallet::Reason (*signAuto)(const char *domain, const uint8_t *bytes, size_t len, uint8_t sig[64]);
  int (*verify)(const uint8_t *msg, size_t len, const uint8_t *sig, const uint8_t *pubkey);   // vk_verify_c: 1 = valid
  bool (*sendFrame)(const uint8_t *mac, const uint8_t *frame, size_t len);   // router::send; null mac = broadcast
  bool (*appRunning)(const char *app_id);                // is that app the one running now?
  // Decimals of an amount in a request: the token table's for a Solana symbol, 2 for the bank
  // rail (cents); -1 when the currency is not known to this badge.
  int (*decimals)(uint8_t rail, const char *currency);
  bool (*payAppRunning)();                               // the running app is config `pay_app`
  void (*notify)(const char *title, const char *body);   // notify::post(title, body, <pay_app>)
};
extern Hooks hooks;

// ---- payee: active requests -------------------------------------------------------------------

struct OpenArgs {
  uint8_t rail = VK_RAIL_SOLANA;     // VK_RAIL_SOLANA or VK_RAIL_BANK
  uint64_t amount = 0;               // raw units; must not be 0
  const char *currency = nullptr;    // 1..4 printable ASCII characters
  const char *name = nullptr;        // 1..32 printable ASCII characters (the claim in the frame)
  uint32_t ttl_s = 0;                // 0 = config `req_ttl_s`
  const char *app_id = nullptr;      // the app the request belongs to; null = ""
};

struct Opened {
  uint8_t req_id[8];
  uint8_t frame[VK_REQ_MAX_LEN];     // the signed REQ, as broadcast
  size_t frame_len;
  uint32_t expiry;                   // unix seconds
};

// The refusals that do not depend on the arguments: VK_NOT_PROVISIONED, VK_NO_TIME, VK_BUSY
// (two are already open). VK_OK when a request could be opened now.
vk::wallet::Reason canOpen();

// Builds the REQ (payee = this badge, random req_id, expiry = now + ttl), signs it once with the
// domain "pay-req" and stores it as an active request. Reasons: those of canOpen(), then
// VK_BAD_ARG, then VK_SIGN_FAILED.
vk::wallet::Reason openRequest(const OpenArgs &args, Opened &out);

bool closeRequest(const uint8_t req_id[8]);          // true if it was active
void closeForApp(const char *app_id);                // the VK_ON_APP_STOP listener's work

enum class State : uint8_t { Open, Closed, Expired };
// False for a req_id this badge does not remember. The last few closed requests are remembered,
// so an app can tell "closed" from "expired".
bool requestStatus(const uint8_t req_id[8], State &state, uint32_t &proofs);
const char *stateName(State state);                  // "open", "closed", "expired"

size_t activeCount();
bool isActive(const uint8_t req_id[8]);

// The feature's service, once per loop: closes requests at their expiry and requests whose app is
// no longer the running one, answers at most one queued CHAL, broadcasts each active request every
// `req_period_ms`, prunes the cache.
void update();

// Route for type 2 (CHAL). Drops a CHAL for a request that is not open, a replayed nonce, or a
// challenger over its budget; queues the rest (CHAL_QUEUE_LEN, one entry per request and MAC, a
// newer CHAL replacing it) and answers the newest queued CHAL now if the rate limits allow, else
// from update(). An answer signs "pay-proof" and unicasts PROOF to the challenger's MAC. `rx_ms`
// is when the radio received it; the 3-argument form uses the current time. Always consumes it.
bool onChal(const uint8_t mac[6], const uint8_t *frame, size_t len, uint32_t rx_ms);
bool onChal(const uint8_t mac[6], const uint8_t *frame, size_t len);

// ---- the two signing domains' validators ------------------------------------------------------

// "pay-req": `bytes` is a REQ frame from its header to the end of the name (63..94 bytes) that
// parses, and its payee_pubkey is `own_key`.
bool validReqBytes(const uint8_t *bytes, size_t len, const uint8_t *own_key);
// "pay-proof": exactly 56 bytes (req_id, nonce, payer key) whose req_id is an active request.
bool validProofBytes(const uint8_t *bytes, size_t len);

// ---- payer: request cache ---------------------------------------------------------------------

struct Cached {
  uint8_t frame[VK_REQ_MAX_LEN];     // the raw frame, to pass as ctx.req
  size_t frame_len;
  vk_req_t req;                      // the same frame, parsed. Its signature verifies with its own
                                     // payee_pubkey; nothing says that key is anyone in particular
  uint8_t mac[6];                    // who to challenge: the first MAC it was heard from, until that
                                     // MAC has been silent for CACHE_MAC_HOLD_MS
  int8_t rssi;                       // as heard from `mac`
  uint32_t heard_ms;                 // when the radio last received it, from any MAC
  uint32_t mac_heard_ms;             // when the radio last received it from `mac`
};

// Route for type 1 (REQ). An entry is keyed by req_id and payee key. A new one is cached (and
// announced) only if its signature verifies with the payee key in it, and at most one such check
// runs per CACHE_VERIFY_GAP_MS. A frame with a cached key but other bytes is ignored: the payee
// sends the same bytes every time. Always returns false: a running app gets the frame too.
bool onReq(const uint8_t mac[6], const uint8_t *frame, size_t len, int8_t rssi, uint32_t rx_ms);

size_t cacheCount();                             // prunes first
const Cached *cacheAt(size_t index);             // 0 .. cacheCount()-1; null past the end

// "<amount>" in display units, using hooks.decimals; the raw number when the currency is unknown.
// Returns the characters written, 0 if `cap` is too small.
size_t formatAmount(uint8_t rail, const char *currency, uint64_t raw, char *out, size_t cap);

// Forgets every active, closed, queued and cached request, the rate-limit state and the stats.
// Does not touch `hooks`.
void reset();

}  // namespace vk::requests
