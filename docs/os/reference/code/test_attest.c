/* Host test for attest_parse() against the 189-byte account built by vectors.mjs (V_ACCT) and mutated copies.
   cc -std=c99 -Wall -Wextra -Wpedantic -O2 -DSOL_HOST_SHA256 -I sdk-headers/wallet \
      attest_parse.c sol_b58.c test_attest.c -o test_attest && ./test_attest */
#include <stdio.h>
#include <string.h>
#include "attest.h"
#include "sol.h"
#include "vectors.h"
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
  uint8_t cred[32], schema[32], bad[sizeof V_ACCT + 8]; char name[ATTEST_NAME_MAX + 1]; int64_t expiry = -1;
  const uint8_t *subject = V_DST_OWNER;                       /* Gn2G..Ecxq, the stand-in merchant */
  CHECK(sol_b58_decode(B58_CRED, cred, 32) == 0 && sol_b58_decode(B58_SCHEMA, schema, 32) == 0);
  CHECK(sizeof V_ACCT == 189);
  CHECK(attest_parse(V_ACCT, sizeof V_ACCT, subject, cred, schema, name, &expiry) == 0);
  CHECK(strcmp(name, "MHacks Merch") == 0 && expiry == 1793664000);
#define MUT(off, val) (memcpy(bad, V_ACCT, sizeof V_ACCT), bad[off] = (uint8_t)(val), \
                       attest_parse(bad, sizeof V_ACCT, subject, cred, schema, name, &expiry))
  CHECK(MUT(0, 0) == -1 && MUT(0, 1) == -1);                  /* credential / schema discriminators */
  CHECK(MUT(1, V_ACCT[1] ^ 1) == -1);                         /* nonce is another key */
  CHECK(MUT(33, V_ACCT[33] ^ 1) == -1);                       /* another credential */
  CHECK(MUT(65, V_ACCT[65] ^ 1) == -1);                       /* another schema */
  CHECK(MUT(97, 17) == -1);                                   /* data_len disagrees with the total length */
  CHECK(MUT(101, 11) == -1 && MUT(101, 13) == -1);            /* name_len != data_len - 4 */
  CHECK(MUT(105, 0x07) == -1 && MUT(110, 0x7F) == -1 && MUT(110, 0xC3) == -1);   /* control / DEL / non-ASCII */
  CHECK(MUT(105, ' ') == -1 && MUT(116, ' ') == -1);          /* leading / trailing space */
  CHECK(MUT(112, ' ') == -1);                                 /* "MHacks  erch": two consecutive spaces */
  CHECK(attest_parse(V_ACCT, sizeof V_ACCT - 1, subject, cred, schema, name, &expiry) == -1);
  memcpy(bad, V_ACCT, sizeof V_ACCT); bad[sizeof V_ACCT] = 0;
  CHECK(attest_parse(bad, sizeof V_ACCT + 1, subject, cred, schema, name, &expiry) == -1);
  CHECK(attest_parse(V_ACCT, 104, subject, cred, schema, name, &expiry) == -1);
  CHECK(attest_parse(V_ACCT, sizeof V_ACCT, V_PAYER, cred, schema, name, &expiry) == -1);   /* asked about another subject */
  /* expiry 0 = never */
  memcpy(bad, V_ACCT, sizeof V_ACCT); memset(bad + 149, 0, 8);
  CHECK(attest_parse(bad, sizeof V_ACCT, subject, cred, schema, name, &expiry) == 0 && expiry == 0);
  /* a 1-character and a 32-character name */
  { uint8_t a1[178], a32[209]; int k;
    memcpy(a1, V_ACCT, 97); a1[97] = 5; a1[98] = a1[99] = a1[100] = 0; a1[101] = 1; a1[102] = a1[103] = a1[104] = 0; a1[105] = 'X';
    memcpy(a1 + 106, V_ACCT + 117, 72);
    CHECK(attest_parse(a1, sizeof a1, subject, cred, schema, name, &expiry) == 0 && strcmp(name, "X") == 0 && expiry == 1793664000);
    memcpy(a32, V_ACCT, 97); a32[97] = 36; a32[98] = a32[99] = a32[100] = 0; a32[101] = 32; a32[102] = a32[103] = a32[104] = 0;
    for (k = 0; k < 32; k++) a32[105 + k] = (uint8_t)('a' + k % 26);
    memcpy(a32 + 137, V_ACCT + 117, 72);
    CHECK(attest_parse(a32, sizeof a32, subject, cred, schema, name, &expiry) == 0 && strlen(name) == 32);
    a32[97] = 37; a32[101] = 33;                              /* 33 characters: over the limit */
    CHECK(attest_parse(a32, sizeof a32, subject, cred, schema, name, &expiry) == -1); }
  printf(fails ? "%d FAILED\n" : "all attest tests passed\n", fails);
  return fails != 0;
}
