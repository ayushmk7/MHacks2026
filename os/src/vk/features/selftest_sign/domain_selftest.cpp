// src/vk/features/selftest_sign/domain_selftest.cpp
// The signing domain of the Tests app's signature check (docs/os/wallet/signing.md): a fixed test
// text and a nonce, shown on an amber hold-to-sign approval that says it pays nothing. The bytes
// signed are "selftest:" + the text, so the signature is useless in every other domain.
// Deleting this folder removes the domain; the Tests app then shows the check as "needs domain".
#include <Arduino.h>
#include <string.h>

#include "../../wallet/approval.h"
#include "../../wallet/signer.h"

namespace {

const char kText[] = "badgeos self test ";
constexpr size_t kHexDigits = 16;
constexpr size_t kLength = sizeof(kText) - 1 + kHexDigits;   // 34

vk::wallet::Reason decodeSelftest(const uint8_t *bytes, size_t len, const vk::wallet::Ctx &ctx,
                                  vk::wallet::ApprovalRequest &out) {
  (void)ctx;
  const size_t head = sizeof(kText) - 1;
  if (bytes == nullptr || len != kLength || memcmp(bytes, kText, head) != 0) return VK_UNDECODABLE;
  for (size_t i = head; i < len; ++i) {
    const char c = (char)bytes[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return VK_UNDECODABLE;
  }
  char nonce[12];
  memcpy(nonce, bytes + head, 8);
  memcpy(nonce + 8, "..", 3);
  strlcpy(out.title, "Self test", sizeof out.title);
  strlcpy(out.headline, "TEST SIGNATURE", sizeof out.headline);
  strlcpy(out.big, "TEST", sizeof out.big);
  strlcpy(out.sub, "a test message only", sizeof out.sub);
  strlcpy(out.lines[0].label, "Signs", sizeof out.lines[0].label);
  strlcpy(out.lines[0].value, "self test text", sizeof out.lines[0].value);
  strlcpy(out.lines[1].label, "Nonce", sizeof out.lines[1].label);
  strlcpy(out.lines[1].value, nonce, sizeof out.lines[1].value);
  strlcpy(out.lines[2].label, "Pays", sizeof out.lines[2].label);
  strlcpy(out.lines[2].value, "nothing", sizeof out.lines[2].value);
  out.line_count = 3;
  out.severity = vk::wallet::Severity::AMBER;
  out.select = vk::wallet::SelectRule::HOLD;
  out.red_reason = VK_OK;
  out.dev_overridable = false;
  return VK_OK;
}

}  // namespace

VK_SIGN_DOMAIN(selftest, "selftest", "selftest:", true, nullptr, kLength, decodeSelftest, nullptr);
