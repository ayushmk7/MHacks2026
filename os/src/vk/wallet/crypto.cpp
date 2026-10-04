// Ed25519 verification backend and random bytes (signing.md, "Crypto backend").
//
// Verification only. Nothing in this file can make a signature: the one signing path is signRaw()
// in signer.cpp. From src/identity/ this file includes ed25519.h alone, and uses its verify().
#include "crypto.h"

#include <Arduino.h>
#include <esp_random.h>

#include "../../badge_log.h"
#include "../vk_build.h"

#if VK_ED25519_BACKEND == 1
#include "vendor/monocypher-ed25519.h"   // Monocypher 4.0.2, vendored only when this backend is chosen
#else
#include "../../identity/ed25519.h"      // upstream's TweetNaCl wrapper
#endif

namespace vk::wallet {

bool verify(const uint8_t *msg, size_t len, const uint8_t sig[64], const uint8_t pubkey[32]) {
  if ((msg == nullptr && len != 0) || sig == nullptr || pubkey == nullptr) return false;

  const uint32_t startedAt = millis();
#if VK_ED25519_BACKEND == 1
  const bool ok = crypto_ed25519_check(sig, pubkey, msg, len) == 0;
#else
  const bool ok = ed25519::verify(msg, len, sig, pubkey);
#endif
  // Measurement M2 reads this line (testing.md, "Measurements").
  badge_log::tagf("vk", "verify %lu ms", (unsigned long)(millis() - startedAt));
  return ok;
}

void randomBytes(uint8_t *out, size_t len) {
  if (out == nullptr || len == 0) return;
  esp_fill_random(out, len);
}

}  // namespace vk::wallet

extern "C" int vk_verify_c(const uint8_t *msg, size_t len, const uint8_t *sig, const uint8_t *pubkey) {
  return vk::wallet::verify(msg, len, sig, pubkey) ? 1 : 0;
}
