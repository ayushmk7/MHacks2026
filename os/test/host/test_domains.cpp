// LINK: src/vk/wallet/signer.cpp src/vk/features/store_reg/domain_store_reg.cpp
// Host suite "domains" (signing.md, "Self-check" and "The signing path").
//   1. The self-check accepts the shipped table and rejects each rule violation.
//   2. The signing path refuses in the documented order and signs prefix || bytes.
//   3. The "store-reg" validator accepts only the registration message for this badge's key.
// When a signing domain is added, add its row to kShipped below.
#include <Arduino.h>

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "../../src/vk/wallet/signer.h"
#include "../../src/vk/wallet/signer_internal.h"
#include "host_ed25519.h"

using namespace vk::wallet;

static int sFailures = 0;
static int sChecks = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++sChecks;                                                               \
    if (!(cond)) {                                                           \
      ++sFailures;                                                           \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
    }                                                                        \
  } while (0)

// ---------------------------------------------------------------------------
// 1. The self-check as a pure function
// ---------------------------------------------------------------------------

// The six rows of signing.md's table: name, prefix, button, has decode, has validate.
static const DomainRow kShipped[] = {
    {"solana", "", true, true, false},
    {"bank", "bank-auth:", true, true, false},
    {"pay-req", "pay-req:", false, false, true},
    {"pay-proof", "pay-proof:", false, false, true},
    {"contact", "contact:", false, false, true},
    {"store-reg", "", false, false, true},
    {"selftest", "selftest:", true, true, false},
};
static const size_t kShippedCount = sizeof kShipped / sizeof kShipped[0];

// The shipped table with one more row; returns the rule that row breaks and its name.
static int checkWith(const DomainRow &extra, const char **bad) {
  DomainRow rows[kShippedCount + 1];
  memcpy(rows, kShipped, sizeof kShipped);
  rows[kShippedCount] = extra;
  return checkDomainTable(rows, kShippedCount + 1, bad);
}

static int checkPrefix(const char *prefix) {
  const char *bad = nullptr;
  return checkWith({"extra", prefix, false, false, true}, &bad);
}

static void testSelfCheckRules() {
  const char *bad = "unset";

  // The shipped table is valid, and so is an empty one.
  CHECK(checkDomainTable(kShipped, kShippedCount, &bad) == 0);
  CHECK(strcmp(bad, "") == 0);
  CHECK(checkDomainTable(kShipped, kShippedCount, nullptr) == 0);
  CHECK(checkDomainTable(nullptr, 0, &bad) == 0);

  // A valid extra row keeps it valid (so the rejections below are caused by the fault alone).
  CHECK(checkWith({"extra", "extra:", false, false, true}, &bad) == 0);
  CHECK(checkWith({"extra", "extra-btn:", true, true, false}, &bad) == 0);

  // Rule 1: a duplicate name.
  CHECK(checkWith({"solana", "other:", false, false, true}, &bad) == 1);
  CHECK(strcmp(bad, "solana") == 0);
  CHECK(checkWith({nullptr, "other:", false, false, true}, &bad) == 1);

  // Rule 2: a malformed prefix.
  CHECK(checkWith({"extra", "Pay:", false, false, true}, &bad) == 2);
  CHECK(strcmp(bad, "extra") == 0);
  CHECK(checkPrefix("pay") == 2);                 // no ':' at the end
  CHECK(checkPrefix("a:") == 2);                  // one character
  CHECK(checkPrefix(":") == 2);
  CHECK(checkPrefix("ab:") == 0);                 // two characters: the shortest allowed
  CHECK(checkPrefix("abcdefghijklmno:") == 0);    // fifteen: the longest allowed
  CHECK(checkPrefix("abcdefghijklmnop:") == 2);   // sixteen
  CHECK(checkPrefix("pay2:") == 2);               // a digit
  CHECK(checkPrefix("pay_req:") == 2);            // an underscore
  CHECK(checkPrefix("pay req:") == 2);            // a space
  CHECK(checkPrefix("pay:req:") == 2);            // ':' before the end
  CHECK(checkPrefix("-pay:") == 2);               // does not begin with a letter
  CHECK(checkPrefix("\x01pay:") == 2);            // would look like a Solana message
  CHECK(checkWith({"extra", nullptr, false, false, true}, &bad) == 2);

  // Rule 3: a prefix that is a prefix of another. Rule 2 allows ':' only at the end, so among
  // well-formed prefixes this can only be two equal ones.
  CHECK(checkWith({"extra", "pay-req:", false, false, true}, &bad) == 3);
  CHECK(strcmp(bad, "extra") == 0);
  CHECK(checkWith({"extra", "bank-auth:", true, true, false}, &bad) == 3);

  // Rule 4: two button domains with an empty prefix.
  CHECK(checkWith({"extra", "", true, true, false}, &bad) == 4);
  CHECK(strcmp(bad, "extra") == 0);
  // A second auto domain with an empty prefix is not what rule 4 forbids.
  CHECK(checkWith({"extra", "", false, false, true}, &bad) == 0);

  // Rule 5: an auto domain without validate; a button domain without decode.
  CHECK(checkWith({"extra", "extra:", false, false, false}, &bad) == 5);
  CHECK(strcmp(bad, "extra") == 0);
  CHECK(checkWith({"extra", "extra:", false, true, false}, &bad) == 5);    // decode does not count for auto
  CHECK(checkWith({"extra", "extra:", true, false, false}, &bad) == 5);
  CHECK(strcmp(bad, "extra") == 0);
  CHECK(checkWith({"extra", "extra:", true, false, true}, &bad) == 5);     // validate does not count for button

  // Rule 6: the reserved prefix.
  CHECK(checkWith({"extra", "registry:", false, false, true}, &bad) == 6);
  CHECK(strcmp(bad, "extra") == 0);
  CHECK(checkWith({"extra", "registry:", true, true, false}, &bad) == 6);

  // The lowest broken rule is the one reported.
  CHECK(checkWith({"bank", "registry:", false, false, false}, &bad) == 1);
}

// ---------------------------------------------------------------------------
// 2 and 3. The signing path over registered rows
// ---------------------------------------------------------------------------

// TEST KEY ONLY: a fixed seed, never used on a badge.
static const uint8_t kSeed[32] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a,
                                  0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
                                  0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f};
static uint8_t sPub[32];

static int sSignCalls = 0;
static uint8_t sSigned[2048];
static size_t sSignedLen = 0;

static bool hookSign(const uint8_t *message, size_t length, uint8_t out[64]) {
  ++sSignCalls;
  sSignedLen = length < sizeof sSigned ? length : sizeof sSigned;
  memcpy(sSigned, message, sSignedLen);
  host_ed25519_sign(kSeed, message, length, out);
  return true;
}
static bool hookSignRefuses(const uint8_t *, size_t, uint8_t *) { ++sSignCalls; return false; }

static const char *sAskedPermission = nullptr;
static bool hookGrantNothing(const char *permission) { sAskedPermission = permission; return false; }

static int sOpenCalls = 0;
static const SignDomain *sOpenDomain = nullptr;
static size_t sOpenLen = 0;
static char sOpenApp[40];
static Reason sOpenReply = VK_OK;
static Reason hookOpen(const SignDomain *domain, const uint8_t *, size_t len, const Ctx &, const char *app_id) {
  ++sOpenCalls;
  sOpenDomain = domain;
  sOpenLen = len;
  strlcpy(sOpenApp, app_id, sizeof sOpenApp);
  return sOpenReply;
}

// Test rows. With the real "store-reg" row from domain_store_reg.cpp the registered table has the
// shape of the shipped one: a bare button domain, a prefixed button domain, a prefixed auto domain.
static Reason decodeNotCalled(const uint8_t *, size_t, const Ctx &, ApprovalRequest &) { return VK_OK; }
static int sValidateCalls = 0;
static bool validateProof(const uint8_t *bytes, size_t len) { ++sValidateCalls; return bytes != nullptr && len == 56; }

VK_SIGN_DOMAIN(t_solana, "solana", "", true, "sign", 1232, decodeNotCalled, nullptr);
VK_SIGN_DOMAIN(t_bank, "bank", "bank-auth:", true, "sign", 512, decodeNotCalled, nullptr);
VK_SIGN_DOMAIN(t_proof, "pay-proof", "pay-proof:", false, nullptr, 56, nullptr, validateProof);
VK_SIGN_DOMAIN(t_internal, "t-internal", "t-internal:", true, nullptr, 64, decodeNotCalled, nullptr);

static bool allZero(const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; ++i) if (p[i]) return false;
  return true;
}

static void resetHooks() {
  hostHooks = HostHooks{};
  hostHooks.public_key = sPub;
  hostHooks.sign = hookSign;
  hostHooks.open = hookOpen;
  sSignCalls = 0;
  sOpenCalls = 0;
  sOpenReply = VK_OK;
  sValidateCalls = 0;
  sAskedPermission = nullptr;
}

static void testRegistryAndKey() {
  CHECK(findDomain(nullptr) == nullptr);
  CHECK(findDomain("nope") == nullptr);
  CHECK(findDomain("solana") != nullptr && findDomain("solana")->needs_button);
  const SignDomain *reg = findDomain("store-reg");
  CHECK(reg != nullptr);
  if (reg) {
    CHECK(strcmp(reg->prefix, "") == 0);
    CHECK(!reg->needs_button);
    CHECK(reg->permission == nullptr);
    CHECK(reg->max_len == 200);
    CHECK(reg->decode == nullptr);
    CHECK(reg->validate != nullptr);
  }

  CHECK(presenceLookup == nullptr);
  CHECK(tokenInfoLookup == nullptr);

  resetHooks();
  CHECK(strcmp(keyLocation(), "software") == 0);
  CHECK(publicKey() == sPub);
  CHECK(addressBase58().length() >= 32);
  CHECK(maxSignBytes() == 1248);
  hostHooks.public_key = nullptr;
  CHECK(strcmp(keyLocation(), "none") == 0);
  CHECK(publicKey() == nullptr);
  CHECK(addressBase58().length() == 0);
}

static void testNothingSignsBeforeSelfCheck() {
  resetHooks();
  uint8_t proof[56];
  memset(proof, 7, sizeof proof);
  uint8_t sig[64];
  memset(sig, 0xEE, sizeof sig);
  CHECK(!selfCheckOk());
  CHECK(signAuto("pay-proof", proof, sizeof proof, sig) == VK_SIGN_FAILED);
  CHECK(allZero(sig, 64));
  CHECK(signForApproval(findDomain("solana"), proof, sizeof proof, sig) == VK_SIGN_FAILED);
  CHECK(sSignCalls == 0);
}

static void testSignAuto() {
  resetHooks();
  uint8_t proof[56];
  for (size_t i = 0; i < sizeof proof; ++i) proof[i] = (uint8_t)i;
  uint8_t sig[64];

  CHECK(signAuto("nope", proof, sizeof proof, sig) == VK_UNSUPPORTED);
  CHECK(signAuto(nullptr, proof, sizeof proof, sig) == VK_UNSUPPORTED);
  CHECK(signAuto("solana", proof, sizeof proof, sig) == VK_UNSUPPORTED);   // a button domain
  CHECK(signAuto("bank", proof, sizeof proof, sig) == VK_UNSUPPORTED);
  CHECK(signAuto("pay-proof", nullptr, sizeof proof, sig) == VK_BAD_ARG);
  CHECK(signAuto("pay-proof", proof, sizeof proof, nullptr) == VK_BAD_ARG);
  CHECK(signAuto("pay-proof", proof, 0, sig) == VK_TOO_LONG);
  CHECK(sValidateCalls == 0);

  uint8_t longer[57] = {0};
  CHECK(signAuto("pay-proof", longer, sizeof longer, sig) == VK_TOO_LONG);
  CHECK(sValidateCalls == 0);                                              // over max_len: never validated
  CHECK(signAuto("pay-proof", proof, 55, sig) == VK_BAD_ARG);              // the validator refuses
  CHECK(sValidateCalls == 1);
  CHECK(sSignCalls == 0);

  // The signature is over prefix || bytes.
  CHECK(signAuto("pay-proof", proof, sizeof proof, sig) == VK_OK);
  CHECK(sSignCalls == 1);
  CHECK(sSignedLen == 10 + sizeof proof);
  CHECK(memcmp(sSigned, "pay-proof:", 10) == 0);
  CHECK(memcmp(sSigned + 10, proof, sizeof proof) == 0);
  uint8_t expected[10 + sizeof proof];
  memcpy(expected, "pay-proof:", 10);
  memcpy(expected + 10, proof, sizeof proof);
  CHECK(host_ed25519_verify(expected, sizeof expected, sig, sPub) == 1);
  CHECK(host_ed25519_verify(proof, sizeof proof, sig, sPub) == 0);         // not over the bare bytes

  // The SE050 limit counts the prefix: 10 + 56 > 60.
  hostHooks.max_sign_bytes = 60;
  CHECK(signAuto("pay-proof", proof, sizeof proof, sig) == VK_TOO_LONG);
  CHECK(allZero(sig, 64));
  hostHooks.max_sign_bytes = 1248;

  // The identity refuses.
  hostHooks.sign = hookSignRefuses;
  memset(sig, 0xEE, sizeof sig);
  CHECK(signAuto("pay-proof", proof, sizeof proof, sig) == VK_SIGN_FAILED);
  CHECK(allZero(sig, 64));
  hostHooks.sign = nullptr;
  CHECK(signAuto("pay-proof", proof, sizeof proof, sig) == VK_SIGN_FAILED);
}

static void testSignForApproval() {
  resetHooks();
  uint8_t msg[214];
  for (size_t i = 0; i < sizeof msg; ++i) msg[i] = (uint8_t)(i * 3 + 1);
  uint8_t sig[64];

  // A bare button domain signs the bytes as they are.
  CHECK(signForApproval(findDomain("solana"), msg, sizeof msg, sig) == VK_OK);
  CHECK(sSignedLen == sizeof msg && memcmp(sSigned, msg, sizeof msg) == 0);
  CHECK(host_ed25519_verify(msg, sizeof msg, sig, sPub) == 1);

  // A prefixed button domain signs prefix || bytes.
  CHECK(signForApproval(findDomain("bank"), msg, 100, sig) == VK_OK);
  CHECK(sSignedLen == 10 + 100 && memcmp(sSigned, "bank-auth:", 10) == 0 && memcmp(sSigned + 10, msg, 100) == 0);

  // Never an auto domain, never without a domain, never nothing.
  const int before = sSignCalls;
  CHECK(signForApproval(findDomain("pay-proof"), msg, 56, sig) == VK_UNSUPPORTED);
  CHECK(signForApproval(findDomain("store-reg"), msg, 56, sig) == VK_UNSUPPORTED);
  CHECK(signForApproval(nullptr, msg, 56, sig) == VK_UNSUPPORTED);
  CHECK(signForApproval(findDomain("solana"), nullptr, 56, sig) == VK_SIGN_FAILED);
  CHECK(signForApproval(findDomain("solana"), msg, 0, sig) == VK_SIGN_FAILED);

  // The length rules hold here too, whatever the caller checked.
  static uint8_t big[1300];
  memset(big, 1, sizeof big);
  CHECK(signForApproval(findDomain("solana"), big, 1233, sig) == VK_TOO_LONG);    // over max_len
  CHECK(signForApproval(findDomain("bank"), big, 513, sig) == VK_TOO_LONG);
  hostHooks.max_sign_bytes = 242;                                                  // an SE050 key
  CHECK(signForApproval(findDomain("solana"), big, 243, sig) == VK_TOO_LONG);
  CHECK(signForApproval(findDomain("bank"), big, 233, sig) == VK_TOO_LONG);        // 10 + 233 > 242
  CHECK(sSignCalls == before);
  CHECK(signForApproval(findDomain("solana"), big, 242, sig) == VK_OK);
  CHECK(signForApproval(findDomain("bank"), big, 232, sig) == VK_OK);
  hostHooks.max_sign_bytes = 1248;
  CHECK(signForApproval(findDomain("solana"), big, 1232, sig) == VK_OK);
  CHECK(sSignedLen == 1232);
}

// Every signature reaches the sign listeners from signRaw(), the one path: auto and button domains,
// signed and refused by the key; nothing for a request refused before the key was asked.
struct SeenSign { const SignDomain *domain; std::string bytes; bool hasSig; Reason result; };
static std::vector<SeenSign> sSeen;
static void recordSign(const SignEvent &e) {
  sSeen.push_back(SeenSign{e.domain, std::string((const char *)e.signed_bytes, e.signed_len), e.sig != nullptr, e.result});
}
VK_ON_SIGN(t_listener, recordSign);

static void testSignListener() {
  resetHooks();
  sSeen.clear();
  uint8_t proof[56];
  for (size_t i = 0; i < sizeof proof; ++i) proof[i] = (uint8_t)(i + 9);
  uint8_t sig[64];

  CHECK(signAuto("pay-proof", proof, sizeof proof, sig) == VK_OK);
  CHECK(sSeen.size() == 1);
  if (sSeen.size() == 1) {
    CHECK(sSeen[0].domain == findDomain("pay-proof") && sSeen[0].result == VK_OK && sSeen[0].hasSig);
    CHECK(sSeen[0].bytes == std::string("pay-proof:") + std::string((const char *)proof, sizeof proof));
  }
  uint8_t msg[214];
  memset(msg, 3, sizeof msg);
  CHECK(signForApproval(findDomain("solana"), msg, sizeof msg, sig) == VK_OK);
  CHECK(sSeen.size() == 2 && sSeen.back().domain == findDomain("solana") && sSeen.back().bytes.size() == sizeof msg);

  // The key refuses: reported, with no signature.
  hostHooks.sign = hookSignRefuses;
  CHECK(signAuto("pay-proof", proof, sizeof proof, sig) == VK_SIGN_FAILED);
  CHECK(sSeen.size() == 3 && sSeen.back().result == VK_SIGN_FAILED && !sSeen.back().hasSig);
  hostHooks.sign = hookSign;

  // Refused before the key was asked: nothing to log.
  CHECK(signAuto("pay-proof", proof, 55, sig) == VK_BAD_ARG);
  hostHooks.max_sign_bytes = 60;
  CHECK(signAuto("pay-proof", proof, sizeof proof, sig) == VK_TOO_LONG);
  hostHooks.max_sign_bytes = 1248;
  CHECK(signAuto("nope", proof, sizeof proof, sig) == VK_UNSUPPORTED);
  CHECK(sSeen.size() == 3);

  // The store-reg path (hook H10) goes through it too.
  const size_t before = sSeen.size();
  (void)signStoreRegistration(String("not a registration"));
  CHECK(sSeen.size() == before);                                           // refused by its validator
}

static void testBegin() {
  resetHooks();
  static uint8_t msg[1300];
  memset(msg, 1, sizeof msg);
  const Ctx ctx;

  // The refusals, in the order of signing.md. Each case also carries every later fault, so the
  // reason returned shows which check ran first.
  hostHooks.provisioned = false;
  hostHooks.granted = hookGrantNothing;
  hostHooks.busy = true;
  CHECK(begin("nope", msg, 0, ctx, "app") == VK_NOT_PROVISIONED);
  hostHooks.provisioned = true;
  CHECK(begin("nope", msg, 0, ctx, "app") == VK_UNSUPPORTED);          // unknown
  CHECK(begin(nullptr, msg, 0, ctx, "app") == VK_UNSUPPORTED);
  CHECK(begin("pay-proof", msg, 0, ctx, "app") == VK_UNSUPPORTED);     // an auto domain
  CHECK(begin("store-reg", msg, 0, ctx, "app") == VK_UNSUPPORTED);
  CHECK(sAskedPermission == nullptr);
  CHECK(begin("solana", msg, 0, ctx, "app") == VK_DENIED);
  CHECK(sAskedPermission != nullptr && strcmp(sAskedPermission, "sign") == 0);
  hostHooks.granted = nullptr;
  CHECK(begin("solana", msg, 0, ctx, "app") == VK_BUSY);
  hostHooks.busy = false;
  CHECK(begin("solana", msg, 0, ctx, "app") == VK_TOO_LONG);           // len == 0
  CHECK(begin("solana", msg, 1233, ctx, "app") == VK_TOO_LONG);        // over max_len
  CHECK(begin("bank", msg, 513, ctx, "app") == VK_TOO_LONG);
  hostHooks.max_sign_bytes = 242;                                      // an SE050 key
  CHECK(begin("solana", msg, 250, ctx, "app") == VK_TOO_LONG);         // a transfer with a memo
  CHECK(begin("bank", msg, 233, ctx, "app") == VK_TOO_LONG);           // 10 + 233 > 242
  CHECK(begin("solana", nullptr, 214, ctx, "app") == VK_BAD_ARG);
  CHECK(sOpenCalls == 0);
  CHECK(sSignCalls == 0);

  // Everything in order: the decoder and the approval get the request.
  CHECK(begin("solana", msg, 214, ctx, "pay") == VK_OK);
  CHECK(sOpenCalls == 1);
  CHECK(sOpenDomain == findDomain("solana"));
  CHECK(sOpenLen == 214);
  CHECK(strcmp(sOpenApp, "pay") == 0);
  CHECK(begin("bank", msg, 232, ctx, nullptr) == VK_OK);               // 10 + 232 == 242
  CHECK(sOpenCalls == 2 && sOpenDomain == findDomain("bank"));
  CHECK(strcmp(sOpenApp, "") == 0);
  hostHooks.max_sign_bytes = 1248;
  CHECK(begin("solana", msg, 1232, ctx, "pay") == VK_OK);

  // A reason from the decoder is returned as it is.
  sOpenReply = VK_UNDECODABLE;
  CHECK(begin("solana", msg, 214, ctx, "pay") == VK_UNDECODABLE);

  // A firmware-internal button domain has no permission to ask about.
  sOpenReply = VK_OK;
  hostHooks.granted = hookGrantNothing;
  sAskedPermission = nullptr;
  CHECK(begin("t-internal", msg, 10, ctx, "") == VK_OK);
  CHECK(sAskedPermission == nullptr);

  // begin() never signs.
  CHECK(sSignCalls == 0);

  // poll() with no approval engine behind it reports nothing.
  uint8_t sig[64];
  Reason reason = VK_OK;
  CHECK(poll(sig, reason) == Poll::IDLE);
  CHECK(reason == VK_IDLE);
}

static Reason signStoreReg(const String &message, uint8_t sig[64]) {
  return signAuto("store-reg", (const uint8_t *)message.c_str(), message.length(), sig);
}

static void testStoreReg() {
  resetHooks();
  uint8_t sig[64];
  const String own = addressBase58();
  const String head = String("solana-badge-register:") + own + ":";

  // The registration message for this badge's key, with nonces of the kinds a broker sends.
  const String good = head + "3q2-7w_AbC.xyz+/==";
  CHECK(signStoreReg(good, sig) == VK_OK);
  CHECK(sSignedLen == good.length() && memcmp(sSigned, good.c_str(), good.length()) == 0);   // no prefix added
  CHECK(host_ed25519_verify((const uint8_t *)good.c_str(), good.length(), sig, sPub) == 1);
  CHECK(signStoreReg(head + "0", sig) == VK_OK);
  const int signedSoFar = sSignCalls;

  // Anything else is refused before the key is touched.
  CHECK(signStoreReg(head, sig) == VK_BAD_ARG);                                    // no nonce
  CHECK(signStoreReg(String("solana-badge-register:") + own, sig) == VK_BAD_ARG);  // no second ':'
  CHECK(signStoreReg(head + "abc def", sig) == VK_BAD_ARG);                        // a space
  CHECK(signStoreReg(head + "abc\n", sig) == VK_BAD_ARG);                          // a newline
  CHECK(signStoreReg(head + "abc\r\nHost: x", sig) == VK_BAD_ARG);
  CHECK(signStoreReg(head + "abc:def", sig) == VK_BAD_ARG);                        // a third ':'
  CHECK(signStoreReg(head + "abc\"", sig) == VK_BAD_ARG);
  CHECK(signStoreReg(String("Solana-badge-register:") + own + ":abc", sig) == VK_BAD_ARG);
  CHECK(signStoreReg(String("solana-badge-registex:") + own + ":abc", sig) == VK_BAD_ARG);
  CHECK(signStoreReg(String(" ") + good, sig) == VK_BAD_ARG);
  CHECK(signStoreReg(String("pay-proof:") + good, sig) == VK_BAD_ARG);

  // Another badge's key, or this one cut short or run on.
  uint8_t otherPub[32];
  uint8_t otherSeed[32];
  memset(otherSeed, 0x55, sizeof otherSeed);
  host_ed25519_keypair(otherSeed, otherPub);
  char otherText[64];
  CHECK(sol_b58_encode(otherPub, 32, otherText, sizeof otherText) > 0);
  CHECK(signStoreReg(String("solana-badge-register:") + otherText + ":abc", sig) == VK_BAD_ARG);
  CHECK(signStoreReg(String("solana-badge-register:") + own.substring(0, own.length() - 1) + ":abc", sig) == VK_BAD_ARG);
  CHECK(signStoreReg(String("solana-badge-register:") + own + "A:abc", sig) == VK_BAD_ARG);

  // A message with an embedded NUL after a valid-looking start.
  {
    uint8_t raw[128];
    const size_t n = good.length();
    memcpy(raw, good.c_str(), n);
    raw[n] = 0;
    raw[n + 1] = 'x';
    CHECK(signAuto("store-reg", raw, n + 2, sig) == VK_BAD_ARG);
  }

  // Not a Solana message: the start of a legacy message with one required signature.
  {
    uint8_t tx[150];
    memset(tx, 0x41, sizeof tx);
    tx[0] = 0x01; tx[1] = 0x00; tx[2] = 0x02; tx[3] = 0x05;
    CHECK(signAuto("store-reg", tx, sizeof tx, sig) == VK_BAD_ARG);
  }

  // Longer than the row's max_len (200), although upstream allows a nonce of 256.
  {
    String longNonce;
    while (head.length() + longNonce.length() < 201) longNonce += "a";
    CHECK(signStoreReg(head + longNonce, sig) == VK_TOO_LONG);
    CHECK(signStoreReg(head + longNonce.substring(1), sig) == VK_OK);              // exactly 200
    CHECK(sSignCalls == signedSoFar + 1);
  }

  // A badge with no identity registers nothing.
  hostHooks.public_key = nullptr;
  CHECK(signStoreReg(good, sig) == VK_BAD_ARG);
  hostHooks.public_key = sPub;

  // Never through the button path.
  CHECK(begin("store-reg", (const uint8_t *)good.c_str(), good.length(), Ctx(), "") == VK_UNSUPPORTED);

  // signStoreRegistration returns "" on any refusal (on the host it has no base64 to return at all).
  CHECK(signStoreRegistration(head + "abc def").length() == 0);
}

// Last: a bad row joins the registry, and from then on nothing signs.
static void testInvalidTableStopsSigning() {
  resetHooks();
  static SignDomain bad("t-bad", "registry:", false, nullptr, 16, nullptr, validateProof);
  const char *name = "";
  CHECK(runSelfCheck(&name) == 6);
  CHECK(strcmp(name, "t-bad") == 0);
  CHECK(!selfCheckOk());

  uint8_t proof[56] = {0};
  uint8_t sig[64];
  CHECK(signAuto("pay-proof", proof, sizeof proof, sig) == VK_SIGN_FAILED);
  CHECK(signForApproval(findDomain("solana"), proof, sizeof proof, sig) == VK_SIGN_FAILED);
  CHECK(sSignCalls == 0);
}

int main() {
  host_ed25519_keypair(kSeed, sPub);

  testSelfCheckRules();
  testRegistryAndKey();
  testNothingSignsBeforeSelfCheck();

  // What the signer service does at boot.
  const char *name = "unset";
  CHECK(runSelfCheck(&name) == 0);
  CHECK(strcmp(name, "") == 0);
  CHECK(selfCheckOk());

  testSignAuto();
  testSignForApproval();
  testBegin();
  testStoreReg();
  testSignListener();
  testInvalidTableStopsSigning();

  if (sFailures) {
    printf("%d of %d domains checks FAILED\n", sFailures, sChecks);
    return 1;
  }
  printf("all domains tests passed\n");
  return 0;
}
