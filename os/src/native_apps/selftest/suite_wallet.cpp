// src/native_apps/selftest/suite_wallet.cpp
// WALLET: the key, the address, the verifier, the signing domain table, the provisioning, and one
// real signature made through the approval (wallet/signing.md, wallet/approval.md).
//
//   key          where the private key is (honest wording: a software key is in flash)
//   address      the base58 of publicKey() is addressBase58()
//   limit        maxSignBytes() is what signing.md gives for that key location
//   selfcheck    the domain table passed the boot self-check (selfCheckOk())
//   vectors      RFC 8032 tests 1 to 3 verify, and a copy of each with one bit changed does not
//   provisioned  `--` while the badge is not provisioned (nothing can be signed then)
//   issuer       the issuer key is set (a public key; shown shortened)
//   tokens       the token table has an entry
//   signature    manual: a harmless test message, approved by a person holding SELECT, signed by
//                the one signing path (vk::wallet::begin), then verified here against the badge's
//                public key. A refusal, a cancel or a timeout is "--", never FAIL.
//
// The signature test needs a signing domain for test messages. None of the domains in signing.md
// fits (each one's decoder accepts only its own kind of bytes), so this file names one, SIGN_DOMAIN,
// and looks it up at run time: until a feature registers it the row ends "-- needs domain". The
// domain to add is specified in apps.md ("Self test"). Nothing here signs except through begin().
#include "../../vk/sdk/badge_sdk.hpp"

#include <stdio.h>
#include <string.h>

#include "../../vk/ui/receipt.h"
#include "../../vk/wallet/crypto.h"
#include "../../vk/wallet/pure/sol.h"     // sol_b58_encode
#include "selftest.h"
#include "shared.h"
#include "suites.h"
#include "ui.h"

namespace selftest {
namespace {

namespace rc = vk::ui::receipt;

// ---- the seam: the signing domain for test messages -------------------------------------------
// Registered (or not) by src/vk/features/selftest_sign/domain_selftest.cpp. Its decoder accepts
// exactly MESSAGE_TEXT followed by 16 lower-case hex digits.
constexpr char SIGN_DOMAIN[] = "selftest";
constexpr char MESSAGE_TEXT[] = "badgeos self test ";
constexpr size_t NONCE_BYTES = 8;
constexpr size_t MESSAGE_LEN = sizeof MESSAGE_TEXT - 1 + 2 * NONCE_BYTES;   // 34
constexpr uint32_t RESULT_MARGIN_MS = 15000;  // after approval_tmo_s: the screen's own timeout comes first

// signing.md, "Key": the largest message each key location can sign.
constexpr size_t SE050_MAX = 242, SOFTWARE_MAX = 1248;

void shortKey(const uint8_t key[32], char *out, size_t cap) {
  char b58[48];
  const size_t n = sol_b58_encode(key, 32, b58, sizeof b58);
  if (n >= 8) snprintf(out, cap, "%.4s..%s", b58, b58 + n - 4);
  else snprintf(out, cap, "?");
}

// ---- automatic --------------------------------------------------------------------------------

void checkKey(Ctx &c) {
  const char *location = vk::wallet::keyLocation();
  if (vk::wallet::publicKey() == nullptr) {
    c.finish(State::Fail, "no identity (%s)", location);
  } else if (strcmp(location, "software") == 0) {
    c.finish(State::Ok, "software, in flash");     // never described as hardware-protected
  } else {
    c.finish(State::Ok, "%s", location);
  }
}

void checkAddress(Ctx &c) {
  const uint8_t *key = vk::wallet::publicKey();
  if (key == nullptr) {
    c.finish(State::Skip, "no key");
    return;
  }
  char b58[48];
  const size_t n = sol_b58_encode(key, 32, b58, sizeof b58);
  const String address = vk::wallet::addressBase58();
  if (n == 0 || address != b58) {
    c.finish(State::Fail, "address is not the key");
    return;
  }
  c.finish(State::Ok, "%.4s..%s", b58, b58 + n - 4);
}

void checkLimit(Ctx &c) {
  const char *location = vk::wallet::keyLocation();
  const size_t limit = vk::wallet::maxSignBytes();
  size_t expected = 0;
  if (strcmp(location, "se050") == 0) expected = SE050_MAX;
  else if (strcmp(location, "software") == 0) expected = SOFTWARE_MAX;
  if (expected == 0) c.finish(State::Skip, "no key");
  else if (limit != expected) c.finish(State::Fail, "%u bytes, expected %u", (unsigned)limit, (unsigned)expected);
  else c.finish(State::Ok, "%u bytes", (unsigned)limit);
}

void checkSelfcheck(Ctx &c) {
  if (vk::wallet::selfCheckOk()) c.finish(State::Ok, "domain table ok");
  else c.finish(State::Fail, "domain table invalid");
}

// Steps 0..2 verify the three vectors; steps 3..5 each verify a copy with one bit changed (the
// signature of test 1, whose message is empty; the message of tests 2 and 3). One per frame.
struct VectorScratch { uint8_t good, forged; uint32_t ms; };
void checkVectors(Ctx &c) {
  VectorScratch &s = c.scratch<VectorScratch>();
  if (c.step < RFC8032_COUNT) {
    const Ed25519Vector &v = RFC8032[c.step];
    const uint32_t before = (uint32_t)micros();
    if (vk::wallet::verify(v.message, v.length, v.signature, v.key)) ++s.good;
    s.ms += (uint32_t)micros() - before;
    ++c.step;
    return;
  }
  if (c.step < 2 * RFC8032_COUNT) {
    const Ed25519Vector &v = RFC8032[c.step - RFC8032_COUNT];
    uint8_t signature[64], message[4] = {};
    memcpy(signature, v.signature, sizeof signature);
    if (v.length) memcpy(message, v.message, v.length);
    if (v.length) message[0] ^= 0x01;
    else signature[63] ^= 0x01;
    if (vk::wallet::verify(v.length ? message : nullptr, v.length, signature, v.key)) ++s.forged;
    ++c.step;
    if (c.step < 2 * RFC8032_COUNT) return;
  }
  if (s.good != RFC8032_COUNT) c.finish(State::Fail, "%u of %u vectors refused", (unsigned)(RFC8032_COUNT - s.good), (unsigned)RFC8032_COUNT);
  else if (s.forged) c.finish(State::Fail, "%u forgeries accepted", (unsigned)s.forged);
  else c.finish(State::Ok, "%u ok, %u forged refused, %lu ms", (unsigned)RFC8032_COUNT, (unsigned)RFC8032_COUNT,
                (unsigned long)((s.ms + 500) / 1000));
}

void checkProvisioned(Ctx &c) {
  if (vk::config::provisioned()) c.finish(State::Ok, "provisioned");
  else c.finish(State::Skip, "setup needed");
}

void checkIssuer(Ctx &c) {
  uint8_t key[32];
  if (!vk::config::key32("issuer_key", key)) {
    c.finish(State::Skip, "not set");
    return;
  }
  char text[16];
  shortKey(key, text, sizeof text);
  c.finish(State::Ok, "%s", text);
}

void checkTokens(Ctx &c) {
  vk_token_t tokens[VK_MAX_TOKENS];
  const size_t n = vk::config::tokens(tokens);
  if (n == 0) c.finish(State::Skip, "none");
  else c.finish(State::Ok, "%u, first %s %u dp", (unsigned)n, tokens[0].symbol, (unsigned)tokens[0].decimals);
}

// ---- manual: a real signature through the approval --------------------------------------------

enum : uint8_t { SIGN_WAITING = 1 };
struct SignScratch {
  char message[MESSAGE_LEN + 1];
  uint32_t deadline;                     // millis(): no result by then ends the test as "--"
};

const vk::wallet::SignDomain *signDomain() {
  const vk::wallet::SignDomain *domain = vk::wallet::findDomain(SIGN_DOMAIN);
  return domain != nullptr && domain->needs_button ? domain : nullptr;
}

void signStart(Ctx &c) {
  SignScratch &s = c.scratch<SignScratch>();
  if (signDomain() == nullptr) {
    c.finish(State::Skip, "needs domain");
    return;
  }
  if (vk::wallet::publicKey() == nullptr) {
    c.finish(State::Skip, "no key");
    return;
  }
  uint8_t nonce[NONCE_BYTES];
  vk::wallet::randomBytes(nonce, sizeof nonce);
  int at = snprintf(s.message, sizeof s.message, "%s", MESSAGE_TEXT);
  for (size_t i = 0; i < sizeof nonce; ++i) at += snprintf(s.message + at, sizeof s.message - at, "%02x", (unsigned)nonce[i]);

  vk::wallet::Ctx none;
  const vk::wallet::Reason reason =
      vk::wallet::begin(SIGN_DOMAIN, (const uint8_t *)s.message, MESSAGE_LEN, none, "selftest");
  switch (reason) {
    case VK_OK:
      break;
    // The badge or the moment, not the signing path: nothing was tested.
    case VK_NOT_PROVISIONED: case VK_BUSY: case VK_DENIED: case VK_UNSUPPORTED:
      c.finish(State::Skip, "%s", vk::wallet::reasonName(reason));
      return;
    default:                 // the path refused a well-formed test message
      c.finish(State::Fail, "begin: %s", vk::wallet::reasonName(reason));
      return;
  }
  c.step = SIGN_WAITING;
  s.deadline = c.now + vk::config::u32("approval_tmo_s") * 1000u + RESULT_MARGIN_MS;
}

// The approval has the screen while it is open and this app is not run; the first frame after it
// closes finds the result.
void signUpdate(Ctx &c) {
  SignScratch &s = c.scratch<SignScratch>();
  if (c.step != SIGN_WAITING) return;
  uint8_t sig[64];
  vk::wallet::Reason why = VK_OK;
  switch (vk::wallet::poll(sig, why)) {
    case vk::wallet::Poll::PENDING:
      break;
    case vk::wallet::Poll::SIGNED: {
      const vk::wallet::SignDomain *domain = signDomain();
      const uint8_t *key = vk::wallet::publicKey();
      // What the signer signed: the domain's prefix, then the bytes.
      uint8_t signedBytes[24 + MESSAGE_LEN];
      const size_t prefix = domain && domain->prefix ? strlen(domain->prefix) : 0;
      if (domain == nullptr || key == nullptr || prefix > 24) {
        c.finish(State::Fail, "signed, cannot verify");
        return;
      }
      memcpy(signedBytes, domain->prefix, prefix);
      memcpy(signedBytes + prefix, s.message, MESSAGE_LEN);
      const uint32_t before = (uint32_t)micros();
      const bool valid = vk::wallet::verify(signedBytes, prefix + MESSAGE_LEN, sig, key);
      const uint32_t ms = ((uint32_t)micros() - before + 500) / 1000;
      if (valid) c.finish(State::Ok, "signed, verified %lu ms", (unsigned long)ms);
      else c.finish(State::Fail, "signature does not verify");
      return;
    }
    case vk::wallet::Poll::FAILED:
      if (why == VK_SIGN_FAILED) c.finish(State::Fail, "%s", vk::wallet::reasonName(why));
      else c.finish(State::Skip, "%s", vk::wallet::reasonName(why));      // cancelled, timeout
      return;
    case vk::wallet::Poll::IDLE:
      c.finish(State::Skip, "no result");
      return;
  }
  if ((int32_t)(c.now - s.deadline) >= 0) c.finish(State::Skip, "no result in time");
}

void signDraw(Ctx &c) {
  SignScratch &s = c.scratch<SignScratch>();
  ui::frame("SIGNATURE TEST");
  rc::row(ui::X0, ui::X1, ui::FIRST_ROW_Y, "DOMAIN", SIGN_DOMAIN);
  rc::row(ui::X0, ui::X1, ui::FIRST_ROW_Y + ui::ROW_PITCH, "MESSAGE", s.message);
  rc::row(ui::X0, ui::X1, ui::FIRST_ROW_Y + 2 * ui::ROW_PITCH, "STATUS", "waiting for the approval");
  display::textCentered("Hold SELECT on the approval to sign it.", display::width() / 2, ui::NOTE_Y, ui::sub());
  rc::footer("", "CANCEL stop");
}

void signKey(Ctx &c, uint8_t key) {
  if (key != BTN_B) return;
  uint8_t sig[64];
  vk::wallet::Reason why;
  vk::wallet::poll(sig, why);            // a result nobody will read is not left waiting
  c.finish(State::Skip, "stopped");
}

const ManualOps SIGN_TEST = {signStart, signUpdate, signDraw, signKey, false};

const Check TABLE[] = {
    {"key", "KEY", Kind::Auto, Profile::Any, checkKey, nullptr, 0},
    {"address", "ADDRESS", Kind::Auto, Profile::Any, checkAddress, nullptr, 0},
    {"limit", "SIGN LIMIT", Kind::Auto, Profile::Any, checkLimit, nullptr, 0},
    {"selfcheck", "DOMAIN TABLE", Kind::Auto, Profile::Any, checkSelfcheck, nullptr, 0},
    {"vectors", "ED25519", Kind::Auto, Profile::Any, checkVectors, nullptr, 0},
    {"provisioned", "PROVISIONED", Kind::Auto, Profile::Any, checkProvisioned, nullptr, 0},
    {"issuer", "ISSUER KEY", Kind::Auto, Profile::Any, checkIssuer, nullptr, 0},
    {"tokens", "TOKENS", Kind::Auto, Profile::Any, checkTokens, nullptr, 0},
    {"signature", "SIGNATURE", Kind::Manual, Profile::Any, nullptr, &SIGN_TEST, 0},
};

}  // namespace

const Suite WALLET = {
    "wallet", "WALLET", Profile::Any, TABLE, sizeof TABLE / sizeof TABLE[0],
    nullptr, nullptr, nullptr, false,
};

}  // namespace selftest
