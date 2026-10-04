/* test_sol - base58, the Solana message decoder (one negative vector per rule), Memo, compact-u16,
   builder round trips, amount format and parse. Spec: docs/os/wallet/solana-payments.md ("Tests"). */
#include "../../src/vk/wallet/pure/sol.h"
#include <stdio.h>
#include <string.h>
#include "vectors.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* Offsets inside V_LEGACY (5 keys, 214 bytes). */
enum { L_NSIG = 0, L_NRO_SIGNED = 1, L_NRO_UNSIGNED = 2, L_NKEYS = 3, L_KEYS = 4, L_BH = 164, L_NIX = 196,
       L_PROG = 197, L_NACC = 198, L_ACC = 199, L_DLEN = 203, L_DATA = 204, L_AMOUNT = 205, L_DECIMALS = 213 };
/* Offsets inside V_MEMO_LEGACY (6 keys, Token instruction first, then Memo). */
enum { M_NKEYS = 3, M_KEYS = 4, M_NIX = 228, M_TOKEN_IX = 229, M_TOKEN_IX_LEN = 17, M_MEMO_IX = 246,
       M_MEMO_NACC = 247, M_MEMO_DLEN = 248, M_MEMO_DATA = 249 };

static uint8_t msg[SOL_TX_MSG_MAX + 8], bad[SOL_TX_MSG_MAX + 8];

/* Deterministic pseudo-random bytes (xorshift64*), so the builder round trips are repeatable. */
static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;
static uint64_t rng(void) {
  rng_state ^= rng_state >> 12; rng_state ^= rng_state << 25; rng_state ^= rng_state >> 27;
  return rng_state * 0x2545F4914F6CDD1DULL;
}
static void rng_fill(uint8_t *p, size_t n) { size_t i; for (i = 0; i < n; i++) p[i] = (uint8_t)(rng() >> 32); }

/* Copies V_LEGACY into bad[] and sets one byte. */
static sol_tx_err_t legacy_with(size_t at, uint8_t value) {
  sol_transfer_t t;
  memcpy(bad, V_LEGACY, sizeof V_LEGACY); bad[at] = value;
  return sol_tx_decode_transfer(bad, sizeof V_LEGACY, &t);
}
/* Copies V_MEMO_LEGACY into bad[] and sets one byte. */
static sol_tx_err_t memo_with(size_t at, uint8_t value) {
  sol_transfer_t t;
  memcpy(bad, V_MEMO_LEGACY, sizeof V_MEMO_LEGACY); bad[at] = value;
  return sol_tx_decode_transfer(bad, sizeof V_MEMO_LEGACY, &t);
}
/* Copies src[0..at), then ins[0..ins_len), then src[at + skip ..) into bad[]; returns the new length. */
static size_t splice(const uint8_t *src, size_t src_len, size_t at, size_t skip, const uint8_t *ins, size_t ins_len) {
  memcpy(bad, src, at);
  memcpy(bad + at, ins, ins_len);
  memcpy(bad + at + ins_len, src + at + skip, src_len - at - skip);
  return src_len - skip + ins_len;
}
/* Builds a transfer of the vector keys with this memo and decodes it. */
static sol_tx_err_t memo_roundtrip(const char *memo, size_t memo_len, sol_transfer_t *t, size_t *n_out) {
  size_t n = sol_tx_build_transfer(V_PAYER, V_SRC, V_DST, V_MINT, V_BH, 1000, 2, (const uint8_t *)memo, memo_len, msg, sizeof msg);
  if (n_out) *n_out = n;
  return sol_tx_decode_transfer(msg, n, t);
}

static void test_base58(void) {
  uint8_t a[32], b[32]; char text[64];
  CHECK(sol_b58_decode(B58_PAYER, a, 32) == 0 && memcmp(a, V_PAYER, 32) == 0);
  CHECK(sol_b58_encode(V_PAYER, 32, text, sizeof text) == strlen(B58_PAYER) && strcmp(text, B58_PAYER) == 0);
  memset(a, 0, 32);
  CHECK(sol_b58_encode(a, 32, text, sizeof text) == 32 && strcmp(text, "11111111111111111111111111111111") == 0);
  CHECK(sol_b58_decode("11111111111111111111111111111111", b, 32) == 0 && memcmp(a, b, 32) == 0);
  CHECK(sol_b58_decode("0OIl", b, 32) == -1);
  CHECK(sol_b58_decode("", b, 32) == -1);
  CHECK(sol_b58_encode(V_PAYER, 32, text, 10) == 0);                       /* output too small */
  CHECK(sol_b58_encode(SOL_TOKEN_PROGRAM_ID, 32, text, sizeof text) && strcmp(text, "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA") == 0);
  CHECK(sol_b58_encode(SOL_MEMO_PROGRAM_ID, 32, text, sizeof text) && strcmp(text, "MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr") == 0);
  CHECK(sol_b58_decode(B58_ISSUER, a, 32) == 0 && memcmp(a, V_ISSUER_PUB, 32) == 0);
  CHECK(sol_b58_decode(B58_DEVICE, a, 32) == 0 && memcmp(a, V_DEVICE_PUB, 32) == 0);
}

static void test_decode_vectors(void) {
  sol_transfer_t t, u; size_t n;
  /* kit-built legacy message, 5 keys */
  CHECK(sizeof V_LEGACY == 214 && sol_tx_decode_transfer(V_LEGACY, sizeof V_LEGACY, &t) == SOL_TX_OK);
  CHECK(t.amount == 1000 && t.decimals == 2 && t.memo == NULL && t.memo_len == 0);
  CHECK(!memcmp(t.fee_payer, V_PAYER, 32) && !memcmp(t.source, V_SRC, 32) && !memcmp(t.destination, V_DST, 32) && !memcmp(t.mint, V_MINT, 32) && !memcmp(t.blockhash, V_BH, 32));
  /* the builder reproduces kit's legacy bytes for this key set */
  n = sol_tx_build_transfer(V_PAYER, V_SRC, V_DST, V_MINT, V_BH, 1000, 2, NULL, 0, msg, sizeof msg);
  CHECK(n == sizeof V_LEGACY && memcmp(msg, V_LEGACY, n) == 0);
  /* second key set: @solana/kit ordered the writable pair and mint/program opposite to raw-byte order.
     The decoder must accept kit's bytes; the builder's bytes differ but decode to the same transfer. */
  CHECK(sizeof V_ALT_LEGACY == 214 && sol_tx_decode_transfer(V_ALT_LEGACY, sizeof V_ALT_LEGACY, &t) == SOL_TX_OK);
  CHECK(t.amount == 1000 && t.decimals == 2 && t.memo == NULL);
  CHECK(!memcmp(t.fee_payer, V_PAYER, 32) && !memcmp(t.source, V_ALT_SRC, 32) && !memcmp(t.destination, V_ALT_DST, 32) && !memcmp(t.mint, V_ALT_MINT, 32) && !memcmp(t.blockhash, V_BH, 32));
  CHECK(memcmp(V_ALT_LEGACY + 36, V_ALT_LEGACY + 68, 32) > 0);            /* kit: writable pair not in ascending bytes */
  CHECK(memcmp(V_ALT_LEGACY + 100, V_ALT_LEGACY + 132, 32) > 0);          /* kit: mint placed before the Token program */
  n = sol_tx_build_transfer(V_PAYER, V_ALT_SRC, V_ALT_DST, V_ALT_MINT, V_BH, 1000, 2, NULL, 0, msg, sizeof msg);
  CHECK(n == 214 && memcmp(msg, V_ALT_LEGACY, n) != 0);                    /* same transfer, different key order */
  CHECK(sol_tx_decode_transfer(msg, n, &u) == SOL_TX_OK);
  CHECK(u.amount == t.amount && u.decimals == t.decimals && u.memo == NULL && !memcmp(u.fee_payer, t.fee_payer, 32) && !memcmp(u.source, t.source, 32)
        && !memcmp(u.destination, t.destination, 32) && !memcmp(u.mint, t.mint, 32) && !memcmp(u.blockhash, t.blockhash, 32));
  /* kit-built 6-key message: the same transfer followed by a Memo; the memo bytes are returned */
  CHECK(sizeof V_MEMO_LEGACY == 259 && V_MEMO_LEGACY[M_NKEYS] == 6 && V_MEMO_LEGACY[M_NIX] == 2);
  CHECK(sol_tx_decode_transfer(V_MEMO_LEGACY, sizeof V_MEMO_LEGACY, &t) == SOL_TX_OK);
  CHECK(t.amount == 1000 && t.decimals == 2);
  CHECK(!memcmp(t.fee_payer, V_PAYER, 32) && !memcmp(t.source, V_SRC, 32) && !memcmp(t.destination, V_DST, 32) && !memcmp(t.mint, V_MINT, 32) && !memcmp(t.blockhash, V_BH, 32));
  CHECK(t.memo == V_MEMO_LEGACY + M_MEMO_DATA && t.memo_len == sizeof V_MEMO_TEXT && memcmp(t.memo, V_MEMO_TEXT, sizeof V_MEMO_TEXT) == 0);
  CHECK(t.memo_len == 10 && memcmp(t.memo, "coffee #42", 10) == 0);
  /* the same two instructions in the other order (Memo first) are accepted too */
  memcpy(bad, V_MEMO_LEGACY, M_TOKEN_IX);
  memcpy(bad + M_TOKEN_IX, V_MEMO_LEGACY + M_MEMO_IX, sizeof V_MEMO_LEGACY - M_MEMO_IX);
  memcpy(bad + M_TOKEN_IX + (sizeof V_MEMO_LEGACY - M_MEMO_IX), V_MEMO_LEGACY + M_TOKEN_IX, M_TOKEN_IX_LEN);
  CHECK(sol_tx_decode_transfer(bad, sizeof V_MEMO_LEGACY, &u) == SOL_TX_OK);
  CHECK(u.amount == 1000 && u.memo_len == 10 && u.memo == bad + M_TOKEN_IX + 3 && memcmp(u.memo, "coffee #42", 10) == 0 && !memcmp(u.destination, V_DST, 32));
  /* a text-domain message is never a transaction */
  CHECK(sol_tx_decode_transfer((const uint8_t *)"pay-req:xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", 48, &t) != SOL_TX_OK);
  CHECK(sol_tx_decode_transfer((const uint8_t *)"registry:v=1\nattestation=none\ndisplay_name=x\n", 45, &t) != SOL_TX_OK);
}

/* One negative vector (at least) per row of the decoder-rule table. */
static void test_decode_rules(void) {
  sol_transfer_t t; size_t n, i;
  static const uint8_t two_byte_5[] = {0x85, 0x00}, two_byte_1[] = {0x81, 0x00}, two_byte_4[] = {0x84, 0x00}, two_byte_10[] = {0x8a, 0x00};

  /* rule 1: length 1..1232 */
  memset(bad, 0, sizeof bad); memcpy(bad, V_LEGACY, sizeof V_LEGACY);
  CHECK(sol_tx_decode_transfer(bad, SOL_TX_MSG_MAX + 1, &t) == SOL_TX_ERR_TOO_LONG);
  CHECK(sol_tx_decode_transfer(V_LEGACY, 0, &t) == SOL_TX_ERR_TRUNCATED);
  CHECK(sol_tx_decode_transfer(V_LEGACY, sizeof V_LEGACY - 1, &t) == SOL_TX_ERR_TRUNCATED);
  for (i = 0; i < sizeof V_LEGACY; i++) CHECK(sol_tx_decode_transfer(V_LEGACY, i, &t) == SOL_TX_ERR_TRUNCATED);            /* every truncated copy */
  for (i = 0; i < sizeof V_MEMO_LEGACY; i++) CHECK(sol_tx_decode_transfer(V_MEMO_LEGACY, i, &t) == SOL_TX_ERR_TRUNCATED);

  /* rule 2: not a versioned message. Named case: a versioned (v0) message. */
  CHECK(V_V0[0] == 0x80 && sol_tx_decode_transfer(V_V0, sizeof V_V0, &t) == SOL_TX_ERR_VERSION);
  CHECK(legacy_with(0, 0x80) == SOL_TX_ERR_VERSION);
  CHECK(legacy_with(0, 0x81) == SOL_TX_ERR_VERSION);

  /* rule 3: header 1,0,2 with 5 keys or 1,0,3 with 6 keys. Named case: two signers. */
  CHECK(legacy_with(L_NSIG, 2) == SOL_TX_ERR_HEADER);
  CHECK(legacy_with(L_NSIG, 0) == SOL_TX_ERR_HEADER);
  CHECK(legacy_with(L_NRO_SIGNED, 1) == SOL_TX_ERR_HEADER);
  CHECK(legacy_with(L_NRO_UNSIGNED, 1) == SOL_TX_ERR_HEADER);
  CHECK(legacy_with(L_NRO_UNSIGNED, 4) == SOL_TX_ERR_HEADER);
  CHECK(legacy_with(L_NRO_UNSIGNED, 3) == SOL_TX_ERR_ACCOUNTS);       /* 1,0,3 needs 6 keys */
  CHECK(legacy_with(L_NKEYS, 6) == SOL_TX_ERR_ACCOUNTS);              /* six keys need 1,0,3 */
  CHECK(legacy_with(L_NKEYS, 4) == SOL_TX_ERR_ACCOUNTS);
  CHECK(memo_with(M_NKEYS, 5) == SOL_TX_ERR_ACCOUNTS);
  CHECK(memo_with(M_NKEYS, 7) == SOL_TX_ERR_ACCOUNTS);
  n = splice(V_LEGACY, sizeof V_LEGACY, L_NKEYS, 1, two_byte_5, 2);   /* key count 5 in two bytes: not the shortest encoding */
  CHECK(sol_tx_decode_transfer(bad, n, &t) == SOL_TX_ERR_ACCOUNTS);

  /* rule 4: 1 instruction with 5 keys, 2 with 6 */
  CHECK(legacy_with(L_NIX, 2) == SOL_TX_ERR_IX_COUNT);
  CHECK(legacy_with(L_NIX, 0) == SOL_TX_ERR_IX_COUNT);
  CHECK(memo_with(M_NIX, 1) == SOL_TX_ERR_IX_COUNT);
  CHECK(memo_with(M_NIX, 3) == SOL_TX_ERR_IX_COUNT);
  n = splice(V_LEGACY, sizeof V_LEGACY, L_NIX, 1, two_byte_1, 2);
  CHECK(sol_tx_decode_transfer(bad, n, &t) == SOL_TX_ERR_IX_COUNT);

  /* rule 5: exactly one Token instruction; with 6 keys the other is Memo.
     Named case: a second non-memo instruction (kit-built: ComputeBudget + transfer, 6 keys, 258 bytes). */
  CHECK(sizeof V_CB_LEGACY == 258 && sol_tx_decode_transfer(V_CB_LEGACY, sizeof V_CB_LEGACY, &t) == SOL_TX_ERR_PROGRAM);
  CHECK(legacy_with(L_PROG, 3) == SOL_TX_ERR_PROGRAM);                 /* program index -> the mint */
  CHECK(legacy_with(L_PROG, 5) == SOL_TX_ERR_PROGRAM);                 /* program index out of range */
  CHECK(legacy_with(L_KEYS + 4 * 32, V_LEGACY[L_KEYS + 4 * 32] ^ 1) == SOL_TX_ERR_PROGRAM);   /* e.g. Token-2022 / any other program */
  CHECK(memo_with(M_KEYS + 3 * 32, V_MEMO_LEGACY[M_KEYS + 3 * 32] ^ 1) == SOL_TX_ERR_PROGRAM);   /* second instruction is not Memo */
  n = splice(V_MEMO_LEGACY, sizeof V_MEMO_LEGACY, M_MEMO_IX, sizeof V_MEMO_LEGACY - M_MEMO_IX, V_MEMO_LEGACY + M_TOKEN_IX, M_TOKEN_IX_LEN);
  CHECK(sol_tx_decode_transfer(bad, n, &t) == SOL_TX_ERR_PROGRAM);     /* two Token instructions */
  n = splice(V_MEMO_LEGACY, sizeof V_MEMO_LEGACY, M_TOKEN_IX, M_TOKEN_IX_LEN, V_MEMO_LEGACY + M_MEMO_IX, sizeof V_MEMO_LEGACY - M_MEMO_IX);
  CHECK(sol_tx_decode_transfer(bad, n, &t) == SOL_TX_ERR_PROGRAM);     /* two Memo instructions, no transfer */
  /* a 5-key message whose only instruction is a Memo */
  memcpy(bad, V_LEGACY, L_NIX + 1); memcpy(bad + L_KEYS + 4 * 32, SOL_MEMO_PROGRAM_ID, 32);
  bad[L_PROG] = 4; bad[L_PROG + 1] = 0; bad[L_PROG + 2] = 2; bad[L_PROG + 3] = 'h'; bad[L_PROG + 4] = 'i';
  CHECK(sol_tx_decode_transfer(bad, L_PROG + 5, &t) == SOL_TX_ERR_PROGRAM);

  /* rule 6: the Token instruction has exactly 4 account indices, all in range */
  CHECK(legacy_with(L_NACC, 3) == SOL_TX_ERR_IX_ACCOUNTS);
  CHECK(legacy_with(L_NACC, 5) == SOL_TX_ERR_IX_ACCOUNTS);
  CHECK(legacy_with(L_ACC + 0, 5) == SOL_TX_ERR_IX_ACCOUNTS);
  CHECK(legacy_with(L_ACC + 1, 200) == SOL_TX_ERR_IX_ACCOUNTS);
  CHECK(memo_with(M_TOKEN_IX + 2, 6) == SOL_TX_ERR_IX_ACCOUNTS);
  n = splice(V_LEGACY, sizeof V_LEGACY, L_NACC, 1, two_byte_4, 2);
  CHECK(sol_tx_decode_transfer(bad, n, &t) == SOL_TX_ERR_IX_ACCOUNTS);

  /* rule 7: data is exactly 10 bytes starting with 12 (TransferChecked) */
  CHECK(legacy_with(L_DATA, 3) == SOL_TX_ERR_IX_DATA);                 /* 3 = Transfer */
  CHECK(legacy_with(L_DLEN, 9) == SOL_TX_ERR_IX_DATA);
  CHECK(legacy_with(L_DLEN, 11) == SOL_TX_ERR_IX_DATA);
  n = splice(V_LEGACY, sizeof V_LEGACY, L_DLEN, 1, two_byte_10, 2);
  CHECK(sol_tx_decode_transfer(bad, n, &t) == SOL_TX_ERR_IX_DATA);

  /* rule 8: authority is account 0 */
  CHECK(legacy_with(L_ACC + 3, 1) == SOL_TX_ERR_AUTHORITY);
  CHECK(legacy_with(L_ACC + 3, 3) == SOL_TX_ERR_AUTHORITY);

  /* rule 9: source/destination are 1 and 2 and differ; mint, Token and Memo programs are distinct indices >= 3 */
  CHECK(legacy_with(L_ACC + 2, V_LEGACY[L_ACC + 0]) == SOL_TX_ERR_ROLES);    /* destination == source */
  CHECK(legacy_with(L_ACC + 0, 0) == SOL_TX_ERR_ROLES);                      /* source is the payer */
  CHECK(legacy_with(L_ACC + 2, 3) == SOL_TX_ERR_ROLES);                      /* destination is a readonly key */
  CHECK(legacy_with(L_ACC + 1, 1) == SOL_TX_ERR_ROLES);                      /* mint is a writable key */
  CHECK(legacy_with(L_ACC + 1, V_LEGACY[L_PROG]) == SOL_TX_ERR_ROLES);       /* mint == Token program */
  CHECK(memo_with(M_TOKEN_IX + 3, V_MEMO_LEGACY[M_MEMO_IX]) == SOL_TX_ERR_ROLES);   /* mint == Memo program */
  memcpy(bad, V_LEGACY, sizeof V_LEGACY);                                     /* Token program at a writable index */
  memcpy(bad + L_KEYS + 1 * 32, SOL_TOKEN_PROGRAM_ID, 32); bad[L_PROG] = 1; bad[L_ACC + 0] = 2; bad[L_ACC + 2] = 1;
  CHECK(sol_tx_decode_transfer(bad, sizeof V_LEGACY, &t) == SOL_TX_ERR_ROLES);

  /* rule 10: amount is not zero */
  memcpy(bad, V_LEGACY, sizeof V_LEGACY); memset(bad + L_AMOUNT, 0, 8);
  CHECK(sol_tx_decode_transfer(bad, sizeof V_LEGACY, &t) == SOL_TX_ERR_AMOUNT_ZERO);
  memcpy(bad, V_LEGACY, sizeof V_LEGACY); memset(bad + L_AMOUNT, 0xFF, 8);    /* the largest amount is fine */
  CHECK(sol_tx_decode_transfer(bad, sizeof V_LEGACY, &t) == SOL_TX_OK && t.amount == 18446744073709551615ULL);

  /* rule 11: the Memo has zero accounts and 1 or more bytes of valid UTF-8. Named case: a Memo with an account. */
  { static const uint8_t one_account[] = {1, 0};
    n = splice(V_MEMO_LEGACY, sizeof V_MEMO_LEGACY, M_MEMO_NACC, 1, one_account, 2);
    CHECK(sol_tx_decode_transfer(bad, n, &t) == SOL_TX_ERR_MEMO); }
  n = splice(V_MEMO_LEGACY, sizeof V_MEMO_LEGACY, M_MEMO_DLEN, sizeof V_MEMO_LEGACY - M_MEMO_DLEN, (const uint8_t *)"\0", 1);
  CHECK(n == M_MEMO_DLEN + 1 && sol_tx_decode_transfer(bad, n, &t) == SOL_TX_ERR_MEMO);   /* empty memo */
  CHECK(memo_with(M_MEMO_DATA + 2, 0xFF) == SOL_TX_ERR_MEMO);          /* not UTF-8 */
  CHECK(memo_with(M_MEMO_DATA + 9, 0xC3) == SOL_TX_ERR_MEMO);          /* a sequence cut off by the end of the memo */
  CHECK(memo_with(M_MEMO_DATA + 0, 0x80) == SOL_TX_ERR_MEMO);          /* a continuation byte with no lead byte */

  /* rule 12: no bytes after the last instruction. Named case: trailing bytes. */
  memcpy(bad, V_LEGACY, sizeof V_LEGACY); bad[sizeof V_LEGACY] = 0;
  CHECK(sol_tx_decode_transfer(bad, sizeof V_LEGACY + 1, &t) == SOL_TX_ERR_TRAILING);
  memcpy(bad, V_MEMO_LEGACY, sizeof V_MEMO_LEGACY); bad[sizeof V_MEMO_LEGACY] = 0;
  CHECK(sol_tx_decode_transfer(bad, sizeof V_MEMO_LEGACY + 1, &t) == SOL_TX_ERR_TRAILING);
  /* a v0 message's trailing lookup-table count is not special: every versioned message is ERR_VERSION */
  memcpy(bad, V_V0, sizeof V_V0); bad[sizeof V_V0 - 1] = 1;
  CHECK(sol_tx_decode_transfer(bad, sizeof V_V0, &t) == SOL_TX_ERR_VERSION);
}

static void test_memo_utf8(void) {
  sol_transfer_t t; size_t n;
  /* valid: 2-, 3- and 4-byte sequences, the extremes of each length */
  CHECK(memo_roundtrip("caf\xC3\xA9 \xE2\x98\x95 \xF0\x9F\x8E\x89", 14, &t, &n) == SOL_TX_OK && t.memo_len == 14 && n == 248 + 1 + 14);
  CHECK(memo_roundtrip("\xC2\x80\xDF\xBF", 4, &t, NULL) == SOL_TX_OK);
  CHECK(memo_roundtrip("\xE0\xA0\x80\xEF\xBF\xBF\xED\x9F\xBF\xEE\x80\x80", 12, &t, NULL) == SOL_TX_OK);
  CHECK(memo_roundtrip("\xF0\x90\x80\x80\xF4\x8F\xBF\xBF", 8, &t, NULL) == SOL_TX_OK);
  /* invalid: the builder refuses them, and so does the decoder when they are patched into a message */
  { static const char *const BAD[] = {
      "ab\xFFzz",                 /* 0xFF never appears */
      "ab\xC0\x80z",              /* overlong NUL */
      "ab\xC1\xBFz",              /* overlong 2-byte */
      "a\xE0\x80\x80z",           /* overlong 3-byte */
      "\xF0\x80\x80\x80z",        /* overlong 4-byte */
      "a\xED\xA0\x80z",           /* a surrogate */
      "\xF4\x90\x80\x80z",        /* above U+10FFFF */
      "\xF8\x88\x80\x80\x80",     /* 5-byte form */
      "abc\xE2\x82",              /* cut off at the end */
      "a\xE2\x41\x80z",           /* bad continuation */
      "\x80xyzw"};                /* stray continuation */
    size_t i;
    for (i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
      CHECK(sol_tx_build_transfer(V_PAYER, V_SRC, V_DST, V_MINT, V_BH, 1000, 2, (const uint8_t *)BAD[i], 5, msg, sizeof msg) == 0);
      CHECK(memo_roundtrip("12345", 5, &t, &n) == SOL_TX_OK && n == 254);
      memcpy(msg + n - 5, BAD[i], 5);
      CHECK(sol_tx_decode_transfer(msg, n, &t) == SOL_TX_ERR_MEMO);
    }
  }
}

static void test_compact_u16(void) {
  sol_transfer_t t; size_t n, i; static char memo[1100];
  for (i = 0; i < sizeof memo; i++) memo[i] = (char)('a' + i % 26);
  /* a 127-byte memo has a one-byte length, a 128-byte memo a two-byte length */
  CHECK(memo_roundtrip(memo, 127, &t, &n) == SOL_TX_OK && n == 248 + 1 + 127 && t.memo_len == 127 && msg[n - 128] == 127);
  CHECK(memo_roundtrip(memo, 128, &t, &n) == SOL_TX_OK && n == 248 + 2 + 128 && t.memo_len == 128 && msg[n - 130] == 0x80 && msg[n - 129] == 0x01);
  /* spec case: a memo of 200 bytes (two-byte length) decodes */
  CHECK(memo_roundtrip(memo, 200, &t, &n) == SOL_TX_OK && n == 248 + 2 + 200 && t.memo_len == 200 && memcmp(t.memo, memo, 200) == 0);
  CHECK(msg[n - 202] == 0xC8 && msg[n - 201] == 0x01 && t.memo == msg + n - 200);
  /* the largest message: 1232 bytes; one more byte of memo does not fit */
  CHECK(memo_roundtrip(memo, 982, &t, &n) == SOL_TX_OK && n == SOL_TX_MSG_MAX && t.memo_len == 982 && memcmp(t.memo, memo, 982) == 0);
  CHECK(sol_tx_build_transfer(V_PAYER, V_SRC, V_DST, V_MINT, V_BH, 1000, 2, (const uint8_t *)memo, 983, msg, sizeof msg) == 0);
  /* spec case: a non-minimal encoding is refused. Memo length 10 written as 0x8A 0x00. */
  CHECK(memo_roundtrip(memo, 10, &t, &n) == SOL_TX_OK && n == 259 && msg[n - 11] == 10);
  memmove(msg + n - 9, msg + n - 10, 10); msg[n - 11] = 0x8A; msg[n - 10] = 0x00;
  CHECK(sol_tx_decode_transfer(msg, n + 1, &t) == SOL_TX_ERR_MEMO);
  /* length 200 written in three bytes (0xC8 0x81 0x00) */
  CHECK(memo_roundtrip(memo, 200, &t, &n) == SOL_TX_OK);
  memmove(msg + n - 199, msg + n - 200, 200); msg[n - 202] = 0xC8; msg[n - 201] = 0x81; msg[n - 200] = 0x00;
  CHECK(sol_tx_decode_transfer(msg, n + 1, &t) == SOL_TX_ERR_MEMO);
  /* a third byte that does not fit 16 bits, or continues */
  CHECK(memo_roundtrip(memo, 200, &t, &n) == SOL_TX_OK);
  memmove(msg + n - 199, msg + n - 200, 200); msg[n - 202] = 0xC8; msg[n - 201] = 0x81; msg[n - 200] = 0x04;
  CHECK(sol_tx_decode_transfer(msg, n + 1, &t) == SOL_TX_ERR_MEMO);
  msg[n - 200] = 0x80;
  CHECK(sol_tx_decode_transfer(msg, n + 1, &t) == SOL_TX_ERR_MEMO);
  /* a length that says more bytes follow than exist */
  CHECK(memo_roundtrip(memo, 200, &t, &n) == SOL_TX_OK);
  msg[n - 201] = 0x02;                                                   /* 0xC8 0x02 = 328 */
  CHECK(sol_tx_decode_transfer(msg, n, &t) == SOL_TX_ERR_TRUNCATED);
}

static void test_builder(void) {
  uint8_t payer[32], src[32], dst[32], mint[32], bh[32], memo[300]; sol_transfer_t t; size_t n, i, k, memo_len, want;
  uint64_t amount; uint8_t decimals; int round;
  /* 100 random key sets, each without and with a memo */
  for (round = 0; round < 100; round++) {
    rng_fill(payer, 32); rng_fill(src, 32); rng_fill(dst, 32); rng_fill(mint, 32); rng_fill(bh, 32);
    amount = rng() | 1; decimals = (uint8_t)(rng() >> 40);
    if (round % 4 == 0) amount = 1 + rng() % 100000;
    memo_len = 1 + (size_t)(rng() % sizeof memo);
    for (i = 0; i < memo_len; i++) memo[i] = (uint8_t)(0x20 + rng() % 95);

    n = sol_tx_build_transfer(payer, src, dst, mint, bh, amount, decimals, NULL, 0, msg, sizeof msg);
    CHECK(n == 214 && msg[0] == 1 && msg[1] == 0 && msg[2] == 2 && msg[3] == 5);
    CHECK(memcmp(msg + 4, payer, 32) == 0 && memcmp(msg + 36, msg + 68, 32) < 0 && memcmp(msg + 100, msg + 132, 32) < 0);
    CHECK(sol_tx_decode_transfer(msg, n, &t) == SOL_TX_OK);
    CHECK(t.amount == amount && t.decimals == decimals && t.memo == NULL && t.memo_len == 0);
    CHECK(!memcmp(t.fee_payer, payer, 32) && !memcmp(t.source, src, 32) && !memcmp(t.destination, dst, 32) && !memcmp(t.mint, mint, 32) && !memcmp(t.blockhash, bh, 32));
    CHECK(sol_tx_build_transfer(payer, src, dst, mint, bh, amount, decimals, NULL, 0, bad, 213) == 0);      /* cap too small */
    CHECK(sol_tx_build_transfer(payer, src, src, mint, bh, amount, decimals, NULL, 0, bad, sizeof bad) == 0);   /* source == destination */

    want = 248 + (memo_len < 128 ? 1 : 2) + memo_len;
    n = sol_tx_build_transfer(payer, src, dst, mint, bh, amount, decimals, memo, memo_len, msg, sizeof msg);
    CHECK(n == want && msg[0] == 1 && msg[1] == 0 && msg[2] == 3 && msg[3] == 6 && msg[228] == 2);
    CHECK(memcmp(msg + 4, payer, 32) == 0 && memcmp(msg + 36, msg + 68, 32) < 0);
    for (k = 3; k < 5; k++) CHECK(memcmp(msg + 4 + 32 * k, msg + 4 + 32 * (k + 1), 32) < 0);              /* readonly keys ascending */
    CHECK(memcmp(msg + 4 + 32 * msg[229], SOL_TOKEN_PROGRAM_ID, 32) == 0);                                 /* the transfer comes first */
    CHECK(memcmp(msg + 4 + 32 * msg[246], SOL_MEMO_PROGRAM_ID, 32) == 0 && msg[247] == 0);                 /* then the Memo, no accounts */
    CHECK(sol_tx_decode_transfer(msg, n, &t) == SOL_TX_OK);
    CHECK(t.amount == amount && t.decimals == decimals && t.memo_len == memo_len && t.memo == msg + n - memo_len && memcmp(t.memo, memo, memo_len) == 0);
    CHECK(!memcmp(t.fee_payer, payer, 32) && !memcmp(t.source, src, 32) && !memcmp(t.destination, dst, 32) && !memcmp(t.mint, mint, 32) && !memcmp(t.blockhash, bh, 32));
    CHECK(sol_tx_build_transfer(payer, src, dst, mint, bh, amount, decimals, memo, memo_len, bad, want - 1) == 0);
    CHECK(sol_tx_build_transfer(payer, src, src, mint, bh, amount, decimals, memo, memo_len, bad, sizeof bad) == 0);
  }
  /* refusals that keep the decoder guarantee */
  CHECK(sol_tx_build_transfer(V_PAYER, V_SRC, V_DST, V_MINT, V_BH, 0, 2, NULL, 0, msg, sizeof msg) == 0);          /* amount 0 */
  CHECK(sol_tx_build_transfer(V_PAYER, V_SRC, V_DST, V_MINT, V_BH, 1000, 2, NULL, 5, msg, sizeof msg) == 0);       /* memo_len without memo */
  /* the kit-built Memo vector and the builder agree on everything but (possibly) key order */
  n = sol_tx_build_transfer(V_PAYER, V_SRC, V_DST, V_MINT, V_BH, 1000, 2, V_MEMO_TEXT, sizeof V_MEMO_TEXT, msg, sizeof msg);
  CHECK(n == sizeof V_MEMO_LEGACY && sol_tx_decode_transfer(msg, n, &t) == SOL_TX_OK && t.memo_len == sizeof V_MEMO_TEXT);
}

static void test_amounts(void) {
  char text[64]; uint64_t raw;
  CHECK(sol_format_amount(1000, 2, text, sizeof text) && strcmp(text, "10.00") == 0);
  CHECK(sol_format_amount(1250, 2, text, sizeof text) == 5 && strcmp(text, "12.50") == 0);
  CHECK(sol_format_amount(5, 2, text, sizeof text) && strcmp(text, "0.05") == 0);
  CHECK(sol_format_amount(50000, 2, text, sizeof text) && strcmp(text, "500.00") == 0);
  CHECK(sol_format_amount(0, 0, text, sizeof text) && strcmp(text, "0") == 0);
  CHECK(sol_format_amount(18446744073709551615ULL, 0, text, sizeof text) && strcmp(text, "18446744073709551615") == 0);
  CHECK(sol_format_amount(50000000, 9, text, sizeof text) && strcmp(text, "0.050000000") == 0);
  CHECK(sol_format_amount(1000, 2, text, 5) == 0 && sol_format_amount(1000, 2, text, 6) == 5);   /* needs room for the NUL */
  CHECK(sol_parse_amount("10", 2, &raw) == 0 && raw == 1000);
  CHECK(sol_parse_amount("0.05", 2, &raw) == 0 && raw == 5);
  CHECK(sol_parse_amount("12.5", 2, &raw) == 0 && raw == 1250);
  CHECK(sol_parse_amount("18446744073709551615", 0, &raw) == 0 && raw == 18446744073709551615ULL);
  CHECK(sol_parse_amount("18446744073709551616", 0, &raw) == -1);
  CHECK(sol_parse_amount("1.234", 2, &raw) == -1 && sol_parse_amount("", 2, &raw) == -1 && sol_parse_amount("1e3", 2, &raw) == -1);
  CHECK(sol_parse_amount("-1", 2, &raw) == -1 && sol_parse_amount("1.2.3", 2, &raw) == -1 && sol_parse_amount(".", 2, &raw) == -1);
}

static void test_err_names(void) {
  int i, j;
  CHECK(SOL_TX_ERR_TRAILING == 14 && SOL_TX_MSG_MAX == 1232);
  CHECK(strcmp(sol_tx_err_name(SOL_TX_OK), "ok") == 0 && strcmp(sol_tx_err_name(SOL_TX_ERR_MEMO), "memo") == 0 && strcmp(sol_tx_err_name(SOL_TX_ERR_TRAILING), "trailing") == 0);
  CHECK(strcmp(sol_tx_err_name(SOL_TX_ERR_IX_COUNT), "ix_count") == 0 && strcmp(sol_tx_err_name(SOL_TX_ERR_VERSION), "version") == 0);
  for (i = 0; i <= SOL_TX_ERR_TRAILING; i++) {
    CHECK(strcmp(sol_tx_err_name((sol_tx_err_t)i), "?") != 0);
    for (j = 0; j < i; j++) CHECK(strcmp(sol_tx_err_name((sol_tx_err_t)i), sol_tx_err_name((sol_tx_err_t)j)) != 0);
  }
  CHECK(strcmp(sol_tx_err_name((sol_tx_err_t)(SOL_TX_ERR_TRAILING + 1)), "?") == 0);
}

int main(void) {
  test_base58();
  test_decode_vectors();
  test_decode_rules();
  test_memo_utf8();
  test_compact_u16();
  test_builder();
  test_amounts();
  test_err_names();
  printf(fails ? "%d FAILED\n" : "all sol tests passed\n", fails);
  return fails != 0;
}
