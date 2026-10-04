// Signing domain "solana": one SPL token transfer (solana-payments.md, "The feature folder").
//
// decodeSolana is the glue of checks.md, "Verdict to screen". It never draws and never signs:
//   1. a supplied record whose issuer signature verifies raises the clock floor;
//   2. the host-tested chain vk_check_solana() decides the verdict from the message bytes, the
//      record, the request, the presence table, the clock and the provisioned config;
//   3. the verdict becomes the ApprovalRequest the firmware's own screen shows;
//   4. the answer is always VK_OK: a red request still opens, closed, and reports its reason.
// Nothing shown comes from the app: the amount and token are read from the decoded bytes and the
// token table, the recipient's name from a record whose issuer signature was checked here.
//
// The first byte of an accepted message is 0x01 (one required signature), so no text domain's
// bytes are a valid message (signing.md, "Why domains cannot be confused").
#include <Arduino.h>

#include <string.h>

#include "../../../badge_log.h"
#include "../../core/clock.h"
#include "../../core/config.h"
#include "../../wallet/crypto.h"
#include "../../wallet/pure/sol.h"
#include "../../wallet/pure/vk_checks.h"
#include "../../wallet/pure/vk_record.h"
#include "../../wallet/signer.h"
#include "../../wallet/approval.h"   // last: it removes the Arduino core's DISABLED macro

// ---------------------------------------------------------------------------------------------
// Config keys owned by this feature (platform/config.md, "Keys")
// ---------------------------------------------------------------------------------------------
VK_CONFIG_KEY(issuer_key, "issuer_key", vk::config::Type::KEY32, nullptr,
              vk::config::F_SECURE | vk::config::F_REQUIRED, 0, 0, "public key that signs registry records");
VK_CONFIG_KEY(tokens, "tokens", vk::config::Type::TOKENS, nullptr,
              vk::config::F_SECURE | vk::config::F_REQUIRED, 1, VK_MAX_TOKENS,
              "payment tokens with caps: mint:decimals:symbol:cap:max[,...]");
VK_CONFIG_KEY(record_ttl_s, "record_ttl_s", vk::config::Type::U32, "30", vk::config::F_SECURE, 5, 3600,
              "maximum age of a registry record under SNTP, seconds");

static_assert(SOL_TX_MSG_MAX == 1232, "the solana domain's max_len below is the decoder's limit");

namespace {

using vk::wallet::ApprovalLine;
using vk::wallet::ApprovalRequest;
using vk::wallet::Ctx;
using vk::wallet::Reason;
using vk::wallet::SelectRule;
using vk::wallet::Severity;

using VerifyFn = int (*)(const uint8_t *msg, size_t len, const uint8_t *sig, const uint8_t *pubkey);

// The verifier used while no issuer key is provisioned. The zero key handed to the chain in that
// case is not enough on its own to make every record fail: 32 zero bytes encode a low-order
// Ed25519 point, and a verifier that does not reject such keys accepts forged signatures for it.
int verifyNothing(const uint8_t *, size_t, const uint8_t *, const uint8_t *) { return 0; }

// The verifier handed to step 1 and to the chain: vk_verify_c, remembering its last answer. Step 1
// and check 5 of the chain both verify the same record ("registry:" + record, signature, issuer
// key), and one Ed25519 verification costs about 0.4 s on the badge. A signature check is a pure
// function of its three inputs, so when the next call has byte-for-byte the same message, signature
// and key, the remembered answer is the answer; any difference is verified afresh. The chain is
// unchanged and still asks for every verification itself. The memory is cleared at the start of
// each decode and holds one entry (the request's check replaces the record's).
constexpr size_t MEMO_MSG_MAX = sizeof(VK_RECORD_PREFIX) - 1 + VK_RECORD_MAX;
struct VerifyMemo {
  bool valid;
  int result;
  size_t len;
  uint8_t msg[MEMO_MSG_MAX];
  uint8_t sig[64];
  uint8_t pubkey[32];
};
VerifyMemo sMemo;

int verifyRemembering(const uint8_t *msg, size_t len, const uint8_t *sig, const uint8_t *pubkey) {
  if (msg == nullptr || sig == nullptr || pubkey == nullptr || len == 0 || len > MEMO_MSG_MAX) {
    return vk_verify_c(msg, len, sig, pubkey);
  }
  if (sMemo.valid && sMemo.len == len && memcmp(sMemo.msg, msg, len) == 0 &&
      memcmp(sMemo.sig, sig, sizeof sMemo.sig) == 0 && memcmp(sMemo.pubkey, pubkey, sizeof sMemo.pubkey) == 0) {
    return sMemo.result;
  }
  const int result = vk_verify_c(msg, len, sig, pubkey);
  sMemo.valid = true;
  sMemo.result = result;
  sMemo.len = len;
  memcpy(sMemo.msg, msg, len);
  memcpy(sMemo.sig, sig, sizeof sMemo.sig);
  memcpy(sMemo.pubkey, pubkey, sizeof sMemo.pubkey);
  return result;
}

// One task and no re-entry (the approval this opens is modal), so the chain's working data is
// static: begin() is reached from inside a Lua callback, where the stack is already deep, and the
// verification below needs what is left of it. sVerdict.token points into sTokens.
vk_token_t sTokens[VK_MAX_TOKENS];
vk_verdict_t sVerdict;
vk_record_t sRecord;          // step 1 only

vk_time_source_t timeSource(vk::clock::Source source) {
  switch (source) {
    case vk::clock::Source::SNTP:  return VK_TIME_SNTP;
    case vk::clock::Source::FLOOR: return VK_TIME_FLOOR;
    case vk::clock::Source::NONE:  break;
  }
  return VK_TIME_NONE;
}

// Anything that is not green or amber is red, and anything that is not press or hold is closed.
Severity severityOf(vk_severity_t severity) {
  switch (severity) {
    case VK_SEV_GREEN: return Severity::GREEN;
    case VK_SEV_AMBER: return Severity::AMBER;
    default:           break;
  }
  return Severity::RED;
}

SelectRule selectOf(vk_select_t select) {
  switch (select) {
    case VK_SEL_PRESS: return SelectRule::PRESS;
    case VK_SEL_HOLD:  return SelectRule::HOLD;
    default:           break;
  }
  return SelectRule::DISABLED;
}

// A 32-byte key as base58, shortened to its first 4 characters, "..", and its last 4.
template <size_t N>
void shortKey(char (&out)[N], const uint8_t key[32]) {
  char text[SOL_B58_PUBKEY_MAX];
  const size_t n = sol_b58_encode(key, 32, text, sizeof text);
  if (n < 8) {                      // cannot happen: 32 bytes are 32 to 44 base58 characters
    strlcpy(out, "?", N);
    return;
  }
  char head[5];
  memcpy(head, text, 4);
  head[4] = '\0';
  strlcpy(out, head, N);
  strlcat(out, "..", N);
  strlcat(out, text + n - 4, N);
}

// "<amount> <unit>" in display units, after an optional lead-in: (1250, 2, "HACK") -> "12.50 HACK".
// The number is at most 21 characters (20 digits and a point), so in every buffer this file passes
// (23 characters or more) the digits are never cut; only an amount near the top of the u64 range
// can lose part of its unit.
template <size_t N>
void amountText(char (&out)[N], const char *lead, uint64_t raw, uint8_t decimals, const char *unit) {
  char number[24];
  if (sol_format_amount(raw, decimals, number, sizeof number) == 0) strlcpy(number, "?", sizeof number);
  strlcpy(out, lead, N);
  strlcat(out, number, N);
  strlcat(out, " ", N);
  strlcat(out, unit, N);
}

// The first N-1 characters of a memo. The decoder accepted it as valid UTF-8; the screen's fonts
// are ASCII, so each character outside printable ASCII becomes one '?' (not one per byte).
template <size_t N>
void memoText(char (&out)[N], const uint8_t *memo, size_t len) {
  size_t written = 0, i = 0;
  while (i < len && written + 1 < N) {
    const uint8_t lead = memo[i];
    out[written++] = (lead >= 0x20 && lead <= 0x7E) ? (char)lead : '?';
    i += lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
  }
  out[written] = '\0';
}

// The next free detail row, or nullptr when all four are taken. Rows are added in priority order
// (Requested, Expected, Limit, Account, Kind, Memo), so what does not fit is what matters least.
ApprovalLine *addLine(ApprovalRequest &out, const char *label) {
  const size_t maxLines = sizeof out.lines / sizeof out.lines[0];
  if (out.line_count >= maxLines) return nullptr;
  ApprovalLine *line = &out.lines[out.line_count++];
  strlcpy(line->label, label, sizeof line->label);
  line->value[0] = '\0';
  return line;
}

// Decimals for a request's amount: those of the provisioned token the request names. A request in a
// currency this badge has no token for is shown in raw units.
uint8_t requestDecimals(const char *currency, const vk_token_t *tokens, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(tokens[i].symbol, currency) == 0) return tokens[i].decimals;
  }
  return 0;
}

Reason decodeSolana(const uint8_t *bytes, size_t len, const Ctx &ctx, ApprovalRequest &out) {
  // The issuer key. Without one no record can be trusted: the chain gets a zero key and a verifier
  // that refuses everything.
  uint8_t issuer[32];
  const bool haveIssuer = vk::config::key32("issuer_key", issuer);
  if (!haveIssuer) memset(issuer, 0, sizeof issuer);
  sMemo.valid = false;
  const VerifyFn verify = haveIssuer ? verifyRemembering : verifyNothing;

  // 1. A supplied record whose issuer signature verifies raises the clock floor to its issued_at
  //    (the clock ignores this once SNTP has set it). The chain checks the record again itself;
  //    verifyRemembering answers that second, identical check without repeating the arithmetic.
  const bool recordSupplied = ctx.record != nullptr && ctx.record_len != 0 && ctx.record_sig != nullptr;
  if (recordSupplied && haveIssuer && vk_record_parse(ctx.record, ctx.record_len, &sRecord) == 0 &&
      vk_record_verify(ctx.record, ctx.record_len, ctx.record_sig, issuer, verify) == 1) {
    vk::clock::raiseTo(sRecord.issued_at);
  }

  // 2. The check chain. The clock is read after step 1, so a record that verified is never judged
  //    with no time at all.
  vk_check_input_t in;
  memset(&in, 0, sizeof in);
  in.msg = bytes;
  in.msg_len = len;
  in.record = ctx.record;
  in.record_len = ctx.record_len;
  in.record_sig = ctx.record_sig;
  in.req = ctx.req;
  in.req_len = ctx.req_len;
  in.own_pubkey = vk::wallet::publicKey();          // null with no identity: the chain then refuses
  in.issuer_key = issuer;
  in.tokens = sTokens;
  in.token_count = vk::config::tokens(sTokens);
  in.now = vk::clock::now();
  in.time_source = timeSource(vk::clock::source());
  in.record_ttl_s = vk::config::u32("record_ttl_s");
  in.verify = verify;
  in.presence = vk::wallet::presenceLookup;         // null without the requests feature: presence is NONE
  vk_check_solana(&in, &sVerdict);

  const vk_verdict_t &v = sVerdict;
  if (v.tx_err != SOL_TX_OK) badge_log::tagf("pay", "undecodable: %s", sol_tx_err_name(v.tx_err));

  // 3. The verdict as an ApprovalRequest, field by field from the table in "Verdict to screen".
  //    v.transfer and v.token are valid only when v.decoded, v.record only when v.record_ok (which
  //    implies decoded), v.req only when v.req_ok.
  out = ApprovalRequest{};
  const bool decoded = v.decoded != 0 && v.token != nullptr;
  const bool recordOk = decoded && v.record_ok != 0;

  strlcpy(out.title, "Pay", sizeof out.title);
  strlcpy(out.headline, vk_headline_text(v.headline), sizeof out.headline);

  if (decoded) {
    amountText(out.big, "", v.transfer.amount, v.token->decimals, v.token->symbol);
    strlcpy(out.sub, "to ", sizeof out.sub);
    strlcat(out.sub, recordOk ? v.record.display_name : "unverified recipient", sizeof out.sub);
  }

  // Detail rows, in priority order; at most four are kept.
  if (decoded && v.headline == VK_HL_WRONG_AMOUNT && v.req_ok) {
    if (ApprovalLine *line = addLine(out, "Requested")) {
      amountText(line->value, "", v.req.amount, requestDecimals(v.req.currency, sTokens, in.token_count),
                 v.req.currency);
    }
  }
  if (recordOk && v.headline == VK_HL_WRONG_RECIPIENT) {
    if (ApprovalLine *line = addLine(out, "Expected")) {
      // A record with no token account has nothing to expect the payment at.
      if (v.record.has_solana) shortKey(line->value, v.record.solana_ata);
      else strlcpy(line->value, "none", sizeof line->value);
    }
  }
  // "The cap forced a hold": the chain applies the cap only to a payment that nothing blocked.
  if (decoded && v.severity != VK_SEV_RED && v.token->cap != 0 && v.transfer.amount > v.token->cap) {
    if (ApprovalLine *line = addLine(out, "Limit")) {
      amountText(line->value, "over ", v.token->cap, v.token->decimals, v.token->symbol);
    }
  }
  if (decoded) {
    if (ApprovalLine *line = addLine(out, "Account")) shortKey(line->value, v.transfer.destination);
  }
  if (recordOk) {
    if (ApprovalLine *line = addLine(out, "Kind")) {
      strlcpy(line->value, v.record.kind == VK_KIND_MERCHANT ? "merchant" : "person", sizeof line->value);
    }
  }
  if (decoded && v.transfer.memo != nullptr && v.transfer.memo_len > 0) {
    if (ApprovalLine *line = addLine(out, "Memo")) memoText(line->value, v.transfer.memo, v.transfer.memo_len);
  }

  out.severity = severityOf(v.severity);
  out.select = selectOf(v.select);
  out.red_reason = v.reason;                        // VK_OK unless red
  out.dev_overridable = v.dev_overridable != 0;     // only "no record supplied" (check 4)
  out.led = nullptr;                                // by severity

  // For the history: who was paid, and how much.
  if (recordOk) {
    memcpy(out.recipient, v.record.device_pubkey, sizeof out.recipient);
    strlcpy(out.recipient_name, v.record.display_name, sizeof out.recipient_name);
  } else if (decoded) {
    memcpy(out.recipient, v.transfer.destination, sizeof out.recipient);
  }
  if (decoded) {
    out.amount = v.transfer.amount;
    out.decimals = v.token->decimals;
    strlcpy(out.symbol, v.token->symbol, sizeof out.symbol);
  }

  // 4. Always open the approval, a red one included. The only refusals without a screen are the
  //    ones vk::wallet::begin() makes itself.
  return VK_OK;
}

}  // namespace

VK_SIGN_DOMAIN(solana, "solana", "", true, "sign", 1232, decodeSolana, nullptr);
