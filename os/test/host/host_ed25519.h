/* host_ed25519.h - Ed25519 for the host tests: upstream's src/identity/tweetnacl.c with a stub
   randombytes(). Linked into every suite by run.sh. Never part of the firmware. */
#ifndef HOST_ED25519_H
#define HOST_ED25519_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* The public key that belongs to a 32-byte seed. */
void host_ed25519_keypair(const uint8_t seed[32], uint8_t pub[32]);

/* Detached signature of msg by the key with that seed. */
void host_ed25519_sign(const uint8_t seed[32], const uint8_t *msg, size_t len, uint8_t sig[64]);

/* 1 = valid. Same type as the pure code's `verify` function pointers
   (vk_record_verify, vk_check_input_t.verify) and the firmware's vk_verify_c. */
int host_ed25519_verify(const uint8_t *msg, size_t len, const uint8_t *sig, const uint8_t *pub);

#ifdef __cplusplus
}
#endif
#endif
