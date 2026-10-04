/* test_checks - the check chain: one test per failure row, every amber condition, the cap, green;
   headline texts; reason names. Spec: docs/os/wallet/checks.md ("The check chain", "Tests"),
   docs/os/reference/reasons.md. */
#include "../../src/vk/wallet/pure/vk_checks.h"
#include <stdio.h>
#include <string.h>
#include "host_ed25519.h"
#include "vectors.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* ---- fixtures ------------------------------------------------------------------------------ */
#define NOW (V_RECORD_ISSUED_AT + 100u)      /* 100 s after the record was issued; the vector REQ is still open */
#define TTL 3600u

static vk_token_t TOKENS[2];                 /* [0] HACK = V_MINT, 2 decimals, cap 100.00, max 1000.00 */

/* presence stub */
static vk_presence_t p_result; static uint8_t p_payee[32], p_seen_req_id[8]; static int p_calls;
static vk_presence_t presence_stub(const uint8_t req_id[8], uint8_t payee_pubkey_out[32], uint8_t nonce_out[16]) {
  p_calls++;
  memcpy(p_seen_req_id, req_id, 8);
  if (p_result != VK_PRESENCE_NONE) { memcpy(payee_pubkey_out, p_payee, 32); memcpy(nonce_out, V_PROOF_NONCE, 16); }
  return p_result;
}

/* The honest payment: vector message, record, request, a PRESENT proof, synced clock. */
static vk_check_input_t base(void) {
  vk_check_input_t in;
  memset(&in, 0, sizeof in);
  in.msg = V_LEGACY; in.msg_len = sizeof V_LEGACY;
  in.record = V_RECORD; in.record_len = sizeof V_RECORD; in.record_sig = V_RECORD_SIG;
  in.req = V_REQ; in.req_len = sizeof V_REQ;
  in.own_pubkey = V_PAYER; in.issuer_key = V_ISSUER_PUB;
  in.tokens = TOKENS; in.token_count = 1;
  in.now = NOW; in.time_source = VK_TIME_SNTP; in.record_ttl_s = TTL;
  in.verify = host_ed25519_verify;
  in.presence = presence_stub;
  p_result = VK_PRESENCE_PRESENT; memcpy(p_payee, V_DEVICE_PUB, 32); p_calls = 0; memset(p_seen_req_id, 0, 8);
  return in;
}

/* The 11 lines of the vector record, for building variants. */
enum { L_V, L_ATTESTATION, L_NAME, L_DEVICE, L_KIND, L_WALLET, L_ATA, L_BANK, L_EXPIRY, L_STATUS, L_ISSUED, L_COUNT };
static char LINES[L_COUNT][128];
static void split_vector(void) {
  size_t i, line = 0, col = 0;
  memset(LINES, 0, sizeof LINES);
  for (i = 0; i < sizeof V_RECORD; i++) {
    if (V_RECORD[i] == '\n') { line++; col = 0; continue; }
    if (line < L_COUNT && col < sizeof LINES[0] - 1) LINES[line][col++] = (char)V_RECORD[i];
  }
}

/* A record like the vector's with line `which` replaced (which < 0: unchanged) and `tail` appended,
   signed by `seed` over "registry:" || bytes. Returns its length. */
static uint8_t rec[600], rec_sig[64];
static size_t make_record(int which, const char *text, const char *tail, const uint8_t *seed) {
  uint8_t signed_bytes[9 + sizeof rec]; size_t n = 0, k; int i;
  for (i = 0; i < L_COUNT; i++) {
    const char *l = (i == which) ? text : LINES[i];
    k = strlen(l);
    if (i) rec[n++] = '\n';
    memcpy(rec + n, l, k); n += k;
  }
  k = strlen(tail); memcpy(rec + n, tail, k); n += k;
  memcpy(signed_bytes, "registry:", 9); memcpy(signed_bytes + 9, rec, n);
  host_ed25519_sign(seed, signed_bytes, 9 + n, rec_sig);
  return n;
}
static void use_record(vk_check_input_t *in, size_t n) { in->record = rec; in->record_len = n; in->record_sig = rec_sig; }

/* A REQ frame from the fields in *r, signed by `seed` (domain pay-req). Returns its length. */
static uint8_t req[VK_REQ_MAX_LEN];
static size_t make_req(vk_req_t *r, const uint8_t *seed) {
  uint8_t signed_bytes[8 + VK_REQ_SIGNED_MAX]; size_t n, signed_len;
  memset(r->sig, 0, 64);
  n = vk_req_build(r, req, sizeof req);
  signed_len = n - 64;
  memcpy(signed_bytes, "pay-req:", 8); memcpy(signed_bytes + 8, req, signed_len);
  host_ed25519_sign(seed, signed_bytes, 8 + signed_len, r->sig);
  n = vk_req_build(r, req, sizeof req);
  return n;
}
static vk_req_t vector_req(void) { vk_req_t r; vk_req_parse(V_REQ, sizeof V_REQ, &r); return r; }

/* A transfer from the vector payer. */
static uint8_t msg[SOL_TX_MSG_MAX];
static size_t make_msg(const uint8_t *destination, const uint8_t *mint, uint64_t amount, uint8_t decimals) {
  return sol_tx_build_transfer(V_PAYER, V_SRC, destination, mint, V_BH, amount, decimals, NULL, 0, msg, sizeof msg);
}

/* ---- assertions ---------------------------------------------------------------------------- */
static void expect_red_at(int line, const vk_verdict_t *v, vk_headline_t headline, const char *text, vk_reason_t reason, int overridable) {
  if (v->severity != VK_SEV_RED || v->select != VK_SEL_DISABLED || v->headline != headline || v->reason != reason ||
      v->dev_overridable != overridable || strcmp(vk_headline_text(v->headline), text) != 0) {
    printf("FAIL %s:%d expected red \"%s\"/%s, got severity %d select %d \"%s\"/%s overridable %d\n", __FILE__, line, text,
           vk_reason_name(reason), (int)v->severity, (int)v->select, vk_headline_text(v->headline), vk_reason_name(v->reason), v->dev_overridable);
    fails++;
  }
}
#define EXPECT_RED(v, headline, text, reason, overridable) expect_red_at(__LINE__, v, headline, text, reason, overridable)

static void expect_ok_at(int line, const vk_verdict_t *v, vk_severity_t severity, vk_headline_t headline, const char *text, vk_select_t select) {
  if (v->severity != severity || v->select != select || v->headline != headline || v->reason != VK_OK || v->dev_overridable != 0 ||
      !v->decoded || !v->record_ok || strcmp(vk_headline_text(v->headline), text) != 0) {
    printf("FAIL %s:%d expected severity %d \"%s\" select %d, got severity %d \"%s\" select %d reason %s\n", __FILE__, line, (int)severity, text,
           (int)select, (int)v->severity, vk_headline_text(v->headline), (int)v->select, vk_reason_name(v->reason));
    fails++;
  }
}
#define EXPECT_GREEN(v) expect_ok_at(__LINE__, v, VK_SEV_GREEN, VK_HL_VERIFIED_PRESENT, "VERIFIED - PRESENT", VK_SEL_PRESS)
#define EXPECT_OK(v, severity, headline, text, select) expect_ok_at(__LINE__, v, severity, headline, text, select)

/* ---- green --------------------------------------------------------------------------------- */
static void test_green(void) {
  vk_check_input_t in = base(); vk_verdict_t v; vk_req_t r = vector_req();
  memset(&v, 0xEE, sizeof v);
  vk_check_solana(&in, &v);
  EXPECT_GREEN(&v);
  CHECK(v.severity == VK_SEV_GREEN && v.select == VK_SEL_PRESS && v.reason == VK_OK && v.headline == VK_HL_VERIFIED_PRESENT && v.dev_overridable == 0);
  CHECK(v.decoded == 1 && v.tx_err == SOL_TX_OK && v.token == &TOKENS[0]);
  CHECK(v.transfer.amount == 1000 && v.transfer.decimals == 2 && v.transfer.memo == NULL && !memcmp(v.transfer.destination, V_DST, 32) && !memcmp(v.transfer.fee_payer, V_PAYER, 32));
  CHECK(v.record_ok == 1 && strcmp(v.record.display_name, "MHacks Merch") == 0 && !memcmp(v.record.device_pubkey, V_DEVICE_PUB, 32) && v.record.kind == VK_KIND_MERCHANT);
  CHECK(v.req_ok == 1 && v.req.amount == 1000 && strcmp(v.req.currency, "HACK") == 0 && !memcmp(v.req.req_id, r.req_id, 8));
  CHECK(v.presence == VK_PRESENCE_PRESENT && p_calls == 1 && memcmp(p_seen_req_id, r.req_id, 8) == 0);
  /* the same payment with a Memo: green, and the memo is exposed */
  in = base(); in.msg = V_MEMO_LEGACY; in.msg_len = sizeof V_MEMO_LEGACY;
  vk_check_solana(&in, &v);
  EXPECT_GREEN(&v);
  CHECK(v.transfer.memo_len == sizeof V_MEMO_TEXT && v.transfer.memo != NULL && memcmp(v.transfer.memo, V_MEMO_TEXT, sizeof V_MEMO_TEXT) == 0);
  /* a second token in the table does not matter */
  in = base(); in.token_count = 2;
  vk_check_solana(&in, &v); EXPECT_GREEN(&v); CHECK(v.token == &TOKENS[0]);
  /* missing arguments are a red verdict, never a crash */
  vk_check_solana(NULL, &v); EXPECT_RED(&v, VK_HL_CANNOT_READ, "CANNOT READ PAYMENT", VK_UNDECODABLE, 0);
  vk_check_solana(&in, NULL);
}

/* ---- failure rows, in table order ---------------------------------------------------------- */
static void test_check_1_cannot_read(void) {
  vk_check_input_t in; vk_verdict_t v;
  /* the message does not decode */
  in = base(); in.msg = V_V0; in.msg_len = sizeof V_V0;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_CANNOT_READ, "CANNOT READ PAYMENT", VK_UNDECODABLE, 0);
  CHECK(v.decoded == 0 && v.tx_err == SOL_TX_ERR_VERSION && v.token == NULL && v.record_ok == 0 && v.req_ok == 0 && p_calls == 0);
  in = base(); in.msg = V_CB_LEGACY; in.msg_len = sizeof V_CB_LEGACY;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_CANNOT_READ, "CANNOT READ PAYMENT", VK_UNDECODABLE, 0); CHECK(v.tx_err == SOL_TX_ERR_PROGRAM);
  in = base(); in.msg_len = sizeof V_LEGACY - 1;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_CANNOT_READ, "CANNOT READ PAYMENT", VK_UNDECODABLE, 0); CHECK(v.tx_err == SOL_TX_ERR_TRUNCATED);
  in = base(); in.msg = NULL; in.msg_len = 0;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_CANNOT_READ, "CANNOT READ PAYMENT", VK_UNDECODABLE, 0);
  /* it decodes, but account 0 is not this badge */
  in = base(); in.own_pubkey = V_DEVICE_PUB;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_CANNOT_READ, "CANNOT READ PAYMENT", VK_UNDECODABLE, 0);
  CHECK(v.decoded == 0 && v.tx_err == SOL_TX_OK && v.record_ok == 0);
  in = base(); in.own_pubkey = NULL;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_CANNOT_READ, "CANNOT READ PAYMENT", VK_UNDECODABLE, 0);
  /* this failure comes first: it wins over a missing record */
  in = base(); in.msg = V_V0; in.msg_len = sizeof V_V0; in.record = NULL; in.record_len = 0; in.record_sig = NULL;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_CANNOT_READ, "CANNOT READ PAYMENT", VK_UNDECODABLE, 0);
}

static void test_check_2_unknown_token(void) {
  vk_check_input_t in; vk_verdict_t v;
  /* the mint is not in the table */
  in = base(); in.msg = V_ALT_LEGACY; in.msg_len = sizeof V_ALT_LEGACY;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_UNKNOWN_TOKEN, "UNKNOWN TOKEN", VK_UNDECODABLE, 0);
  CHECK(v.decoded == 0 && v.token == NULL && v.tx_err == SOL_TX_OK);
  in = base(); in.token_count = 0;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_UNKNOWN_TOKEN, "UNKNOWN TOKEN", VK_UNDECODABLE, 0);
  in = base(); in.tokens = NULL;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_UNKNOWN_TOKEN, "UNKNOWN TOKEN", VK_UNDECODABLE, 0);
  /* the mint is in the table but the decimals differ */
  in = base(); in.msg = msg; in.msg_len = make_msg(V_DST, V_MINT, 1000, 6);
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_UNKNOWN_TOKEN, "UNKNOWN TOKEN", VK_UNDECODABLE, 0);
  CHECK(v.decoded == 0 && v.record_ok == 0);
  /* the second table entry is found too */
  in = base(); in.token_count = 2; in.msg = V_ALT_LEGACY; in.msg_len = sizeof V_ALT_LEGACY; in.record = NULL;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_UNVERIFIED, "UNVERIFIED RECIPIENT", VK_UNVERIFIED, 1); CHECK(v.decoded == 1 && v.token == &TOKENS[1]);
}

static void test_check_3_over_limit(void) {
  vk_check_input_t in; vk_verdict_t v;
  in = base(); in.msg = msg; in.msg_len = make_msg(V_DST, V_MINT, 100001, 2);          /* max is 100000 */
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_OVER_LIMIT, "OVER LIMIT", VK_OVER_CAP, 0);
  CHECK(v.decoded == 1 && v.token == &TOKENS[0] && v.transfer.amount == 100001 && v.record_ok == 0);
  /* it comes before the record checks */
  in.record = NULL; in.record_len = 0;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_OVER_LIMIT, "OVER LIMIT", VK_OVER_CAP, 0);
  /* exactly the max is allowed (no request here: amber, and over the cap so a hold) */
  in = base(); in.req = NULL; in.req_len = 0; in.msg = msg; in.msg_len = make_msg(V_DST, V_MINT, 100000, 2);
  vk_check_solana(&in, &v); EXPECT_OK(&v, VK_SEV_AMBER, VK_HL_NOT_PRESENT, "VERIFIED - NOT PRESENT", VK_SEL_HOLD);
  /* max 0 = no max */
  TOKENS[0].max = 0;
  in.msg_len = make_msg(V_DST, V_MINT, 18446744073709551615ULL, 2);
  vk_check_solana(&in, &v); EXPECT_OK(&v, VK_SEV_AMBER, VK_HL_NOT_PRESENT, "VERIFIED - NOT PRESENT", VK_SEL_HOLD);
  TOKENS[0].max = 100000;
}

static void test_check_4_no_record(void) {
  vk_check_input_t in; vk_verdict_t v;
  in = base(); in.record = NULL; in.record_len = 0; in.record_sig = NULL;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_UNVERIFIED, "UNVERIFIED RECIPIENT", VK_UNVERIFIED, 1);     /* the one dev-overridable failure */
  CHECK(v.decoded == 1 && v.token == &TOKENS[0] && v.record_ok == 0 && v.req_ok == 0 && p_calls == 0);
  in = base(); in.record_sig = NULL;                                                                          /* a record without its signature */
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_UNVERIFIED, "UNVERIFIED RECIPIENT", VK_UNVERIFIED, 1);
  in = base(); in.record_len = 0;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_UNVERIFIED, "UNVERIFIED RECIPIENT", VK_UNVERIFIED, 1);
}

static void test_check_5_bad_record(void) {
  vk_check_input_t in; vk_verdict_t v; uint8_t sig[64]; size_t n;
  /* the issuer signature is not valid */
  in = base(); memcpy(sig, V_RECORD_SIG, 64); sig[7] ^= 1; in.record_sig = sig;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_UNVERIFIED, "UNVERIFIED RECIPIENT", VK_UNVERIFIED, 0);
  CHECK(v.record_ok == 0 && v.decoded == 1);
  /* signed, but by a key that is not the issuer (here the recipient's own device key) */
  in = base(); n = make_record(-1, "", "", V_DEVICE_SEED); use_record(&in, n);
  CHECK(n == sizeof V_RECORD && memcmp(rec, V_RECORD, n) == 0);
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_UNVERIFIED, "UNVERIFIED RECIPIENT", VK_UNVERIFIED, 0);
  /* the issuer key is not the provisioned one */
  in = base(); in.issuer_key = V_DEVICE_PUB;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_UNVERIFIED, "UNVERIFIED RECIPIENT", VK_UNVERIFIED, 0);
  in = base(); in.issuer_key = NULL;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_UNVERIFIED, "UNVERIFIED RECIPIENT", VK_UNVERIFIED, 0);
  in = base(); in.verify = NULL;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_UNVERIFIED, "UNVERIFIED RECIPIENT", VK_UNVERIFIED, 0);
  /* correctly signed by the issuer, but not a canonical record */
  in = base(); n = make_record(-1, "", "\n", V_ISSUER_SEED); use_record(&in, n);
  CHECK(vk_record_verify(rec, n, rec_sig, V_ISSUER_PUB, host_ed25519_verify) == 1);
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_UNVERIFIED, "UNVERIFIED RECIPIENT", VK_UNVERIFIED, 0);
  in = base(); n = make_record(L_KIND, "kind=shop", "", V_ISSUER_SEED); use_record(&in, n);
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_UNVERIFIED, "UNVERIFIED RECIPIENT", VK_UNVERIFIED, 0);
  /* a record edited after signing */
  in = base(); memcpy(rec, V_RECORD, sizeof V_RECORD); rec[40] ^= 1; in.record = rec;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_UNVERIFIED, "UNVERIFIED RECIPIENT", VK_UNVERIFIED, 0);
}

static void test_check_6_revoked(void) {
  vk_check_input_t in = base(); vk_verdict_t v;
  use_record(&in, make_record(L_STATUS, "status=revoked", "", V_ISSUER_SEED));
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_REVOKED, "REVOKED", VK_REVOKED, 0);
  CHECK(v.record_ok == 1 && v.record.status == VK_STATUS_REVOKED && v.req_ok == 0 && p_calls == 0);
  /* revoked wins over expired */
  in.now = V_RECORD_EXPIRY + 5;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_REVOKED, "REVOKED", VK_REVOKED, 0);
}

static void test_check_8_expired(void) {
  vk_check_input_t in; vk_verdict_t v;
  in = base(); in.req = NULL; in.req_len = 0; in.record_ttl_s = 0xFFFFFFFFu;           /* isolate expiry from freshness */
  in.now = V_RECORD_EXPIRY;                                                            /* expiry > now is required */
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_EXPIRED, "EXPIRED", VK_EXPIRED, 0); CHECK(v.record_ok == 1);
  in.now = V_RECORD_EXPIRY + 1000;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_EXPIRED, "EXPIRED", VK_EXPIRED, 0);
  in.now = V_RECORD_EXPIRY - 1;
  vk_check_solana(&in, &v); EXPECT_OK(&v, VK_SEV_AMBER, VK_HL_NOT_PRESENT, "VERIFIED - NOT PRESENT", VK_SEL_HOLD);
  /* expiry is checked under FLOOR too */
  in.time_source = VK_TIME_FLOOR; in.now = V_RECORD_EXPIRY;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_EXPIRED, "EXPIRED", VK_EXPIRED, 0);
  /* expired wins over stale */
  in = base(); in.now = V_RECORD_EXPIRY + 1;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_EXPIRED, "EXPIRED", VK_EXPIRED, 0);
}

static void test_check_9_stale(void) {
  vk_check_input_t in; vk_verdict_t v;
  in = base(); in.req = NULL; in.req_len = 0;
  in.now = V_RECORD_ISSUED_AT + TTL + 1;                                               /* older than record_ttl_s */
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_STALE, "STALE RECORD", VK_EXPIRED, 0); CHECK(v.record_ok == 1);
  in.now = V_RECORD_ISSUED_AT + TTL;                                                   /* exactly the limit is fresh */
  vk_check_solana(&in, &v); EXPECT_OK(&v, VK_SEV_AMBER, VK_HL_NOT_PRESENT, "VERIFIED - NOT PRESENT", VK_SEL_HOLD);
  in.now = V_RECORD_ISSUED_AT;
  vk_check_solana(&in, &v); EXPECT_OK(&v, VK_SEV_AMBER, VK_HL_NOT_PRESENT, "VERIFIED - NOT PRESENT", VK_SEL_HOLD);
  /* issued in the future: up to 60 s of clock skew is accepted */
  in.now = V_RECORD_ISSUED_AT - 60;
  vk_check_solana(&in, &v); EXPECT_OK(&v, VK_SEV_AMBER, VK_HL_NOT_PRESENT, "VERIFIED - NOT PRESENT", VK_SEL_HOLD);
  in.now = V_RECORD_ISSUED_AT - 61;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_STALE, "STALE RECORD", VK_EXPIRED, 0);
  in.now = 5; in.record_ttl_s = 0xFFFFFFFFu;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_STALE, "STALE RECORD", VK_EXPIRED, 0);
  /* SNTP only: under FLOOR an old record is not stale (the clock was just set from a record) */
  in = base(); in.req = NULL; in.req_len = 0; in.now = V_RECORD_ISSUED_AT + TTL + 1; in.time_source = VK_TIME_FLOOR;
  vk_check_solana(&in, &v); EXPECT_OK(&v, VK_SEV_AMBER, VK_HL_CLOCK_UNSYNCED, "CLOCK UNSYNCED", VK_SEL_HOLD);
}

static void test_check_10_wrong_recipient(void) {
  vk_check_input_t in; vk_verdict_t v; size_t n;
  /* the app built a transfer to someone other than the record's account */
  in = base(); in.msg = msg; in.msg_len = make_msg(V_ALT_DST, V_MINT, 1000, 2);
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_WRONG_RECIPIENT, "WRONG RECIPIENT", VK_MISMATCH, 0);
  CHECK(v.decoded == 1 && v.record_ok == 1 && v.req_ok == 0 && p_calls == 0 && memcmp(v.record.solana_ata, V_DST, 32) == 0);
  /* paying the recipient's wallet address instead of the token account */
  in = base(); in.msg = msg; in.msg_len = make_msg(V_DST_OWNER, V_MINT, 1000, 2);
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_WRONG_RECIPIENT, "WRONG RECIPIENT", VK_MISMATCH, 0);
  /* the record has no solana_ata */
  in = base(); n = make_record(L_ATA, "solana_ata=", "", V_ISSUER_SEED); use_record(&in, n);
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_WRONG_RECIPIENT, "WRONG RECIPIENT", VK_MISMATCH, 0); CHECK(v.record_ok == 1 && v.record.has_solana == 0);
}

static void test_check_11_bad_request(void) {
  vk_check_input_t in; vk_verdict_t v; vk_req_t r; uint8_t copy[sizeof V_REQ], other_pub[32]; size_t n;
  /* does not parse */
  in = base(); in.req = (const uint8_t *)"not a request"; in.req_len = 13;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_BAD_REQUEST, "BAD REQUEST", VK_UNVERIFIED, 0);
  CHECK(v.record_ok == 1 && v.req_ok == 0 && p_calls == 0 && v.presence == VK_PRESENCE_NONE);
  in = base(); in.req_len = sizeof V_REQ - 1;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_BAD_REQUEST, "BAD REQUEST", VK_UNVERIFIED, 0);
  /* the signature does not verify: a flipped signature bit, an edited amount, an edited name */
  in = base(); memcpy(copy, V_REQ, sizeof V_REQ); copy[sizeof V_REQ - 1] ^= 1; in.req = copy;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_BAD_REQUEST, "BAD REQUEST", VK_UNVERIFIED, 0);
  in = base(); memcpy(copy, V_REQ, sizeof V_REQ); copy[37] ^= 1; in.req = copy;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_BAD_REQUEST, "BAD REQUEST", VK_UNVERIFIED, 0);
  in = base(); memcpy(copy, V_REQ, sizeof V_REQ); copy[62] = 'm'; in.req = copy;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_BAD_REQUEST, "BAD REQUEST", VK_UNVERIFIED, 0);
  /* signed without the pay-req prefix */
  in = base(); r = vector_req(); host_ed25519_sign(V_DEVICE_SEED, V_REQ, r.signed_len, r.sig);
  n = vk_req_build(&r, req, sizeof req); in.req = req; in.req_len = n;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_BAD_REQUEST, "BAD REQUEST", VK_UNVERIFIED, 0);
  /* an impostor's own, correctly self-signed request (its key is not the record's device key) */
  host_ed25519_keypair(V_ISSUER_SEED, other_pub);
  in = base(); r = vector_req(); memcpy(r.payee_pubkey, other_pub, 32); n = make_req(&r, V_ISSUER_SEED); in.req = req; in.req_len = n;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_BAD_REQUEST, "BAD REQUEST", VK_UNVERIFIED, 0);
  /* signed by the record's device key, but naming another payee */
  in = base(); r = vector_req(); memcpy(r.payee_pubkey, other_pub, 32); n = make_req(&r, V_DEVICE_SEED); in.req = req; in.req_len = n;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_BAD_REQUEST, "BAD REQUEST", VK_UNVERIFIED, 0);
  /* a bank-rail request */
  in = base(); r = vector_req(); r.rail = VK_RAIL_BANK; n = make_req(&r, V_DEVICE_SEED); in.req = req; in.req_len = n;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_BAD_REQUEST, "BAD REQUEST", VK_UNVERIFIED, 0);
  /* an expired request: expiry > now is required */
  in = base(); in.now = V_REQ_EXPIRY;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_BAD_REQUEST, "BAD REQUEST", VK_UNVERIFIED, 0);
  in = base(); in.now = V_REQ_EXPIRY - 1;
  vk_check_solana(&in, &v); EXPECT_GREEN(&v);
  /* the check does not depend on the requests feature: with no presence lookup a bad request is still red */
  in = base(); in.presence = NULL; in.req_len = sizeof V_REQ - 1;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_BAD_REQUEST, "BAD REQUEST", VK_UNVERIFIED, 0);
  /* a re-signed copy of the vector request is accepted (the helper itself is sound) */
  in = base(); r = vector_req(); n = make_req(&r, V_DEVICE_SEED); in.req = req; in.req_len = n;
  CHECK(n == sizeof V_REQ && memcmp(req, V_REQ, n) == 0);
  vk_check_solana(&in, &v); EXPECT_GREEN(&v);
}

static void test_check_12_wrong_amount(void) {
  vk_check_input_t in; vk_verdict_t v; vk_req_t r; size_t n;
  /* the transaction is for more than the request */
  in = base(); in.msg = msg; in.msg_len = make_msg(V_DST, V_MINT, 1001, 2);
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_WRONG_AMOUNT, "WRONG AMOUNT", VK_MISMATCH, 0);
  CHECK(v.req_ok == 1 && v.req.amount == 1000 && v.transfer.amount == 1001 && p_calls == 0);
  in = base(); in.msg = msg; in.msg_len = make_msg(V_DST, V_MINT, 999, 2);
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_WRONG_AMOUNT, "WRONG AMOUNT", VK_MISMATCH, 0);
  /* the request names another currency */
  in = base(); r = vector_req(); memset(r.currency, 0, sizeof r.currency); memcpy(r.currency, "USD", 3); n = make_req(&r, V_DEVICE_SEED); in.req = req; in.req_len = n;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_WRONG_AMOUNT, "WRONG AMOUNT", VK_MISMATCH, 0); CHECK(v.req_ok == 1);
  in = base(); r = vector_req(); memset(r.currency, 0, sizeof r.currency); memcpy(r.currency, "HAC", 3); n = make_req(&r, V_DEVICE_SEED); in.req = req; in.req_len = n;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_WRONG_AMOUNT, "WRONG AMOUNT", VK_MISMATCH, 0);
}

static void test_check_13_bad_proof(void) {
  vk_check_input_t in; vk_verdict_t v;
  /* the proof's signature was invalid */
  in = base(); p_result = VK_PRESENCE_BAD_SIG;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_BAD_PROOF, "BAD PROOF", VK_BAD_PROOF, 0);
  CHECK(v.presence == VK_PRESENCE_BAD_SIG && v.req_ok == 1 && p_calls == 1);
  /* a good proof, but checked against another key than this record's */
  in = base(); memcpy(p_payee, V_ISSUER_PUB, 32);
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_BAD_PROOF, "BAD PROOF", VK_BAD_PROOF, 0); CHECK(v.presence == VK_PRESENCE_PRESENT);
  in = base(); p_result = VK_PRESENCE_LATE; memcpy(p_payee, V_ISSUER_PUB, 32);
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_BAD_PROOF, "BAD PROOF", VK_BAD_PROOF, 0); CHECK(v.presence == VK_PRESENCE_LATE);
  /* NONE and PENDING pass this check whatever key the slot holds; they are amber below */
  in = base(); p_result = VK_PRESENCE_PENDING; memcpy(p_payee, V_ISSUER_PUB, 32);
  vk_check_solana(&in, &v); EXPECT_OK(&v, VK_SEV_AMBER, VK_HL_NOT_PRESENT, "VERIFIED - NOT PRESENT", VK_SEL_HOLD); CHECK(v.presence == VK_PRESENCE_PENDING);
}

/* ---- the verdict table when nothing failed ------------------------------------------------- */
static void test_amber_clock_unsynced(void) {
  vk_check_input_t in; vk_verdict_t v;
  /* row 1: FLOOR, even with a request and a PRESENT proof */
  in = base(); in.time_source = VK_TIME_FLOOR;
  vk_check_solana(&in, &v); EXPECT_OK(&v, VK_SEV_AMBER, VK_HL_CLOCK_UNSYNCED, "CLOCK UNSYNCED", VK_SEL_HOLD);
  CHECK(v.req_ok == 1 && v.presence == VK_PRESENCE_PRESENT);
  /* row 1 comes before row 2: FLOOR with no request is CLOCK UNSYNCED, not NOT PRESENT */
  in = base(); in.time_source = VK_TIME_FLOOR; in.req = NULL; in.req_len = 0;
  vk_check_solana(&in, &v); EXPECT_OK(&v, VK_SEV_AMBER, VK_HL_CLOCK_UNSYNCED, "CLOCK UNSYNCED", VK_SEL_HOLD);
  /* NONE cannot occur in the firmware; it is never better than FLOOR */
  in = base(); in.time_source = VK_TIME_NONE;
  vk_check_solana(&in, &v); EXPECT_OK(&v, VK_SEV_AMBER, VK_HL_CLOCK_UNSYNCED, "CLOCK UNSYNCED", VK_SEL_HOLD);
  /* failures still win under FLOOR */
  in = base(); in.time_source = VK_TIME_FLOOR; p_result = VK_PRESENCE_BAD_SIG;
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_BAD_PROOF, "BAD PROOF", VK_BAD_PROOF, 0);
}

static void test_amber_not_present(void) {
  vk_check_input_t in; vk_verdict_t v;
  /* no request supplied (a record-only payment, e.g. a shop inside a game) */
  in = base(); in.req = NULL; in.req_len = 0;
  vk_check_solana(&in, &v); EXPECT_OK(&v, VK_SEV_AMBER, VK_HL_NOT_PRESENT, "VERIFIED - NOT PRESENT", VK_SEL_HOLD);
  CHECK(v.req_ok == 0 && v.presence == VK_PRESENCE_NONE && p_calls == 0);
  /* presence NONE, PENDING, LATE */
  in = base(); p_result = VK_PRESENCE_NONE;
  vk_check_solana(&in, &v); EXPECT_OK(&v, VK_SEV_AMBER, VK_HL_NOT_PRESENT, "VERIFIED - NOT PRESENT", VK_SEL_HOLD); CHECK(v.presence == VK_PRESENCE_NONE && v.req_ok == 1 && p_calls == 1);
  in = base(); p_result = VK_PRESENCE_PENDING;
  vk_check_solana(&in, &v); EXPECT_OK(&v, VK_SEV_AMBER, VK_HL_NOT_PRESENT, "VERIFIED - NOT PRESENT", VK_SEL_HOLD); CHECK(v.presence == VK_PRESENCE_PENDING);
  in = base(); p_result = VK_PRESENCE_LATE;
  vk_check_solana(&in, &v); EXPECT_OK(&v, VK_SEV_AMBER, VK_HL_NOT_PRESENT, "VERIFIED - NOT PRESENT", VK_SEL_HOLD); CHECK(v.presence == VK_PRESENCE_LATE);
  /* the presence lookup is NULL (the requests feature is absent) */
  in = base(); in.presence = NULL;
  vk_check_solana(&in, &v); EXPECT_OK(&v, VK_SEV_AMBER, VK_HL_NOT_PRESENT, "VERIFIED - NOT PRESENT", VK_SEL_HOLD); CHECK(v.presence == VK_PRESENCE_NONE && v.req_ok == 1);
}

static void test_cap(void) {
  vk_check_input_t in; vk_verdict_t v; vk_req_t r; size_t n;
  /* green above the cap (100.00): SELECT becomes a hold, the colour stays */
  in = base(); r = vector_req(); r.amount = 10001; n = make_req(&r, V_DEVICE_SEED); in.req = req; in.req_len = n;
  in.msg = msg; in.msg_len = make_msg(V_DST, V_MINT, 10001, 2);
  vk_check_solana(&in, &v); EXPECT_OK(&v, VK_SEV_GREEN, VK_HL_VERIFIED_PRESENT, "VERIFIED - PRESENT", VK_SEL_HOLD);
  /* exactly the cap: still a single press */
  in = base(); r = vector_req(); r.amount = 10000; n = make_req(&r, V_DEVICE_SEED); in.req = req; in.req_len = n;
  in.msg = msg; in.msg_len = make_msg(V_DST, V_MINT, 10000, 2);
  vk_check_solana(&in, &v); EXPECT_GREEN(&v);
  /* cap 0 = no cap */
  TOKENS[0].cap = 0;
  in = base(); r = vector_req(); r.amount = 99999; n = make_req(&r, V_DEVICE_SEED); in.req = req; in.req_len = n;
  in.msg = msg; in.msg_len = make_msg(V_DST, V_MINT, 99999, 2);
  vk_check_solana(&in, &v); EXPECT_GREEN(&v);
  TOKENS[0].cap = 10000;
  /* amber is a hold above and below the cap */
  in = base(); in.req = NULL; in.req_len = 0; in.msg = msg; in.msg_len = make_msg(V_DST, V_MINT, 10001, 2);
  vk_check_solana(&in, &v); EXPECT_OK(&v, VK_SEV_AMBER, VK_HL_NOT_PRESENT, "VERIFIED - NOT PRESENT", VK_SEL_HOLD);
  in = base(); in.req = NULL; in.req_len = 0;
  vk_check_solana(&in, &v); EXPECT_OK(&v, VK_SEV_AMBER, VK_HL_NOT_PRESENT, "VERIFIED - NOT PRESENT", VK_SEL_HOLD);
  /* a red verdict stays disabled above the cap */
  in = base(); in.record = NULL; in.msg = msg; in.msg_len = make_msg(V_DST, V_MINT, 10001, 2);
  vk_check_solana(&in, &v); EXPECT_RED(&v, VK_HL_UNVERIFIED, "UNVERIFIED RECIPIENT", VK_UNVERIFIED, 1);
}

/* ---- texts and names ------------------------------------------------------------------------ */
static void test_headline_texts(void) {
  static const struct { vk_headline_t h; const char *text; } T[] = {
    {VK_HL_VERIFIED_PRESENT, "VERIFIED - PRESENT"}, {VK_HL_NOT_PRESENT, "VERIFIED - NOT PRESENT"}, {VK_HL_CLOCK_UNSYNCED, "CLOCK UNSYNCED"},
    {VK_HL_CANNOT_READ, "CANNOT READ PAYMENT"}, {VK_HL_UNKNOWN_TOKEN, "UNKNOWN TOKEN"}, {VK_HL_UNVERIFIED, "UNVERIFIED RECIPIENT"},
    {VK_HL_REVOKED, "REVOKED"}, {VK_HL_EXPIRED, "EXPIRED"}, {VK_HL_STALE, "STALE RECORD"}, {VK_HL_WRONG_RECIPIENT, "WRONG RECIPIENT"},
    {VK_HL_WRONG_AMOUNT, "WRONG AMOUNT"}, {VK_HL_BAD_REQUEST, "BAD REQUEST"}, {VK_HL_BAD_PROOF, "BAD PROOF"}, {VK_HL_OVER_LIMIT, "OVER LIMIT"}};
  size_t i;
  CHECK(sizeof T / sizeof T[0] == 14 && VK_HL_OVER_LIMIT == 13);
  for (i = 0; i < sizeof T / sizeof T[0]; i++) CHECK(strcmp(vk_headline_text(T[i].h), T[i].text) == 0);
  CHECK(strcmp(vk_headline_text((vk_headline_t)14), "?") == 0);
}

/* Every vk_reason_t has a distinct lower-case name, in the order of reasons.md. */
static void test_reason_names(void) {
  static const char *const NAMES[] = {"ok", "cancelled", "timeout", "undecodable", "unverified", "revoked", "expired", "mismatch",
    "bad_proof", "over_cap", "no_time", "busy", "denied", "not_provisioned", "too_long", "sign_failed", "bad_arg", "unsupported", "idle"};
  int i, j; const char *s;
  CHECK(sizeof NAMES / sizeof NAMES[0] == 19 && VK_REASON_COUNT == 19);
  CHECK(VK_OK == 0 && VK_CANCELLED == 1 && VK_TIMEOUT == 2 && VK_UNDECODABLE == 3 && VK_UNVERIFIED == 4 && VK_REVOKED == 5 && VK_EXPIRED == 6);
  CHECK(VK_MISMATCH == 7 && VK_BAD_PROOF == 8 && VK_OVER_CAP == 9 && VK_NO_TIME == 10 && VK_BUSY == 11 && VK_DENIED == 12 && VK_NOT_PROVISIONED == 13);
  CHECK(VK_TOO_LONG == 14 && VK_SIGN_FAILED == 15 && VK_BAD_ARG == 16 && VK_UNSUPPORTED == 17 && VK_IDLE == 18);
  for (i = 0; i <= VK_IDLE; i++) {
    const char *name = vk_reason_name((vk_reason_t)i);
    CHECK(strcmp(name, NAMES[i]) == 0 && name[0] != 0);
    for (s = name; *s; s++) CHECK((*s >= 'a' && *s <= 'z') || *s == '_');            /* lower-case */
    for (j = 0; j < i; j++) CHECK(strcmp(name, vk_reason_name((vk_reason_t)j)) != 0); /* distinct */
  }
  CHECK(strcmp(vk_reason_name((vk_reason_t)19), "?") == 0 && strcmp(vk_reason_name((vk_reason_t)-1), "?") == 0);
}

int main(void) {
  memset(TOKENS, 0, sizeof TOKENS);
  memcpy(TOKENS[0].mint, V_MINT, 32); TOKENS[0].decimals = 2; memcpy(TOKENS[0].symbol, "HACK", 5); TOKENS[0].cap = 10000; TOKENS[0].max = 100000;
  memcpy(TOKENS[1].mint, V_ALT_MINT, 32); TOKENS[1].decimals = 2; memcpy(TOKENS[1].symbol, "ALT", 4);
  CHECK(VK_MAX_TOKENS == 3);
  split_vector();

  test_green();
  test_check_1_cannot_read();
  test_check_2_unknown_token();
  test_check_3_over_limit();
  test_check_4_no_record();
  test_check_5_bad_record();
  test_check_6_revoked();
  test_check_8_expired();
  test_check_9_stale();
  test_check_10_wrong_recipient();
  test_check_11_bad_request();
  test_check_12_wrong_amount();
  test_check_13_bad_proof();
  test_amber_clock_unsynced();
  test_amber_not_present();
  test_cap();
  test_headline_texts();
  test_reason_names();
  printf(fails ? "%d FAILED\n" : "all checks tests passed\n", fails);
  return fails != 0;
}
