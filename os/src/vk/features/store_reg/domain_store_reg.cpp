// Signing domain "store-reg": the app-store registration message (signing.md, "Domain table").
//
// An auto domain with no prefix, so its validator is what keeps it apart from every other domain:
// it accepts exactly "solana-badge-register:<own pubkey base58>:<nonce>" and nothing else. The
// message begins with fixed lower-case ASCII text, so it can never be a Solana message (whose first
// byte is 0x01), and the key in it must be this badge's own.
//
// Reached only through vk::wallet::signStoreRegistration (hook H10 in upstream's broker_client.cpp).
#include <Arduino.h>

#include <string.h>

#include "../../wallet/signer.h"

namespace {

const char kHead[] = "solana-badge-register:";

// Upstream's isSafeNonce (src/net/broker_client.cpp), copied: the same length bound and the same
// character set, over raw bytes instead of a String.
bool isSafeNonce(const uint8_t *s, size_t length) {
  if (length == 0 || length > 256) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = (char)s[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '-' || c == '+' || c == '/' || c == '=';
    if (!ok) return false;
  }
  return true;
}

// True only for kHead, then this badge's own public key in base58, then ':', then a safe nonce.
// Neither base58 nor the nonce character set contains ':', so the three parts cannot run together.
bool validateStoreReg(const uint8_t *bytes, size_t len) {
  if (bytes == nullptr) return false;
  const String own = vk::wallet::addressBase58();
  const size_t headLen = sizeof(kHead) - 1;
  const size_t keyLen = own.length();
  if (keyLen == 0) return false;                         // no identity: nothing to register
  if (len < headLen + keyLen + 2) return false;          // ':' and at least one nonce character
  if (memcmp(bytes, kHead, headLen) != 0) return false;
  if (memcmp(bytes + headLen, own.c_str(), keyLen) != 0) return false;
  if (bytes[headLen + keyLen] != ':') return false;
  return isSafeNonce(bytes + headLen + keyLen + 1, len - headLen - keyLen - 1);
}

}  // namespace

VK_SIGN_DOMAIN(store_reg, "store-reg", "", false, nullptr, 200, nullptr, validateStoreReg);
