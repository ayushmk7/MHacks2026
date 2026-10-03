#include "pay_proto.h"
#include "sol.h"
#include "monocypher-ed25519.h"
#include <stdio.h>
#include <string.h>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
int main(void) {
  uint8_t seed[32], sk[64], pk[32], payer[32], wire[PAY_REQ_LEN], signed_bytes[PAY_REQ_SIGNED_LEN], pw[PAY_PROOF_LEN], ps[PAY_PROOF_SIGNED_LEN], cw[PAY_CHAL_LEN], iw[PAY_IAM_LEN], rw[PAY_RCPT_LEN], rs[PAY_RCPT_SIGNED_LEN];
  pay_req_t r, r2; pay_chal_t c, c2; pay_proof_t p, p2; pay_iam_t m, m2; sol_transfer_t t; int i;
  for (i = 0; i < 32; i++) { seed[i] = (uint8_t)(i + 1); payer[i] = (uint8_t)(0xA0 + i); }
  crypto_ed25519_key_pair(sk, pk, seed);
  memset(&r, 0, sizeof r); memcpy(r.payee, pk, 32); r.amount = 1000; memcpy(r.mint_tag, "\x06\x9b\x88\x57", 4);
  for (i = 0; i < 8; i++) r.id[i] = (uint8_t)(0x11 * (i + 1));
  r.ttl_s = 120; r.name_len = 12; memcpy(r.name, "MHacks Merch", 13);
  pay_req_encode(&r, wire); pay_req_signed_bytes(wire, signed_bytes);
  crypto_ed25519_sign(r.sig, sk, signed_bytes, sizeof signed_bytes); pay_req_encode(&r, wire);
  CHECK(pay_peek(wire, sizeof wire) == PAY_T_REQ);
  CHECK(pay_req_decode(wire, sizeof wire, &r2) == 0 && r2.amount == 1000 && r2.ttl_s == 120 && !strcmp(r2.name, "MHacks Merch") && !memcmp(r2.id, r.id, 8));
  pay_req_signed_bytes(wire, signed_bytes);
  CHECK(crypto_ed25519_check(r2.sig, r2.payee, signed_bytes, sizeof signed_bytes) == 0);
  wire[36] ^= 1; pay_req_signed_bytes(wire, signed_bytes);                 /* tampered amount */
  CHECK(crypto_ed25519_check(r2.sig, r2.payee, signed_bytes, sizeof signed_bytes) != 0); wire[36] ^= 1;
  wire[58] = 33; CHECK(pay_req_decode(wire, sizeof wire, &r2) == -1); wire[58] = 12;      /* name too long */
  wire[59] = 0x07; CHECK(pay_req_decode(wire, sizeof wire, &r2) == -1); wire[59] = 'M';   /* control char */
  wire[80] = 'x'; CHECK(pay_req_decode(wire, sizeof wire, &r2) == -1); wire[80] = 0;      /* dirty padding */
  CHECK(pay_req_decode(wire, sizeof wire - 1, &r2) == -1);
  wire[2] = 2; CHECK(pay_peek(wire, sizeof wire) == 0); wire[2] = 1;                      /* future version ignored */
  memcpy(c.id, r.id, 8); for (i = 0; i < 16; i++) c.nonce[i] = (uint8_t)(0xF0 ^ i); memcpy(c.payer, payer, 32);
  pay_chal_encode(&c, cw); CHECK(pay_chal_decode(cw, sizeof cw, &c2) == 0 && !memcmp(&c, &c2, sizeof c));
  memcpy(p.id, r.id, 8); memcpy(p.payee, pk, 32); pay_proof_signed_bytes(c.id, c.nonce, c.payer, ps);
  crypto_ed25519_sign(p.sig, sk, ps, sizeof ps); pay_proof_encode(&p, pw);
  CHECK(pay_proof_decode(pw, sizeof pw, &p2) == 0 && crypto_ed25519_check(p2.sig, p2.payee, ps, sizeof ps) == 0);
  c.nonce[0] ^= 1; pay_proof_signed_bytes(c.id, c.nonce, c.payer, ps);     /* a different nonce must not verify */
  CHECK(crypto_ed25519_check(p2.sig, p2.payee, ps, sizeof ps) != 0);
  memcpy(m.pubkey, pk, 32); m.name_len = 10; memcpy(m.name, "badge-4F2A", 11); pay_iam_encode(&m, iw);
  CHECK(pay_iam_decode(iw, sizeof iw, &m2) == 0 && !strcmp(m2.name, "badge-4F2A"));
  { pay_rcpt_t x, y; memcpy(x.id, r.id, 8); memset(x.tx_sig, 7, 64); pay_rcpt_signed_bytes(x.id, x.tx_sig, rs);
    crypto_ed25519_sign(x.sig, sk, rs, sizeof rs); pay_rcpt_encode(&x, rw); CHECK(pay_rcpt_decode(rw, sizeof rw, &y) == 0 && crypto_ed25519_check(y.sig, pk, rs, sizeof rs) == 0); }
  CHECK(PAY_REQ_LEN <= 240 && PAY_CHAL_LEN <= 64 && PAY_PROOF_LEN <= 128 && PAY_RCPT_LEN <= 240);
  CHECK(PAY_REQ_SIGNED_LEN <= 180 && PAY_PROOF_SIGNED_LEN <= 180 && PAY_RCPT_SIGNED_LEN <= 180);   /* fits the unmodified SE050 limit */
  /* domain separation: none of the signed byte strings is a transaction the decoder accepts */
  pay_req_signed_bytes(wire, signed_bytes); CHECK(sol_tx_decode_transfer(signed_bytes, sizeof signed_bytes, &t) != SOL_TX_OK);
  CHECK(sol_tx_decode_transfer(ps, sizeof ps, &t) != SOL_TX_OK && sol_tx_decode_transfer(rs, sizeof rs, &t) != SOL_TX_OK);
  CHECK(signed_bytes[0] == 0x70 && signed_bytes[3] == 0x2d);   /* 'p' = 112 required signatures, '-' = 45 account keys */
  printf(fails ? "%d FAILED\n" : "all pay tests passed\n", fails);
  return fails != 0;
}
