// src/native_apps/selftest/checks_cases.cpp
// The cases of the CHECKS suite. One honest payment (the host suite's test vectors: a transfer of
// 10.00 HACK, its issuer-signed record, the payee's signed request, a PRESENT proof, a synced clock)
// and one change to it per row, each with the verdict wallet/checks.md and reference/reasons.md give.
// The inputs never come from the badge's own config, key or clock: the cases are the same on every
// badge, provisioned or not. Adding a case is one entry in CASES.
#include "checks_cases.h"

#include <stdio.h>
#include <string.h>

#include "../../vk/wallet/pure/sol.h"
#include "../../vk/wallet/pure/vk_checks.h"
#include "../../vk/wallet/pure/vk_frames.h"
#include "../../vk/wallet/pure/vk_payment.h"
#include "checks_vectors.h"

namespace selftest::cases {
namespace {

constexpr uint32_t NOW = CV_RECORD_ISSUED_AT + 100u;   // 100 s after the record was issued; the request is open
constexpr uint32_t TTL = 3600u;

struct Fixture {
  vk_check_input_t in;
  vk_token_t token;
  uint8_t recordSig[64];
  uint8_t msg[SOL_TX_MSG_MAX];           // the vector transfer with the request's memo
};

vk_presence_t sPresence = VK_PRESENCE_PRESENT;

// The payer's presence table, as the requests feature would answer: the proof came from the payee.
vk_presence_t presence(const uint8_t req_id[8], uint8_t payee_out[32], uint8_t nonce_out[16]) {
  (void)req_id;
  if (sPresence != VK_PRESENCE_NONE) {
    memcpy(payee_out, CV_DEVICE_PUB, 32);
    memset(nonce_out, 0x5A, 16);
  }
  return sPresence;
}

void honest(Fixture &f, VerifyFn verify) {
  memset(&f, 0, sizeof f);
  memcpy(f.token.mint, CV_MINT, 32);
  f.token.decimals = 2;
  memcpy(f.token.symbol, "HACK", 5);
  f.token.cap = 10000;                   // 100.00
  f.token.max = 100000;                  // 1000.00
  memcpy(f.recordSig, CV_RECORD_SIG, 64);
  vk_check_input_t &in = f.in;
  // A payment that answers a request must carry the request id as its memo (checks.md, check 12a):
  // the vector transfer's own keys and amount, rebuilt with that memo.
  in.msg = CV_LEGACY;  in.msg_len = sizeof CV_LEGACY;
  sol_transfer_t t;
  vk_req_t req;
  if (sol_tx_decode_transfer(CV_LEGACY, sizeof CV_LEGACY, &t) == SOL_TX_OK && vk_req_parse(CV_REQ, sizeof CV_REQ, &req) == 0) {
    char memo[VK_REQ_MEMO_LEN + 1];
    vk_req_memo(req.req_id, memo);
    const size_t n = sol_tx_build_transfer(t.fee_payer, t.source, t.destination, t.mint, t.blockhash, t.amount, t.decimals,
                                           (const uint8_t *)memo, VK_REQ_MEMO_LEN, f.msg, sizeof f.msg);
    if (n != 0) { in.msg = f.msg;  in.msg_len = n; }
  }
  in.record = CV_RECORD;  in.record_len = sizeof CV_RECORD;  in.record_sig = f.recordSig;
  in.req = CV_REQ;  in.req_len = sizeof CV_REQ;
  in.own_pubkey = CV_PAYER;  in.issuer_key = CV_ISSUER_PUB;
  in.tokens = &f.token;  in.token_count = 1;
  in.now = NOW;  in.time_source = VK_TIME_SNTP;  in.record_ttl_s = TTL;
  in.verify = verify;
  in.presence = presence;
  sPresence = VK_PRESENCE_PRESENT;
}

// ---- the changes ------------------------------------------------------------------------------
void none(Fixture &) {}
void noRequest(Fixture &f) { f.in.req = nullptr; f.in.req_len = 0; }
void floorClock(Fixture &f) { f.in.time_source = VK_TIME_FLOOR; }
void versioned(Fixture &f) { f.in.msg = CV_V0; f.in.msg_len = sizeof CV_V0; }
void notThisBadge(Fixture &f) { f.in.own_pubkey = CV_DEVICE_PUB; }
void otherMint(Fixture &f) { f.in.msg = CV_ALT_LEGACY; f.in.msg_len = sizeof CV_ALT_LEGACY; }
void lowMax(Fixture &f) { f.token.max = 999; }                       // the payment is 1000
void noRecord(Fixture &f) { f.in.record = nullptr; f.in.record_len = 0; f.in.record_sig = nullptr; }
void forgedRecord(Fixture &f) { f.recordSig[0] ^= 0x01; }
void pastExpiry(Fixture &f) { f.in.now = CV_RECORD_EXPIRY + 1u; }
void oldRecord(Fixture &f) { f.in.now = CV_RECORD_ISSUED_AT + TTL + 1u; }
void badProof(Fixture &) { sPresence = VK_PRESENCE_BAD_SIG; }

struct Case {
  const char *name;
  const char *label;
  void (*change)(Fixture &);
  vk_severity_t severity;
  vk_headline_t headline;
  vk_reason_t reason;
  vk_select_t select;
  int devOverridable;
};

const Case CASES[] = {
    {"green", "GREEN", none, VK_SEV_GREEN, VK_HL_VERIFIED_PRESENT, VK_OK, VK_SEL_PRESS, 0},
    {"not_present", "NOT PRESENT", noRequest, VK_SEV_AMBER, VK_HL_NOT_PRESENT, VK_OK, VK_SEL_HOLD, 0},
    {"unsynced", "CLOCK UNSYNCED", floorClock, VK_SEV_AMBER, VK_HL_CLOCK_UNSYNCED, VK_OK, VK_SEL_HOLD, 0},
    {"cannot_read", "CANNOT READ", versioned, VK_SEV_RED, VK_HL_CANNOT_READ, VK_UNDECODABLE, VK_SEL_DISABLED, 0},
    {"not_mine", "NOT THIS BADGE", notThisBadge, VK_SEV_RED, VK_HL_CANNOT_READ, VK_UNDECODABLE, VK_SEL_DISABLED, 0},
    {"unknown_token", "UNKNOWN TOKEN", otherMint, VK_SEV_RED, VK_HL_UNKNOWN_TOKEN, VK_UNDECODABLE, VK_SEL_DISABLED, 0},
    {"over_limit", "OVER LIMIT", lowMax, VK_SEV_RED, VK_HL_OVER_LIMIT, VK_OVER_CAP, VK_SEL_DISABLED, 0},
    {"no_record", "NO RECORD", noRecord, VK_SEV_RED, VK_HL_UNVERIFIED, VK_UNVERIFIED, VK_SEL_DISABLED, 1},
    {"forged_record", "FORGED RECORD", forgedRecord, VK_SEV_RED, VK_HL_UNVERIFIED, VK_UNVERIFIED, VK_SEL_DISABLED, 0},
    {"expired", "EXPIRED", pastExpiry, VK_SEV_RED, VK_HL_EXPIRED, VK_EXPIRED, VK_SEL_DISABLED, 0},
    {"stale", "STALE RECORD", oldRecord, VK_SEV_RED, VK_HL_STALE, VK_EXPIRED, VK_SEL_DISABLED, 0},
    {"bad_proof", "BAD PROOF", badProof, VK_SEV_RED, VK_HL_BAD_PROOF, VK_BAD_PROOF, VK_SEL_DISABLED, 0},
};
constexpr size_t CASE_COUNT = sizeof CASES / sizeof CASES[0];

const char *severityName(vk_severity_t s) {
  switch (s) {
    case VK_SEV_GREEN: return "green";
    case VK_SEV_AMBER: return "amber";
    default:           return "red";
  }
}

}  // namespace

size_t count() { return CASE_COUNT; }
const char *name(size_t index) { return index < CASE_COUNT ? CASES[index].name : ""; }
const char *label(size_t index) { return index < CASE_COUNT ? CASES[index].label : ""; }

bool run(size_t index, VerifyFn verify, char *value, size_t cap) {
  if (index >= CASE_COUNT || verify == nullptr) {
    snprintf(value, cap, "no case");
    return false;
  }
  const Case &c = CASES[index];
  Fixture f;
  honest(f, verify);
  c.change(f);
  vk_verdict_t v;
  memset(&v, 0, sizeof v);
  vk_check_solana(&f.in, &v);
  sPresence = VK_PRESENCE_PRESENT;

  const bool same = v.severity == c.severity && v.headline == c.headline && v.reason == c.reason &&
                    v.select == c.select && (v.dev_overridable != 0) == (c.devOverridable != 0);
  if (same) {
    snprintf(value, cap, "%s %s", severityName(v.severity), vk_headline_text(v.headline));
  } else {
    snprintf(value, cap, "got %s %s %s", severityName(v.severity), vk_headline_text(v.headline), vk_reason_name(v.reason));
  }
  return same;
}

}  // namespace selftest::cases
