// src/vk/features/balance/balance.h
// The balance feature (ui/ui.md, "Balance"): the last known balance and token account of the default
// (first) token, learned from one getTokenAccountsByOwner call to `rpc_url`. The rest of the firmware
// reaches it through vk::wallet::tokenInfoLookup and never includes this header.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../wallet/pure/sol.h"   // sol_b58_decode
#include "../../wallet/reason.h"

namespace vk::balance {

bool known();
uint64_t raw();
bool tokenAccount(uint8_t out[32]);
bool refresh(uint32_t timeoutMs);      // one blocking fetch

// ---- Added by WP33 (balance.cpp). The four declarations above are the spec's. ----

constexpr uint32_t FETCH_TIMEOUT_MS = 3000;

// refresh() with the reason for a failure, for wallet.refresh_balance:
//   VK_OK               a balance and a token account were stored
//   VK_NOT_PROVISIONED  no token table, no rpc_url or no badge key
//   VK_TIMEOUT          no network, or the request failed or timed out
//   VK_UNSUPPORTED      the node answered, but with no token account for this badge
vk::wallet::Reason fetch(uint32_t timeoutMs);

// ---- Added for the non-blocking poll (ui.md, "Balance"). ----
//
// The service's poll runs on a background task: the loop never waits for the network. These tell a
// screen how much to trust the balance it shows (the launcher reaches the balance itself through
// vk::wallet::tokenInfoLookup, which is unchanged: the last balance stays known while it is stale).
enum class Freshness : uint8_t {
  NONE,      // joined to a network, no balance fetched yet
  FRESH,     // the last poll succeeded and the balance is recent
  STALE,     // a balance is known, but the last poll failed or it is older than three poll periods
  OFFLINE,   // not joined to a network: nothing can be fetched
};

struct Status {
  Freshness freshness;
  uint32_t ageMs;          // since the stored balance was fetched; 0 when there is none
  uint32_t failures;       // consecutive failed polls of the service
  uint32_t nextPollInMs;   // until the service's next poll; 0 when one is due or running
  bool polling;            // a background poll is in flight
};
Status status();

// Pure, for the host suite test_balance.

inline const char *freshnessName(Freshness f) {
  switch (f) {
    case Freshness::NONE: return "none";
    case Freshness::FRESH: return "fresh";
    case Freshness::STALE: return "stale";
    case Freshness::OFFLINE: return "offline";
  }
  return "?";
}

// Wait before the next poll: the period after a success; after n consecutive failures the period
// times 2^n, capped at max_s (never below the period). period_s 0 (poll off) gives 0.
inline uint32_t retryDelayMs(uint32_t period_s, uint32_t failures, uint32_t max_s) {
  if (period_s == 0) return 0;
  const uint64_t cap = (uint64_t)(max_s > period_s ? max_s : period_s) * 1000u;
  uint64_t delay = (uint64_t)period_s * 1000u;
  for (uint32_t i = 0; i < failures && delay < cap; ++i) delay *= 2;
  return (uint32_t)(delay < cap ? delay : cap);
}

inline Freshness freshness(bool joined, bool known, uint32_t ageMs, uint32_t failures, uint32_t period_s) {
  if (!joined) return Freshness::OFFLINE;
  if (!known) return Freshness::NONE;
  if (failures > 0) return Freshness::STALE;
  if (period_s != 0 && (uint64_t)ageMs > (uint64_t)period_s * 3000u) return Freshness::STALE;
  return Freshness::FRESH;
}

// The reply scanner. Pure and in the header so that the host suite can run it without linking
// balance.cpp, which needs the network.
namespace scan {

inline bool blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Copies the first string value whose key is `key` (written with its quotes: "\"pubkey\"") into
// `out`: `"key" : "value"`. False when there is none, or when it is empty or does not fit.
// An occurrence of the key that is not followed by a string value is skipped.
inline bool stringValue(const char *text, size_t length, const char *key, char *out, size_t cap) {
  const size_t keyLength = strlen(key);
  for (size_t i = 0; i + keyLength <= length; ++i) {
    if (memcmp(text + i, key, keyLength) != 0) continue;
    size_t at = i + keyLength;
    while (at < length && blank(text[at])) ++at;
    if (at >= length || text[at] != ':') continue;
    ++at;
    while (at < length && blank(text[at])) ++at;
    if (at >= length || text[at] != '"') continue;
    ++at;
    size_t n = 0;
    while (at + n < length && text[at + n] != '"') ++n;
    if (at + n >= length) return false;              // the value never ends: a cut reply
    if (n == 0 || n >= cap) return false;
    memcpy(out, text + at, n);
    out[n] = '\0';
    return true;
  }
  return false;
}

}  // namespace scan

// Scans a getTokenAccountsByOwner reply (encoding jsonParsed) for the first account's address
// (`"pubkey":"<base58>"`) and balance (`"amount":"<digits>"`, inside tokenAmount). A string search,
// not a JSON parser; the two fields may come in either order. False, with nothing written, unless
// both are present and well formed: an error reply and a reply with no account have neither.
inline bool scanReply(const char *reply, size_t length, uint8_t accountOut[32], uint64_t &rawOut) {
  if (reply == nullptr) return false;

  char address[SOL_B58_PUBKEY_MAX];
  if (!scan::stringValue(reply, length, "\"pubkey\"", address, sizeof address)) return false;
  uint8_t account[32];
  if (sol_b58_decode(address, account, sizeof account) != 0) return false;

  char digits[21];                                   // a u64 has at most 20 digits
  if (!scan::stringValue(reply, length, "\"amount\"", digits, sizeof digits)) return false;
  uint64_t value = 0;
  for (const char *p = digits; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
    const uint64_t digit = (uint64_t)(*p - '0');
    if (value > (UINT64_MAX - digit) / 10) return false;   // does not fit a u64
    value = value * 10 + digit;
  }

  memcpy(accountOut, account, sizeof account);
  rawOut = value;
  return true;
}

}  // namespace vk::balance
