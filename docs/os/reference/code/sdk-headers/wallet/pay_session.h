/* src/wallet/pay_session.h - payee session, payer inbox, pending challenges, presence table, replay ring.
   Main loop only. All tables are RAM; nothing here survives a reboot. */
#ifndef PAY_SESSION_H
#define PAY_SESSION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pay_proto.h"
#include "wallet.h"
#ifdef __cplusplus
extern "C" {
#endif

#define PAY_INBOX_MAX            4      /* verified REQs, one per payee key */
#define PAY_PENDING_MAX          4      /* challenges awaiting a PROOF, one per counterparty MAC */
#define PAY_PRESENCE_MAX         8      /* verified presence entries */
#define PAY_PEERS_MAX            8      /* IAM answers */
#define PAY_REPLAY_MAX          32      /* request ids that reached a terminal state */
#define PAY_REBROADCAST_MS    1000
#define PAY_PROOF_TIMEOUT_MS  1500
#define PAY_CHALLENGE_ATTEMPTS   3
#define PAY_PRESENCE_TTL_MS  60000
#define PAY_PEER_TTL_MS      60000
#define PAY_PROOFS_PER_SESSION   8
#define PAY_PROOFS_PER_MAC       3
#define PAY_PROOF_MIN_GAP_MS   200
#define PAY_IAM_PER_SECOND       5
/* Build switch PAY_MEASURE (default 0, wallet_defaults.h): when 1, PAY_PROOFS_PER_SESSION, PAY_PROOFS_PER_MAC and
   PAY_CHALLENGE_ATTEMPTS are not enforced (PAY_PROOF_MIN_GAP_MS still is). For measuring the PROOF round trip only. */

typedef enum { PAY_SESSION_IDLE = 0, PAY_SESSION_OPEN, PAY_SESSION_RECEIVE, PAY_SESSION_PAID,
               PAY_SESSION_EXPIRED } pay_session_state_t;        /* == badge_pay_state_t */

typedef struct {
  pay_session_state_t state;
  uint8_t  id[PAY_ID_LEN];              /* all zero in receive mode */
  uint64_t amount;                      /* 0 in receive mode */
  uint32_t opened_ms, ttl_ms;
  uint8_t  proofs;                      /* PROOFs signed in this session */
  uint32_t last_proof_ms;
  bool     has_payer;                   /* last challenger */
  uint8_t  payer[32];
  uint8_t  payer_mac[6];
  bool     has_tx;                      /* PAID hint received */
  uint8_t  tx_sig[64];
  uint8_t  paid_mac[6];                 /* MAC the PAID hint came from; RCPT goes there */
  bool     rcpt_sent;
  char     owner_app[33];               /* app id that opened the session; closed when that app stops */
  uint8_t  req_wire[PAY_REQ_LEN];       /* the signed REQ being rebroadcast (unused in receive mode) */
} pay_session_t;

typedef struct {
  pay_req_t req;                        /* decoded */
  uint8_t   wire[PAY_REQ_LEN];          /* exact bytes, for the byte-identical rebroadcast check */
  uint8_t   mac[6];                     /* MAC of first receipt; never changed afterwards */
  int8_t    rssi;                       /* of the latest frame from that MAC */
  uint32_t  first_ms, last_ms;
  bool      sig_ok;                     /* always true unless built with PAY_VERIFY_REQ 0 */
} pay_inbox_entry_t;

typedef enum { PAY_PRESENCE_NONE = 0, PAY_PRESENCE_PENDING, PAY_PRESENCE_PRESENT, PAY_PRESENCE_LATE,
               PAY_PRESENCE_BAD_SIG, PAY_PRESENCE_TIMEOUT } pay_presence_state_t;   /* == badge_presence_t */

/* ---- lifecycle ---- */
void pay_session_begin(void);
void pay_session_update(uint32_t now_ms);     /* rebroadcast REQ, expire session/inbox/peers/presence, time out challenges */
void pay_session_on_frame(const uint8_t mac[6], const uint8_t *data, size_t len, int8_t rssi, uint32_t rx_ms);

/* ---- payee side (called by wallet.cpp after the confirm screen) ---- */
badge_err_t pay_session_open_request(const uint8_t req_wire[PAY_REQ_LEN], const char *owner_app);   /* busy if not idle */
badge_err_t pay_session_open_receive(uint16_t ttl_s, const char *owner_app);
void        pay_session_close(void);
void        pay_session_get(pay_session_t *out);
void        pay_session_note_proof(const uint8_t payer[32], const uint8_t payer_mac[6], uint32_t now_ms);
void        pay_session_note_receipt_sent(void);

/* ---- payer side ---- */
size_t      pay_inbox_list(pay_inbox_entry_t *out, size_t max, int8_t rssi_min);   /* strongest first */
bool        pay_inbox_find(const uint8_t id[PAY_ID_LEN], pay_inbox_entry_t *out);
void        pay_inbox_dismiss(const uint8_t id[PAY_ID_LEN]);                       /* removes the entry, adds id to the replay ring */
bool        pay_replay_contains(const uint8_t id[PAY_ID_LEN]);
badge_err_t pay_send_hello(const uint8_t mac[6]);
bool        pay_peer_get(const uint8_t mac[6], pay_iam_t *out, uint32_t *age_ms);
badge_err_t pay_send_challenge(const uint8_t mac[6], const uint8_t *id_or_null);   /* rate_limited after PAY_CHALLENGE_ATTEMPTS */
pay_presence_state_t pay_presence_by_mac(const uint8_t mac[6], uint32_t *elapsed_ms);
/* For the signing gate: newest result for a payee key. id_or_null narrows it to one request. */
pay_presence_state_t pay_presence_by_payee(const uint8_t payee[32], const uint8_t *id_or_null,
                                           uint32_t *age_ms, uint32_t *elapsed_ms);
badge_err_t pay_send_paid(const uint8_t mac[6], const uint8_t id[PAY_ID_LEN], const uint8_t tx_sig[64]);

#ifdef __cplusplus
}
#endif
#endif
