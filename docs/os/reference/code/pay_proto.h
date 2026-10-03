/* pay_proto.h - wire codec for the badge-to-badge payment messages (ESP-NOW application payloads).
   Pure C99, no heap. All integers little-endian. Signing and verification are supplied by the caller. */
#ifndef PAY_PROTO_H
#define PAY_PROTO_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PAY_MAGIC0 0x48 /* 'H' */
#define PAY_MAGIC1 0x50 /* 'P' */
#define PAY_VERSION 0x01

typedef enum {
  PAY_T_REQ = 0x01, PAY_T_CHAL = 0x02, PAY_T_PROOF = 0x03, PAY_T_HELLO = 0x04,
  PAY_T_IAM = 0x05, PAY_T_PAID = 0x06, PAY_T_RCPT = 0x07
} pay_type_t;

#define PAY_HDR_LEN     4
#define PAY_REQ_LEN   155
#define PAY_CHAL_LEN   60
#define PAY_PROOF_LEN 108
#define PAY_HELLO_LEN   4
#define PAY_IAM_LEN    69
#define PAY_PAID_LEN   76
#define PAY_RCPT_LEN  140
#define PAY_ID_LEN      8
#define PAY_NONCE_LEN  16
#define PAY_NAME_MAX   32

/* Domain-separation prefixes. The byte string that is signed is prefix || fields. */
#define PAY_DOMAIN_REQ   "pay-req:"     /* 8 bytes  */
#define PAY_DOMAIN_PROOF "pay-proof:"   /* 10 bytes */
#define PAY_DOMAIN_RCPT  "pay-rcpt:"    /* 9 bytes  */
#define PAY_REQ_SIGNED_LEN   (8 + 91)   /* 99  */
#define PAY_PROOF_SIGNED_LEN (10 + 56)  /* 66  */
#define PAY_RCPT_SIGNED_LEN  (9 + 72)   /* 81  */

typedef struct {
  uint8_t  payee[32];
  uint64_t amount;                 /* raw base units */
  uint8_t  mint_tag[4];            /* first 4 bytes of the mint public key */
  uint8_t  id[PAY_ID_LEN];
  uint16_t ttl_s;
  uint8_t  name_len;               /* 1..32 */
  char     name[PAY_NAME_MAX + 1]; /* NUL-terminated copy */
  uint8_t  sig[64];
} pay_req_t;

typedef struct { uint8_t id[PAY_ID_LEN]; uint8_t nonce[PAY_NONCE_LEN]; uint8_t payer[32]; } pay_chal_t;
typedef struct { uint8_t id[PAY_ID_LEN]; uint8_t payee[32]; uint8_t sig[64]; } pay_proof_t;
typedef struct { uint8_t pubkey[32]; uint8_t name_len; char name[PAY_NAME_MAX + 1]; } pay_iam_t;
typedef struct { uint8_t id[PAY_ID_LEN]; uint8_t tx_sig[64]; } pay_paid_t;
typedef struct { uint8_t id[PAY_ID_LEN]; uint8_t tx_sig[64]; uint8_t sig[64]; } pay_rcpt_t;

/* Returns the message type if buf is a well-formed header of this protocol version, else 0. */
int pay_peek(const uint8_t *buf, size_t len);

/* 1 if name is 1..32 bytes of printable ASCII (0x20..0x7E) with no leading/trailing space. */
int pay_name_ok(const char *name, size_t len);

/* Encoders write exactly PAY_*_LEN bytes. pay_req_encode leaves the signature field as given in r->sig. */
void pay_req_encode(const pay_req_t *r, uint8_t out[PAY_REQ_LEN]);
void pay_chal_encode(const pay_chal_t *c, uint8_t out[PAY_CHAL_LEN]);
void pay_proof_encode(const pay_proof_t *p, uint8_t out[PAY_PROOF_LEN]);
void pay_hello_encode(uint8_t out[PAY_HELLO_LEN]);
void pay_iam_encode(const pay_iam_t *m, uint8_t out[PAY_IAM_LEN]);
void pay_paid_encode(const pay_paid_t *m, uint8_t out[PAY_PAID_LEN]);
void pay_rcpt_encode(const pay_rcpt_t *m, uint8_t out[PAY_RCPT_LEN]);

/* Decoders return 0 on success, -1 on wrong length, header, or field rule. */
int pay_req_decode(const uint8_t *buf, size_t len, pay_req_t *r);
int pay_chal_decode(const uint8_t *buf, size_t len, pay_chal_t *c);
int pay_proof_decode(const uint8_t *buf, size_t len, pay_proof_t *p);
int pay_iam_decode(const uint8_t *buf, size_t len, pay_iam_t *m);
int pay_paid_decode(const uint8_t *buf, size_t len, pay_paid_t *m);
int pay_rcpt_decode(const uint8_t *buf, size_t len, pay_rcpt_t *m);

/* The exact bytes that are signed / verified. */
void pay_req_signed_bytes(const uint8_t req_wire[PAY_REQ_LEN], uint8_t out[PAY_REQ_SIGNED_LEN]);
void pay_proof_signed_bytes(const uint8_t id[PAY_ID_LEN], const uint8_t nonce[PAY_NONCE_LEN],
                            const uint8_t payer[32], uint8_t out[PAY_PROOF_SIGNED_LEN]);
void pay_rcpt_signed_bytes(const uint8_t id[PAY_ID_LEN], const uint8_t tx_sig[64],
                           uint8_t out[PAY_RCPT_SIGNED_LEN]);

#ifdef __cplusplus
}
#endif
#endif
