/* test_frames - every ESP-NOW frame type: round trip, strict lengths, strict fields, signed_len and
   the signed-bytes helpers against the vectors. Spec: docs/os/protocol/espnow.md ("Frames", "Codec"). */
#include "../../src/vk/wallet/pure/vk_frames.h"
#include <stdio.h>
#include <string.h>
#include "host_ed25519.h"
#include "vectors.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint8_t frame[VK_FRAME_MAX + 16], copy[VK_FRAME_MAX + 16], again[VK_FRAME_MAX + 16];

static void fill(uint8_t *p, size_t n, uint8_t seed) { size_t i; for (i = 0; i < n; i++) p[i] = (uint8_t)(seed + 7 * i); }

/* 1 if sig is pub's signature over prefix || bytes. */
static int verify_prefixed(const char *prefix, const uint8_t *bytes, size_t len, const uint8_t *sig, const uint8_t *pub) {
  uint8_t buf[16 + VK_FRAME_MAX]; const size_t p = strlen(prefix);
  memcpy(buf, prefix, p); memcpy(buf + p, bytes, len);
  return host_ed25519_verify(buf, p + len, sig, pub);
}
static void sign_prefixed(const uint8_t *seed, const char *prefix, const uint8_t *bytes, size_t len, uint8_t sig[64]) {
  uint8_t buf[16 + VK_FRAME_MAX]; const size_t p = strlen(prefix);
  memcpy(buf, prefix, p); memcpy(buf + p, bytes, len);
  host_ed25519_sign(seed, buf, p + len, sig);
}

/* Generic strictness checks for a frame of `len` bytes that `parse` accepts: every truncated copy,
   an over-long copy, another type, and a damaged header are refused. */
typedef int (*parse_fn)(const uint8_t *, size_t, void *);
static void check_strict(const uint8_t *good, size_t len, parse_fn parse, void *out, int line) {
  size_t i; int bad = 0;
  if (parse(good, len, out) != 0) bad++;
  for (i = 0; i < len; i++) if (parse(good, i, out) == 0) bad++;                 /* each truncated copy */
  memcpy(copy, good, len); copy[len] = 0; copy[len + 1] = 0;
  if (parse(copy, len + 1, out) == 0) bad++;                                     /* over-long */
  if (parse(copy, len + 2, out) == 0) bad++;
  memcpy(copy, good, len); copy[0] = 'X'; if (parse(copy, len, out) == 0) bad++; /* magic */
  memcpy(copy, good, len); copy[1] = 'k'; if (parse(copy, len, out) == 0) bad++;
  memcpy(copy, good, len); copy[2] = 2;   if (parse(copy, len, out) == 0) bad++; /* version */
  memcpy(copy, good, len); copy[3] ^= 0x40; if (parse(copy, len, out) == 0) bad++; /* another type */
  if (parse(NULL, len, out) == 0) bad++;
  if (bad) { printf("FAIL %s:%d strictness (%d cases)\n", __FILE__, line, bad); fails++; }
}
static int p_req(const uint8_t *f, size_t n, void *o) { return vk_req_parse(f, n, (vk_req_t *)o); }
static int p_chal(const uint8_t *f, size_t n, void *o) { return vk_chal_parse(f, n, (vk_chal_t *)o); }
static int p_proof(const uint8_t *f, size_t n, void *o) { return vk_proof_parse(f, n, (vk_proof_t *)o); }
static int p_result(const uint8_t *f, size_t n, void *o) { return vk_result_parse(f, n, (vk_result_t *)o); }
static int p_hello(const uint8_t *f, size_t n, void *o) { return vk_hello_parse(f, n, (vk_hello_t *)o); }
static int p_card(const uint8_t *f, size_t n, void *o) { return vk_card_parse(f, n, (vk_card_t *)o); }

static void test_frame_type(void) {
  static const uint8_t a[] = {'V', 'K', 1, 1}, b[] = {'V', 'K', 1, 200, 9, 9}, c[] = {'V', 'K', 2, 1}, d[] = {'S', 'B', 'D', 'G', 2};
  CHECK(vk_frame_type(a, sizeof a) == VK_T_REQ);
  CHECK(vk_frame_type(b, sizeof b) == 200);                 /* an app-range type is still a VK frame */
  CHECK(vk_frame_type(c, sizeof c) == -1);                  /* version 2 */
  CHECK(vk_frame_type(d, sizeof d) == -1);
  CHECK(vk_frame_type(a, 3) == -1 && vk_frame_type(a, 0) == -1 && vk_frame_type(NULL, 4) == -1);
  CHECK(VK_T_REQ == 1 && VK_T_CHAL == 2 && VK_T_PROOF == 3 && VK_T_RESULT == 4 && VK_T_CONTACT_HELLO == 16 && VK_T_CONTACT_CARD == 17);
}

static void test_host_ed25519(void) {
  uint8_t pub[32], sig[64], signed_bytes[9 + sizeof V_RECORD];
  host_ed25519_keypair(V_ISSUER_SEED, pub); CHECK(memcmp(pub, V_ISSUER_PUB, 32) == 0);
  host_ed25519_keypair(V_DEVICE_SEED, pub); CHECK(memcmp(pub, V_DEVICE_PUB, 32) == 0);
  /* Ed25519 is deterministic: the host signer reproduces the signature node:crypto made */
  memcpy(signed_bytes, "registry:", 9); memcpy(signed_bytes + 9, V_RECORD, sizeof V_RECORD);
  host_ed25519_sign(V_ISSUER_SEED, signed_bytes, sizeof signed_bytes, sig);
  CHECK(memcmp(sig, V_RECORD_SIG, 64) == 0);
  CHECK(host_ed25519_verify(signed_bytes, sizeof signed_bytes, V_RECORD_SIG, V_ISSUER_PUB) == 1);
  CHECK(host_ed25519_verify(signed_bytes, sizeof signed_bytes, V_RECORD_SIG, V_DEVICE_PUB) == 0);
  CHECK(host_ed25519_verify(signed_bytes, sizeof signed_bytes - 1, V_RECORD_SIG, V_ISSUER_PUB) == 0);
  sig[5] ^= 1; CHECK(host_ed25519_verify(signed_bytes, sizeof signed_bytes, sig, V_ISSUER_PUB) == 0);
}

static void test_req(void) {
  vk_req_t r, q; size_t n, name_len; uint8_t sig[64];
  /* the vector */
  CHECK(sizeof V_REQ == 62 + 12 + 64 && vk_frame_type(V_REQ, sizeof V_REQ) == VK_T_REQ);
  CHECK(vk_req_parse(V_REQ, sizeof V_REQ, &r) == 0);
  CHECK(r.rail == VK_RAIL_SOLANA && memcmp(r.payee_pubkey, V_DEVICE_PUB, 32) == 0 && r.amount == V_REQ_AMOUNT);
  CHECK(strcmp(r.currency, "HACK") == 0 && r.expiry == V_REQ_EXPIRY && r.name_len == 12 && strcmp(r.name, "MHacks Merch") == 0);
  CHECK(memcmp(r.req_id, V_REQ + 49, 8) == 0 && memcmp(r.sig, V_REQ + sizeof V_REQ - 64, 64) == 0);
  CHECK(r.signed_len == V_REQ_SIGNED_LEN && r.signed_len == 62u + r.name_len && r.signed_len <= VK_REQ_SIGNED_MAX);
  /* the signature is the payee's, domain pay-req, over the first signed_len bytes */
  CHECK(verify_prefixed(VK_PREFIX_PAY_REQ, V_REQ, r.signed_len, r.sig, V_DEVICE_PUB) == 1);
  CHECK(verify_prefixed(VK_PREFIX_PAY_REQ, V_REQ, r.signed_len - 1, r.sig, V_DEVICE_PUB) == 0);
  CHECK(verify_prefixed(VK_PREFIX_PAY_PROOF, V_REQ, r.signed_len, r.sig, V_DEVICE_PUB) == 0);
  CHECK(verify_prefixed("", V_REQ, r.signed_len, r.sig, V_DEVICE_PUB) == 0);
  sign_prefixed(V_DEVICE_SEED, VK_PREFIX_PAY_REQ, V_REQ, r.signed_len, sig); CHECK(memcmp(sig, r.sig, 64) == 0);
  /* build reproduces the vector byte for byte; signed_len of the input is ignored */
  r.signed_len = 0;
  n = vk_req_build(&r, frame, sizeof frame);
  CHECK(n == sizeof V_REQ && memcmp(frame, V_REQ, n) == 0);
  CHECK(vk_req_build(&r, frame, sizeof V_REQ - 1) == 0);
  check_strict(V_REQ, sizeof V_REQ, p_req, &q, __LINE__);

  /* round trips: every name length, both rails, short currency */
  for (name_len = 1; name_len <= 32; name_len++) {
    memset(&r, 0, sizeof r);
    r.rail = (uint8_t)(name_len % 2 ? VK_RAIL_SOLANA : VK_RAIL_BANK);
    fill(r.payee_pubkey, 32, (uint8_t)name_len); r.amount = 0x0102030405060708ULL + name_len;
    memcpy(r.currency, name_len % 2 ? "HACK" : "USD", name_len % 2 ? 4 : 3);
    fill(r.req_id, 8, 0x90); r.expiry = 0xA1B2C3D4u; r.name_len = (uint8_t)name_len;
    memset(r.name, 'a' + (int)(name_len % 26), name_len); fill(r.sig, 64, 0x33);
    n = vk_req_build(&r, frame, sizeof frame);
    CHECK(n == 62 + name_len + 64 && n >= VK_REQ_MIN_LEN && n <= VK_REQ_MAX_LEN);
    CHECK(frame[0] == 'V' && frame[1] == 'K' && frame[2] == 1 && frame[3] == VK_T_REQ && frame[4] == r.rail && frame[61] == name_len);
    CHECK(frame[37] == (uint8_t)(0x08 + name_len) && frame[44] == 0x01);            /* amount little-endian */
    CHECK(frame[57] == 0xD4 && frame[60] == 0xA1);                                  /* expiry little-endian */
    CHECK(name_len % 2 || (frame[45] == 'U' && frame[47] == 'D' && frame[48] == 0)); /* NUL-padded currency */
    CHECK(vk_req_parse(frame, n, &q) == 0);
    CHECK(q.rail == r.rail && q.amount == r.amount && q.expiry == r.expiry && q.name_len == r.name_len && q.signed_len == 62 + name_len);
    CHECK(!memcmp(q.payee_pubkey, r.payee_pubkey, 32) && !strcmp(q.currency, r.currency) && !memcmp(q.req_id, r.req_id, 8));
    CHECK(!memcmp(q.name, r.name, name_len) && q.name[name_len] == 0 && !memcmp(q.sig, r.sig, 64));
    CHECK(vk_req_build(&q, again, sizeof again) == n && memcmp(again, frame, n) == 0);
    if (name_len == 1 || name_len == 17 || name_len == 32) check_strict(frame, n, p_req, &q, __LINE__);
  }

  /* strict fields */
  memcpy(frame, V_REQ, sizeof V_REQ);
  frame[4] = 0; CHECK(vk_req_parse(frame, sizeof V_REQ, &q) != 0);                   /* unknown rail */
  frame[4] = 3; CHECK(vk_req_parse(frame, sizeof V_REQ, &q) != 0);
  memcpy(frame, V_REQ, sizeof V_REQ); frame[61] = 0;  CHECK(vk_req_parse(frame, sizeof V_REQ, &q) != 0);    /* name_len 0 */
  memcpy(frame, V_REQ, sizeof V_REQ); frame[61] = 11; CHECK(vk_req_parse(frame, sizeof V_REQ, &q) != 0);    /* name_len disagrees with the length */
  memcpy(frame, V_REQ, sizeof V_REQ); frame[61] = 13; CHECK(vk_req_parse(frame, sizeof V_REQ, &q) != 0);
  memset(frame, 'A', sizeof frame); memcpy(frame, V_REQ, 61); frame[61] = 33;        /* name_len 33 with a matching length */
  CHECK(vk_req_parse(frame, 62 + 33 + 64, &q) != 0);
  memcpy(frame, V_REQ, sizeof V_REQ); frame[62 + 3] = 0x1F; CHECK(vk_req_parse(frame, sizeof V_REQ, &q) != 0);   /* control character in the name */
  memcpy(frame, V_REQ, sizeof V_REQ); frame[62 + 3] = 0x7F; CHECK(vk_req_parse(frame, sizeof V_REQ, &q) != 0);
  memcpy(frame, V_REQ, sizeof V_REQ); frame[62 + 3] = 0xC3; CHECK(vk_req_parse(frame, sizeof V_REQ, &q) != 0);   /* not ASCII */
  memcpy(frame, V_REQ, sizeof V_REQ); frame[62 + 3] = 0;    CHECK(vk_req_parse(frame, sizeof V_REQ, &q) != 0);
  memcpy(frame, V_REQ, sizeof V_REQ); memset(frame + 45, 0, 4); CHECK(vk_req_parse(frame, sizeof V_REQ, &q) != 0);   /* empty currency */
  memcpy(frame, V_REQ, sizeof V_REQ); frame[46] = 0;    CHECK(vk_req_parse(frame, sizeof V_REQ, &q) != 0);       /* "H\0CK": text after the padding */
  memcpy(frame, V_REQ, sizeof V_REQ); frame[45] = 0x07; CHECK(vk_req_parse(frame, sizeof V_REQ, &q) != 0);       /* not printable */
  /* build refuses what parse would refuse */
  CHECK(vk_req_parse(V_REQ, sizeof V_REQ, &r) == 0);
  q = r; q.rail = 0;     CHECK(vk_req_build(&q, frame, sizeof frame) == 0);
  q = r; q.name_len = 0; CHECK(vk_req_build(&q, frame, sizeof frame) == 0);
  q = r; q.name_len = 33; CHECK(vk_req_build(&q, frame, sizeof frame) == 0);
  q = r; q.name[2] = '\n'; CHECK(vk_req_build(&q, frame, sizeof frame) == 0);
  q = r; q.currency[0] = 0; CHECK(vk_req_build(&q, frame, sizeof frame) == 0);
  q = r; memcpy(q.currency, "HACKS", 5); CHECK(vk_req_build(&q, frame, sizeof frame) == 0);
}

static void test_chal(void) {
  vk_chal_t c, d; size_t n;
  fill(c.req_id, 8, 1); fill(c.nonce, 16, 40); fill(c.payer_pubkey, 32, 90);
  n = vk_chal_build(&c, frame, sizeof frame);
  CHECK(n == 60 && n == VK_CHAL_LEN && frame[3] == VK_T_CHAL);
  CHECK(!memcmp(frame + 4, c.req_id, 8) && !memcmp(frame + 12, c.nonce, 16) && !memcmp(frame + 28, c.payer_pubkey, 32));
  memset(&d, 0, sizeof d);
  CHECK(vk_chal_parse(frame, n, &d) == 0 && memcmp(&c, &d, sizeof c) == 0);
  CHECK(vk_chal_build(&d, again, sizeof again) == n && memcmp(again, frame, n) == 0);
  CHECK(vk_chal_build(&c, again, 59) == 0);
  check_strict(frame, n, p_chal, &d, __LINE__);
}

static void test_proof(void) {
  vk_proof_t p, q; vk_req_t r; size_t n; uint8_t signed_bytes[56], sig[64];
  CHECK(vk_req_parse(V_REQ, sizeof V_REQ, &r) == 0);
  /* the signed-bytes helper matches the vector: req_id || nonce || payer_pubkey, domain pay-proof */
  CHECK(sizeof V_PROOF_NONCE == 16 && sizeof V_PROOF_PAYER == 32 && sizeof V_PROOF_SIG == 64 && memcmp(V_PROOF_PAYER, V_PAYER, 32) == 0);
  CHECK(vk_proof_signed_bytes(r.req_id, V_PROOF_NONCE, V_PROOF_PAYER, signed_bytes) == 56);
  CHECK(!memcmp(signed_bytes, r.req_id, 8) && !memcmp(signed_bytes + 8, V_PROOF_NONCE, 16) && !memcmp(signed_bytes + 24, V_PROOF_PAYER, 32));
  CHECK(verify_prefixed(VK_PREFIX_PAY_PROOF, signed_bytes, 56, V_PROOF_SIG, V_DEVICE_PUB) == 1);
  CHECK(verify_prefixed(VK_PREFIX_PAY_REQ, signed_bytes, 56, V_PROOF_SIG, V_DEVICE_PUB) == 0);
  CHECK(verify_prefixed(VK_PREFIX_PAY_PROOF, signed_bytes, 56, V_PROOF_SIG, V_ISSUER_PUB) == 0);
  sign_prefixed(V_DEVICE_SEED, VK_PREFIX_PAY_PROOF, signed_bytes, 56, sig); CHECK(memcmp(sig, V_PROOF_SIG, 64) == 0);
  signed_bytes[10] ^= 1;                                                    /* another nonce: the proof does not carry over */
  CHECK(verify_prefixed(VK_PREFIX_PAY_PROOF, signed_bytes, 56, V_PROOF_SIG, V_DEVICE_PUB) == 0);
  /* frame round trip */
  memcpy(p.req_id, r.req_id, 8); memcpy(p.sig, V_PROOF_SIG, 64);
  n = vk_proof_build(&p, frame, sizeof frame);
  CHECK(n == 76 && n == VK_PROOF_LEN && frame[3] == VK_T_PROOF && !memcmp(frame + 4, p.req_id, 8) && !memcmp(frame + 12, p.sig, 64));
  memset(&q, 0, sizeof q);
  CHECK(vk_proof_parse(frame, n, &q) == 0 && memcmp(&p, &q, sizeof p) == 0);
  CHECK(vk_proof_build(&q, again, sizeof again) == n && memcmp(again, frame, n) == 0);
  CHECK(vk_proof_build(&p, again, 75) == 0);
  check_strict(frame, n, p_proof, &q, __LINE__);
}

static void test_result(void) {
  vk_result_t r, q; size_t n; uint8_t status;
  for (status = 0; status <= 2; status++) {
    fill(r.req_id, 8, 5); r.status = status; fill(r.ref, 64, 77);
    n = vk_result_build(&r, frame, sizeof frame);
    CHECK(n == 77 && n == VK_RESULT_LEN && frame[3] == VK_T_RESULT && frame[12] == status && !memcmp(frame + 4, r.req_id, 8) && !memcmp(frame + 13, r.ref, 64));
    memset(&q, 0xEE, sizeof q);
    CHECK(vk_result_parse(frame, n, &q) == 0 && q.status == status && !memcmp(q.req_id, r.req_id, 8) && !memcmp(q.ref, r.ref, 64));
    CHECK(vk_result_build(&q, again, sizeof again) == n && memcmp(again, frame, n) == 0);
    check_strict(frame, n, p_result, &q, __LINE__);
  }
  CHECK(VK_RESULT_OK == 0 && VK_RESULT_REJECTED == 1 && VK_RESULT_FAILED == 2);
  frame[12] = 3; CHECK(vk_result_parse(frame, 77, &q) != 0);              /* unknown status */
  r.status = 3; CHECK(vk_result_build(&r, again, sizeof again) == 0);
  r.status = 0; CHECK(vk_result_build(&r, again, 76) == 0);
}

static void test_hello(void) {
  vk_hello_t h, g; size_t n, name_len;
  for (name_len = 1; name_len <= 32; name_len++) {
    memset(&h, 0, sizeof h);
    fill(h.pubkey, 32, (uint8_t)(3 * name_len)); fill(h.nonce, 16, 200); h.name_len = (uint8_t)name_len; memset(h.name, 'Z' - (int)(name_len % 20), name_len);
    n = vk_hello_build(&h, frame, sizeof frame);
    CHECK(n == 53 + name_len && n <= VK_HELLO_MAX_LEN && frame[3] == VK_T_CONTACT_HELLO && frame[52] == name_len);
    CHECK(!memcmp(frame + 4, h.pubkey, 32) && !memcmp(frame + 36, h.nonce, 16) && !memcmp(frame + 53, h.name, name_len));
    CHECK(vk_hello_parse(frame, n, &g) == 0);
    CHECK(!memcmp(g.pubkey, h.pubkey, 32) && !memcmp(g.nonce, h.nonce, 16) && g.name_len == name_len && !memcmp(g.name, h.name, name_len) && g.name[name_len] == 0);
    CHECK(vk_hello_build(&g, again, sizeof again) == n && memcmp(again, frame, n) == 0);
    CHECK(vk_hello_build(&h, again, n - 1) == 0);
    if (name_len == 1 || name_len == 9 || name_len == 32) check_strict(frame, n, p_hello, &g, __LINE__);
  }
  memset(&h, 0, sizeof h); h.name_len = 5; memcpy(h.name, "Alice", 5);
  n = vk_hello_build(&h, frame, sizeof frame); CHECK(n == 58);
  frame[52] = 0;  CHECK(vk_hello_parse(frame, n, &g) != 0);
  frame[52] = 4;  CHECK(vk_hello_parse(frame, n, &g) != 0);
  frame[52] = 5; frame[55] = 0x7F; CHECK(vk_hello_parse(frame, n, &g) != 0);
  memset(frame + 53, 'A', 33); frame[52] = 33; CHECK(vk_hello_parse(frame, 53 + 33, &g) != 0);
  h.name_len = 0;  CHECK(vk_hello_build(&h, frame, sizeof frame) == 0);
  h.name_len = 33; CHECK(vk_hello_build(&h, frame, sizeof frame) == 0);
  h.name_len = 5; h.name[4] = '\t'; CHECK(vk_hello_build(&h, frame, sizeof frame) == 0);
}

static void test_card(void) {
  vk_card_t c, d; size_t n, m, name_len; uint8_t signed_bytes[113], peer_pub[32];
  for (name_len = 1; name_len <= 32; name_len++) {
    memset(&c, 0, sizeof c);
    fill(c.peer_pubkey, 32, 11); fill(c.peer_nonce, 16, 22); fill(c.pubkey, 32, 33); c.name_len = (uint8_t)name_len;
    memset(c.name, '0' + (int)(name_len % 10), name_len); fill(c.sig, 64, 44);
    n = vk_card_build(&c, frame, sizeof frame);
    CHECK(n == 85 + name_len + 64 && n >= 150 && n <= VK_CARD_MAX_LEN && frame[3] == VK_T_CONTACT_CARD && frame[84] == name_len);
    CHECK(!memcmp(frame + 4, c.peer_pubkey, 32) && !memcmp(frame + 36, c.peer_nonce, 16) && !memcmp(frame + 52, c.pubkey, 32));
    CHECK(!memcmp(frame + 85, c.name, name_len) && !memcmp(frame + 85 + name_len, c.sig, 64));
    CHECK(vk_card_parse(frame, n, &d) == 0);
    CHECK(!memcmp(d.peer_pubkey, c.peer_pubkey, 32) && !memcmp(d.peer_nonce, c.peer_nonce, 16) && !memcmp(d.pubkey, c.pubkey, 32));
    CHECK(d.name_len == name_len && !memcmp(d.name, c.name, name_len) && d.name[name_len] == 0 && !memcmp(d.sig, c.sig, 64));
    CHECK(vk_card_build(&d, again, sizeof again) == n && memcmp(again, frame, n) == 0);
    CHECK(vk_card_build(&c, again, n - 1) == 0);
    /* signed bytes: peer_nonce[16] || peer_pubkey[32] || pubkey[32] || name_len[1] || name */
    m = vk_card_signed_bytes(&d, signed_bytes);
    CHECK(m == 81 + name_len && m <= VK_CARD_SIGNED_MAX);
    CHECK(!memcmp(signed_bytes, c.peer_nonce, 16) && !memcmp(signed_bytes + 16, c.peer_pubkey, 32) && !memcmp(signed_bytes + 48, c.pubkey, 32));
    CHECK(signed_bytes[80] == name_len && !memcmp(signed_bytes + 81, c.name, name_len));
    if (name_len == 1 || name_len == 20 || name_len == 32) check_strict(frame, n, p_card, &d, __LINE__);
  }
  /* a card the vector device makes for the vector payer, signed in domain contact, verifies after a round trip */
  memset(&c, 0, sizeof c);
  memcpy(peer_pub, V_PAYER, 32);
  memcpy(c.peer_pubkey, peer_pub, 32); memcpy(c.peer_nonce, V_PROOF_NONCE, 16); memcpy(c.pubkey, V_DEVICE_PUB, 32);
  c.name_len = 12; memcpy(c.name, "MHacks Merch", 12);
  m = vk_card_signed_bytes(&c, signed_bytes); CHECK(m == 93);
  sign_prefixed(V_DEVICE_SEED, VK_PREFIX_CONTACT, signed_bytes, m, c.sig);
  n = vk_card_build(&c, frame, sizeof frame); CHECK(n == 161);
  CHECK(vk_card_parse(frame, n, &d) == 0);
  m = vk_card_signed_bytes(&d, signed_bytes);
  CHECK(verify_prefixed(VK_PREFIX_CONTACT, signed_bytes, m, d.sig, d.pubkey) == 1);
  d.peer_nonce[0] ^= 1;                                     /* replayed against another swap nonce */
  m = vk_card_signed_bytes(&d, signed_bytes);
  CHECK(verify_prefixed(VK_PREFIX_CONTACT, signed_bytes, m, d.sig, d.pubkey) == 0);
  /* strict fields */
  frame[84] = 0;  CHECK(vk_card_parse(frame, n, &d) != 0);
  frame[84] = 11; CHECK(vk_card_parse(frame, n, &d) != 0);
  frame[84] = 12; frame[85] = 0x19; CHECK(vk_card_parse(frame, n, &d) != 0);
  memset(frame + 85, 'A', 33 + 64); frame[84] = 33; CHECK(vk_card_parse(frame, 85 + 33 + 64, &d) != 0);
  c.name_len = 0;  CHECK(vk_card_build(&c, frame, sizeof frame) == 0 && vk_card_signed_bytes(&c, signed_bytes) == 0);
  c.name_len = 33; CHECK(vk_card_build(&c, frame, sizeof frame) == 0 && vk_card_signed_bytes(&c, signed_bytes) == 0);
}

/* A frame of one type is never accepted by another type's parser. */
static void test_cross_type(void) {
  vk_req_t r; vk_chal_t c; vk_proof_t p; vk_result_t s; vk_hello_t h; vk_card_t k; size_t n;
  memset(&c, 1, sizeof c); n = vk_chal_build(&c, frame, sizeof frame);
  CHECK(vk_req_parse(frame, n, &r) != 0 && vk_proof_parse(frame, n, &p) != 0 && vk_result_parse(frame, n, &s) != 0 && vk_hello_parse(frame, n, &h) != 0 && vk_card_parse(frame, n, &k) != 0);
  CHECK(vk_chal_parse(V_REQ, sizeof V_REQ, &c) != 0 && vk_proof_parse(V_REQ, sizeof V_REQ, &p) != 0 && vk_card_parse(V_REQ, sizeof V_REQ, &k) != 0 && vk_hello_parse(V_REQ, sizeof V_REQ, &h) != 0);
  /* a REQ's bytes relabelled as a CARD of the same length */
  memcpy(frame, V_REQ, sizeof V_REQ); frame[3] = VK_T_CONTACT_CARD;
  CHECK(vk_req_parse(frame, sizeof V_REQ, &r) != 0);
}

int main(void) {
  test_frame_type();
  test_host_ed25519();
  test_req();
  test_chal();
  test_proof();
  test_result();
  test_hello();
  test_card();
  test_cross_type();
  printf(fails ? "%d FAILED\n" : "all frames tests passed\n", fails);
  return fails != 0;
}
