// LINK: src/vk/wallet/vendor/monocypher.c src/vk/wallet/vendor/monocypher-ed25519.c
/* test_mono - the vendored Monocypher 4.0.2 as the Ed25519 verification backend
   (VK_ED25519_BACKEND 1; docs/os/wallet/signing.md, "Crypto backend").

   crypto_ed25519_check is called exactly as src/vk/wallet/crypto.cpp calls it:
   (signature, public key, message, length), 0 = valid. It must accept the two signatures in
   vectors.h, refuse every copy of them with one bit flipped, and give the same answer as the
   TweetNaCl backend (host_ed25519_verify) on 50 random messages. */
#include "../../src/vk/wallet/vendor/monocypher-ed25519.h"
#include <stdio.h>
#include <string.h>
#include "../../src/vk/wallet/pure/vk_frames.h"
#include "../../src/vk/wallet/pure/vk_record.h"
#include "host_ed25519.h"
#include "vectors.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* The backend as crypto.cpp wraps it: 1 = valid. */
static int mono_verify(const uint8_t *msg, size_t len, const uint8_t *sig, const uint8_t *pub) {
  return crypto_ed25519_check(sig, pub, msg, len) == 0;
}

/* Deterministic bytes (xorshift64*), so a failure can be reproduced. */
static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint64_t rng_next(void) {
  rng_state ^= rng_state >> 12; rng_state ^= rng_state << 25; rng_state ^= rng_state >> 27;
  return rng_state * 0x2545f4914f6cdd1dull;
}
static void rng_fill(uint8_t *out, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) out[i] = (uint8_t)(rng_next() >> 32);
}

/* A valid (msg, sig, pub) is accepted; every copy with one bit flipped in the message, the
   signature or the key is refused. Returns the number of flipped copies that were accepted. */
static size_t accepted_flips(const uint8_t *msg, size_t len, const uint8_t sig[64], const uint8_t pub[32]) {
  uint8_t m[640], s[64], p[32];
  size_t bit, bad = 0;
  if (len > sizeof m) return (size_t)-1;
  memcpy(m, msg, len); memcpy(s, sig, 64); memcpy(p, pub, 32);
  for (bit = 0; bit < 64 * 8; bit++) {
    s[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    if (mono_verify(m, len, s, p)) bad++;
    s[bit / 8] ^= (uint8_t)(1u << (bit % 8));
  }
  for (bit = 0; bit < 32 * 8; bit++) {
    p[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    if (mono_verify(m, len, s, p)) bad++;
    p[bit / 8] ^= (uint8_t)(1u << (bit % 8));
  }
  for (bit = 0; bit < len * 8; bit++) {
    m[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    if (mono_verify(m, len, s, p)) bad++;
    m[bit / 8] ^= (uint8_t)(1u << (bit % 8));
  }
  /* the copies are back to the originals */
  if (!mono_verify(m, len, s, p)) bad++;
  return bad;
}

/* The registry record: the issuer signs "registry:" || record. */
static void test_record_vector(void) {
  uint8_t msg[9 + sizeof V_RECORD], sig[64];
  CHECK(strlen(VK_RECORD_PREFIX) == 9);
  memcpy(msg, VK_RECORD_PREFIX, 9); memcpy(msg + 9, V_RECORD, sizeof V_RECORD);

  CHECK(crypto_ed25519_check(V_RECORD_SIG, V_ISSUER_PUB, msg, sizeof msg) == 0);
  CHECK(host_ed25519_verify(msg, sizeof msg, V_RECORD_SIG, V_ISSUER_PUB) == 1);
  /* the same thing through the pure code's verify pointer, as the firmware runs it */
  CHECK(vk_record_verify(V_RECORD, sizeof V_RECORD, V_RECORD_SIG, V_ISSUER_PUB, mono_verify) == 1);

  /* one bit flipped: the named cases, then every bit of the signature, the key and the message */
  memcpy(sig, V_RECORD_SIG, 64); sig[0] ^= 1;
  CHECK(crypto_ed25519_check(sig, V_ISSUER_PUB, msg, sizeof msg) != 0);
  memcpy(sig, V_RECORD_SIG, 64); sig[63] ^= 0x10;
  CHECK(crypto_ed25519_check(sig, V_ISSUER_PUB, msg, sizeof msg) != 0);
  CHECK(accepted_flips(msg, sizeof msg, V_RECORD_SIG, V_ISSUER_PUB) == 0);

  /* wrong key, bare record (no prefix), one byte short */
  CHECK(crypto_ed25519_check(V_RECORD_SIG, V_DEVICE_PUB, msg, sizeof msg) != 0);
  CHECK(crypto_ed25519_check(V_RECORD_SIG, V_ISSUER_PUB, V_RECORD, sizeof V_RECORD) != 0);
  CHECK(crypto_ed25519_check(V_RECORD_SIG, V_ISSUER_PUB, msg, sizeof msg - 1) != 0);
}

/* The payment request: the payee's badge signs "pay-req:" || frame[0 .. signed_len); the
   signature is the last 64 bytes of the frame. */
static void test_req_vector(void) {
  uint8_t msg[8 + V_REQ_SIGNED_LEN], sig[64];
  const uint8_t *req_sig = V_REQ + V_REQ_SIGNED_LEN;
  vk_req_t r;
  CHECK(strlen(VK_PREFIX_PAY_REQ) == 8);
  CHECK(sizeof V_REQ == V_REQ_SIGNED_LEN + 64);
  CHECK(vk_req_parse(V_REQ, sizeof V_REQ, &r) == 0 && r.signed_len == V_REQ_SIGNED_LEN);
  CHECK(memcmp(r.sig, req_sig, 64) == 0);
  memcpy(msg, VK_PREFIX_PAY_REQ, 8); memcpy(msg + 8, V_REQ, V_REQ_SIGNED_LEN);

  CHECK(crypto_ed25519_check(req_sig, V_DEVICE_PUB, msg, sizeof msg) == 0);
  CHECK(host_ed25519_verify(msg, sizeof msg, req_sig, V_DEVICE_PUB) == 1);

  memcpy(sig, req_sig, 64); sig[31] ^= 0x80;
  CHECK(crypto_ed25519_check(sig, V_DEVICE_PUB, msg, sizeof msg) != 0);
  memcpy(sig, req_sig, 64); sig[32] ^= 1;
  CHECK(crypto_ed25519_check(sig, V_DEVICE_PUB, msg, sizeof msg) != 0);
  CHECK(accepted_flips(msg, sizeof msg, req_sig, V_DEVICE_PUB) == 0);

  /* wrong key, frame bytes without the prefix */
  CHECK(crypto_ed25519_check(req_sig, V_ISSUER_PUB, msg, sizeof msg) != 0);
  CHECK(crypto_ed25519_check(req_sig, V_DEVICE_PUB, V_REQ, V_REQ_SIGNED_LEN) != 0);
}

/* 50 random keys and messages signed by TweetNaCl: Monocypher accepts each, derives the same
   public key and the same signature from the seed (the two backends are interchangeable), and
   agrees with TweetNaCl on a copy with one random bit flipped. */
static void test_agrees_with_tweetnacl(void) {
  enum { ROUNDS = 50, MAX_LEN = 300 };
  int i;
  for (i = 0; i < ROUNDS; i++) {
    uint8_t seed[32], seed_copy[32], pub[32], sig[64], msg[MAX_LEN + 1];
    uint8_t mono_sk[64], mono_pub[32], mono_sig[64];
    size_t len = (i == 0) ? 0 : (i == 1) ? MAX_LEN : (size_t)(rng_next() % (MAX_LEN + 1));
    size_t where, bit;
    int host_ok, mono_ok;

    rng_fill(seed, 32); rng_fill(msg, sizeof msg);
    host_ed25519_keypair(seed, pub);
    host_ed25519_sign(seed, msg, len, sig);

    host_ok = host_ed25519_verify(msg, len, sig, pub);
    mono_ok = mono_verify(msg, len, sig, pub);
    CHECK(host_ok == 1);
    CHECK(mono_ok == host_ok);

    memcpy(seed_copy, seed, 32);                       /* crypto_ed25519_key_pair wipes its seed */
    crypto_ed25519_key_pair(mono_sk, mono_pub, seed_copy);
    crypto_ed25519_sign(mono_sig, mono_sk, msg, len);
    CHECK(memcmp(mono_pub, pub, 32) == 0);
    CHECK(memcmp(mono_sig, sig, 64) == 0);

    /* one random bit flipped in the signature, the key or (when there is one) the message */
    where = (size_t)(rng_next() % (len ? 3 : 2));
    if (where == 0) { bit = (size_t)(rng_next() % (64 * 8)); sig[bit / 8] ^= (uint8_t)(1u << (bit % 8)); }
    else if (where == 1) { bit = (size_t)(rng_next() % (32 * 8)); pub[bit / 8] ^= (uint8_t)(1u << (bit % 8)); }
    else { bit = (size_t)(rng_next() % (len * 8)); msg[bit / 8] ^= (uint8_t)(1u << (bit % 8)); }
    host_ok = host_ed25519_verify(msg, len, sig, pub);
    mono_ok = mono_verify(msg, len, sig, pub);
    CHECK(mono_ok == host_ok);
    CHECK(mono_ok == 0);
  }
}

/* crypto.cpp lets a null message through when the length is 0. */
static void test_empty_message(void) {
  uint8_t pub[32], sig[64], byte = 0;
  host_ed25519_keypair(V_DEVICE_SEED, pub);
  CHECK(memcmp(pub, V_DEVICE_PUB, 32) == 0);
  host_ed25519_sign(V_DEVICE_SEED, &byte, 0, sig);
  CHECK(crypto_ed25519_check(sig, pub, NULL, 0) == 0);
  CHECK(crypto_ed25519_check(sig, pub, &byte, 0) == 0);
  CHECK(crypto_ed25519_check(sig, pub, &byte, 1) != 0);
  CHECK(crypto_ed25519_check(sig, V_ISSUER_PUB, NULL, 0) != 0);
}

int main(void) {
  test_record_vector();
  test_req_vector();
  test_agrees_with_tweetnacl();
  test_empty_message();
  if (fails) { printf("%d mono checks failed\n", fails); return 1; }
  printf("all mono tests passed\n");
  return 0;
}
