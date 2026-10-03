#ifndef WALLET_H
#define WALLET_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sol.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t badge_err_t;                 /* values: enum in app_host/badge_api.h; meanings: the badge_err_t table */

typedef enum {                               /* what the approval screen says about WHO */
  WALLET_ID_VERIFIED = 0,                    /* valid attestation for the recipient key */
  WALLET_ID_UNVERIFIED = 1,                  /* no attestation account */
  WALLET_ID_MISMATCH = 2,                    /* claimed name conflicts with on-chain or known names */
  WALLET_ID_REVOKED = 3,                     /* was verified on this badge before, account is gone */
  WALLET_ID_EXPIRED = 4,                     /* attestation past expiry and the clock is trustworthy */
  WALLET_ID_UNKNOWN = 5                      /* could not check (offline, RPC error, untrusted route) */
} wallet_identity_t;

typedef enum {                               /* what the approval screen says about PRESENCE */
  WALLET_PRESENCE_PRESENT = 0,               /* fresh PROOF verified within the deadline */
  WALLET_PRESENCE_NOT_CHECKED = 1,           /* no handshake was attempted for this recipient */
  WALLET_PRESENCE_NOT_PRESENT = 2            /* handshake failed, late, bad signature, or request unknown */
} wallet_presence_t;

typedef enum { WALLET_SEV_GREEN = 0, WALLET_SEV_AMBER = 1, WALLET_SEV_RED = 2 } wallet_severity_t;

typedef struct {                             /* untrusted, optional; every pointer may be NULL */
  const uint8_t *recipient;                  /* 32 B: owner wallet the app believes it is paying */
  const char    *claimed_name;               /* what the requester called itself */
  const char    *claimed_amount;             /* raw units, decimal string, as the app displayed it */
  const uint8_t *request_id;                 /* 8 B: binds this payment to a received REQ */
} wallet_hint_t;

typedef struct {                             /* result of a pure decode + local policy, no network, no UI */
  sol_transfer_t tx;
  char amount_ui[24];                        /* "10.00" */
  char symbol[8];                            /* "HACK", or "" when the mint is not the configured one */
  bool mint_known, source_is_own_ata, payer_is_self;
} wallet_decoded_t;

typedef struct {
  bool     ready;                            /* identity ready AND mint configured */
  uint8_t  pubkey[32];
  uint8_t  key_source;                       /* 0 none, 1 se050, 2 software  (identity::Source) */
  uint8_t  mint[32];
  uint8_t  token_account[32];                /* ATA(pubkey, mint) */
  uint8_t  decimals;
  char     symbol[8];
  uint64_t cap, max;                         /* raw units */
  uint16_t deadline_ms;
  bool     block_red, tls_pinned, clock_synced;
  uint16_t attest_ttl_s;
  int8_t   rssi_min;
  bool     registry_set;                     /* cred and schema are both configured */
  uint8_t  cred[32], schema[32];             /* SAS credential and schema addresses */
  char     rpc_url[96];                      /* NUL-terminated */
  char     dash_url[64];                     /* NUL-terminated, "" when unset */
} wallet_info_t;

/* ---- lifecycle (main loop only) ---- */
void wallet_begin(void);
void wallet_update(void);
bool wallet_ready(void);
void wallet_get_info(wallet_info_t *out);

/* ---- transactions ---- */
/* Pure: decode + local checks. Used by apps to preview; the screen never trusts it.
   BADGE_OK whenever the decoder accepts the bytes; the local checks are reported ONLY through
   out->mint_known, out->source_is_own_ata and out->payer_is_self (false when the wallet is not ready).
   BADGE_ERR_UNKNOWN_INSTRUCTION with *detail set when the decoder rejects (including too_long).
   BADGE_ERR_BAD_ARG when msg or out is NULL. detail may be NULL. */
badge_err_t wallet_decode(const uint8_t *msg, size_t len, wallet_decoded_t *out, sol_tx_err_t *detail);

/* THE gate. Blocks: decode -> policy -> identity refresh -> approval screen -> sign -> self-verify.
   Returns BADGE_OK and 64 signature bytes only after SELECT on the approval screen.
   One 60 s budget covers the whole call (identity check, approval, large-payment confirmation). */
badge_err_t wallet_sign_transaction(const uint8_t *msg, size_t len, const wallet_hint_t *hint,
                                    uint8_t sig_out[64]);

/* ---- payment protocol (see pay_session.h for the tables) ---- */
#define WALLET_PAY_REQ_TTL_MAX_S      120   /* a larger ttl_s is clamped to this */
#define WALLET_PAY_RECEIVE_TTL_MAX_S  600   /* a larger ttl_s is clamped to this */
/* Confirm screen, signs REQ, opens the session and starts broadcasting.
   bad_arg: amount == 0 or ttl_s == 0.  over_limit: amount > max.  busy: a session is already open.
   Also: denied, not_ready, no_display, rate_limited, rejected, approval_timeout, sign_failed. */
badge_err_t wallet_pay_request(uint64_t amount, uint16_t ttl_s, uint8_t id_out[8]);
/* Confirm screen, opens a receive session (request id zero).  bad_arg: ttl_s == 0.  busy: a session is open.
   Also: denied, not_ready, no_display, rate_limited, rejected, approval_timeout. */
badge_err_t wallet_pay_receive(uint16_t ttl_s);
/* F19: signs and sends RCPT to the MAC the PAID hint came from. Once per session.
   no_session: no session in state paid.  rate_limited: already sent.  Also: sign_failed, io. */
badge_err_t wallet_pay_receipt(void);
void        wallet_pay_cancel(void);
void        wallet_pay_on_frame(const uint8_t mac[6], const uint8_t *data, size_t len, int8_t rssi, uint32_t rx_ms);

/* ---- used only by broker_client when WALLET_ENABLE_BROKER ---- */
typedef enum { WALLET_DOMAIN_BROKER = 1 } wallet_domain_t;   /* "solana-badge-register:" */
badge_err_t wallet_sign_domain(wallet_domain_t d, const uint8_t *suffix, size_t len, uint8_t sig_out[64]);

const char *badge_err_name(badge_err_t e);   /* "rejected", ... */

#ifdef __cplusplus
}
#endif
#endif
