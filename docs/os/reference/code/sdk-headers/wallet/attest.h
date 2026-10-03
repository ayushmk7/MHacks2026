/* src/wallet/attest.h - Solana Attestation Service check: fetch, parse, cache, known names.
   attest_parse() is pure C99 (attest_parse.c, host-tested); the rest is attest.cpp. Main loop only. */
#ifndef ATTEST_H
#define ATTEST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "wallet.h"
#ifdef __cplusplus
extern "C" {
#endif

#define ATTEST_NAME_MAX          32
#define ATTEST_CACHE_ENTRIES     16
#define ATTEST_KNOWN_ENTRIES     16
#define ATTEST_KNOWN_RECORD_LEN  72     /* pubkey[32] name_len[1] name[32] last_seen_uptime_s[4 LE] reserved[3] */
#define ATTEST_RPC_TIMEOUT_MS  4000
#define ATTEST_CLOCK_SYNCED_AFTER 1750000000   /* time() above this means the clock was set */

#define ATTEST_F_CACHED        0x01     /* answered from the RAM cache, no network call */
#define ATTEST_F_TLS_UNPINNED  0x02     /* fetched over TLS without the rpc-ca pin */
#define ATTEST_F_VIA_BRIDGE    0x04     /* only route was the phone bridge; status is UNKNOWN */
#define ATTEST_F_NAME_KNOWN_ELSEWHERE 0x08   /* MISMATCH because the claim is a known name of another key */

typedef struct {
  wallet_identity_t status;             /* numeric values are the wallet_identity_t values, 0..5 */
  char     name[ATTEST_NAME_MAX + 1];   /* attested name when status is VERIFIED or MISMATCH-with-attestation, else "" */
  int64_t  expiry;                      /* seconds since 1970, 0 = never or not attested */
  uint32_t age_ms;                      /* since the fetch that produced this result */
  uint8_t  flags;                       /* ATTEST_F_* */
  uint8_t  other_key[32];               /* with ATTEST_F_NAME_KNOWN_ELSEWHERE: the key that holds the claimed name */
} attest_result_t;

/* Field checks for one attestation account under the configured credential and schema (pure, no I/O).
   Checks: length, discriminator 2, nonce == subject, credential, schema, data_len/name_len consistency,
   name 1..32 printable ASCII without leading/trailing/double spaces.
   Returns 0 and fills name_out (NUL-terminated) and *expiry_out; -1 if any check fails. */
int attest_parse(const uint8_t *acct, size_t len, const uint8_t subject[32], const uint8_t cred[32],
                 const uint8_t schema[32], char name_out[ATTEST_NAME_MAX + 1], int64_t *expiry_out);

void attest_begin(void);                /* loads /wallet/known.bin */
void attest_clear_cache(void);          /* on config change of mint, cred, schema or rpc_url */

/* Blocking (<= ATTEST_RPC_TIMEOUT_MS) unless a fresh cache entry exists and force is false.
   claimed_name may be NULL. BADGE_OK with out->status set (network failure gives WALLET_ID_UNKNOWN,
   never an error). BADGE_ERR_BAD_ARG for NULL subject/out; BADGE_ERR_NOT_READY when cred or schema
   is not configured. */
badge_err_t attest_check(const uint8_t subject[32], const char *claimed_name, bool force, attest_result_t *out);
/* The badge's own key, no claim. */
badge_err_t attest_self(bool force, attest_result_t *out);
/* Never blocks. False when there is no cache entry (fresh or stale) for subject. */
bool attest_cached(const uint8_t subject[32], attest_result_t *out);

/* Known-names store (/wallet/known.bin). */
bool   attest_known_lookup(const uint8_t subject[32], char name_out[ATTEST_NAME_MAX + 1]);
bool   attest_known_by_name(const char *name, uint8_t key_out[32]);   /* exact byte comparison */
size_t attest_known_count(void);
void   attest_forget_known(void);       /* Settings > Wallet > Forget known names */

#ifdef __cplusplus
}
#endif
#endif
