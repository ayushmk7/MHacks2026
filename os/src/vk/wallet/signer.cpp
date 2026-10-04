// Wallet core, WP01 stub. The real signing path (signRaw, the domain self-check, begin, signAuto)
// arrives in WP11; until then nothing here signs except the store registration, which is forwarded
// to upstream unchanged so the fork behaves like upstream (upstream-hooks.md, rule 5).
//
// This is the only file under src/vk/ that may include src/identity/identity.h.
#include "signer.h"

#include <string.h>

#include "signer_internal.h"

#ifndef VK_HOST_TEST
#include "../../identity/identity.h"
#endif

namespace vk::wallet {

vk_presence_t (*presenceLookup)(const uint8_t req_id[8], uint8_t payee_pubkey_out[32], uint8_t nonce_out[16]) = nullptr;
bool (*tokenInfoLookup)(const uint8_t mint[32], TokenInfo &out) = nullptr;

const SignDomain *findDomain(const char *name) {
  if (name == nullptr) return nullptr;
  for (const SignDomain *d = SignDomain::first(); d; d = d->next()) {
    if (strcmp(d->name, name) == 0) return d;
  }
  return nullptr;
}

Reason signAuto(const char *domain, const uint8_t *bytes, size_t len, uint8_t sig[64]) {
  (void)domain; (void)bytes; (void)len; (void)sig;
  return VK_UNSUPPORTED;   // stub (WP11)
}

Reason begin(const char *domain, const uint8_t *bytes, size_t len, const Ctx &ctx, const char *app_id) {
  (void)domain; (void)bytes; (void)len; (void)ctx; (void)app_id;
  return VK_UNSUPPORTED;   // stub (WP11)
}

Poll poll(uint8_t sig[64], Reason &reason) {
  (void)sig;
  reason = VK_IDLE;
  return Poll::IDLE;       // stub (WP11: approval::takeResult)
}

Reason signForApproval(const SignDomain *domain, const uint8_t *bytes, size_t len, uint8_t sig[64]) {
  (void)domain; (void)bytes; (void)len; (void)sig;
  return VK_SIGN_FAILED;   // stub (WP11)
}

bool selfCheckOk() { return false; }   // stub: the self-check does not exist yet (WP11)

#ifndef VK_HOST_TEST

const char *keyLocation() {
  if (!identity::ready()) return "none";
  switch (identity::source()) {
    case identity::Source::SecureElement: return "se050";
    case identity::Source::Software:      return "software";
    default:                              return "none";
  }
}

size_t maxSignBytes() { return identity::source() == identity::Source::SecureElement ? 242 : 1248; }

const uint8_t *publicKey() { return identity::ready() ? identity::publicKey() : nullptr; }

String addressBase58() { return identity::ready() ? identity::publicKeyBase58() : String(""); }

// Hook H10. Stub: exactly what upstream did at this call site.
String signStoreRegistration(const String &message) { return identity::signBase64(message); }

#else  // VK_HOST_TEST: no identity on the host

const char *keyLocation() { return "none"; }
size_t maxSignBytes() { return 1248; }
const uint8_t *publicKey() { return nullptr; }
String addressBase58() { return String(""); }
String signStoreRegistration(const String &message) { (void)message; return String(""); }

#endif

}  // namespace vk::wallet
