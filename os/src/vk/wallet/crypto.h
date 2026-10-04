// Ed25519 verification backend and random bytes (signing.md, "Crypto backend").
// Header only until crypto.cpp is written (WP11).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vk::wallet {
bool verify(const uint8_t *msg, size_t len, const uint8_t sig[64], const uint8_t pubkey[32]);
void randomBytes(uint8_t *out, size_t len);        // esp_fill_random
}
// C adapter with the signature the pure code's function pointers expect; returns 1 when valid.
extern "C" int vk_verify_c(const uint8_t *msg, size_t len, const uint8_t *sig, const uint8_t *pubkey);
