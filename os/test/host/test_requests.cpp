// LINK: src/vk/features/requests/requests.cpp src/vk/features/requests/presence.cpp
// test_requests - the payment-request tables driven with a fake clock, fake radio, fake config and
// real Ed25519: the payee's active requests and rate limits, the payer's request cache and
// presence slots. Spec: docs/os/protocol/espnow.md ("Payment requests", "Presence", "Request cache").
#include "../../src/vk/features/requests/presence.h"
#include "../../src/vk/features/requests/requests.h"

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "host_ed25519.h"
#include "vectors.h"

using namespace vk;
using requests::State;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// ---- fakes ------------------------------------------------------------------------------------
// Two badges are played by one process: the "device" of vectors.h is the payee, and a key made
// from PAYER_SEED is the payer. `own_key` and `own_seed` say which one the code under test is
// right now.

static const uint8_t PAYER_SEED[32] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
                                       7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
static uint8_t PAYER_PUB[32];

static const uint8_t MAC_PAYEE[6] = {0x02, 0xAA, 0, 0, 0, 0x01};
static const uint8_t MAC_PAYER[6] = {0x02, 0xBB, 0, 0, 0, 0x02};
static const uint8_t MAC_OTHER[6] = {0x02, 0xCC, 0, 0, 0, 0x03};
static const uint8_t MAC_SPOOF_BASE[6] = {0x02, 0xDD, 0, 0, 0, 0};   // an attacker's made-up MACs: [5] varies

static uint32_t t_ms = 0;
static uint32_t t_unix = 0;
static bool clock_ok = true;
static bool is_provisioned = true;
static const uint8_t *own_key = nullptr;
static const uint8_t *own_seed = nullptr;
static std::string running_app;
static bool pay_app_running = false;

static uint32_t cfg_ttl_s = 60, cfg_period_ms = 1000, cfg_max_proofs = 8, cfg_gap_ms = 200, cfg_presence_ms = 1500;

static uint8_t random_next = 1;
static int sign_calls = 0;
static bool sign_fails = false;
static int verify_calls = 0;

struct Sent {
  bool broadcast;
  uint8_t mac[6];
  std::vector<uint8_t> frame;
};
static std::vector<Sent> sent;
static bool send_ok = true;

struct Note { std::string title, body; };
static std::vector<Note> notes;

static uint32_t f_now_ms() { return t_ms; }
static bool f_clock_ok() { return clock_ok; }
static uint32_t f_unix() { return clock_ok ? t_unix : 0; }
static bool f_provisioned() { return is_provisioned; }
static const uint8_t *f_own_key() { return own_key; }
static uint32_t f_config(const char *key) {
  if (strcmp(key, "req_ttl_s") == 0) return cfg_ttl_s;
  if (strcmp(key, "req_period_ms") == 0) return cfg_period_ms;
  if (strcmp(key, "req_max_proofs") == 0) return cfg_max_proofs;
  if (strcmp(key, "req_gap_ms") == 0) return cfg_gap_ms;
  if (strcmp(key, "presence_ms") == 0) return cfg_presence_ms;
  CHECK(!"unexpected config key");
  return 0;
}
// Every call gives different bytes, so req_ids and nonces never repeat within a run.
static void f_random(uint8_t *out, size_t len) {
  for (size_t i = 0; i < len; i++) out[i] = (uint8_t)(random_next * 31 + i * 7 + 1);
  random_next++;
}
// What vk::wallet::signAuto does: the domain's validator, then prefix ‖ bytes signed with the key.
static wallet::Reason f_sign(const char *domain, const uint8_t *bytes, size_t len, uint8_t sig[64]) {
  sign_calls++;
  std::string prefix;
  if (strcmp(domain, "pay-req") == 0) {
    if (len == 0 || len > VK_REQ_SIGNED_MAX) return VK_TOO_LONG;
    if (!requests::validReqBytes(bytes, len, own_key)) return VK_BAD_ARG;
    prefix = VK_PREFIX_PAY_REQ;
  } else if (strcmp(domain, "pay-proof") == 0) {
    if (len == 0 || len > VK_PROOF_SIGNED_LEN) return VK_TOO_LONG;
    if (!requests::validProofBytes(bytes, len)) return VK_BAD_ARG;
    prefix = VK_PREFIX_PAY_PROOF;
  } else {
    return VK_UNSUPPORTED;
  }
  if (sign_fails || own_seed == nullptr) return VK_SIGN_FAILED;
  std::vector<uint8_t> message(prefix.begin(), prefix.end());
  message.insert(message.end(), bytes, bytes + len);
  host_ed25519_sign(own_seed, message.data(), message.size(), sig);
  return VK_OK;
}
static int f_verify(const uint8_t *msg, size_t len, const uint8_t *sig, const uint8_t *pubkey) {
  verify_calls++;
  return host_ed25519_verify(msg, len, sig, pubkey);
}
static bool f_send(const uint8_t *mac, const uint8_t *frame, size_t len) {
  Sent s;
  s.broadcast = mac == nullptr;
  memset(s.mac, 0, 6);
  if (mac) memcpy(s.mac, mac, 6);
  s.frame.assign(frame, frame + len);
  sent.push_back(s);
  return send_ok;
}
static bool f_app_running(const char *app_id) { return running_app == app_id; }
static int f_decimals(uint8_t rail, const char *currency) {
  if (rail == VK_RAIL_BANK) return 2;
  return strcmp(currency, "HACK") == 0 ? 2 : -1;
}
static bool f_pay_app_running() { return pay_app_running; }
static void f_notify(const char *title, const char *body) { notes.push_back({title, body}); }

static void bePayee() { own_key = V_DEVICE_PUB; own_seed = V_DEVICE_SEED; }
static void bePayer() { own_key = PAYER_PUB; own_seed = PAYER_SEED; }

// A clean slate: both tables empty, default config, the clock set, the badge provisioned.
static void fresh() {
  requests::reset();
  presence::reset();
  requests::hooks = {f_now_ms, f_clock_ok, f_unix, f_provisioned, f_own_key, f_config, f_random, f_sign,
                     f_verify, f_send, f_app_running, f_decimals, f_pay_app_running, f_notify};
  t_ms = 100000;
  t_unix = 1790000000u;
  clock_ok = true;
  is_provisioned = true;
  running_app = "request";
  pay_app_running = false;
  cfg_ttl_s = 60; cfg_period_ms = 1000; cfg_max_proofs = 8; cfg_gap_ms = 200; cfg_presence_ms = 1500;
  sign_calls = 0; sign_fails = false; verify_calls = 0;
  sent.clear(); send_ok = true;
  notes.clear();
  bePayee();
}

static requests::OpenArgs args(uint64_t amount = 1000, const char *app = "request") {
  requests::OpenArgs a;
  a.rail = VK_RAIL_SOLANA;
  a.amount = amount;
  a.currency = "HACK";
  a.name = "Demo Merchant";
  a.ttl_s = 0;
  a.app_id = app;
  return a;
}

// A REQ with the given id, signed by the vectors' device key (the payee) as its firmware would.
static std::vector<uint8_t> makeReq(uint8_t id, uint32_t expiry, const char *name = "Shop", uint64_t amount = 1000,
                                    const char *currency = "HACK", uint8_t rail = VK_RAIL_SOLANA) {
  vk_req_t r;
  memset(&r, 0, sizeof r);
  r.rail = rail;
  memcpy(r.payee_pubkey, V_DEVICE_PUB, 32);
  r.amount = amount;
  strncpy(r.currency, currency, 4);
  memset(r.req_id, id, 8);
  r.expiry = expiry;
  r.name_len = (uint8_t)strlen(name);
  memcpy(r.name, name, r.name_len);
  uint8_t out[VK_REQ_MAX_LEN];
  const size_t n = vk_req_build(&r, out, sizeof out);
  CHECK(n != 0);
  r.signed_len = n - 64;
  const char *prefix = VK_PREFIX_PAY_REQ;
  std::vector<uint8_t> message(prefix, prefix + strlen(prefix));
  message.insert(message.end(), out, out + r.signed_len);
  host_ed25519_sign(V_DEVICE_SEED, message.data(), message.size(), out + r.signed_len);
  return std::vector<uint8_t>(out, out + n);
}

static std::vector<uint8_t> makeChal(const uint8_t req_id[8], uint8_t nonceFill, const uint8_t payer[32]) {
  vk_chal_t c;
  memcpy(c.req_id, req_id, 8);
  memset(c.nonce, nonceFill, 16);
  memcpy(c.payer_pubkey, payer, 32);
  uint8_t out[VK_CHAL_LEN];
  CHECK(vk_chal_build(&c, out, sizeof out) == VK_CHAL_LEN);
  return std::vector<uint8_t>(out, out + VK_CHAL_LEN);
}

// A PROOF for (req_id, nonce, payer) signed with `seed`.
static std::vector<uint8_t> makeProof(const uint8_t req_id[8], const uint8_t nonce[16], const uint8_t payer[32],
                                      const uint8_t seed[32]) {
  uint8_t message[10 + VK_PROOF_SIGNED_LEN];
  memcpy(message, VK_PREFIX_PAY_PROOF, 10);
  vk_proof_signed_bytes(req_id, nonce, payer, message + 10);
  vk_proof_t p;
  memcpy(p.req_id, req_id, 8);
  host_ed25519_sign(seed, message, sizeof message, p.sig);
  uint8_t out[VK_PROOF_LEN];
  CHECK(vk_proof_build(&p, out, sizeof out) == VK_PROOF_LEN);
  return std::vector<uint8_t>(out, out + VK_PROOF_LEN);
}

static size_t countSent(int type) {
  size_t n = 0;
  for (const Sent &s : sent) n += vk_frame_type(s.frame.data(), s.frame.size()) == type ? 1 : 0;
  return n;
}

// The nonce of the last CHAL that went out.
static bool lastChal(vk_chal_t &out) {
  for (size_t i = sent.size(); i-- > 0;) {
    if (vk_chal_parse(sent[i].frame.data(), sent[i].frame.size(), &out) == 0) return true;
  }
  return false;
}

// ---- payee: opening ---------------------------------------------------------------------------

static void test_open_builds_a_signed_req() {
  fresh();
  requests::Opened o;
  CHECK(requests::canOpen() == VK_OK);
  CHECK(requests::openRequest(args(), o) == VK_OK);
  CHECK(sign_calls == 1);                                   // "sign it once"
  CHECK(requests::activeCount() == 1);
  CHECK(requests::isActive(o.req_id));
  CHECK(o.expiry == t_unix + cfg_ttl_s);

  vk_req_t r;
  CHECK(vk_req_parse(o.frame, o.frame_len, &r) == 0);
  CHECK(r.rail == VK_RAIL_SOLANA);
  CHECK(memcmp(r.payee_pubkey, V_DEVICE_PUB, 32) == 0);     // payee = this badge
  CHECK(r.amount == 1000);
  CHECK(strcmp(r.currency, "HACK") == 0);
  CHECK(memcmp(r.req_id, o.req_id, 8) == 0);
  CHECK(r.expiry == o.expiry);
  CHECK(strcmp(r.name, "Demo Merchant") == 0);
  CHECK(o.frame_len == r.signed_len + 64);

  // The signature is what check 11 of the check chain verifies: "pay-req:" ‖ frame[0..signed_len).
  static const char prefix[] = VK_PREFIX_PAY_REQ;
  std::vector<uint8_t> message(prefix, prefix + sizeof prefix - 1);
  message.insert(message.end(), o.frame, o.frame + r.signed_len);
  CHECK(host_ed25519_verify(message.data(), message.size(), r.sig, V_DEVICE_PUB) == 1);

  // An explicit lifetime, the bank rail, a different app.
  requests::OpenArgs a = args(250, "duel");
  a.rail = VK_RAIL_BANK; a.currency = "USD"; a.ttl_s = 15; a.name = "x";
  requests::Opened o2;
  CHECK(requests::openRequest(a, o2) == VK_OK);
  CHECK(o2.expiry == t_unix + 15);
  CHECK(vk_req_parse(o2.frame, o2.frame_len, &r) == 0);
  CHECK(r.rail == VK_RAIL_BANK && strcmp(r.currency, "USD") == 0 && r.amount == 250);
  CHECK(memcmp(o.req_id, o2.req_id, 8) != 0);
}

static void test_third_open_is_busy() {
  fresh();
  requests::Opened a, b, c;
  CHECK(requests::openRequest(args(), a) == VK_OK);
  CHECK(requests::openRequest(args(), b) == VK_OK);
  CHECK(requests::canOpen() == VK_BUSY);
  const int signsBefore = sign_calls;
  CHECK(requests::openRequest(args(), c) == VK_BUSY);
  CHECK(sign_calls == signsBefore);                         // refused before anything is signed
  CHECK(requests::activeCount() == 2);
  // Closing one makes room.
  CHECK(requests::closeRequest(a.req_id));
  CHECK(requests::openRequest(args(), c) == VK_OK);
  CHECK(requests::activeCount() == 2);
}

static void test_open_refusals() {
  fresh();
  requests::Opened o;

  is_provisioned = false;
  CHECK(requests::openRequest(args(), o) == VK_NOT_PROVISIONED);
  is_provisioned = true;

  clock_ok = false;                                         // the expiry is a real time
  CHECK(requests::canOpen() == VK_NO_TIME);
  CHECK(requests::openRequest(args(), o) == VK_NO_TIME);
  clock_ok = true;

  // not_provisioned is reported before no_time, and both before busy.
  is_provisioned = false; clock_ok = false;
  CHECK(requests::canOpen() == VK_NOT_PROVISIONED);
  is_provisioned = true; clock_ok = true;

  requests::OpenArgs a = args();
  a.amount = 0;
  CHECK(requests::openRequest(a, o) == VK_BAD_ARG);
  a = args(); a.rail = 3;
  CHECK(requests::openRequest(a, o) == VK_BAD_ARG);
  a = args(); a.currency = "";
  CHECK(requests::openRequest(a, o) == VK_BAD_ARG);
  a = args(); a.currency = "TOOLONG";
  CHECK(requests::openRequest(a, o) == VK_BAD_ARG);
  a = args(); a.currency = nullptr;
  CHECK(requests::openRequest(a, o) == VK_BAD_ARG);
  a = args(); a.name = "";
  CHECK(requests::openRequest(a, o) == VK_BAD_ARG);
  a = args(); a.name = "123456789012345678901234567890123";       // 33 characters
  CHECK(requests::openRequest(a, o) == VK_BAD_ARG);
  a = args(); a.name = "caf\xC3\xA9";                              // not printable ASCII
  CHECK(requests::openRequest(a, o) == VK_BAD_ARG);
  a = args(); a.app_id = "123456789012345678901234567890123";     // 33 characters
  CHECK(requests::openRequest(a, o) == VK_BAD_ARG);
  a = args(); a.ttl_s = UINT32_MAX;                               // the expiry would wrap
  CHECK(requests::openRequest(a, o) == VK_BAD_ARG);
  CHECK(sign_calls == 0);
  CHECK(requests::activeCount() == 0);

  sign_fails = true;
  CHECK(requests::openRequest(args(), o) == VK_SIGN_FAILED);
  sign_fails = false;
  own_key = nullptr;                                        // no identity
  CHECK(requests::openRequest(args(), o) == VK_SIGN_FAILED);
  bePayee();
  CHECK(requests::activeCount() == 0);
  CHECK(sent.empty());
}

// ---- payee: closing ---------------------------------------------------------------------------

static void test_expiry_closes() {
  fresh();
  requests::Opened o;
  CHECK(requests::openRequest(args(), o) == VK_OK);
  State st; uint32_t proofs = 99;
  CHECK(requests::requestStatus(o.req_id, st, proofs) && st == State::Open && proofs == 0);

  t_unix = o.expiry - 1;
  requests::update();
  CHECK(requests::isActive(o.req_id));
  t_unix = o.expiry;
  sent.clear();
  requests::update();
  CHECK(!requests::isActive(o.req_id));
  CHECK(requests::activeCount() == 0);
  CHECK(countSent(VK_T_REQ) == 0);                          // an expired request is not broadcast again
  CHECK(requests::requestStatus(o.req_id, st, proofs) && st == State::Expired);
  CHECK(strcmp(requests::stateName(st), "expired") == 0);
  CHECK(!requests::closeRequest(o.req_id));                 // nothing left to close

  // A CHAL that arrives after the expiry, before any service pass, is not answered either.
  CHECK(requests::openRequest(args(), o) == VK_OK);
  t_unix = o.expiry + 5;
  sent.clear();
  const int signsBefore = sign_calls;
  CHECK(requests::onChal(MAC_PAYER, makeChal(o.req_id, 1, PAYER_PUB).data(), VK_CHAL_LEN));
  CHECK(sign_calls == signsBefore && sent.empty());
  // And the slot it held is free again for canOpen().
  CHECK(requests::canOpen() == VK_OK);
}

static void test_close_and_status() {
  fresh();
  requests::Opened o;
  CHECK(requests::openRequest(args(), o) == VK_OK);
  State st; uint32_t proofs = 0;
  CHECK(requests::closeRequest(o.req_id));
  CHECK(!requests::closeRequest(o.req_id));
  CHECK(requests::requestStatus(o.req_id, st, proofs) && st == State::Closed);
  CHECK(strcmp(requests::stateName(State::Open), "open") == 0);
  CHECK(strcmp(requests::stateName(State::Closed), "closed") == 0);

  const uint8_t unknown[8] = {9, 9, 9, 9, 9, 9, 9, 9};
  CHECK(!requests::requestStatus(unknown, st, proofs));
  CHECK(!requests::closeRequest(unknown));
  CHECK(!requests::isActive(unknown));

  // Only the last few closed requests are remembered; the first one is eventually forgotten.
  for (int i = 0; i < 6; i++) {
    requests::Opened x;
    CHECK(requests::openRequest(args(), x) == VK_OK);
    CHECK(requests::closeRequest(x.req_id));
    CHECK(requests::requestStatus(x.req_id, st, proofs) && st == State::Closed);
  }
  CHECK(!requests::requestStatus(o.req_id, st, proofs));
}

static void test_app_stop_closes_only_that_apps_requests() {
  fresh();
  requests::Opened a, b;
  CHECK(requests::openRequest(args(1000, "request"), a) == VK_OK);
  CHECK(requests::openRequest(args(2000, "duel"), b) == VK_OK);
  requests::closeForApp("other");
  CHECK(requests::activeCount() == 2);
  requests::closeForApp("");                                // upstream calls stop() with no app running
  CHECK(requests::activeCount() == 2);
  requests::closeForApp("request");
  CHECK(!requests::isActive(a.req_id));
  CHECK(requests::isActive(b.req_id));
  State st; uint32_t proofs;
  CHECK(requests::requestStatus(a.req_id, st, proofs) && st == State::Closed);
  CHECK(requests::requestStatus(b.req_id, st, proofs) && st == State::Open);
  requests::closeForApp("duel");
  CHECK(requests::activeCount() == 0);

  // The service closes a request whose app is not the running one (one opened from on_stop).
  running_app = "request";
  CHECK(requests::openRequest(args(1000, "request"), a) == VK_OK);
  requests::update();
  CHECK(requests::isActive(a.req_id));
  running_app = "";
  sent.clear();
  requests::update();
  CHECK(!requests::isActive(a.req_id));
  CHECK(sent.empty());
}

// ---- payee: rebroadcast -------------------------------------------------------------------------

static void test_rebroadcast_every_period() {
  fresh();
  requests::Opened o;
  CHECK(requests::openRequest(args(), o) == VK_OK);
  CHECK(sent.empty());                                      // nothing leaves from inside the Lua call
  requests::update();
  CHECK(sent.size() == 1 && sent[0].broadcast);
  CHECK(sent[0].frame.size() == o.frame_len && memcmp(sent[0].frame.data(), o.frame, o.frame_len) == 0);
  t_ms += 999;
  requests::update();
  CHECK(sent.size() == 1);
  t_ms += 1;
  requests::update();
  CHECK(sent.size() == 2);
  CHECK(sent[1].frame == sent[0].frame);                    // "the same bytes every time"
  cfg_period_ms = 250;
  t_ms += 250;
  requests::update();
  CHECK(sent.size() == 3);

  // Two requests are each broadcast; a closed one stops.
  requests::Opened o2;
  CHECK(requests::openRequest(args(77), o2) == VK_OK);
  t_ms += 250;
  sent.clear();
  requests::update();
  CHECK(sent.size() == 2);
  CHECK(requests::closeRequest(o.req_id));
  t_ms += 250;
  sent.clear();
  requests::update();
  CHECK(sent.size() == 1);
  CHECK(sent[0].frame.size() == o2.frame_len && memcmp(sent[0].frame.data(), o2.frame, o2.frame_len) == 0);

  // millis() wrapping does not stall the broadcast.
  t_ms = 0xFFFFFF00u;
  requests::update();
  sent.clear();
  t_ms += 0x200;                                            // wraps past zero
  requests::update();
  CHECK(sent.size() == 1);
}

// ---- payee: answering CHAL --------------------------------------------------------------------

static void test_chal_for_unknown_request_is_dropped() {
  fresh();
  requests::Opened o;
  CHECK(requests::openRequest(args(), o) == VK_OK);
  const int signsBefore = sign_calls;
  const uint8_t unknown[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  CHECK(requests::onChal(MAC_PAYER, makeChal(unknown, 1, PAYER_PUB).data(), VK_CHAL_LEN));
  CHECK(sign_calls == signsBefore);
  CHECK(sent.empty());

  // A malformed CHAL (one byte short) and a frame of another type are dropped too.
  std::vector<uint8_t> chal = makeChal(o.req_id, 1, PAYER_PUB);
  CHECK(requests::onChal(MAC_PAYER, chal.data(), chal.size() - 1));
  CHECK(requests::onChal(MAC_PAYER, o.frame, o.frame_len));
  CHECK(sign_calls == signsBefore && sent.empty());

  // A request that was closed no longer answers.
  CHECK(requests::closeRequest(o.req_id));
  CHECK(requests::onChal(MAC_PAYER, chal.data(), chal.size()));
  CHECK(sign_calls == signsBefore && sent.empty());
}

static void test_proof_verifies_with_the_payee_key() {
  fresh();
  requests::Opened o;
  CHECK(requests::openRequest(args(), o) == VK_OK);
  uint8_t nonce[16];
  memset(nonce, 0x5A, sizeof nonce);
  CHECK(requests::onChal(MAC_PAYER, makeChal(o.req_id, 0x5A, PAYER_PUB).data(), VK_CHAL_LEN));
  CHECK(sent.size() == 1);
  CHECK(!sent[0].broadcast && memcmp(sent[0].mac, MAC_PAYER, 6) == 0);     // unicast to the challenger

  vk_proof_t p;
  CHECK(vk_proof_parse(sent[0].frame.data(), sent[0].frame.size(), &p) == 0);
  CHECK(memcmp(p.req_id, o.req_id, 8) == 0);
  uint8_t message[10 + VK_PROOF_SIGNED_LEN];
  memcpy(message, VK_PREFIX_PAY_PROOF, 10);
  vk_proof_signed_bytes(o.req_id, nonce, PAYER_PUB, message + 10);
  CHECK(host_ed25519_verify(message, sizeof message, p.sig, V_DEVICE_PUB) == 1);
  // Not valid for another payer or another nonce.
  vk_proof_signed_bytes(o.req_id, nonce, V_ISSUER_PUB, message + 10);
  CHECK(host_ed25519_verify(message, sizeof message, p.sig, V_DEVICE_PUB) == 0);

  State st; uint32_t proofs = 0;
  CHECK(requests::requestStatus(o.req_id, st, proofs) && proofs == 1);
}

// The PROOF frame sent to `mac` last, parsed; false if none.
static bool lastProofTo(const uint8_t mac[6], vk_proof_t &out) {
  for (size_t i = sent.size(); i-- > 0;) {
    if (memcmp(sent[i].mac, mac, 6) == 0 && vk_proof_parse(sent[i].frame.data(), sent[i].frame.size(), &out) == 0) return true;
  }
  return false;
}

static bool proofSignsNonce(const vk_proof_t &p, uint8_t nonceFill, const uint8_t payer[32]) {
  uint8_t nonce[16];
  memset(nonce, nonceFill, sizeof nonce);
  uint8_t message[10 + VK_PROOF_SIGNED_LEN];
  memcpy(message, VK_PREFIX_PAY_PROOF, 10);
  vk_proof_signed_bytes(p.req_id, nonce, payer, message + 10);
  return host_ed25519_verify(message, sizeof message, p.sig, V_DEVICE_PUB) == 1;
}

static void test_proof_rate_limits() {
  State st; uint32_t proofs = 0;

  // req_max_proofs answers per challenger MAC per request (T-REQ4: one badge, nine CHALs, three answers).
  fresh();
  cfg_max_proofs = 3;
  requests::Opened o, o2;
  CHECK(requests::openRequest(args(), o) == VK_OK);
  for (uint8_t i = 0; i < 9; i++) {
    t_ms += 1500;                                           // beyond the gap and the bucket
    CHECK(requests::onChal(MAC_PAYER, makeChal(o.req_id, i, PAYER_PUB).data(), VK_CHAL_LEN));
  }
  CHECK(countSent(VK_T_PROOF) == 3);
  CHECK(requests::stats().chal_over_budget == 6);
  const int signsBefore = sign_calls;
  t_ms += 1500;
  CHECK(requests::onChal(MAC_PAYER, makeChal(o.req_id, 0x77, PAYER_PUB).data(), VK_CHAL_LEN));
  requests::update();
  CHECK(sign_calls == signsBefore);                         // refused before anything is signed
  // Another MAC has its own budget, and so does the same MAC on another request.
  CHECK(requests::onChal(MAC_OTHER, makeChal(o.req_id, 0x50, PAYER_PUB).data(), VK_CHAL_LEN));
  CHECK(countSent(VK_T_PROOF) == 4);
  CHECK(requests::requestStatus(o.req_id, st, proofs) && proofs == 4);   // all challengers together
  CHECK(requests::openRequest(args(), o2) == VK_OK);
  t_ms += 1500;
  CHECK(requests::onChal(MAC_PAYER, makeChal(o2.req_id, 0x51, PAYER_PUB).data(), VK_CHAL_LEN));
  CHECK(countSent(VK_T_PROOF) == 5);

  // Only CHALLENGERS_PER_REQUEST MACs are counted; the least recent is forgotten (and so starts afresh).
  fresh();
  cfg_max_proofs = 1;
  CHECK(requests::openRequest(args(), o) == VK_OK);
  uint8_t mac[6];
  memcpy(mac, MAC_SPOOF_BASE, 6);
  for (uint8_t i = 0; i <= requests::CHALLENGERS_PER_REQUEST; i++) {
    t_ms += 1500;
    mac[5] = i;
    CHECK(requests::onChal(mac, makeChal(o.req_id, i, PAYER_PUB).data(), VK_CHAL_LEN));
  }
  CHECK(countSent(VK_T_PROOF) == requests::CHALLENGERS_PER_REQUEST + 1);
  t_ms += 1500;
  mac[5] = 0;                                               // forgotten: answered again
  CHECK(requests::onChal(mac, makeChal(o.req_id, 0x60, PAYER_PUB).data(), VK_CHAL_LEN));
  t_ms += 1500;
  mac[5] = (uint8_t)requests::CHALLENGERS_PER_REQUEST;      // still counted: refused
  CHECK(requests::onChal(mac, makeChal(o.req_id, 0x61, PAYER_PUB).data(), VK_CHAL_LEN));
  CHECK(countSent(VK_T_PROOF) == requests::CHALLENGERS_PER_REQUEST + 2);

  // req_gap_ms is global: measured from the end of the last signature, whatever request and MAC.
  // A CHAL that arrives inside the gap waits in the queue and is answered by the service.
  fresh();
  CHECK(requests::openRequest(args(), o) == VK_OK);
  CHECK(requests::openRequest(args(), o2) == VK_OK);
  CHECK(requests::onChal(MAC_PAYER, makeChal(o.req_id, 1, PAYER_PUB).data(), VK_CHAL_LEN));
  CHECK(countSent(VK_T_PROOF) == 1);
  t_ms += 199;
  CHECK(requests::onChal(MAC_OTHER, makeChal(o2.req_id, 2, PAYER_PUB).data(), VK_CHAL_LEN));
  requests::update();
  CHECK(countSent(VK_T_PROOF) == 1);
  t_ms += 1;
  requests::update();
  CHECK(countSent(VK_T_PROOF) == 2);
  vk_proof_t p;
  CHECK(lastProofTo(MAC_OTHER, p) && memcmp(p.req_id, o2.req_id, 8) == 0 && proofSignsNonce(p, 2, PAYER_PUB));

  // The bucket: ANSWER_BURST answers back to back, then one per ANSWER_REFILL_MS however many ask.
  fresh();
  cfg_gap_ms = 0;
  CHECK(requests::openRequest(args(), o) == VK_OK);
  for (uint8_t i = 0; i < 6; i++) {
    mac[5] = i;
    CHECK(requests::onChal(mac, makeChal(o.req_id, i, PAYER_PUB).data(), VK_CHAL_LEN));
  }
  CHECK(countSent(VK_T_PROOF) == requests::ANSWER_BURST);
  t_ms += requests::ANSWER_REFILL_MS - 1;
  requests::update();
  CHECK(countSent(VK_T_PROOF) == requests::ANSWER_BURST);
  t_ms += 1;
  requests::update();
  requests::update();
  CHECK(countSent(VK_T_PROOF) == requests::ANSWER_BURST + 1);
  // A long quiet period refills the bucket to ANSWER_BURST, not more.
  t_ms += 100 * requests::ANSWER_REFILL_MS;
  for (uint8_t i = 10; i < 16; i++) {
    mac[5] = i;
    CHECK(requests::onChal(mac, makeChal(o.req_id, i, PAYER_PUB).data(), VK_CHAL_LEN));
  }
  CHECK(countSent(VK_T_PROOF) == 2 * requests::ANSWER_BURST + 1);

  // A signature that fails is not counted as a proof, but it starts the gap.
  fresh();
  CHECK(requests::openRequest(args(), o) == VK_OK);
  sign_fails = true;
  CHECK(requests::onChal(MAC_PAYER, makeChal(o.req_id, 4, PAYER_PUB).data(), VK_CHAL_LEN));
  sign_fails = false;
  CHECK(sent.empty());
  CHECK(requests::requestStatus(o.req_id, st, proofs) && proofs == 0);
  CHECK(requests::onChal(MAC_PAYER, makeChal(o.req_id, 5, PAYER_PUB).data(), VK_CHAL_LEN));   // inside the gap
  CHECK(sent.empty());
  t_ms += 200;
  requests::update();
  CHECK(countSent(VK_T_PROOF) == 1);
}

static void test_chal_queue() {
  requests::Opened o, o2;
  vk_proof_t p;
  uint8_t mac[6];
  memcpy(mac, MAC_SPOOF_BASE, 6);

  // Newest first: of two CHALs that waited, the later one is answered first.
  fresh();
  CHECK(requests::openRequest(args(), o) == VK_OK);
  CHECK(requests::onChal(MAC_OTHER, makeChal(o.req_id, 1, V_ISSUER_PUB).data(), VK_CHAL_LEN));
  t_ms += 10;
  mac[5] = 1;
  CHECK(requests::onChal(mac, makeChal(o.req_id, 2, V_ISSUER_PUB).data(), VK_CHAL_LEN));
  t_ms += 10;
  CHECK(requests::onChal(MAC_PAYER, makeChal(o.req_id, 3, PAYER_PUB).data(), VK_CHAL_LEN));
  t_ms += 200;
  sent.clear();
  requests::update();
  CHECK(countSent(VK_T_PROOF) == 1 && lastProofTo(MAC_PAYER, p));
  t_ms += 200;
  sent.clear();
  requests::update();
  CHECK(countSent(VK_T_PROOF) == 1 && lastProofTo(mac, p));

  // A newer CHAL from the same MAC for the same request replaces the queued one.
  fresh();
  CHECK(requests::openRequest(args(), o) == VK_OK);
  CHECK(requests::onChal(MAC_OTHER, makeChal(o.req_id, 1, V_ISSUER_PUB).data(), VK_CHAL_LEN));
  t_ms += 10;
  CHECK(requests::onChal(MAC_PAYER, makeChal(o.req_id, 0x21, PAYER_PUB).data(), VK_CHAL_LEN));
  t_ms += 10;
  CHECK(requests::onChal(MAC_PAYER, makeChal(o.req_id, 0x22, PAYER_PUB).data(), VK_CHAL_LEN));
  t_ms += 200;
  sent.clear();
  requests::update();
  t_ms += 200;
  requests::update();
  CHECK(countSent(VK_T_PROOF) == 1);                        // one answer for MAC_PAYER, for the newer nonce
  CHECK(lastProofTo(MAC_PAYER, p) && proofSignsNonce(p, 0x22, PAYER_PUB));

  // A full queue drops its oldest entry for a newer CHAL.
  fresh();
  CHECK(requests::openRequest(args(), o) == VK_OK);
  CHECK(requests::onChal(MAC_OTHER, makeChal(o.req_id, 1, V_ISSUER_PUB).data(), VK_CHAL_LEN));   // answered
  for (uint8_t i = 0; i <= requests::CHAL_QUEUE_LEN; i++) {
    t_ms += 1;
    mac[5] = i;
    CHECK(requests::onChal(mac, makeChal(o.req_id, (uint8_t)(0x30 + i), V_ISSUER_PUB).data(), VK_CHAL_LEN));
  }
  CHECK(requests::stats().chal_displaced == 1);

  // A CHAL that waited longer than presence_ms is dropped unanswered: the payer would judge it late.
  fresh();
  CHECK(requests::openRequest(args(), o) == VK_OK);
  CHECK(requests::onChal(MAC_OTHER, makeChal(o.req_id, 1, V_ISSUER_PUB).data(), VK_CHAL_LEN));
  CHECK(requests::onChal(MAC_PAYER, makeChal(o.req_id, 2, PAYER_PUB).data(), VK_CHAL_LEN, t_ms - 1400));
  t_ms += 101;                                              // the CHAL is 1501 ms old: dropped before the rate is asked
  sent.clear();
  requests::update();
  CHECK(countSent(VK_T_PROOF) == 0);
  CHECK(requests::stats().chal_stale == 1);

  // Closing a request drops what is queued for it.
  fresh();
  CHECK(requests::openRequest(args(), o) == VK_OK);
  CHECK(requests::openRequest(args(), o2) == VK_OK);
  CHECK(requests::onChal(MAC_OTHER, makeChal(o2.req_id, 1, V_ISSUER_PUB).data(), VK_CHAL_LEN));
  CHECK(requests::onChal(MAC_PAYER, makeChal(o.req_id, 2, PAYER_PUB).data(), VK_CHAL_LEN));
  CHECK(requests::closeRequest(o.req_id));
  t_ms += 200;
  sent.clear();
  const int signsBefore = sign_calls;
  requests::update();
  CHECK(sign_calls == signsBefore && countSent(VK_T_PROOF) == 0);
}

// ---- the validators of the two signing domains ------------------------------------------------

static void test_validators() {
  fresh();
  // pay-req: the vector REQ up to the end of its name, with the payee's own key.
  CHECK(requests::validReqBytes(V_REQ, V_REQ_SIGNED_LEN, V_DEVICE_PUB));
  CHECK(!requests::validReqBytes(V_REQ, V_REQ_SIGNED_LEN, PAYER_PUB));     // someone else's request
  CHECK(!requests::validReqBytes(V_REQ, V_REQ_SIGNED_LEN, nullptr));
  CHECK(!requests::validReqBytes(nullptr, V_REQ_SIGNED_LEN, V_DEVICE_PUB));
  CHECK(!requests::validReqBytes(V_REQ, V_REQ_SIGNED_LEN - 1, V_DEVICE_PUB));   // cut inside the name
  CHECK(!requests::validReqBytes(V_REQ, V_REQ_SIGNED_LEN + 1, V_DEVICE_PUB));
  CHECK(!requests::validReqBytes(V_REQ, sizeof V_REQ, V_DEVICE_PUB));      // the whole frame is not the signed part
  CHECK(!requests::validReqBytes(V_REQ, 0, V_DEVICE_PUB));
  uint8_t copy[VK_REQ_SIGNED_MAX];
  memcpy(copy, V_REQ, V_REQ_SIGNED_LEN);
  copy[3] = VK_T_CHAL;                                      // not a REQ header
  CHECK(!requests::validReqBytes(copy, V_REQ_SIGNED_LEN, V_DEVICE_PUB));
  memcpy(copy, V_REQ, V_REQ_SIGNED_LEN);
  copy[4] = 9;                                              // unknown rail
  CHECK(!requests::validReqBytes(copy, V_REQ_SIGNED_LEN, V_DEVICE_PUB));
  memcpy(copy, V_REQ, V_REQ_SIGNED_LEN);
  copy[0] = 'r';                                            // text that is not a frame
  CHECK(!requests::validReqBytes(copy, V_REQ_SIGNED_LEN, V_DEVICE_PUB));

  // pay-proof: exactly 56 bytes whose req_id is an active request.
  requests::Opened o;
  CHECK(requests::openRequest(args(), o) == VK_OK);
  uint8_t bytes[VK_PROOF_SIGNED_LEN + 1];
  memset(bytes, 0x11, sizeof bytes);
  memcpy(bytes, o.req_id, 8);
  CHECK(requests::validProofBytes(bytes, VK_PROOF_SIGNED_LEN));
  CHECK(!requests::validProofBytes(bytes, VK_PROOF_SIGNED_LEN - 1));
  CHECK(!requests::validProofBytes(bytes, VK_PROOF_SIGNED_LEN + 1));
  CHECK(!requests::validProofBytes(nullptr, VK_PROOF_SIGNED_LEN));
  bytes[0] ^= 1;                                            // not one of ours
  CHECK(!requests::validProofBytes(bytes, VK_PROOF_SIGNED_LEN));
  bytes[0] ^= 1;
  CHECK(requests::closeRequest(o.req_id));
  CHECK(!requests::validProofBytes(bytes, VK_PROOF_SIGNED_LEN));          // closed: no longer signable
}

// ---- payer: the request cache -----------------------------------------------------------------

static void test_cache_keeps_eight_distinct() {
  fresh();
  bePayer();
  const uint32_t expiry = t_unix + 600;
  for (uint8_t id = 1; id <= 8; id++) {
    const std::vector<uint8_t> f = makeReq(id, expiry);
    CHECK(!requests::onReq(MAC_PAYEE, f.data(), f.size(), -40, t_ms));     // false: the app gets it too
    t_ms += requests::CACHE_VERIFY_GAP_MS;                                  // one signature check per gap
  }
  CHECK(requests::cacheCount() == 8);
  CHECK(notes.size() == 8);

  // The same request heard again is not a new entry, and is not announced again. Heard from
  // another MAC while its first MAC is still sending it, the entry keeps the first MAC.
  const std::vector<uint8_t> again = makeReq(3, expiry);
  const int verifiesBefore = verify_calls;
  CHECK(!requests::onReq(MAC_OTHER, again.data(), again.size(), -70, t_ms));
  CHECK(verify_calls == verifiesBefore);                    // the same bytes: nothing to check again
  CHECK(requests::cacheCount() == 8);
  CHECK(notes.size() == 8);
  auto entry = [](uint8_t id) -> const requests::Cached * {
    for (size_t i = 0; i < requests::cacheCount(); i++) {
      if (requests::cacheAt(i)->req.req_id[0] == id) return requests::cacheAt(i);
    }
    return nullptr;
  };
  const requests::Cached *c = entry(3);
  CHECK(c != nullptr);
  CHECK(memcmp(c->mac, MAC_PAYEE, 6) == 0);
  CHECK(c->rssi == -40);                                    // as heard from that MAC
  CHECK(c->heard_ms == t_ms);                               // heard, from anyone
  CHECK(c->frame_len == again.size() && memcmp(c->frame, again.data(), again.size()) == 0);
  CHECK(c->req.amount == 1000 && strcmp(c->req.name, "Shop") == 0);
  CHECK(requests::cacheAt(8) == nullptr);
  // Once the first MAC has been silent for CACHE_MAC_HOLD_MS, the other MAC takes the entry.
  const uint32_t firstHeard = c->mac_heard_ms;
  t_ms = firstHeard + requests::CACHE_MAC_HOLD_MS - 1;
  CHECK(!requests::onReq(MAC_OTHER, again.data(), again.size(), -70, t_ms));
  CHECK(memcmp(entry(3)->mac, MAC_PAYEE, 6) == 0);
  t_ms = firstHeard + requests::CACHE_MAC_HOLD_MS;
  CHECK(!requests::onReq(MAC_OTHER, again.data(), again.size(), -70, t_ms));
  CHECK(memcmp(entry(3)->mac, MAC_OTHER, 6) == 0 && entry(3)->rssi == -70);
  CHECK(entry(3)->mac_heard_ms == t_ms);

  // A ninth distinct request replaces the one that has been silent longest (id 1).
  t_ms += requests::CACHE_VERIFY_GAP_MS;
  const std::vector<uint8_t> ninth = makeReq(9, expiry);
  CHECK(!requests::onReq(MAC_PAYEE, ninth.data(), ninth.size(), -40, t_ms));
  CHECK(requests::cacheCount() == 8);
  bool has1 = false, has9 = false, has2 = false;
  for (size_t i = 0; i < requests::cacheCount(); i++) {
    const uint8_t id = requests::cacheAt(i)->req.req_id[0];
    has1 |= id == 1; has2 |= id == 2; has9 |= id == 9;
  }
  CHECK(!has1 && has2 && has9);

  // A frame that is not a well-formed REQ is never cached.
  std::vector<uint8_t> bad = makeReq(10, expiry);
  bad.pop_back();
  CHECK(!requests::onReq(MAC_PAYEE, bad.data(), bad.size(), -40, t_ms));
  bad = makeReq(10, expiry);
  bad[61] = 0;                                              // name_len 0
  CHECK(!requests::onReq(MAC_PAYEE, bad.data(), bad.size(), -40, t_ms));
  CHECK(requests::cacheCount() == 8);

  // A new request heard within CACHE_VERIFY_GAP_MS of the last check is left for its next broadcast.
  t_ms += requests::CACHE_VERIFY_GAP_MS;
  const std::vector<uint8_t> f11 = makeReq(11, expiry), f12 = makeReq(12, expiry);
  CHECK(!requests::onReq(MAC_PAYEE, f11.data(), f11.size(), -40, t_ms));
  CHECK(!requests::onReq(MAC_PAYEE, f12.data(), f12.size(), -40, t_ms + 1));
  CHECK(entry(11) != nullptr && entry(12) == nullptr);
  CHECK(requests::stats().req_deferred == 1);
  t_ms += requests::CACHE_VERIFY_GAP_MS;
  CHECK(!requests::onReq(MAC_PAYEE, f12.data(), f12.size(), -40, t_ms));
  CHECK(entry(12) != nullptr);
}

static void test_cache_drops_by_age_and_expiry() {
  fresh();
  bePayer();
  const std::vector<uint8_t> a = makeReq(1, t_unix + 600);
  const std::vector<uint8_t> b = makeReq(2, t_unix + 600);
  const std::vector<uint8_t> c = makeReq(3, t_unix + 20);
  requests::onReq(MAC_PAYEE, c.data(), c.size(), -40, t_ms - 2 * requests::CACHE_VERIFY_GAP_MS);
  t_ms += requests::CACHE_VERIFY_GAP_MS;                    // one signature check per gap; a is heard last
  requests::onReq(MAC_PAYEE, b.data(), b.size(), -40, t_ms - requests::CACHE_VERIFY_GAP_MS);
  t_ms += requests::CACHE_VERIFY_GAP_MS;
  requests::onReq(MAC_PAYEE, a.data(), a.size(), -40, t_ms);
  CHECK(requests::cacheCount() == 3);

  // b is heard again 20 s later; a is not.
  t_ms += 20000; t_unix += 19;
  requests::onReq(MAC_PAYEE, b.data(), b.size(), -40, t_ms);
  requests::onReq(MAC_PAYEE, c.data(), c.size(), -40, t_ms);
  CHECK(requests::cacheCount() == 3);

  // At c's expiry it goes, although it was heard a second ago.
  t_ms += 1000; t_unix += 1;
  CHECK(requests::cacheCount() == 2);

  // 30 s after a was last heard it goes; b stays.
  t_ms += 8999;
  CHECK(requests::cacheCount() == 2);
  t_ms += 1;
  CHECK(requests::cacheCount() == 1);
  CHECK(requests::cacheAt(0)->req.req_id[0] == 2);
  // The service prunes as well.
  t_ms += 30000;
  requests::update();
  CHECK(requests::cacheAt(0) == nullptr);
  CHECK(requests::cacheCount() == 0);

  // A request that is already past its expiry when it arrives is never cached or announced.
  notes.clear();
  const std::vector<uint8_t> old = makeReq(4, t_unix);
  CHECK(!requests::onReq(MAC_PAYEE, old.data(), old.size(), -40, t_ms));
  CHECK(requests::cacheCount() == 0 && notes.empty());

  // With no clock the expiry cannot be judged: only the 30 s rule applies.
  clock_ok = false;
  CHECK(!requests::onReq(MAC_PAYEE, old.data(), old.size(), -40, t_ms));
  CHECK(requests::cacheCount() == 1);
  t_ms += 30000;
  CHECK(requests::cacheCount() == 0);

  // A dropped request that is heard again is new again.
  clock_ok = true;
  notes.clear();
  const std::vector<uint8_t> d = makeReq(5, t_unix + 600);
  requests::onReq(MAC_PAYEE, d.data(), d.size(), -40, t_ms);
  t_ms += 30000;
  CHECK(requests::cacheCount() == 0);
  requests::onReq(MAC_PAYEE, d.data(), d.size(), -40, t_ms);
  CHECK(requests::cacheCount() == 1 && notes.size() == 2);
}

static void test_cache_notification() {
  fresh();
  bePayer();
  const std::vector<uint8_t> a = makeReq(1, t_unix + 600, "Demo Merchant", 1000, "HACK");
  requests::onReq(MAC_PAYEE, a.data(), a.size(), -40, t_ms);
  CHECK(notes.size() == 1);
  CHECK(notes[0].title == "Payment request");
  CHECK(notes[0].body == "Demo Merchant 10.00 HACK");

  // While the pay app is the running app nothing is posted, but the request is still cached.
  pay_app_running = true;
  const std::vector<uint8_t> b = makeReq(2, t_unix + 600);
  t_ms += requests::CACHE_VERIFY_GAP_MS;
  requests::onReq(MAC_PAYEE, b.data(), b.size(), -40, t_ms);
  CHECK(notes.size() == 1);
  CHECK(requests::cacheCount() == 2);
  pay_app_running = false;

  // Bank rail: cents. An unknown currency: the raw number.
  const std::vector<uint8_t> c = makeReq(3, t_unix + 600, "Bank", 1250, "USD", VK_RAIL_BANK);
  t_ms += requests::CACHE_VERIFY_GAP_MS;
  requests::onReq(MAC_PAYEE, c.data(), c.size(), -40, t_ms);
  CHECK(notes.size() == 2 && notes[1].body == "Bank 12.50 USD");
  const std::vector<uint8_t> d = makeReq(4, t_unix + 600, "Odd", 1250, "XYZ");
  t_ms += requests::CACHE_VERIFY_GAP_MS;
  requests::onReq(MAC_PAYEE, d.data(), d.size(), -40, t_ms);
  CHECK(notes.size() == 3 && notes[2].body == "Odd 1250 XYZ");

  // A long name is shortened so the amount still fits the 39 characters a note shows.
  const std::vector<uint8_t> e = makeReq(5, t_unix + 600, "A very long merchant name, 32 ch", 123456789, "HACK");
  t_ms += requests::CACHE_VERIFY_GAP_MS;
  requests::onReq(MAC_PAYEE, e.data(), e.size(), -40, t_ms);
  CHECK(notes.size() == 4);
  CHECK(notes[3].body.size() == 39);
  CHECK(notes[3].body == "A very long merchant na 1234567.89 HACK");

  char text[24];
  CHECK(requests::formatAmount(VK_RAIL_SOLANA, "HACK", 5, text, sizeof text) == 4 && strcmp(text, "0.05") == 0);
  CHECK(requests::formatAmount(VK_RAIL_SOLANA, "HACK", 5, text, 2) == 0);
}

// ---- payer: presence --------------------------------------------------------------------------

// Opens a request as the payee and returns it; leaves the code under test playing the payer.
static requests::Opened openAsPayee() {
  bePayee();
  requests::Opened o;
  CHECK(requests::openRequest(args(), o) == VK_OK);
  bePayer();
  sent.clear();
  return o;
}

static void test_challenge_fills_a_slot_pending() {
  fresh();
  const requests::Opened o = openAsPayee();
  uint8_t key[32], nonce[16];
  CHECK(presence::lookup(o.req_id, key, nonce) == VK_PRESENCE_NONE);

  CHECK(presence::challenge(MAC_PAYEE, o.frame, o.frame_len) == VK_OK);
  CHECK(sent.size() == 1 && !sent[0].broadcast && memcmp(sent[0].mac, MAC_PAYEE, 6) == 0);
  vk_chal_t chal;
  CHECK(vk_chal_parse(sent[0].frame.data(), sent[0].frame.size(), &chal) == 0);
  CHECK(memcmp(chal.req_id, o.req_id, 8) == 0);
  CHECK(memcmp(chal.payer_pubkey, PAYER_PUB, 32) == 0);

  // presenceLookup returns the stored key and the nonce that was sent.
  memset(key, 0, sizeof key); memset(nonce, 0, sizeof nonce);
  CHECK(presence::lookup(o.req_id, key, nonce) == VK_PRESENCE_PENDING);
  CHECK(memcmp(key, V_DEVICE_PUB, 32) == 0);
  CHECK(memcmp(nonce, chal.nonce, 16) == 0);
  CHECK(presence::lookup(o.req_id, nullptr, nullptr) == VK_PRESENCE_PENDING);
  CHECK(strcmp(presence::resultName(VK_PRESENCE_PENDING), "pending") == 0);

  // Refusals: not a REQ, no mac, no identity. None of them disturbs the slot.
  CHECK(presence::challenge(MAC_PAYEE, o.frame, o.frame_len - 1) == VK_BAD_ARG);
  CHECK(presence::challenge(MAC_PAYEE, sent[0].frame.data(), sent[0].frame.size()) == VK_BAD_ARG);
  CHECK(presence::challenge(nullptr, o.frame, o.frame_len) == VK_BAD_ARG);
  own_key = nullptr;
  CHECK(presence::challenge(MAC_PAYEE, o.frame, o.frame_len) == VK_SIGN_FAILED);
  bePayer();
  CHECK(sent.size() == 1);
  CHECK(presence::lookup(o.req_id, key, nonce) == VK_PRESENCE_PENDING);
  CHECK(memcmp(nonce, chal.nonce, 16) == 0);

  // A radio that refuses the frame still leaves the slot PENDING; the app may challenge again.
  send_ok = false;
  const requests::Opened o2 = openAsPayee();
  CHECK(presence::challenge(MAC_PAYEE, o2.frame, o2.frame_len) == VK_OK);
  CHECK(presence::lookup(o2.req_id, nullptr, nullptr) == VK_PRESENCE_PENDING);
}

static void test_valid_proof_in_time_is_present() {
  fresh();
  const requests::Opened o = openAsPayee();
  CHECK(presence::challenge(MAC_PAYEE, o.frame, o.frame_len) == VK_OK);
  const uint32_t t0 = t_ms;
  vk_chal_t chal;
  CHECK(lastChal(chal));

  // The payee's firmware answers the CHAL; its PROOF goes back to the payer.
  bePayee();
  sent.clear();
  uint8_t chalFrame[VK_CHAL_LEN];
  CHECK(vk_chal_build(&chal, chalFrame, sizeof chalFrame) == VK_CHAL_LEN);
  CHECK(requests::onChal(MAC_PAYER, chalFrame, sizeof chalFrame));
  CHECK(sent.size() == 1);
  const std::vector<uint8_t> proof = sent[0].frame;
  bePayer();

  t_ms = t0 + 2000;                                         // the loop gets to it late...
  CHECK(presence::onProof(MAC_PAYEE, proof.data(), proof.size(), t0 + 1500));   // ...but the radio had it at the deadline
  uint8_t key[32], nonce[16];
  CHECK(presence::lookup(o.req_id, key, nonce) == VK_PRESENCE_PRESENT);
  CHECK(memcmp(key, V_DEVICE_PUB, 32) == 0 && memcmp(nonce, chal.nonce, 16) == 0);
  CHECK(presence::lastProofMs() == 1500);
  CHECK(strcmp(presence::resultName(presence::lookup(o.req_id, nullptr, nullptr)), "present") == 0);
}

static void test_late_bad_sig_other_mac_first_decides() {
  uint8_t key[32], nonce[16];
  vk_chal_t chal;

  // Late: valid, but one millisecond past presence_ms.
  fresh();
  requests::Opened o = openAsPayee();
  CHECK(presence::challenge(MAC_PAYEE, o.frame, o.frame_len) == VK_OK);
  CHECK(lastChal(chal));
  std::vector<uint8_t> good = makeProof(o.req_id, chal.nonce, PAYER_PUB, V_DEVICE_SEED);
  CHECK(presence::onProof(MAC_PAYEE, good.data(), good.size(), t_ms + cfg_presence_ms + 1));
  CHECK(presence::lookup(o.req_id, key, nonce) == VK_PRESENCE_LATE);
  CHECK(strcmp(presence::resultName(VK_PRESENCE_LATE), "late") == 0);

  // Bad signature: signed by a key that is not the request's payee (an impostor's made-up PROOF).
  // It is ignored and counted; it decides nothing, and the slot never becomes BAD_SIG.
  fresh();
  o = openAsPayee();
  CHECK(presence::challenge(MAC_PAYEE, o.frame, o.frame_len) == VK_OK);
  CHECK(lastChal(chal));
  std::vector<uint8_t> forged = makeProof(o.req_id, chal.nonce, PAYER_PUB, V_ISSUER_SEED);
  CHECK(presence::onProof(MAC_PAYEE, forged.data(), forged.size(), t_ms + 10));
  CHECK(presence::lookup(o.req_id, key, nonce) == VK_PRESENCE_PENDING);
  CHECK(memcmp(key, V_DEVICE_PUB, 32) == 0);
  CHECK(presence::badProofs(o.req_id) == 1);
  CHECK(strcmp(presence::resultName(VK_PRESENCE_BAD_SIG), "bad_sig") == 0);   // the name stays for the enum
  // The first valid PROOF decides, and after it nothing is checked again.
  good = makeProof(o.req_id, chal.nonce, PAYER_PUB, V_DEVICE_SEED);
  CHECK(presence::onProof(MAC_PAYEE, good.data(), good.size(), t_ms + 20));
  CHECK(presence::lookup(o.req_id, nullptr, nullptr) == VK_PRESENCE_PRESENT);
  const int verifiesBefore = verify_calls;
  CHECK(presence::onProof(MAC_PAYEE, forged.data(), forged.size(), t_ms + 30));
  CHECK(presence::lookup(o.req_id, nullptr, nullptr) == VK_PRESENCE_PRESENT);
  CHECK(verify_calls == verifiesBefore);

  // A real PROOF for another nonce (a recorded one, replayed), and one for another payer: ignored.
  fresh();
  o = openAsPayee();
  CHECK(presence::challenge(MAC_PAYEE, o.frame, o.frame_len) == VK_OK);
  CHECK(lastChal(chal));
  uint8_t otherNonce[16];
  memcpy(otherNonce, chal.nonce, 16);
  otherNonce[0] ^= 1;
  std::vector<uint8_t> replay = makeProof(o.req_id, otherNonce, PAYER_PUB, V_DEVICE_SEED);
  CHECK(presence::onProof(MAC_PAYEE, replay.data(), replay.size(), t_ms + 10));
  std::vector<uint8_t> forOther = makeProof(o.req_id, chal.nonce, V_ISSUER_PUB, V_DEVICE_SEED);
  CHECK(presence::onProof(MAC_PAYEE, forOther.data(), forOther.size(), t_ms + 10));
  CHECK(presence::lookup(o.req_id, nullptr, nullptr) == VK_PRESENCE_PENDING);
  CHECK(presence::badProofs(o.req_id) == 2);

  // Only invalid PROOFs until the deadline and after: the slot stays PENDING (amber), never red.
  // A valid one after the deadline is LATE. At most PROOF_CHECKS_MAX are checked per challenge;
  // the rest are counted unchecked. A new challenge starts a new count.
  fresh();
  o = openAsPayee();
  CHECK(presence::challenge(MAC_PAYEE, o.frame, o.frame_len) == VK_OK);
  CHECK(lastChal(chal));
  forged = makeProof(o.req_id, chal.nonce, PAYER_PUB, V_ISSUER_SEED);
  for (uint32_t i = 0; i < presence::PROOF_CHECKS_MAX - 1; i++) {
    CHECK(presence::onProof(MAC_PAYEE, forged.data(), forged.size(), t_ms + 10 + i));
  }
  CHECK(verify_calls == (int)presence::PROOF_CHECKS_MAX - 1);
  good = makeProof(o.req_id, chal.nonce, PAYER_PUB, V_DEVICE_SEED);
  CHECK(presence::onProof(MAC_PAYEE, good.data(), good.size(), t_ms + cfg_presence_ms + 1));
  CHECK(presence::lookup(o.req_id, nullptr, nullptr) == VK_PRESENCE_LATE);
  fresh();
  o = openAsPayee();
  CHECK(presence::challenge(MAC_PAYEE, o.frame, o.frame_len) == VK_OK);
  CHECK(lastChal(chal));
  forged = makeProof(o.req_id, chal.nonce, PAYER_PUB, V_ISSUER_SEED);
  for (uint32_t i = 0; i < presence::PROOF_CHECKS_MAX + 4; i++) {
    CHECK(presence::onProof(MAC_PAYEE, forged.data(), forged.size(), t_ms + 10 + i));
  }
  CHECK(verify_calls == (int)presence::PROOF_CHECKS_MAX);
  CHECK(presence::badProofs(o.req_id) == presence::PROOF_CHECKS_MAX + 4);
  CHECK(presence::lookup(o.req_id, nullptr, nullptr) == VK_PRESENCE_PENDING);
  CHECK(presence::challenge(MAC_PAYEE, o.frame, o.frame_len) == VK_OK);
  CHECK(lastChal(chal));
  CHECK(presence::badProofs(o.req_id) == 0);
  good = makeProof(o.req_id, chal.nonce, PAYER_PUB, V_DEVICE_SEED);
  CHECK(presence::onProof(MAC_PAYEE, good.data(), good.size(), t_ms + 50));
  CHECK(presence::lookup(o.req_id, nullptr, nullptr) == VK_PRESENCE_PRESENT);

  // A PROOF from another MAC is ignored, valid or not; the slot stays PENDING for the real one.
  fresh();
  o = openAsPayee();
  CHECK(presence::challenge(MAC_PAYEE, o.frame, o.frame_len) == VK_OK);
  CHECK(lastChal(chal));
  good = makeProof(o.req_id, chal.nonce, PAYER_PUB, V_DEVICE_SEED);
  forged = makeProof(o.req_id, chal.nonce, PAYER_PUB, V_ISSUER_SEED);
  CHECK(presence::onProof(MAC_OTHER, forged.data(), forged.size(), t_ms + 10));
  CHECK(presence::onProof(MAC_OTHER, good.data(), good.size(), t_ms + 10));
  CHECK(presence::lookup(o.req_id, nullptr, nullptr) == VK_PRESENCE_PENDING);
  CHECK(verify_calls == 0);
  // A PROOF for a request that was never challenged, and a malformed one, are ignored too.
  const uint8_t unknown[8] = {4, 4, 4, 4, 4, 4, 4, 4};
  std::vector<uint8_t> stray = makeProof(unknown, chal.nonce, PAYER_PUB, V_DEVICE_SEED);
  CHECK(presence::onProof(MAC_PAYEE, stray.data(), stray.size(), t_ms + 10));
  CHECK(presence::lookup(unknown, nullptr, nullptr) == VK_PRESENCE_NONE);
  CHECK(presence::onProof(MAC_PAYEE, good.data(), good.size() - 1, t_ms + 10));
  CHECK(presence::lookup(o.req_id, nullptr, nullptr) == VK_PRESENCE_PENDING);
  // The first PROOF decides: the valid one in time is PRESENT, and a forged one after it changes nothing.
  CHECK(presence::onProof(MAC_PAYEE, good.data(), good.size(), t_ms + 10));
  CHECK(presence::lookup(o.req_id, nullptr, nullptr) == VK_PRESENCE_PRESENT);
  CHECK(presence::onProof(MAC_PAYEE, forged.data(), forged.size(), t_ms + 20));
  CHECK(presence::onProof(MAC_PAYEE, good.data(), good.size(), t_ms + 5000));
  CHECK(presence::lookup(o.req_id, nullptr, nullptr) == VK_PRESENCE_PRESENT);
}

static void test_rechallenge_replaces_the_slot() {
  fresh();
  const requests::Opened o = openAsPayee();
  vk_chal_t first, second;
  CHECK(presence::challenge(MAC_PAYEE, o.frame, o.frame_len) == VK_OK);
  CHECK(lastChal(first));
  const std::vector<uint8_t> oldProof = makeProof(o.req_id, first.nonce, PAYER_PUB, V_DEVICE_SEED);
  const uint32_t oldRx = t_ms + 900;

  // The exchange looked lost, so the app challenges again: same slot, fresh nonce, new t0.
  t_ms += 1000;
  CHECK(presence::challenge(MAC_PAYEE, o.frame, o.frame_len) == VK_OK);
  CHECK(lastChal(second));
  CHECK(memcmp(first.nonce, second.nonce, 16) != 0);
  uint8_t nonce[16];
  CHECK(presence::lookup(o.req_id, nullptr, nonce) == VK_PRESENCE_PENDING);
  CHECK(memcmp(nonce, second.nonce, 16) == 0);

  // The answer to the first challenge, received by the radio before the second was sent, is not
  // judged against the new nonce.
  CHECK(presence::onProof(MAC_PAYEE, oldProof.data(), oldProof.size(), oldRx));
  CHECK(presence::lookup(o.req_id, nullptr, nullptr) == VK_PRESENCE_PENDING);
  CHECK(verify_calls == 0);

  const std::vector<uint8_t> newProof = makeProof(o.req_id, second.nonce, PAYER_PUB, V_DEVICE_SEED);
  CHECK(presence::onProof(MAC_PAYEE, newProof.data(), newProof.size(), t_ms + 300));
  CHECK(presence::lookup(o.req_id, nullptr, nullptr) == VK_PRESENCE_PRESENT);
  CHECK(presence::lastProofMs() == 300);

  // A decided slot can be challenged again too: it goes back to PENDING.
  CHECK(presence::challenge(MAC_PAYEE, o.frame, o.frame_len) == VK_OK);
  CHECK(presence::lookup(o.req_id, nullptr, nullptr) == VK_PRESENCE_PENDING);

  // The deadline holds across a millis() wrap.
  fresh();
  t_ms = 0xFFFFFF00u;
  const requests::Opened w = openAsPayee();
  CHECK(presence::challenge(MAC_PAYEE, w.frame, w.frame_len) == VK_OK);
  CHECK(lastChal(first));
  const std::vector<uint8_t> wrapped = makeProof(w.req_id, first.nonce, PAYER_PUB, V_DEVICE_SEED);
  CHECK(presence::onProof(MAC_PAYEE, wrapped.data(), wrapped.size(), t_ms + 0x200));   // rx_ms has wrapped
  CHECK(presence::lookup(w.req_id, nullptr, nullptr) == VK_PRESENCE_PRESENT);
  CHECK(presence::lastProofMs() == 0x200);
}

static void test_oldest_slot_is_reused() {
  fresh();
  bePayer();
  std::vector<std::vector<uint8_t>> frames;
  for (uint8_t id = 1; id <= 5; id++) frames.push_back(makeReq(id, t_unix + 600));
  uint8_t ids[5][8];
  for (int i = 0; i < 5; i++) memset(ids[i], i + 1, 8);

  for (int i = 0; i < 4; i++) {
    CHECK(presence::challenge(MAC_PAYEE, frames[i].data(), frames[i].size()) == VK_OK);
    t_ms += 10;
  }
  for (int i = 0; i < 4; i++) CHECK(presence::lookup(ids[i], nullptr, nullptr) == VK_PRESENCE_PENDING);
  CHECK(presence::lookup(ids[4], nullptr, nullptr) == VK_PRESENCE_NONE);

  // Request 1 is challenged again, so request 2 is now the oldest.
  CHECK(presence::challenge(MAC_PAYEE, frames[0].data(), frames[0].size()) == VK_OK);
  t_ms += 10;
  // The table is full: a fifth request takes the oldest slot.
  CHECK(presence::challenge(MAC_PAYEE, frames[4].data(), frames[4].size()) == VK_OK);
  CHECK(presence::lookup(ids[1], nullptr, nullptr) == VK_PRESENCE_NONE);
  CHECK(presence::lookup(ids[0], nullptr, nullptr) == VK_PRESENCE_PENDING);
  CHECK(presence::lookup(ids[2], nullptr, nullptr) == VK_PRESENCE_PENDING);
  CHECK(presence::lookup(ids[3], nullptr, nullptr) == VK_PRESENCE_PENDING);
  CHECK(presence::lookup(ids[4], nullptr, nullptr) == VK_PRESENCE_PENDING);

  uint8_t key[32];
  memset(key, 0xEE, sizeof key);
  CHECK(presence::lookup(ids[1], key, nullptr) == VK_PRESENCE_NONE);
  CHECK(key[0] == 0xEE);                                    // nothing is written for NONE

  presence::reset();
  for (int i = 0; i < 5; i++) CHECK(presence::lookup(ids[i], nullptr, nullptr) == VK_PRESENCE_NONE);
  CHECK(strcmp(presence::resultName(VK_PRESENCE_NONE), "none") == 0);
}

// ---- both sides together: the honest exchange of espnow.md "Sequences" --------------------------

static void test_honest_exchange() {
  fresh();
  bePayee();
  requests::Opened o;
  CHECK(requests::openRequest(args(), o) == VK_OK);
  requests::update();                                       // the broadcast
  CHECK(sent.size() == 1 && sent[0].broadcast);
  const std::vector<uint8_t> req = sent[0].frame;

  // Payer: cache, notify, challenge.
  bePayer();
  sent.clear();
  CHECK(!requests::onReq(MAC_PAYEE, req.data(), req.size(), -50, t_ms));
  CHECK(requests::cacheCount() == 1 && notes.size() == 1);
  const requests::Cached *c = requests::cacheAt(0);
  CHECK(presence::challenge(c->mac, c->frame, c->frame_len) == VK_OK);
  const uint32_t t0 = t_ms;
  CHECK(sent.size() == 1 && vk_frame_type(sent[0].frame.data(), sent[0].frame.size()) == VK_T_CHAL);
  const std::vector<uint8_t> chal = sent[0].frame;

  // Payee: sign the PROOF.
  bePayee();
  sent.clear();
  t_ms += 5;
  CHECK(requests::onChal(MAC_PAYER, chal.data(), chal.size()));
  CHECK(sent.size() == 1 && memcmp(sent[0].mac, MAC_PAYER, 6) == 0);
  const std::vector<uint8_t> proof = sent[0].frame;

  // Payer: judge it.
  bePayer();
  t_ms += 900;
  CHECK(presence::onProof(MAC_PAYEE, proof.data(), proof.size(), t_ms));
  uint8_t key[32], nonce[16];
  CHECK(presence::lookup(o.req_id, key, nonce) == VK_PRESENCE_PRESENT);
  CHECK(memcmp(key, c->req.payee_pubkey, 32) == 0);
  CHECK(presence::lastProofMs() == t_ms - t0);

  // An impostor replays the REQ from its own MAC. While the payee keeps broadcasting, the cache
  // keeps the payee's MAC. Challenged at the impostor's MAC (T-REQ3, or a payer that only ever heard
  // the replay), nobody who holds the payee key answers, so the slot stays PENDING (amber).
  CHECK(!requests::onReq(MAC_OTHER, req.data(), req.size(), -30, t_ms));
  c = requests::cacheAt(0);
  CHECK(memcmp(c->mac, MAC_PAYEE, 6) == 0);
  CHECK(presence::challenge(MAC_OTHER, c->frame, c->frame_len) == VK_OK);
  // The real payee's PROOF for the new nonce would come from another MAC and is ignored.
  vk_chal_t again;
  CHECK(lastChal(again));
  const std::vector<uint8_t> real = makeProof(o.req_id, again.nonce, PAYER_PUB, V_DEVICE_SEED);
  CHECK(presence::onProof(MAC_PAYEE, real.data(), real.size(), t_ms + 100));
  CHECK(presence::lookup(o.req_id, nullptr, nullptr) == VK_PRESENCE_PENDING);
}

// ---- attacks: someone in radio range tries to spoil an honest payment ---------------------------
// Audit finding 9 / build-list item 20. Each test plays an attacker who can send any frame from any
// MAC (an ESP32 can set its own MAC) but does not hold the payee's key. None of them may turn an
// honest exchange into anything but PRESENT, and none may leave a lasting effect once it stops.

// The CHAL the payer's firmware sent last, answered by the payee's firmware: the PROOF frame, or
// an empty vector when the payee did not answer.
static std::vector<uint8_t> payeeAnswers(const uint8_t chalFrame[VK_CHAL_LEN], const uint8_t mac[6]) {
  bePayee();
  sent.clear();
  requests::onChal(mac, chalFrame, VK_CHAL_LEN);
  requests::update();                                       // a queued answer goes out on the next pass
  std::vector<uint8_t> proof;
  for (const Sent &s : sent) {
    if (vk_frame_type(s.frame.data(), s.frame.size()) == VK_T_PROOF && memcmp(s.mac, mac, 6) == 0) proof = s.frame;
  }
  bePayer();
  sent.clear();
  return proof;
}

// Attack 1a: eight CHALs from the attacker's own MAC used to exhaust req_max_proofs for the whole
// request, so the honest payer's CHAL afterwards was never answered.
static void test_attack_chal_flood_one_mac() {
  fresh();
  bePayee();
  requests::Opened o;
  CHECK(requests::openRequest(args(), o) == VK_OK);
  for (int i = 0; i < 16; i++) {
    t_ms += 1500;                                           // slow enough for every rate limit
    requests::onChal(MAC_OTHER, makeChal(o.req_id, (uint8_t)(0x40 + i), V_ISSUER_PUB).data(), VK_CHAL_LEN);
  }
  t_ms += 1500;
  bePayer();
  CHECK(presence::challenge(MAC_PAYEE, o.frame, o.frame_len) == VK_OK);
  vk_chal_t chal;
  CHECK(lastChal(chal));
  uint8_t chalFrame[VK_CHAL_LEN];
  vk_chal_build(&chal, chalFrame, sizeof chalFrame);
  const std::vector<uint8_t> proof = payeeAnswers(chalFrame, MAC_PAYER);
  CHECK(!proof.empty());                                    // the honest payer is still answered
  CHECK(presence::onProof(MAC_PAYEE, proof.data(), proof.size(), t_ms + 50));
  CHECK(presence::lookup(o.req_id, nullptr, nullptr) == VK_PRESENCE_PRESENT);
}

// Attack 1b: the same with a fresh spoofed MAC per CHAL. While it lasts the attacker competes for
// the payee's answer rate; once it stops the honest payer is answered (nothing was used up).
static void test_attack_chal_flood_spoofed_macs() {
  fresh();
  bePayee();
  requests::Opened o;
  CHECK(requests::openRequest(args(), o) == VK_OK);
  uint8_t mac[6];
  memcpy(mac, MAC_SPOOF_BASE, 6);
  for (int i = 0; i < 64; i++) {
    t_ms += 300;
    mac[5] = (uint8_t)i;
    requests::onChal(mac, makeChal(o.req_id, (uint8_t)i, V_ISSUER_PUB).data(), VK_CHAL_LEN);
    requests::update();
  }
  t_ms += 1500;
  const std::vector<uint8_t> chal = makeChal(o.req_id, 0xC3, PAYER_PUB);
  sent.clear();
  CHECK(requests::onChal(MAC_PAYER, chal.data(), chal.size()));
  requests::update();
  CHECK(countSent(VK_T_PROOF) == 1);
  vk_proof_t p;
  CHECK(lastProofTo(MAC_PAYER, p) && proofSignsNonce(p, 0xC3, PAYER_PUB));
}

// Attack 2: one forged PROOF, sent from the payee's MAC before the real one arrives, used to decide
// the slot BAD_SIG (red BAD PROOF) and the real PROOF was then ignored.
static void test_attack_forged_proof_first() {
  fresh();
  const requests::Opened o = openAsPayee();
  CHECK(presence::challenge(MAC_PAYEE, o.frame, o.frame_len) == VK_OK);
  vk_chal_t chal;
  CHECK(lastChal(chal));
  // Junk signature, a signature by another key, and the right key over a wrong nonce.
  std::vector<uint8_t> junk = makeProof(o.req_id, chal.nonce, PAYER_PUB, V_DEVICE_SEED);
  junk[20] ^= 0xFF;
  const std::vector<uint8_t> otherKey = makeProof(o.req_id, chal.nonce, PAYER_PUB, V_ISSUER_SEED);
  uint8_t wrongNonce[16];
  memcpy(wrongNonce, chal.nonce, 16);
  wrongNonce[15] ^= 1;
  const std::vector<uint8_t> otherNonce = makeProof(o.req_id, wrongNonce, PAYER_PUB, V_DEVICE_SEED);
  const std::vector<uint8_t> otherPayer = makeProof(o.req_id, chal.nonce, V_ISSUER_PUB, V_DEVICE_SEED);
  CHECK(presence::onProof(MAC_PAYEE, junk.data(), junk.size(), t_ms + 5));
  CHECK(presence::onProof(MAC_PAYEE, otherKey.data(), otherKey.size(), t_ms + 6));
  CHECK(presence::onProof(MAC_PAYEE, otherNonce.data(), otherNonce.size(), t_ms + 7));
  CHECK(presence::onProof(MAC_PAYEE, otherPayer.data(), otherPayer.size(), t_ms + 8));
  CHECK(presence::lookup(o.req_id, nullptr, nullptr) == VK_PRESENCE_PENDING);       // not red
  const std::vector<uint8_t> good = makeProof(o.req_id, chal.nonce, PAYER_PUB, V_DEVICE_SEED);
  CHECK(presence::onProof(MAC_PAYEE, good.data(), good.size(), t_ms + 250));
  CHECK(presence::lookup(o.req_id, nullptr, nullptr) == VK_PRESENCE_PRESENT);
}

// Attack 3: replay. A PROOF recorded from an earlier challenge (of this request or of an earlier
// one) is sent again after the payer challenged afresh. It used to decide BAD_SIG.
static void test_attack_replayed_proof() {
  fresh();
  const requests::Opened earlier = openAsPayee();
  CHECK(presence::challenge(MAC_PAYEE, earlier.frame, earlier.frame_len) == VK_OK);
  vk_chal_t c0;
  CHECK(lastChal(c0));
  const std::vector<uint8_t> recordedEarlier = makeProof(earlier.req_id, c0.nonce, PAYER_PUB, V_DEVICE_SEED);
  CHECK(presence::onProof(MAC_PAYEE, recordedEarlier.data(), recordedEarlier.size(), t_ms + 100));
  CHECK(presence::lookup(earlier.req_id, nullptr, nullptr) == VK_PRESENCE_PRESENT);

  t_ms += 5000;
  const requests::Opened o = openAsPayee();
  CHECK(presence::challenge(MAC_PAYEE, o.frame, o.frame_len) == VK_OK);
  vk_chal_t c1;
  CHECK(lastChal(c1));
  const std::vector<uint8_t> recorded = makeProof(o.req_id, c1.nonce, PAYER_PUB, V_DEVICE_SEED);
  t_ms += 2000;                                             // the first exchange looked lost: challenge again
  CHECK(presence::challenge(MAC_PAYEE, o.frame, o.frame_len) == VK_OK);
  vk_chal_t c2;
  CHECK(lastChal(c2));
  // The attacker replays the answer to the first challenge, after the second one was sent.
  CHECK(presence::onProof(MAC_PAYEE, recorded.data(), recorded.size(), t_ms + 10));
  CHECK(presence::lookup(o.req_id, nullptr, nullptr) == VK_PRESENCE_PENDING);
  // A PROOF from the earlier request does not even name this one's slot.
  CHECK(presence::onProof(MAC_PAYEE, recordedEarlier.data(), recordedEarlier.size(), t_ms + 11));
  CHECK(presence::lookup(o.req_id, nullptr, nullptr) == VK_PRESENCE_PENDING);
  const std::vector<uint8_t> good = makeProof(o.req_id, c2.nonce, PAYER_PUB, V_DEVICE_SEED);
  CHECK(presence::onProof(MAC_PAYEE, good.data(), good.size(), t_ms + 300));
  CHECK(presence::lookup(o.req_id, nullptr, nullptr) == VK_PRESENCE_PRESENT);
}

// Attack 3b: a recorded CHAL is replayed to the payee, from the honest payer's MAC. Each copy used
// to cost a signature and a unit of the request's budget. A CHAL of a request that has closed is
// not answered at all.
static void test_attack_replayed_chal() {
  fresh();
  bePayee();
  requests::Opened old, o;
  CHECK(requests::openRequest(args(), old) == VK_OK);
  const std::vector<uint8_t> oldChal = makeChal(old.req_id, 0x31, PAYER_PUB);
  CHECK(requests::onChal(MAC_PAYER, oldChal.data(), oldChal.size()));
  CHECK(countSent(VK_T_PROOF) == 1);
  CHECK(requests::closeRequest(old.req_id));
  CHECK(requests::openRequest(args(), o) == VK_OK);
  const std::vector<uint8_t> chal = makeChal(o.req_id, 0x32, PAYER_PUB);
  t_ms += 1500;
  CHECK(requests::onChal(MAC_PAYER, chal.data(), chal.size()));
  CHECK(countSent(VK_T_PROOF) == 2);
  const int signsBefore = sign_calls;
  for (int i = 0; i < 10; i++) {
    t_ms += 1500;
    CHECK(requests::onChal(MAC_PAYER, chal.data(), chal.size()));
    CHECK(requests::onChal(MAC_OTHER, chal.data(), chal.size()));
    CHECK(requests::onChal(MAC_PAYER, oldChal.data(), oldChal.size()));
    requests::update();
  }
  CHECK(sign_calls == signsBefore);
  CHECK(countSent(VK_T_PROOF) == 2);
}

static std::vector<uint8_t> makeSignedReq(uint8_t id, uint32_t expiry, uint64_t amount = 1000) {
  return makeReq(id, expiry, "Shop", amount);
}

// Attack 4: the cache. A replay of the honest REQ from the attacker's MAC used to move the entry's
// MAC (the payer then challenged the attacker: amber), and a REQ with the same req_id but other
// bytes used to replace the honest frame (red BAD REQUEST, or the wrong amount listed).
static void test_attack_req_cache() {
  fresh();
  bePayer();
  const std::vector<uint8_t> honest = makeSignedReq(1, t_unix + 600);
  CHECK(!requests::onReq(MAC_PAYEE, honest.data(), honest.size(), -50, t_ms));
  CHECK(requests::cacheCount() == 1 && notes.size() == 1);

  // Replayed byte for byte, from another MAC, while the payee keeps broadcasting.
  for (int i = 0; i < 5; i++) {
    t_ms += 300;
    CHECK(!requests::onReq(MAC_OTHER, honest.data(), honest.size(), -20, t_ms));
    t_ms += 700;
    CHECK(!requests::onReq(MAC_PAYEE, honest.data(), honest.size(), -50, t_ms));
    t_ms += 1;
    CHECK(!requests::onReq(MAC_OTHER, honest.data(), honest.size(), -20, t_ms));
  }
  CHECK(requests::cacheCount() == 1);
  CHECK(memcmp(requests::cacheAt(0)->mac, MAC_PAYEE, 6) == 0);

  // The same req_id and payee key with another amount (the signature no longer matches).
  std::vector<uint8_t> altered = makeSignedReq(1, t_unix + 600, 999999);
  altered[altered.size() - 1] ^= 1;
  CHECK(!requests::onReq(MAC_PAYEE, altered.data(), altered.size(), -50, t_ms));
  CHECK(requests::cacheCount() == 1);
  const requests::Cached *c = requests::cacheAt(0);
  CHECK(c->frame_len == honest.size() && memcmp(c->frame, honest.data(), honest.size()) == 0);
  CHECK(c->req.amount == 1000);

  // A REQ whose signature does not verify is neither cached nor announced.
  std::vector<uint8_t> forged = makeSignedReq(2, t_unix + 600);
  forged[forged.size() - 1] ^= 1;
  CHECK(!requests::onReq(MAC_OTHER, forged.data(), forged.size(), -50, t_ms));
  CHECK(requests::cacheCount() == 1 && notes.size() == 1);
}

// ---- null hooks: nothing crashes, everything is refused ---------------------------------------

static void test_null_hooks() {
  fresh();
  requests::hooks = {};
  requests::Opened o;
  CHECK(requests::canOpen() == VK_NOT_PROVISIONED);
  CHECK(requests::openRequest(args(), o) == VK_NOT_PROVISIONED);
  requests::update();
  CHECK(requests::onChal(MAC_PAYER, V_REQ, sizeof V_REQ));
  CHECK(!requests::onReq(MAC_PAYEE, V_REQ, sizeof V_REQ, -40, 0));
  CHECK(requests::cacheCount() == 0);                       // no verifier: nothing can be checked, nothing cached
  CHECK(presence::challenge(MAC_PAYEE, V_REQ, sizeof V_REQ) == VK_SIGN_FAILED);
  uint8_t proof[VK_PROOF_LEN] = {'V', 'K', 1, VK_T_PROOF};
  CHECK(presence::onProof(MAC_PAYEE, proof, sizeof proof, 0));
  char text[8];
  CHECK(requests::formatAmount(VK_RAIL_SOLANA, "HACK", 12, text, sizeof text) == 2 && strcmp(text, "12") == 0);
}

int main() {
  host_ed25519_keypair(PAYER_SEED, PAYER_PUB);

  test_open_builds_a_signed_req();
  test_third_open_is_busy();
  test_open_refusals();
  test_expiry_closes();
  test_close_and_status();
  test_app_stop_closes_only_that_apps_requests();
  test_rebroadcast_every_period();
  test_chal_for_unknown_request_is_dropped();
  test_proof_verifies_with_the_payee_key();
  test_proof_rate_limits();
  test_chal_queue();
  test_validators();
  test_cache_keeps_eight_distinct();
  test_cache_drops_by_age_and_expiry();
  test_cache_notification();
  test_challenge_fills_a_slot_pending();
  test_valid_proof_in_time_is_present();
  test_late_bad_sig_other_mac_first_decides();
  test_rechallenge_replaces_the_slot();
  test_oldest_slot_is_reused();
  test_honest_exchange();
  test_attack_chal_flood_one_mac();
  test_attack_chal_flood_spoofed_macs();
  test_attack_forged_proof_first();
  test_attack_replayed_proof();
  test_attack_replayed_chal();
  test_attack_req_cache();
  test_null_hooks();

  if (fails) {
    printf("%d requests test(s) FAILED\n", fails);
    return 1;
  }
  printf("all requests tests passed\n");
  return 0;
}
