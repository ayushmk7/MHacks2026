/* vk_checks.h - the token table, the presence result and the check chain. Pure C99, no heap.
   Spec: docs/os/wallet/solana-payments.md ("Token table"),
         docs/os/wallet/checks.md ("Presence lookup", "The check chain"). */
#ifndef VK_CHECKS_H
#define VK_CHECKS_H
#include <stddef.h>
#include <stdint.h>
#include "sol.h"
#include "vk_reason.h"
#include "vk_record.h"
#include "vk_frames.h"
#ifdef __cplusplus
extern "C" {
#endif

/* ---- token table ---------------------------------------------------------- */
typedef struct {
  uint8_t  mint[32];
  uint8_t  decimals;
  char     symbol[5];      /* NUL-terminated, 1..4 chars of [A-Z0-9]: it must fit the 4-byte currency field of a REQ frame */
  uint64_t cap;            /* raw units; above this SELECT becomes a hold. 0 = no cap */
  uint64_t max;            /* raw units; above this the payment is blocked. 0 = no max */
} vk_token_t;
#define VK_MAX_TOKENS 3   /* keeps the provisioning line under the 256-byte serial receive buffer */

/* ---- presence ------------------------------------------------------------- */
typedef enum { VK_PRESENCE_NONE, VK_PRESENCE_PENDING, VK_PRESENCE_PRESENT, VK_PRESENCE_LATE, VK_PRESENCE_BAD_SIG } vk_presence_t;

/* ---- the check chain ------------------------------------------------------ */
typedef enum { VK_TIME_NONE, VK_TIME_FLOOR, VK_TIME_SNTP } vk_time_source_t;
typedef enum { VK_SEV_GREEN, VK_SEV_AMBER, VK_SEV_RED } vk_severity_t;
typedef enum { VK_SEL_PRESS, VK_SEL_HOLD, VK_SEL_DISABLED } vk_select_t;
typedef enum {
  VK_HL_VERIFIED_PRESENT, VK_HL_NOT_PRESENT, VK_HL_CLOCK_UNSYNCED,
  VK_HL_CANNOT_READ, VK_HL_UNKNOWN_TOKEN, VK_HL_UNVERIFIED, VK_HL_REVOKED, VK_HL_EXPIRED, VK_HL_STALE,
  VK_HL_WRONG_RECIPIENT, VK_HL_WRONG_AMOUNT, VK_HL_BAD_REQUEST, VK_HL_BAD_PROOF,
  VK_HL_OVER_LIMIT
} vk_headline_t;
const char *vk_headline_text(vk_headline_t h);

typedef struct {
  const uint8_t *msg;        size_t msg_len;
  const uint8_t *record;     size_t record_len;   const uint8_t *record_sig;   /* NULL when absent */
  const uint8_t *req;        size_t req_len;                                   /* NULL when absent */
  const uint8_t *own_pubkey; const uint8_t *issuer_key;
  const vk_token_t *tokens;  size_t token_count;
  uint32_t now;              vk_time_source_t time_source;   uint32_t record_ttl_s;
  int (*verify)(const uint8_t *msg, size_t len, const uint8_t *sig, const uint8_t *pubkey);   /* 1 = valid */
  vk_presence_t (*presence)(const uint8_t req_id[8], uint8_t payee_pubkey_out[32], uint8_t nonce_out[16]);   /* may be NULL: presence is then NONE */
} vk_check_input_t;

typedef struct {
  vk_severity_t severity;  vk_select_t select;  vk_reason_t reason;  vk_headline_t headline;
  int dev_overridable;
  int decoded;       sol_transfer_t transfer;   const vk_token_t *token;   /* valid when decoded */
  sol_tx_err_t tx_err;                                                     /* the decoder's result, for the log */
  int record_ok;     vk_record_t record;                                   /* valid when record_ok */
  int req_ok;        vk_req_t req;                                         /* vk_frames.h; valid when req_ok */
  vk_presence_t presence;
} vk_verdict_t;

/* Runs checks 1..13 of checks.md in order; the first that fails decides the verdict (red,
   select DISABLED, its reason and headline). If none fails the verdict is amber or green with
   reason VK_OK, and the token's cap may turn SELECT into a hold.

   Field notes:
   - `decoded` is 1 once checks 1 and 2 passed (the message decodes, account 0 is own_pubkey, the
     token is in the table with matching decimals); `transfer` and `token` are valid from then on.
     `transfer.memo` points into in->msg.
   - `record_ok` is 1 once check 5 passed, `req_ok` once check 11 passed.
   - `presence` is the lookup's result when a request passed check 12, otherwise VK_PRESENCE_NONE.
   - `dev_overridable` is 1 only when check 4 failed (no record supplied).
   - A record counts as supplied only if record, record_len and record_sig are all non-zero; a
     request only if req and req_len are.
   - time_source VK_TIME_NONE cannot occur in the firmware (the caller raises the clock from the
     verified record first); if passed it is treated like VK_TIME_FLOOR. */
void vk_check_solana(const vk_check_input_t *in, vk_verdict_t *out);

#ifdef __cplusplus
}
#endif
#endif
