#include "sol.h"
#include <stdio.h>
#include <string.h>
#include "vectors.h"
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static void unhex(const char *h, uint8_t *o) { unsigned v; size_t i; for (i = 0; h[2*i]; i++) { sscanf(h + 2*i, "%2x", &v); o[i] = (uint8_t)v; } }

int main(void) {
  uint8_t a[32], b[32], c[32], msg[SOL_TX_MSG_MAX], bad[SOL_TX_MSG_MAX]; char text[64]; sol_transfer_t t; size_t n; int i; uint64_t raw;
  /* base58 round trip against kit's encodings */
  CHECK(sol_b58_decode(B58_PAYER, a, 32) == 0 && memcmp(a, V_PAYER, 32) == 0);
  CHECK(sol_b58_encode(V_PAYER, 32, text, sizeof text) == strlen(B58_PAYER) && strcmp(text, B58_PAYER) == 0);
  memset(a, 0, 32); CHECK(sol_b58_encode(a, 32, text, sizeof text) == 32 && strcmp(text, "11111111111111111111111111111111") == 0);
  CHECK(sol_b58_decode("11111111111111111111111111111111", b, 32) == 0 && memcmp(a, b, 32) == 0);
  CHECK(sol_b58_decode("0OIl", b, 32) == -1);
  CHECK(sol_b58_encode(SOL_TOKEN_PROGRAM_ID, 32, text, sizeof text) && strcmp(text, "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA") == 0);
  CHECK(sol_b58_encode(SOL_ATA_PROGRAM_ID, 32, text, sizeof text) && strcmp(text, "ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL") == 0);
  CHECK(sol_b58_encode(SOL_SAS_PROGRAM_ID, 32, text, sizeof text) && strcmp(text, "22zoJMtdu4tQc2PzL74ZUT7FrwgB1Udec8DdW4yw4BdG") == 0);
  /* curve membership */
  for (i = 0; i < CURVE_N; i++) { unhex(CURVE_HEX[i], a); CHECK(sol_is_on_curve(a) == CURVE_ON[i]); }
  /* PDAs */
  CHECK(sol_b58_decode(B58_OWNER1, a, 32) == 0 && sol_b58_decode(B58_MINT, b, 32) == 0);
  CHECK(sol_ata(a, b, c) == 0 && sol_b58_encode(c, 32, text, sizeof text) && strcmp(text, B58_ATA1) == 0);
  CHECK(sol_ata(V_PAYER, V_MINT, c) == 0 && memcmp(c, V_SRC, 32) == 0);
  CHECK(sol_ata(V_DST_OWNER, V_MINT, c) == 0 && memcmp(c, V_DST, 32) == 0);
  CHECK(sol_b58_decode(B58_CRED, a, 32) == 0 && sol_b58_decode(B58_SCHEMA, b, 32) == 0);
  CHECK(sol_sas_attestation_pda(a, b, V_DST_OWNER, c) == 0 && memcmp(c, V_ATT, 32) == 0);
  CHECK(sol_b58_encode(c, 32, text, sizeof text) && strcmp(text, B58_ATT) == 0);
  /* decoder: kit-built legacy and v0 */
  CHECK(sol_tx_decode_transfer(V_LEGACY, sizeof V_LEGACY, &t) == SOL_TX_OK);
  CHECK(t.version == 0xFF && t.amount == 1000 && t.decimals == 2);
  CHECK(!memcmp(t.fee_payer, V_PAYER, 32) && !memcmp(t.source, V_SRC, 32) && !memcmp(t.destination, V_DST, 32) && !memcmp(t.mint, V_MINT, 32) && !memcmp(t.blockhash, V_BH, 32));
  CHECK(sol_tx_decode_transfer(V_V0, sizeof V_V0, &t) == SOL_TX_OK && t.version == 0 && t.amount == 1000);
  /* builder reproduces kit's legacy bytes */
  n = sol_tx_build_transfer(V_PAYER, V_SRC, V_DST, V_MINT, V_BH, 1000, 2, msg, sizeof msg);
  CHECK(n == sizeof V_LEGACY && memcmp(msg, V_LEGACY, n) == 0);
  /* second key set: @solana/kit ordered the writable pair and mint/program opposite to raw-byte order.
     The decoder must accept kit's bytes; the builder's bytes differ but decode to the same transfer. */
  CHECK(sol_ata(V_PAYER, V_ALT_MINT, c) == 0 && memcmp(c, V_ALT_SRC, 32) == 0);
  CHECK(sol_ata(V_ALT_OWNER, V_ALT_MINT, c) == 0 && memcmp(c, V_ALT_DST, 32) == 0);
  CHECK(sizeof V_ALT_LEGACY == 214 && sol_tx_decode_transfer(V_ALT_LEGACY, sizeof V_ALT_LEGACY, &t) == SOL_TX_OK);
  CHECK(t.version == 0xFF && t.amount == 1000 && t.decimals == 2);
  CHECK(!memcmp(t.fee_payer, V_PAYER, 32) && !memcmp(t.source, V_ALT_SRC, 32) && !memcmp(t.destination, V_ALT_DST, 32) && !memcmp(t.mint, V_ALT_MINT, 32) && !memcmp(t.blockhash, V_BH, 32));
  CHECK(memcmp(V_ALT_LEGACY + 36, V_ALT_LEGACY + 68, 32) > 0);            /* kit: writable pair not in ascending bytes */
  CHECK(memcmp(V_ALT_LEGACY + 100, V_ALT_LEGACY + 132, 32) > 0);          /* kit: mint placed before the Token program */
  n = sol_tx_build_transfer(V_PAYER, V_ALT_SRC, V_ALT_DST, V_ALT_MINT, V_BH, 1000, 2, msg, sizeof msg);
  CHECK(n == 214 && memcmp(msg, V_ALT_LEGACY, n) != 0);                    /* same transfer, different key order */
  { sol_transfer_t u; CHECK(sol_tx_decode_transfer(msg, n, &u) == SOL_TX_OK && memcmp(&u, &t, sizeof u) == 0); }
  /* negative cases */
  CHECK(sol_tx_decode_transfer(V_LEGACY, sizeof V_LEGACY - 1, &t) == SOL_TX_ERR_TRUNCATED);
  memcpy(bad, V_LEGACY, sizeof V_LEGACY); bad[sizeof V_LEGACY] = 0; CHECK(sol_tx_decode_transfer(bad, sizeof V_LEGACY + 1, &t) == SOL_TX_ERR_TRAILING);
  memcpy(bad, V_LEGACY, sizeof V_LEGACY); bad[0] = 2; CHECK(sol_tx_decode_transfer(bad, sizeof V_LEGACY, &t) == SOL_TX_ERR_HEADER);
  memcpy(bad, V_LEGACY, sizeof V_LEGACY); bad[204] = 3; CHECK(sol_tx_decode_transfer(bad, sizeof V_LEGACY, &t) == SOL_TX_ERR_IX_DATA);     /* 3 = Transfer */
  memcpy(bad, V_LEGACY, sizeof V_LEGACY); bad[197] = 3; CHECK(sol_tx_decode_transfer(bad, sizeof V_LEGACY, &t) == SOL_TX_ERR_PROGRAM);     /* program index -> mint */
  memcpy(bad, V_LEGACY, sizeof V_LEGACY); bad[4 + 4*32] ^= 1; CHECK(sol_tx_decode_transfer(bad, sizeof V_LEGACY, &t) == SOL_TX_ERR_PROGRAM); /* e.g. Token-2022 / any other program */
  memcpy(bad, V_LEGACY, sizeof V_LEGACY); bad[196] = 2; CHECK(sol_tx_decode_transfer(bad, sizeof V_LEGACY, &t) == SOL_TX_ERR_IX_COUNT);
  memcpy(bad, V_LEGACY, sizeof V_LEGACY); bad[202] = 1; CHECK(sol_tx_decode_transfer(bad, sizeof V_LEGACY, &t) == SOL_TX_ERR_AUTHORITY);
  memcpy(bad, V_LEGACY, sizeof V_LEGACY); bad[201] = 2; CHECK(sol_tx_decode_transfer(bad, sizeof V_LEGACY, &t) == SOL_TX_ERR_ROLES);       /* dst == src */
  memcpy(bad, V_LEGACY, sizeof V_LEGACY); memset(bad + 205, 0, 8); CHECK(sol_tx_decode_transfer(bad, sizeof V_LEGACY, &t) == SOL_TX_ERR_AMOUNT_ZERO);
  memcpy(bad, V_V0, sizeof V_V0); bad[0] = 0x81; CHECK(sol_tx_decode_transfer(bad, sizeof V_V0, &t) == SOL_TX_ERR_VERSION);
  memcpy(bad, V_V0, sizeof V_V0); bad[sizeof V_V0 - 1] = 1; CHECK(sol_tx_decode_transfer(bad, sizeof V_V0, &t) == SOL_TX_ERR_LOOKUPS);
  CHECK(sizeof V_CB_LEGACY == 258 && sol_tx_decode_transfer(V_CB_LEGACY, sizeof V_CB_LEGACY, &t) == SOL_TX_ERR_TOO_LONG);   /* kit-built: ComputeBudget + transfer, 6 keys, 258 bytes */
  memcpy(bad, V_LEGACY, sizeof V_LEGACY); bad[3] = 6; CHECK(sol_tx_decode_transfer(bad, sizeof V_LEGACY, &t) == SOL_TX_ERR_ACCOUNTS);   /* six account keys */
  CHECK(sol_tx_decode_transfer((const uint8_t *)"pay-req:xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", 48, &t) != SOL_TX_OK);
  /* amounts */
  CHECK(sol_format_amount(1000, 2, text, sizeof text) && strcmp(text, "10.00") == 0);
  CHECK(sol_format_amount(5, 2, text, sizeof text) && strcmp(text, "0.05") == 0);
  CHECK(sol_format_amount(50000, 2, text, sizeof text) && strcmp(text, "500.00") == 0);
  CHECK(sol_format_amount(18446744073709551615ULL, 0, text, sizeof text) && strcmp(text, "18446744073709551615") == 0);
  CHECK(sol_format_amount(50000000, 9, text, sizeof text) && strcmp(text, "0.050000000") == 0);
  CHECK(sol_parse_amount("10", 2, &raw) == 0 && raw == 1000);
  CHECK(sol_parse_amount("0.05", 2, &raw) == 0 && raw == 5);
  CHECK(sol_parse_amount("12.5", 2, &raw) == 0 && raw == 1250);
  CHECK(sol_parse_amount("1.234", 2, &raw) == -1 && sol_parse_amount("", 2, &raw) == -1 && sol_parse_amount("1e3", 2, &raw) == -1);
  /* attestation account layout (full parser: attest_parse.c, test_attest.c): disc 2, nonce, credential, schema, u32 len, borsh string, signer, i64 expiry */
  CHECK(V_ACCT[0] == 2 && memcmp(V_ACCT + 1, V_DST_OWNER, 32) == 0 && V_ACCT[97] == 16 && V_ACCT[101] == 12 && memcmp(V_ACCT + 105, "MHacks Merch", 12) == 0);
  printf(fails ? "%d FAILED\n" : "all sol tests passed\n", fails);
  return fails != 0;
}
