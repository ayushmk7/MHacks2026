#include "pay_proto.h"
#include <string.h>

static void hdr(uint8_t *o, uint8_t type) { o[0] = PAY_MAGIC0; o[1] = PAY_MAGIC1; o[2] = PAY_VERSION; o[3] = type; }
static int hdr_ok(const uint8_t *b, size_t len, size_t want, uint8_t type) {
  return len == want && b[0] == PAY_MAGIC0 && b[1] == PAY_MAGIC1 && b[2] == PAY_VERSION && b[3] == type;
}
int pay_peek(const uint8_t *b, size_t len) {
  if (len < PAY_HDR_LEN || b[0] != PAY_MAGIC0 || b[1] != PAY_MAGIC1 || b[2] != PAY_VERSION) return 0;
  return (b[3] >= PAY_T_REQ && b[3] <= PAY_T_RCPT) ? b[3] : 0;
}
int pay_name_ok(const char *name, size_t len) {
  size_t i;
  if (len < 1 || len > PAY_NAME_MAX) return 0;
  if (name[0] == ' ' || name[len - 1] == ' ') return 0;
  for (i = 0; i < len; i++) if ((unsigned char)name[i] < 0x20 || (unsigned char)name[i] > 0x7E) return 0;
  return 1;
}
static int name_decode(const uint8_t *len_byte, char out[PAY_NAME_MAX + 1], uint8_t *out_len) {
  const uint8_t n = len_byte[0]; size_t i;
  if (!pay_name_ok((const char *)len_byte + 1, n)) return -1;
  for (i = n; i < PAY_NAME_MAX; i++) if (len_byte[1 + i] != 0) return -1;   /* padding must be zero */
  memcpy(out, len_byte + 1, n); out[n] = '\0'; *out_len = n;
  return 0;
}
static void name_encode(uint8_t *len_byte, const char *name, uint8_t n) {
  len_byte[0] = n; memset(len_byte + 1, 0, PAY_NAME_MAX); memcpy(len_byte + 1, name, n);
}

void pay_req_encode(const pay_req_t *r, uint8_t o[PAY_REQ_LEN]) {
  int i;
  hdr(o, PAY_T_REQ);
  memcpy(o + 4, r->payee, 32);
  for (i = 0; i < 8; i++) o[36 + i] = (uint8_t)(r->amount >> (8 * i));
  memcpy(o + 44, r->mint_tag, 4);
  memcpy(o + 48, r->id, 8);
  o[56] = (uint8_t)r->ttl_s; o[57] = (uint8_t)(r->ttl_s >> 8);
  name_encode(o + 58, r->name, r->name_len);
  memcpy(o + 91, r->sig, 64);
}
int pay_req_decode(const uint8_t *b, size_t len, pay_req_t *r) {
  int i;
  if (!hdr_ok(b, len, PAY_REQ_LEN, PAY_T_REQ)) return -1;
  memcpy(r->payee, b + 4, 32);
  r->amount = 0; for (i = 0; i < 8; i++) r->amount |= (uint64_t)b[36 + i] << (8 * i);
  memcpy(r->mint_tag, b + 44, 4);
  memcpy(r->id, b + 48, 8);
  r->ttl_s = (uint16_t)(b[56] | (b[57] << 8));
  if (name_decode(b + 58, r->name, &r->name_len)) return -1;
  memcpy(r->sig, b + 91, 64);
  if (r->amount == 0 || r->ttl_s == 0) return -1;
  return 0;
}
void pay_req_signed_bytes(const uint8_t w[PAY_REQ_LEN], uint8_t out[PAY_REQ_SIGNED_LEN]) {
  memcpy(out, PAY_DOMAIN_REQ, 8); memcpy(out + 8, w, 91);
}

void pay_chal_encode(const pay_chal_t *c, uint8_t o[PAY_CHAL_LEN]) {
  hdr(o, PAY_T_CHAL); memcpy(o + 4, c->id, 8); memcpy(o + 12, c->nonce, 16); memcpy(o + 28, c->payer, 32);
}
int pay_chal_decode(const uint8_t *b, size_t len, pay_chal_t *c) {
  if (!hdr_ok(b, len, PAY_CHAL_LEN, PAY_T_CHAL)) return -1;
  memcpy(c->id, b + 4, 8); memcpy(c->nonce, b + 12, 16); memcpy(c->payer, b + 28, 32); return 0;
}
void pay_proof_encode(const pay_proof_t *p, uint8_t o[PAY_PROOF_LEN]) {
  hdr(o, PAY_T_PROOF); memcpy(o + 4, p->id, 8); memcpy(o + 12, p->payee, 32); memcpy(o + 44, p->sig, 64);
}
int pay_proof_decode(const uint8_t *b, size_t len, pay_proof_t *p) {
  if (!hdr_ok(b, len, PAY_PROOF_LEN, PAY_T_PROOF)) return -1;
  memcpy(p->id, b + 4, 8); memcpy(p->payee, b + 12, 32); memcpy(p->sig, b + 44, 64); return 0;
}
void pay_proof_signed_bytes(const uint8_t id[8], const uint8_t nonce[16], const uint8_t payer[32],
                            uint8_t out[PAY_PROOF_SIGNED_LEN]) {
  memcpy(out, PAY_DOMAIN_PROOF, 10); memcpy(out + 10, id, 8); memcpy(out + 18, nonce, 16); memcpy(out + 34, payer, 32);
}
void pay_hello_encode(uint8_t o[PAY_HELLO_LEN]) { hdr(o, PAY_T_HELLO); }
void pay_iam_encode(const pay_iam_t *m, uint8_t o[PAY_IAM_LEN]) {
  hdr(o, PAY_T_IAM); memcpy(o + 4, m->pubkey, 32); name_encode(o + 36, m->name, m->name_len);
}
int pay_iam_decode(const uint8_t *b, size_t len, pay_iam_t *m) {
  if (!hdr_ok(b, len, PAY_IAM_LEN, PAY_T_IAM)) return -1;
  memcpy(m->pubkey, b + 4, 32); return name_decode(b + 36, m->name, &m->name_len);
}
void pay_paid_encode(const pay_paid_t *m, uint8_t o[PAY_PAID_LEN]) {
  hdr(o, PAY_T_PAID); memcpy(o + 4, m->id, 8); memcpy(o + 12, m->tx_sig, 64);
}
int pay_paid_decode(const uint8_t *b, size_t len, pay_paid_t *m) {
  if (!hdr_ok(b, len, PAY_PAID_LEN, PAY_T_PAID)) return -1;
  memcpy(m->id, b + 4, 8); memcpy(m->tx_sig, b + 12, 64); return 0;
}
void pay_rcpt_encode(const pay_rcpt_t *m, uint8_t o[PAY_RCPT_LEN]) {
  hdr(o, PAY_T_RCPT); memcpy(o + 4, m->id, 8); memcpy(o + 12, m->tx_sig, 64); memcpy(o + 76, m->sig, 64);
}
int pay_rcpt_decode(const uint8_t *b, size_t len, pay_rcpt_t *m) {
  if (!hdr_ok(b, len, PAY_RCPT_LEN, PAY_T_RCPT)) return -1;
  memcpy(m->id, b + 4, 8); memcpy(m->tx_sig, b + 12, 64); memcpy(m->sig, b + 76, 64); return 0;
}
void pay_rcpt_signed_bytes(const uint8_t id[8], const uint8_t tx_sig[64], uint8_t out[PAY_RCPT_SIGNED_LEN]) {
  memcpy(out, PAY_DOMAIN_RCPT, 9); memcpy(out + 9, id, 8); memcpy(out + 17, tx_sig, 64);
}
