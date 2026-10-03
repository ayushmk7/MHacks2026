/* src/wallet/wallet_crypto.h - Ed25519 verification and random bytes. No access to the badge key. */
#ifndef WALLET_CRYPTO_H
#define WALLET_CRYPTO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* true if `sig` is a valid Ed25519 signature by `pubkey` over `msg`. Backend per WALLET_ED25519_BACKEND
   (1 Monocypher crypto_ed25519_check, 0 TweetNaCl crypto_sign_open). */
bool wallet_crypto_verify(const uint8_t pubkey[32], const uint8_t *msg, size_t len, const uint8_t sig[64]);

/* esp_fill_random(). True hardware entropy only while Wi-Fi or BLE is up; used for nonces and request ids. */
void wallet_crypto_random(uint8_t *out, size_t len);

#ifdef __cplusplus
}
#endif
#endif
