/* test_payment - binding a payment to its request: the request memo (16 lower-case hex characters of
   req_id), the wire-transaction split and the payee's check of a fetched transaction
   (vk_payment_verify), and the daily-limit text parser and window (vk_day_*).
   Spec: docs/os/wallet/solana-payments.md ("Request memo", "Checking a received payment"),
         docs/os/wallet/checks.md ("Daily limit"). */
#include "../../src/vk/wallet/pure/vk_payment.h"
#include "../../src/vk/wallet/pure/vk_checks.h"
#include <stdio.h>
#include <string.h>
#include "host_ed25519.h"
#include "vectors.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static const uint8_t REQ_ID[8] = {0x13, 0xce, 0xc0, 0xad, 0xb3, 0xe6, 0x9e, 0x6e};   /* V_REQ's req_id */

/* ---- the request memo ----------------------------------------------------------------------- */
static void test_req_memo(void) {
  char memo[VK_REQ_MEMO_LEN + 1];
  const uint8_t zeros[8] = {0};
  const uint8_t ff[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  vk_req_t r;
  CHECK(VK_REQ_MEMO_LEN == 16);
  CHECK(vk_req_parse(V_REQ, sizeof V_REQ, &r) == 0 && memcmp(r.req_id, REQ_ID, 8) == 0);
  memset(memo, 'x', sizeof memo);
  vk_req_memo(REQ_ID, memo);
  CHECK(strcmp(memo, "13cec0adb3e69e6e") == 0);
  vk_req_memo(zeros, memo); CHECK(strcmp(memo, "0000000000000000") == 0);
  vk_req_memo(ff, memo);    CHECK(strcmp(memo, "ffffffffffffffff") == 0);

  CHECK(vk_memo_is_req((const uint8_t *)"13cec0adb3e69e6e", 16, REQ_ID) == 1);
  CHECK(vk_memo_is_req((const uint8_t *)"13CEC0ADB3E69E6E", 16, REQ_ID) == 0);   /* upper case is not the encoding */
  CHECK(vk_memo_is_req((const uint8_t *)"13cec0adb3e69e6f", 16, REQ_ID) == 0);
  CHECK(vk_memo_is_req((const uint8_t *)"13cec0adb3e69e6e ", 17, REQ_ID) == 0);   /* nothing after it */
  CHECK(vk_memo_is_req((const uint8_t *)"13cec0adb3e69e6", 15, REQ_ID) == 0);
  CHECK(vk_memo_is_req((const uint8_t *)"0x13cec0adb3e69e", 16, REQ_ID) == 0);
  CHECK(vk_memo_is_req(NULL, 0, REQ_ID) == 0);
  CHECK(vk_memo_is_req((const uint8_t *)"13cec0adb3e69e6e", 16, NULL) == 0);

  /* hex parsing for the Lua side: 16 characters of either case */
  {
    uint8_t id[8];
    CHECK(vk_req_id_parse("13cec0adb3e69e6e", 16, id) == 0 && memcmp(id, REQ_ID, 8) == 0);
    CHECK(vk_req_id_parse("13CEC0ADB3E69E6E", 16, id) == 0 && memcmp(id, REQ_ID, 8) == 0);
    CHECK(vk_req_id_parse("13cec0adb3e69e6", 15, id) != 0);
    CHECK(vk_req_id_parse("13cec0adb3e69e6g", 16, id) != 0);
    CHECK(vk_req_id_parse(NULL, 16, id) != 0);
  }
}

/* ---- fixtures for the payee's check ------------------------------------------------------------ */
/* A payment as a payer badge sends it: payer = the device key pair of the vectors (we hold its
   seed), to V_DST, 10.00 HACK, with the request memo; wire = 0x01 || sig || msg. */
static uint8_t PAYER_PUB[32];
static uint8_t msg[SOL_TX_MSG_MAX], wire[1 + 64 + SOL_TX_MSG_MAX];
static size_t msg_len, wire_len;

static size_t build(const uint8_t *dst, const uint8_t *mint, uint64_t amount, uint8_t decimals, const char *memo) {
  msg_len = sol_tx_build_transfer(PAYER_PUB, V_SRC, dst, mint, V_BH, amount, decimals,
                                  (const uint8_t *)memo, memo ? strlen(memo) : 0, msg, sizeof msg);
  wire[0] = 1;
  host_ed25519_sign(V_DEVICE_SEED, msg, msg_len, wire + 1);
  memcpy(wire + 65, msg, msg_len);
  wire_len = 65 + msg_len;
  return msg_len;
}

static vk_pay_expect_t expect(void) {
  vk_pay_expect_t e;
  memset(&e, 0, sizeof e);
  e.to = V_DST; e.mint = V_MINT; e.decimals = 2; e.amount = 1000; e.req_id = REQ_ID;
  e.verify = host_ed25519_verify;
  return e;
}

static void test_wire_split(void) {
  const uint8_t *sig = NULL, *m = NULL; size_t n = 0;
  uint8_t bad[2 + 128 + 214];
  build(V_DST, V_MINT, 1000, 2, "13cec0adb3e69e6e");
  CHECK(vk_wire_split(wire, wire_len, &sig, &m, &n) == 0 && sig == wire + 1 && m == wire + 65 && n == msg_len);
  CHECK(vk_wire_split(wire, 65, &sig, &m, &n) != 0);             /* no message */
  CHECK(vk_wire_split(wire, 10, &sig, &m, &n) != 0);
  CHECK(vk_wire_split(NULL, 0, &sig, &m, &n) != 0);
  /* two signatures: the badge's payments have exactly one */
  memset(bad, 0, sizeof bad); bad[0] = 2; memcpy(bad + 129, msg, 214);
  CHECK(vk_wire_split(bad, sizeof bad, &sig, &m, &n) != 0);
  bad[0] = 0; CHECK(vk_wire_split(bad, sizeof bad, &sig, &m, &n) != 0);
  /* a non-minimal signature count (0x81 0x00) is not accepted */
  bad[0] = 0x81; bad[1] = 0x00; CHECK(vk_wire_split(bad, sizeof bad, &sig, &m, &n) != 0);
}

static void test_verify_ok(void) {
  vk_pay_expect_t e = expect(); vk_pay_seen_t seen;
  build(V_DST, V_MINT, 1000, 2, "13cec0adb3e69e6e");
  memset(&seen, 0xEE, sizeof seen);
  CHECK(vk_payment_verify(wire, wire_len, &e, &seen) == VK_PAY_OK);
  CHECK(memcmp(seen.sig, wire + 1, 64) == 0 && memcmp(seen.transfer.fee_payer, PAYER_PUB, 32) == 0);
  CHECK(seen.transfer.amount == 1000 && memcmp(seen.transfer.destination, V_DST, 32) == 0);
  /* seen may be NULL */
  CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_OK);
  /* optional fields that match */
  e.payer = PAYER_PUB; e.sig = wire + 1;
  CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_OK);
  /* without a verifier the payer's signature is not checked */
  e = expect(); e.verify = NULL; wire[5] ^= 1;
  CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_OK);
  wire[5] ^= 1;
  CHECK(strcmp(vk_pay_err_name(VK_PAY_OK), "ok") == 0);
}

static void test_verify_refusals(void) {
  vk_pay_expect_t e; uint8_t other[32];
  memset(other, 0x42, sizeof other);

  /* arguments */
  build(V_DST, V_MINT, 1000, 2, "13cec0adb3e69e6e");
  CHECK(vk_payment_verify(wire, wire_len, NULL, NULL) == VK_PAY_ERR_ARG);
  e = expect(); e.to = NULL;     CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_ERR_ARG);
  e = expect(); e.mint = NULL;   CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_ERR_ARG);
  e = expect(); e.req_id = NULL; CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_ERR_ARG);
  e = expect(); e.amount = 0;    CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_ERR_ARG);

  /* not a one-signature wire transaction */
  e = expect();
  CHECK(vk_payment_verify(wire, 64, &e, NULL) == VK_PAY_ERR_WIRE);
  CHECK(vk_payment_verify(msg, msg_len, &e, NULL) != VK_PAY_OK);          /* a bare message is not a wire tx */
  /* a message the decoder refuses (cut short; extra instructions; a versioned message) */
  CHECK(vk_payment_verify(wire, wire_len - 1, &e, NULL) == VK_PAY_ERR_SHAPE);
  wire_len = 65 + sizeof V_CB_LEGACY; memcpy(wire + 65, V_CB_LEGACY, sizeof V_CB_LEGACY);
  CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_ERR_SHAPE);
  wire_len = 65 + sizeof V_V0; memcpy(wire + 65, V_V0, sizeof V_V0);
  CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_ERR_SHAPE);

  /* the payer's signature does not verify over the message */
  build(V_DST, V_MINT, 1000, 2, "13cec0adb3e69e6e"); wire[1] ^= 1;
  CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_ERR_SIGNATURE);
  /* a message signed by someone other than its fee payer */
  build(V_DST, V_MINT, 1000, 2, "13cec0adb3e69e6e"); host_ed25519_sign(V_ISSUER_SEED, msg, msg_len, wire + 1);
  CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_ERR_SIGNATURE);
  /* another signature than the one the RESULT frame carried */
  build(V_DST, V_MINT, 1000, 2, "13cec0adb3e69e6e"); e = expect(); e.sig = other;
  CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_ERR_SIG_MISMATCH);
  /* another payer than the one who asked for presence */
  e = expect(); e.payer = other;
  CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_ERR_PAYER);

  /* another mint; the right mint with other decimals */
  e = expect();
  build(V_DST, V_ALT_MINT, 1000, 2, "13cec0adb3e69e6e");
  CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_ERR_MINT);
  build(V_DST, V_MINT, 1000, 6, "13cec0adb3e69e6e");
  CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_ERR_MINT);
  /* another token account (the payee's wallet address instead of its token account included) */
  build(V_ALT_DST, V_MINT, 1000, 2, "13cec0adb3e69e6e");
  CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_ERR_RECIPIENT);
  build(V_DST_OWNER, V_MINT, 1000, 2, "13cec0adb3e69e6e");
  CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_ERR_RECIPIENT);
  /* too little, too much */
  build(V_DST, V_MINT, 999, 2, "13cec0adb3e69e6e");
  CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_ERR_AMOUNT);
  build(V_DST, V_MINT, 1001, 2, "13cec0adb3e69e6e");
  CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_ERR_AMOUNT);
  /* the memo: missing, another request, upper case, free text */
  build(V_DST, V_MINT, 1000, 2, NULL);
  CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_ERR_MEMO);
  build(V_DST, V_MINT, 1000, 2, "13cec0adb3e69e6f");
  CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_ERR_MEMO);
  build(V_DST, V_MINT, 1000, 2, "13CEC0ADB3E69E6E");
  CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_ERR_MEMO);
  build(V_DST, V_MINT, 1000, 2, "coffee #42");
  CHECK(vk_payment_verify(wire, wire_len, &e, NULL) == VK_PAY_ERR_MEMO);

  /* names */
  CHECK(strcmp(vk_pay_err_name(VK_PAY_ERR_ARG), "arg") == 0);
  CHECK(strcmp(vk_pay_err_name(VK_PAY_ERR_WIRE), "wire") == 0);
  CHECK(strcmp(vk_pay_err_name(VK_PAY_ERR_SHAPE), "shape") == 0);
  CHECK(strcmp(vk_pay_err_name(VK_PAY_ERR_SIGNATURE), "signature") == 0);
  CHECK(strcmp(vk_pay_err_name(VK_PAY_ERR_SIG_MISMATCH), "sig") == 0);
  CHECK(strcmp(vk_pay_err_name(VK_PAY_ERR_PAYER), "payer") == 0);
  CHECK(strcmp(vk_pay_err_name(VK_PAY_ERR_MINT), "mint") == 0);
  CHECK(strcmp(vk_pay_err_name(VK_PAY_ERR_RECIPIENT), "recipient") == 0);
  CHECK(strcmp(vk_pay_err_name(VK_PAY_ERR_AMOUNT), "amount") == 0);
  CHECK(strcmp(vk_pay_err_name(VK_PAY_ERR_MEMO), "memo") == 0);
  CHECK(strcmp(vk_pay_err_name((vk_pay_err_t)99), "?") == 0);
  CHECK(vk_pay_reason(VK_PAY_OK) == VK_OK && vk_pay_reason(VK_PAY_ERR_ARG) == VK_BAD_ARG);
  CHECK(vk_pay_reason(VK_PAY_ERR_WIRE) == VK_UNDECODABLE && vk_pay_reason(VK_PAY_ERR_SHAPE) == VK_UNDECODABLE);
  CHECK(vk_pay_reason(VK_PAY_ERR_SIGNATURE) == VK_BAD_PROOF);
  CHECK(vk_pay_reason(VK_PAY_ERR_SIG_MISMATCH) == VK_MISMATCH && vk_pay_reason(VK_PAY_ERR_MEMO) == VK_MISMATCH);
  CHECK(vk_pay_reason(VK_PAY_ERR_PAYER) == VK_MISMATCH && vk_pay_reason(VK_PAY_ERR_MINT) == VK_MISMATCH);
  CHECK(vk_pay_reason(VK_PAY_ERR_RECIPIENT) == VK_MISMATCH && vk_pay_reason(VK_PAY_ERR_AMOUNT) == VK_MISMATCH);
}

/* ---- the daily limit: text form and window ----------------------------------------------------- */
static vk_token_t TOK[3];

static void test_day_parse(void) {
  uint64_t lim[VK_MAX_TOKENS];
  /* unset: no limit for any token */
  CHECK(vk_day_limits_parse("", TOK, 2, lim) == 0 && lim[0] == 0 && lim[1] == 0);
  CHECK(vk_day_limits_parse(NULL, TOK, 2, lim) == 0 && lim[0] == 0 && lim[1] == 0);
  /* one token, display units */
  CHECK(vk_day_limits_parse("HACK:50.00", TOK, 2, lim) == 0 && lim[0] == 5000 && lim[1] == 0);
  CHECK(vk_day_limits_parse("HACK:50", TOK, 2, lim) == 0 && lim[0] == 5000);
  CHECK(vk_day_limits_parse("HACK:0.5", TOK, 2, lim) == 0 && lim[0] == 50);
  /* two tokens, any order; 0 = no limit (as for cap and max) */
  CHECK(vk_day_limits_parse("ALT:7.5,HACK:0", TOK, 2, lim) == 0 && lim[0] == 0 && lim[1] == 750);
  /* refused: unknown symbol, a symbol twice, too many decimals, empty amount, junk, a trailing comma */
  CHECK(vk_day_limits_parse("USD:5", TOK, 2, lim) != 0);
  CHECK(vk_day_limits_parse("HACK:5,HACK:6", TOK, 2, lim) != 0);
  CHECK(vk_day_limits_parse("HACK:5.001", TOK, 2, lim) != 0);
  CHECK(vk_day_limits_parse("HACK:", TOK, 2, lim) != 0);
  CHECK(vk_day_limits_parse("HACK", TOK, 2, lim) != 0);
  CHECK(vk_day_limits_parse("HACK:-5", TOK, 2, lim) != 0);
  CHECK(vk_day_limits_parse("HACK:5,", TOK, 2, lim) != 0);
  CHECK(vk_day_limits_parse("hack:5", TOK, 2, lim) != 0);
  CHECK(vk_day_limits_parse("HACK:5 ", TOK, 2, lim) != 0);
  CHECK(vk_day_limits_parse("HACK:99999999999999999999", TOK, 2, lim) != 0);
  /* a symbol that is a prefix of another is not confused with it */
  CHECK(vk_day_limits_parse("HAC:5", TOK, 2, lim) != 0);
}

static void test_day_window(void) {
  const uint32_t now = 1790000000u;
  CHECK(VK_DAY_SECONDS == 86400u);
  CHECK(vk_day_counts(now, now) == 1);
  CHECK(vk_day_counts(now - 1, now) == 1);
  CHECK(vk_day_counts(now - 86399, now) == 1);
  CHECK(vk_day_counts(now - 86400, now) == 0);            /* exactly a day ago has left the window */
  CHECK(vk_day_counts(now - 90000, now) == 0);
  CHECK(vk_day_counts(now + 500, now) == 1);              /* written later than "now": the clock went back; count it */
  CHECK(vk_day_counts(0, now) == 1);                      /* written with no clock: unknown time counts */
  CHECK(vk_day_counts(12345, 0) == 1);                    /* no clock now: everything counts */
  CHECK(vk_day_counts(5, 100) == 1);                      /* now < a day: nothing can be a day old */
}

int main(void) {
  memset(TOK, 0, sizeof TOK);
  memcpy(TOK[0].mint, V_MINT, 32); TOK[0].decimals = 2; memcpy(TOK[0].symbol, "HACK", 5);
  memcpy(TOK[1].mint, V_ALT_MINT, 32); TOK[1].decimals = 2; memcpy(TOK[1].symbol, "ALT", 4);
  host_ed25519_keypair(V_DEVICE_SEED, PAYER_PUB);
  CHECK(memcmp(PAYER_PUB, V_DEVICE_PUB, 32) == 0);

  test_req_memo();
  test_wire_split();
  test_verify_ok();
  test_verify_refusals();
  test_day_parse();
  test_day_window();
  printf(fails ? "%d FAILED\n" : "all payment tests passed\n", fails);
  return fails != 0;
}
