/* host_ed25519.c - see host_ed25519.h. Wraps the vendored TweetNaCl, which is left untouched. */
#include "host_ed25519.h"
#include <stdlib.h>
#include <string.h>
#include "../../src/identity/tweetnacl.h"

/* TweetNaCl's one platform hook. On the host it is not random: crypto_sign_keypair() asks for the
   32-byte seed, and host_ed25519_keypair() supplies the caller's seed through this buffer. */
static unsigned char s_next_seed[32];
void randombytes(unsigned char *out, unsigned long long n);
void randombytes(unsigned char *out, unsigned long long n) {
  unsigned long long i;
  for (i = 0; i < n; i++) out[i] = s_next_seed[i % 32];
}

void host_ed25519_keypair(const uint8_t seed[32], uint8_t pub[32]) {
  unsigned char sk[64];
  memcpy(s_next_seed, seed, 32);
  crypto_sign_keypair(pub, sk);
  memset(s_next_seed, 0, 32);
}

void host_ed25519_sign(const uint8_t seed[32], const uint8_t *msg, size_t len, uint8_t sig[64]) {
  unsigned char sk[64];
  unsigned long long smlen = 0;
  unsigned char *sm = (unsigned char *)malloc(len + 64);
  memcpy(sk, seed, 32);
  host_ed25519_keypair(seed, sk + 32);          /* TweetNaCl's secret key is seed || public key */
  memset(sig, 0, 64);
  if (!sm) return;
  crypto_sign(sm, &smlen, msg, len, sk);        /* sm = signature || message */
  memcpy(sig, sm, 64);
  free(sm);
}

int host_ed25519_verify(const uint8_t *msg, size_t len, const uint8_t *sig, const uint8_t *pub) {
  unsigned long long mlen = 0;
  unsigned char *sm, *m;
  int ok;
  if (!sig || !pub || (!msg && len)) return 0;
  sm = (unsigned char *)malloc(len + 64);
  m = (unsigned char *)malloc(len + 64);
  if (!sm || !m) { free(sm); free(m); return 0; }
  memcpy(sm, sig, 64);
  if (len) memcpy(sm + 64, msg, len);
  ok = crypto_sign_open(m, &mlen, sm, len + 64, pub) == 0;
  free(sm);
  free(m);
  return ok;
}
