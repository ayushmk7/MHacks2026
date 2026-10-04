/* vk_payment.h - what binds an on-chain payment to the request it answers. Pure C99, no heap.
   Spec: docs/os/wallet/solana-payments.md ("Request memo", "Checking a received payment").

   The payer: a transfer that answers a request carries one Memo whose data is exactly the request
   id as 16 lower-case hex characters (00-Interfaces section 5: "In memos, req_id is written as 16
   hex characters"). The check chain refuses a request payment without it (checks.md, check 12a).

   The payee: before it shows PAID it fetches the transaction the RESULT frame names and checks with
   vk_payment_verify() that it pays this payee's token account the requested amount of the
   requested mint and carries this request's memo. RESULT frames are unsigned; this is the check
   that makes a forged RESULT worthless. */
#ifndef VK_PAYMENT_H
#define VK_PAYMENT_H
#include <stddef.h>
#include <stdint.h>
#include "sol.h"
#include "vk_reason.h"
#ifdef __cplusplus
extern "C" {
#endif

/* ---- the request memo -------------------------------------------------------------------------- */
#define VK_REQ_MEMO_LEN 16

/* out = req_id as 16 lower-case hex characters and a NUL. */
void vk_req_memo(const uint8_t req_id[8], char out[VK_REQ_MEMO_LEN + 1]);

/* 1 if memo[0..len) is exactly vk_req_memo(req_id) (16 bytes, lower case, nothing before or after). */
int vk_memo_is_req(const uint8_t *memo, size_t len, const uint8_t req_id[8]);

/* Parses 16 hex characters (either case) into 8 bytes. 0 on success. */
int vk_req_id_parse(const char *text, size_t len, uint8_t out[8]);

/* ---- the wire transaction ---------------------------------------------------------------------- */
/* A wire transaction is compact-u16 signature count || signatures || message. The badge's payments
   have exactly one signer, so this accepts only a count of 1 (the single byte 0x01), 64 bytes of
   signature and a non-empty message. 0 on success; *sig and *msg point into tx. */
int vk_wire_split(const uint8_t *tx, size_t len, const uint8_t **sig, const uint8_t **msg, size_t *msg_len);

/* ---- the payee's check ------------------------------------------------------------------------- */
typedef enum {
  VK_PAY_OK = 0,
  VK_PAY_ERR_ARG,            /* expect is NULL, or to / mint / req_id is NULL, or amount is 0 */
  VK_PAY_ERR_WIRE,           /* not a one-signature wire transaction */
  VK_PAY_ERR_SHAPE,          /* the message is not one the decoder accepts (solana-payments.md, rules 1-12) */
  VK_PAY_ERR_SIGNATURE,      /* the signature does not verify over the message with its fee payer's key */
  VK_PAY_ERR_SIG_MISMATCH,   /* another signature than expect->sig */
  VK_PAY_ERR_PAYER,          /* another fee payer than expect->payer */
  VK_PAY_ERR_MINT,           /* another mint, or other decimals */
  VK_PAY_ERR_RECIPIENT,      /* another destination token account */
  VK_PAY_ERR_AMOUNT,         /* another amount */
  VK_PAY_ERR_MEMO            /* no memo, or not exactly this request's memo */
} vk_pay_err_t;

typedef struct {
  const uint8_t *to;         /* 32: the token account that must be credited (required) */
  const uint8_t *mint;       /* 32 (required) */
  uint8_t decimals;          /* the token's decimals */
  uint64_t amount;           /* raw units, not 0 (required) */
  const uint8_t *req_id;     /* 8: the request the memo must name (required) */
  const uint8_t *payer;      /* 32 or NULL: the fee payer (the payer badge's key) must be this one */
  const uint8_t *sig;        /* 64 or NULL: the transaction's signature must be this one */
  /* NULL: the payer's signature over the message is not checked. 1 = valid. */
  int (*verify)(const uint8_t *msg, size_t len, const uint8_t *sig, const uint8_t *pubkey);
} vk_pay_expect_t;

typedef struct {
  sol_transfer_t transfer;   /* the decoded transfer; transfer.memo points into tx */
  uint8_t sig[64];           /* the transaction's signature (its id on chain) */
} vk_pay_seen_t;

/* Checks a wire transaction against what the payee expects, in the order of the enum above; the
   first difference is the answer. `seen` (may be NULL) is filled once the message decoded.
   It reads bytes only: whether the transaction landed and succeeded (getTransaction's meta.err is
   null) is the caller's check. */
vk_pay_err_t vk_payment_verify(const uint8_t *tx, size_t len, const vk_pay_expect_t *expect, vk_pay_seen_t *seen);

const char *vk_pay_err_name(vk_pay_err_t e);    /* "ok", "arg", "wire", "shape", "signature", "sig", "payer",
                                                   "mint", "recipient", "amount", "memo"; "?" otherwise */
vk_reason_t vk_pay_reason(vk_pay_err_t e);       /* ok, bad_arg, undecodable (wire, shape), bad_proof
                                                   (signature), mismatch (everything else) */

#ifdef __cplusplus
}
#endif
#endif
