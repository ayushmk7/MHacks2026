// Wallet core: the domain table, its self-check and the one signing path (signing.md).
//
// The key gate. signRaw() below is the only function in the firmware that asks the identity for a
// signature, and this is the only file under src/vk/ that may include src/identity/identity.h.
// scripts/preflash-check.sh (check 2) fails the build if a signing call appears anywhere else.
//
// Host tests (test/host/test_domains.cpp) compile this file with -DVK_HOST_TEST and nothing else
// from the firmware: every call into the identity, the config store, the permissions and the
// approval engine is behind #ifndef VK_HOST_TEST, and the host build reads `hostHooks` instead.
#include "signer.h"

#include <stdlib.h>
#include <string.h>

#include "signer_internal.h"

#ifndef VK_HOST_TEST
#include "../../badge_log.h"
#include "../../identity/identity.h"
#include "../core/config.h"
#include "../core/serial.h"
#include "../core/service.h"
#include "../host/permissions.h"
#include "approval.h"
#endif

namespace vk::wallet {

vk_presence_t (*presenceLookup)(const uint8_t req_id[8], uint8_t payee_pubkey_out[32], uint8_t nonce_out[16]) = nullptr;
bool (*tokenInfoLookup)(const uint8_t mint[32], TokenInfo &out) = nullptr;
bool (*batteryCritical)() = nullptr;
bool (*spentLookup)(const vk_token_t *tokens, size_t count, uint32_t now, uint64_t out[VK_MAX_TOKENS]) = nullptr;

#ifdef VK_HOST_TEST
HostHooks hostHooks;
#endif

// ---------------------------------------------------------------------------
// Self-check (signing.md, "Self-check")
// ---------------------------------------------------------------------------

namespace {

// Signed by the issuer, never by a badge.
const char *const kReservedPrefixes[] = {"registry:"};

// The largest message the identity is ever asked to sign, prefix included (signing.md, "Key").
constexpr size_t kMaxSignBytes = 1248;
#ifndef VK_HOST_TEST
constexpr size_t kSe050SignBytes = 242;   // an SE050 key, after hook H12
#endif

bool sSelfCheckOk = false;

// Rule 2: 2 to 15 characters of [a-z-], then ':'. The first character must be a letter, which is
// what "Why domains cannot be confused" relies on: every text domain's bytes begin with 0x61-0x7A.
bool prefixWellFormed(const char *prefix) {
  const size_t length = strlen(prefix);
  if (length < 3 || length > 16) return false;
  if (prefix[length - 1] != ':') return false;
  if (prefix[0] < 'a' || prefix[0] > 'z') return false;
  for (size_t i = 0; i + 1 < length; ++i) {
    const char c = prefix[i];
    if (!((c >= 'a' && c <= 'z') || c == '-')) return false;
  }
  return true;
}

bool startsWith(const char *text, const char *head) { return strncmp(text, head, strlen(head)) == 0; }

}  // namespace

int checkDomainTable(const DomainRow *rows, size_t count, const char **bad_name) {
  const char *unused = "";
  if (bad_name == nullptr) bad_name = &unused;
  *bad_name = "";
  if (count == 0) return 0;
  if (rows == nullptr) return 1;

  // 1. Names are unique. A row with no name cannot be told apart from any other.
  for (size_t i = 0; i < count; ++i) {
    if (rows[i].name == nullptr) { *bad_name = "(null)"; return 1; }
    for (size_t j = 0; j < i; ++j) {
      if (strcmp(rows[i].name, rows[j].name) == 0) { *bad_name = rows[i].name; return 1; }
    }
  }

  // 2. Every non-empty prefix is well formed.
  for (size_t i = 0; i < count; ++i) {
    if (rows[i].prefix == nullptr) { *bad_name = rows[i].name; return 2; }
    if (rows[i].prefix[0] != '\0' && !prefixWellFormed(rows[i].prefix)) { *bad_name = rows[i].name; return 2; }
  }

  // 3. No non-empty prefix is a prefix of another (two equal prefixes included).
  for (size_t i = 0; i < count; ++i) {
    if (rows[i].prefix[0] == '\0') continue;
    for (size_t j = 0; j < i; ++j) {
      if (rows[j].prefix[0] == '\0') continue;
      if (startsWith(rows[i].prefix, rows[j].prefix) || startsWith(rows[j].prefix, rows[i].prefix)) {
        *bad_name = rows[i].name;
        return 3;
      }
    }
  }

  // 4. At most one button domain has an empty prefix.
  bool bareButtonSeen = false;
  for (size_t i = 0; i < count; ++i) {
    if (!rows[i].needs_button || rows[i].prefix[0] != '\0') continue;
    if (bareButtonSeen) { *bad_name = rows[i].name; return 4; }
    bareButtonSeen = true;
  }

  // 5. Every auto domain has a validate function, every button domain a decode function.
  for (size_t i = 0; i < count; ++i) {
    const bool ok = rows[i].needs_button ? rows[i].has_decode : rows[i].has_validate;
    if (!ok) { *bad_name = rows[i].name; return 5; }
  }

  // 6. No prefix is reserved.
  for (size_t i = 0; i < count; ++i) {
    for (const char *reserved : kReservedPrefixes) {
      if (strcmp(rows[i].prefix, reserved) == 0) { *bad_name = rows[i].name; return 6; }
    }
  }

  return 0;
}

namespace {

// Runs the six rules over the registered rows and records the result. Returns the broken rule
// (0 = valid, -1 = the copy of the table could not be allocated).
int checkRegisteredDomains(const char **bad_name) {
  sSelfCheckOk = false;
  *bad_name = "";

  size_t count = 0;
  for (const SignDomain *d = SignDomain::first(); d; d = d->next()) ++count;
  if (count == 0) {          // nothing can sign, so there is nothing to confuse
    sSelfCheckOk = true;
    return 0;
  }

  DomainRow *rows = (DomainRow *)malloc(count * sizeof(DomainRow));
  if (rows == nullptr) {
    *bad_name = "(out of memory)";
    return -1;
  }
  size_t at = 0;
  for (const SignDomain *d = SignDomain::first(); d && at < count; d = d->next(), ++at) {
    rows[at].name = d->name;
    rows[at].prefix = d->prefix;
    rows[at].needs_button = d->needs_button;
    rows[at].has_decode = d->decode != nullptr;
    rows[at].has_validate = d->validate != nullptr;
  }
  // `*bad_name` points at the domain's own name, not into `rows`, so it outlives the copy.
  const int rule = checkDomainTable(rows, at, bad_name);
  free(rows);

  sSelfCheckOk = (rule == 0);
  return rule;
}

}  // namespace

bool selfCheckOk() { return sSelfCheckOk; }

#ifdef VK_HOST_TEST
int runSelfCheck(const char **bad_name) {
  const char *unused = "";
  return checkRegisteredDomains(bad_name ? bad_name : &unused);
}
#endif

// ---------------------------------------------------------------------------
// The badge key (signing.md, "Key")
// ---------------------------------------------------------------------------

#ifndef VK_HOST_TEST

const char *keyLocation() {
  if (!identity::ready()) return "none";
  switch (identity::source()) {
    case identity::Source::SecureElement: return "se050";
    case identity::Source::Software:      return "software";
    default:                              return "none";
  }
}

size_t maxSignBytes() {
  return identity::source() == identity::Source::SecureElement ? kSe050SignBytes : kMaxSignBytes;
}

const uint8_t *publicKey() { return identity::ready() ? identity::publicKey() : nullptr; }

String addressBase58() { return identity::ready() ? identity::publicKeyBase58() : String(""); }

#else  // VK_HOST_TEST: the test supplies the key

const char *keyLocation() { return hostHooks.public_key ? "software" : "none"; }

size_t maxSignBytes() { return hostHooks.max_sign_bytes; }

const uint8_t *publicKey() { return hostHooks.public_key; }

String addressBase58() {
  if (hostHooks.public_key == nullptr) return String("");
  char text[64];
  const size_t n = sol_b58_encode(hostHooks.public_key, 32, text, sizeof text);
  return n ? String(text) : String("");
}

#endif

// ---------------------------------------------------------------------------
// The signing path (signing.md, "The signing path")
// ---------------------------------------------------------------------------

const SignDomain *findDomain(const char *name) {
  if (name == nullptr) return nullptr;
  for (const SignDomain *d = SignDomain::first(); d; d = d->next()) {
    if (strcmp(d->name, name) == 0) return d;
  }
  return nullptr;
}

// prefix ‖ bytes is assembled here. Static, not on the stack: signAuto is reached from inside Lua
// callbacks, where the stack is already deep. There is one task, so one buffer is enough.
static uint8_t sSignBuffer[kMaxSignBytes];

// Every signature the badge makes goes through this function, and only this function calls the
// identity. It trusts nothing its callers checked: the self-check result and both length rules are
// tested again here.
static Reason signRaw(const SignDomain *domain, const uint8_t *bytes, size_t len, uint8_t sig[64]) {
  if (sig == nullptr) return VK_SIGN_FAILED;
  memset(sig, 0, 64);
  if (!selfCheckOk()) return VK_SIGN_FAILED;
  if (domain == nullptr || domain->prefix == nullptr || bytes == nullptr || len == 0) return VK_SIGN_FAILED;

  if (len > domain->max_len || len > kMaxSignBytes) return VK_TOO_LONG;
  const size_t prefixLen = strlen(domain->prefix);
  const size_t total = prefixLen + len;
  if (total > maxSignBytes() || total > sizeof sSignBuffer) return VK_TOO_LONG;

  memcpy(sSignBuffer, domain->prefix, prefixLen);
  memcpy(sSignBuffer + prefixLen, bytes, len);

#ifndef VK_HOST_TEST
  const uint32_t startedAt = millis();
  const bool ok = identity::sign(sSignBuffer, total, sig);
  badge_log::tagf("vk", "sign %s %u bytes %lu ms", domain->name, (unsigned)total,
                  (unsigned long)(millis() - startedAt));
  if (!ok) badge_log::tagf("vk", "sign %s FAILED", domain->name);
#else
  const bool ok = hostHooks.sign != nullptr && hostHooks.sign(sSignBuffer, total, sig);
#endif

  if (!ok) memset(sig, 0, 64);

  // Every attempt reaches the signature log from here, the one place every signature passes, so no
  // domain (today's or a future one) can sign without leaving a record. The listeners only queue.
  const SignEvent event{domain, sSignBuffer, total, ok ? sig : nullptr, ok ? VK_OK : VK_SIGN_FAILED};
  for (const SignListener *listener = SignListener::first(); listener; listener = listener->next()) {
    if (listener->fn) listener->fn(event);
  }
  return event.result;
}

Reason signAuto(const char *domain, const uint8_t *bytes, size_t len, uint8_t sig[64]) {
  const SignDomain *d = findDomain(domain);
  if (d == nullptr || d->needs_button || d->validate == nullptr) return VK_UNSUPPORTED;
  if (bytes == nullptr || sig == nullptr) return VK_BAD_ARG;
  // The length is checked before the validator runs, so a validator never sees more than max_len.
  if (len == 0 || len > d->max_len) return VK_TOO_LONG;
  if (!d->validate(bytes, len)) return VK_BAD_ARG;
  return signRaw(d, bytes, len, sig);
}

Reason signForApproval(const SignDomain *domain, const uint8_t *bytes, size_t len, uint8_t sig[64]) {
  // Only a button domain is ever signed because of a press; auto domains go through signAuto,
  // where their validator runs.
  if (domain == nullptr || !domain->needs_button) return VK_UNSUPPORTED;
  return signRaw(domain, bytes, len, sig);
}

Reason begin(const char *domain, const uint8_t *bytes, size_t len, const Ctx &ctx, const char *app_id) {
#ifndef VK_HOST_TEST
  if (!vk::config::provisioned()) return VK_NOT_PROVISIONED;
#else
  if (!hostHooks.provisioned) return VK_NOT_PROVISIONED;
#endif

  const SignDomain *d = findDomain(domain);
  if (d == nullptr || !d->needs_button || d->decode == nullptr || d->prefix == nullptr) return VK_UNSUPPORTED;

  // A domain with no permission name is firmware-internal: there is nothing for an app to hold,
  // and no app-facing function passes its name here.
  if (d->permission != nullptr) {
#ifndef VK_HOST_TEST
    if (!vk::host::granted(d->permission)) return VK_DENIED;
#else
    if (hostHooks.granted != nullptr && !hostHooks.granted(d->permission)) return VK_DENIED;
#endif
  }

  // An approval is open, or a result is waiting to be polled.
#ifndef VK_HOST_TEST
  if (approval::active() || approval::peekResult() != Poll::IDLE) return VK_BUSY;
#else
  if (hostHooks.busy) return VK_BUSY;
#endif

  // A signature that browns out half-way is worse than one refused: nothing new starts on a critical battery.
  if (batteryCritical != nullptr && batteryCritical()) return VK_LOW_BATTERY;

  if (len == 0 || len > d->max_len) return VK_TOO_LONG;
  if (strlen(d->prefix) + len > maxSignBytes()) return VK_TOO_LONG;
  if (bytes == nullptr) return VK_BAD_ARG;
  if (app_id == nullptr) app_id = "";

#ifndef VK_HOST_TEST
  ApprovalRequest request{};
  const Reason decoded = d->decode(bytes, len, ctx, request);
  if (decoded != VK_OK) return decoded;
  // The engine copies the request, the bytes and the app id. It refuses only when an approval is
  // already active, which was ruled out above.
  return approval::open(request, d, bytes, len, app_id) ? VK_OK : VK_BUSY;
#else
  if (hostHooks.open == nullptr) return VK_UNSUPPORTED;
  return hostHooks.open(d, bytes, len, ctx, app_id);
#endif
}

Poll poll(uint8_t sig[64], Reason &reason) {
#ifndef VK_HOST_TEST
  return approval::takeResult(sig, reason);
#else
  (void)sig;
  reason = VK_IDLE;
  return Poll::IDLE;
#endif
}

// Hook H10: upstream's store client asks for the registration signature here. The message must be
// exactly what the "store-reg" validator accepts (features/store_reg/domain_store_reg.cpp).
String signStoreRegistration(const String &message) {
  uint8_t sig[64];
  if (signAuto("store-reg", (const uint8_t *)message.c_str(), message.length(), sig) != VK_OK) return String("");
#ifndef VK_HOST_TEST
  return identity::base64Encode(sig, sizeof sig);
#else
  return String("");       // the host has no base64; the test checks signAuto("store-reg") itself
#endif
}

// ---------------------------------------------------------------------------
// Boot: the self-check service and the VKINFO fields
// ---------------------------------------------------------------------------

#ifndef VK_HOST_TEST
namespace {

// Runs from vk::begin(), before the radios start, so before anything can ask for a signature.
void signerBegin() {
  const char *name = "";
  const int rule = checkRegisteredDomains(&name);
  if (rule == 0) {
    badge_log::tagf("vk", "selfcheck ok");
  } else {
    badge_log::tagf("vk", "DOMAIN TABLE INVALID: %d %s", rule, name);
  }
}

String infoPubkey() { return addressBase58(); }
String infoKey() { return String(keyLocation()); }
String infoSelfCheck() { return String(selfCheckOk() ? "1" : "0"); }

VK_SERVICE(signer, signerBegin, nullptr);

// Registered in reverse, so that among the fields of this file VKINFO shows pubkey, key, selfcheck
// (a registry lists its newest entry first).
VK_INFO_FIELD(selfcheck, "selfcheck", infoSelfCheck);
VK_INFO_FIELD(key, "key", infoKey);
VK_INFO_FIELD(pubkey, "pubkey", infoPubkey);

}  // namespace
#endif

}  // namespace vk::wallet
