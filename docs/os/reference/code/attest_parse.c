/* attest_parse(): field checks for one SAS attestation account under the configured credential and schema.
   Pure C99, no heap. Declared in attest.h. Account layout (little-endian):
     0 discriminator (2) | 1 nonce[32] | 33 credential[32] | 65 schema[32] | 97 data_len D (u32)
     101 name_len N (u32, == D - 4) | 105 name[N] | 101+D signer[32] | 133+D expiry (i64) | 141+D token_account[32]
   Total length 173 + D. */
#include <string.h>
#include "attest.h"

static uint32_t rd_u32le(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int attest_parse(const uint8_t *acct, size_t len, const uint8_t subject[32], const uint8_t cred[32],
                 const uint8_t schema[32], char name_out[ATTEST_NAME_MAX + 1], int64_t *expiry_out) {
  uint32_t d, n, i;
  uint64_t e = 0;
  if (len < 105) return -1;                                   /* must reach both length fields */
  if (acct[0] != 2) return -1;                                /* discriminator: Attestation */
  if (memcmp(acct + 1, subject, 32) != 0) return -1;          /* nonce == subject public key */
  if (memcmp(acct + 33, cred, 32) != 0) return -1;            /* our credential */
  if (memcmp(acct + 65, schema, 32) != 0) return -1;          /* our schema */
  d = rd_u32le(acct + 97);
  if (d < 5 || d > 36 || len != 173 + (size_t)d) return -1;   /* data_len and total length agree */
  n = rd_u32le(acct + 101);
  if (n != d - 4 || n < 1 || n > ATTEST_NAME_MAX) return -1;  /* one Borsh string, nothing else */
  if (acct[105] == ' ' || acct[105 + n - 1] == ' ') return -1;
  for (i = 0; i < n; i++) {
    const uint8_t ch = acct[105 + i];
    if (ch < 0x20 || ch > 0x7E) return -1;                    /* printable ASCII only */
    if (ch == ' ' && i + 1 < n && acct[106 + i] == ' ') return -1;   /* no two consecutive spaces */
  }
  memcpy(name_out, acct + 105, n);
  name_out[n] = '\0';
  for (i = 0; i < 8; i++) e |= (uint64_t)acct[133 + d + i] << (8 * i);   /* i64 LE, 0 = never */
  *expiry_out = (int64_t)e;
  return 0;
}
