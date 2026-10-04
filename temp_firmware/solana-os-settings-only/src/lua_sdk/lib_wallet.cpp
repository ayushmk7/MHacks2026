/*
  badge.wallet - DEV STAND-IN for A's wallet API (00-Interfaces §4), enough for
  every harness app (R2, R3, R4, R6) to run end to end. A's firmware replaces it.

    wallet.stub              true, so an app can tell this from A's wallet
    wallet.pubkey()          32 raw bytes, or nil when there is no identity
    wallet.address()         base58 Solana address, or nil
    wallet.key_location()    "se050" | "nvs", or nil
    wallet.time_ok()         true once SNTP has set the clock
    wallet.sign_proof(req_id, nonce, payer_pubkey)
                             64-byte sig over "pay-proof:" || req_id[8] || nonce[16]
                             || payer_pubkey[32]; nil, reason on failure
    wallet.verify_proof(req_id, nonce, payer_pubkey, payee_pubkey, sig)
                             true if sig is a valid PROOF signature by payee_pubkey.
                             Harness only: A's API does this inside check_proof
    wallet.sign_request(req_frame)
                             64-byte sig over "pay-req:" || frame, for a REQ frame
                             (header..name, 00 §5) naming this badge as payee
    wallet.begin_solana(msg, ctx) / wallet.begin_bank(payload, ctx)
                             true, then the approval screen takes over; or nil, reason
    wallet.poll()            "pending" | 64-byte sig | nil, reason

  What the stand-in does NOT do: no registry record, REQ or presence checks
  (ctx is ignored), so every approval is "unverified" and only signs in a
  VK_DEV_ALLOW_UNVERIFIED build. The byte checks are real: wallet_decode.cpp
  implements P1-A §4.2 and 00 §5/§6 and passes the R6 fuzz set on the host
  (tools/wallet_decode_test.cpp).

  Signing domains (00 §3, P1-A §4.5): sign_proof and sign_request only ever
  sign their own prefix over fixed-shape input; Solana and bank bytes are
  signed only after a SELECT press on the firmware-drawn screen.
*/
#include <Arduino.h>

#include "../badge_log.h"
#include "../config.h"
#include "../hal/display.h"
#include "../identity/ed25519.h"
#include "../identity/identity.h"
#include "../net/wifi_mgr.h"
#include "../ui/theme.h"
#include "lua_bindings.h"
#include "lua_runtime.h"
#include "wallet_approval.h"
#include "wallet_decode.h"

extern "C" {
#include "../lua/lauxlib.h"
#include "../lua/lua.h"
}

namespace {

constexpr char PROOF_PREFIX[] = "pay-proof:";
constexpr size_t PROOF_PREFIX_LEN = sizeof(PROOF_PREFIX) - 1;
constexpr size_t PROOF_MSG_LEN = PROOF_PREFIX_LEN + 8 + 16 + 32;
constexpr char REQ_PREFIX[] = "pay-req:";
constexpr size_t REQ_PREFIX_LEN = sizeof(REQ_PREFIX) - 1;
constexpr char BANK_PREFIX[] = "bank-auth:";
constexpr size_t BANK_PREFIX_LEN = sizeof(BANK_PREFIX) - 1;

// An SE050 signature over I2C, or a TweetNaCl verify (two scalar multiplications),
// can outlast the 250 ms callback budget; give the caller room rather than letting
// the budget hook kill the app.
constexpr uint32_t SIGN_EXTENSION_MS = 2000;
// Presses this soon after the screen opens are ignored, so the SELECT that
// started the payment cannot also approve it (P1-A §4.4 "fresh press").
constexpr uint32_t APPROVAL_ARM_MS = 400;

// ── approval state ─────────────────────────────────────────────────────────
enum class Phase : uint8_t { Idle, Pending, Done };
enum class Rail : uint8_t { Solana, Bank };

Phase sPhase = Phase::Idle;
Rail sRail = Rail::Solana;
uint8_t sBytes[BANK_PREFIX_LEN + wallet_decode::MAX_MESSAGE];  // exactly what SELECT signs
size_t sLength = 0;
uint32_t sOpenedAt = 0;
uint8_t sSignature[64];
const char *sReason = nullptr;  // set when Done without a signature
// Display lines, decoded from the bytes at begin_* time.
String sAmount, sTo, sFrom, sNote;

bool pinnedMint(uint8_t out[32]) {
  static uint8_t mint[32];
  static int8_t state = -1;  // -1 not parsed, 0 bad, 1 ok
  if (state < 0) {
    state = wallet_decode::base58To32(VK_HACK_MINT, mint) ? 1 : 0;
    if (!state) badge_log::tagf("wallet", "VK_HACK_MINT is not a valid address; Solana payments refused");
  }
  if (state) memcpy(out, mint, 32);
  return state == 1;
}

String shortKey(const uint8_t key[32]) {
  const String b58 = identity::base58Encode(key, 32);
  return b58.substring(0, 6) + "..." + b58.substring(b58.length() - 4);
}

String formatUnits(uint64_t amount, uint8_t decimals) {
  uint64_t scale = 1;
  for (uint8_t i = 0; i < decimals; ++i) scale *= 10;
  char text[32];
  snprintf(text, sizeof(text), "%llu.%0*llu", (unsigned long long)(amount / scale), (int)decimals,
           (unsigned long long)(amount % scale));
  return text;
}

void finish(const char *reason) {
  sPhase = Phase::Done;
  sReason = reason;
  display::invalidate();  // the app's own frame comes back on the next on_draw
  badge_log::tagf("wallet", "approval ended: %s", reason ? reason : "signed");
}

// Common front of begin_*: busy, then the dev-build gate.
const char *beginGate() {
  if (sPhase != Phase::Idle) return "busy";
  if (!identity::ready()) return "unverified";
#if !VK_DEV_ALLOW_UNVERIFIED
  // No registry check here at all, so without the dev flag nothing may be approved.
  return "unverified";
#endif
  return nullptr;
}

void open(Rail rail) {
  sRail = rail;
  sPhase = Phase::Pending;
  sReason = nullptr;
  sOpenedAt = millis();
  display::invalidate();
  badge_log::tagf("wallet", "approval open: %s %s, %u B", rail == Rail::Solana ? "solana" : "bank",
                  sAmount.c_str(), (unsigned)sLength);
}

}  // namespace

// ── approval screen (wallet_approval.h) ────────────────────────────────────
namespace wallet_approval {

bool active() { return sPhase == Phase::Pending; }

void reset() {
  sPhase = Phase::Idle;
  sReason = nullptr;
  sLength = 0;
}

void button(uint8_t key, bool pressed) {
  if (!pressed || sPhase != Phase::Pending) return;
  if (millis() - sOpenedAt < APPROVAL_ARM_MS) return;
  if (key == BTN_B) return finish("cancelled");
  if (key != BTN_A) return;
  if (!identity::sign(sBytes, sLength, sSignature)) {
    // The SE050 APDU path signs at most se050_apdu::MAX_SIGN_MESSAGE_BYTES (180 B);
    // a Solana message or bank payload is longer. Software keys have no such limit.
    badge_log::tagf("wallet", "sign failed (%s key, %u B)", identity::sourceName(), (unsigned)sLength);
    return finish("sign_failed");
  }
  finish(nullptr);
}

void draw() {
  if (sPhase != Phase::Pending) return;
  const uint32_t elapsed = millis() - sOpenedAt;
  if (elapsed >= VK_APPROVAL_TIMEOUT_MS) return finish("timeout");

  LGFX_Sprite &c = display::canvas();
  c.fillScreen(theme::BG);
  c.fillRect(0, 0, display::width(), 20, theme::WARN);
  display::textCentered("DEV BUILD - UNVERIFIED", display::width() / 2, 6, theme::BLACK, 1);

  display::text(sRail == Rail::Solana ? "Approve Solana payment" : "Approve bank payment", 12, 30,
                theme::WHITE, 2);
  display::text(sAmount.c_str(), 12, 62, theme::GREEN, 3);
  display::text(sTo.c_str(), 12, 100, theme::TEXT, 1);
  display::text(sFrom.c_str(), 12, 114, theme::MUTED, 1);
  display::text(sNote.c_str(), 12, 128, theme::MUTED, 1);

  display::text("Recipient NOT verified: no registry check", 12, 150, theme::WARN, 1);
  const bool clock = wifi_mgr::timeSynced();
  display::text(clock ? "Clock: set by SNTP" : "Clock: NOT set", 12, 164, clock ? theme::MUTED : theme::WARN, 1);
  char left[32];
  snprintf(left, sizeof(left), "Closes in %u s", (unsigned)((VK_APPROVAL_TIMEOUT_MS - elapsed + 999) / 1000));
  display::text(left, 12, 178, theme::MUTED, 1);

  display::text("SELECT: sign", 12, 214, theme::GREEN, 2);
  display::textRight("CANCEL: reject", display::width() - 12, 214, theme::ERR, 2);
  display::touch();
}

}  // namespace wallet_approval

namespace bindings {
namespace {

// Strict: a number is not a byte string here, even though Lua would coerce it.
const char *checkFixed(lua_State *L, int arg, size_t expected, const char *name) {
  if (lua_type(L, arg) != LUA_TSTRING) {
    luaL_error(L, "wallet: %s must be a %d-byte string", name, (int)expected);
  }
  size_t length = 0;
  const char *data = lua_tolstring(L, arg, &length);
  if (length != expected) {
    luaL_error(L, "wallet: %s must be %d bytes, got %d", name, (int)expected, (int)length);
  }
  return data;
}

// "pay-proof:" || req_id || nonce || payer_pubkey, from Lua arguments 1..3.
void proofMessage(lua_State *L, uint8_t out[PROOF_MSG_LEN]) {
  const char *reqId = checkFixed(L, 1, 8, "req_id");
  const char *nonce = checkFixed(L, 2, 16, "nonce");
  const char *payer = checkFixed(L, 3, 32, "payer_pubkey");
  memcpy(out, PROOF_PREFIX, PROOF_PREFIX_LEN);
  memcpy(out + PROOF_PREFIX_LEN, reqId, 8);
  memcpy(out + PROOF_PREFIX_LEN + 8, nonce, 16);
  memcpy(out + PROOF_PREFIX_LEN + 24, payer, 32);
}

int refuse(lua_State *L, const char *reason) {
  lua_pushnil(L);
  lua_pushstring(L, reason);
  return 2;
}

int l_pubkey(lua_State *L) {
  if (!identity::ready()) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushlstring(L, (const char *)identity::publicKey(), 32);
  return 1;
}

int l_address(lua_State *L) {
  if (!identity::ready()) {
    lua_pushnil(L);
    return 1;
  }
  const String address = identity::publicKeyBase58();
  lua_pushstring(L, address.c_str());
  return 1;
}

int l_key_location(lua_State *L) {
  switch (identity::source()) {
    case identity::Source::SecureElement: lua_pushstring(L, "se050"); break;
    case identity::Source::Software: lua_pushstring(L, "nvs"); break;
    default: lua_pushnil(L); break;
  }
  return 1;
}

int l_time_ok(lua_State *L) {
  lua_pushboolean(L, wifi_mgr::timeSynced());
  return 1;
}

int l_sign_proof(lua_State *L) {
  uint8_t message[PROOF_MSG_LEN];
  proofMessage(L, message);
  runtime::extendDeadline(SIGN_EXTENSION_MS);
  uint8_t signature[64];
  if (!identity::sign(message, sizeof(message), signature)) {
    return refuse(L, identity::ready() ? "sign_failed" : "no_identity");
  }
  lua_pushlstring(L, (const char *)signature, sizeof(signature));
  return 1;
}

int l_verify_proof(lua_State *L) {
  uint8_t message[PROOF_MSG_LEN];
  proofMessage(L, message);
  const char *payee = checkFixed(L, 4, 32, "payee_pubkey");
  const char *signature = checkFixed(L, 5, 64, "sig");
  runtime::extendDeadline(SIGN_EXTENSION_MS);
  lua_pushboolean(L, ed25519::verify(message, sizeof(message), (const uint8_t *)signature,
                                     (const uint8_t *)payee));
  return 1;
}

int l_sign_request(lua_State *L) {
  if (lua_type(L, 1) != LUA_TSTRING) return refuse(L, "invalid_req");
  if (!identity::ready()) return refuse(L, "no_identity");
  size_t length = 0;
  const uint8_t *frame = (const uint8_t *)lua_tolstring(L, 1, &length);
  const char *why = wallet_decode::request(frame, length, identity::publicKey());
  if (why) {
    badge_log::tagf("wallet", "sign_request refused: %s", why);
    return refuse(L, "invalid_req");
  }
  uint8_t message[REQ_PREFIX_LEN + wallet_decode::REQ_MAX];
  memcpy(message, REQ_PREFIX, REQ_PREFIX_LEN);
  memcpy(message + REQ_PREFIX_LEN, frame, length);
  runtime::extendDeadline(SIGN_EXTENSION_MS);
  uint8_t signature[64];
  if (!identity::sign(message, REQ_PREFIX_LEN + length, signature)) return refuse(L, "sign_failed");
  lua_pushlstring(L, (const char *)signature, sizeof(signature));
  return 1;
}

int l_begin_solana(lua_State *L) {
  if (const char *gate = beginGate()) return refuse(L, gate);
  if (lua_type(L, 1) != LUA_TSTRING) return refuse(L, "undecodable");
  uint8_t mint[32];
  if (!pinnedMint(mint)) return refuse(L, "undecodable");
  size_t length = 0;
  const uint8_t *msg = (const uint8_t *)lua_tolstring(L, 1, &length);
  wallet_decode::Solana tx;
  const char *why = wallet_decode::solana(msg, length, identity::publicKey(), mint, VK_HACK_DECIMALS, &tx);
  if (why) {
    badge_log::tagf("wallet", "begin_solana refused: %s", why);
    return refuse(L, "undecodable");
  }
  sAmount = formatUnits(tx.amount, VK_HACK_DECIMALS) + " HACK";
  sTo = "to   " + shortKey(tx.destination) + " (token acct)";
  sFrom = "from " + shortKey(tx.source);
  sNote = "no memo";
  if (tx.memo) {
    String memo;
    for (size_t i = 0; i < tx.memoLength && i < 40; ++i) memo += (char)tx.memo[i];
    sNote = "memo " + memo;
  }
  memcpy(sBytes, msg, length);  // raw message: Solana signs it with no prefix
  sLength = length;
  open(Rail::Solana);
  lua_pushboolean(L, true);
  return 1;
}

int l_begin_bank(lua_State *L) {
  if (const char *gate = beginGate()) return refuse(L, gate);
  if (lua_type(L, 1) != LUA_TSTRING) return refuse(L, "undecodable");
  size_t length = 0;
  const uint8_t *payload = (const uint8_t *)lua_tolstring(L, 1, &length);
  wallet_decode::Bank bank;
  const char *why = wallet_decode::bank(payload, length, &bank);
  if (why) {
    badge_log::tagf("wallet", "begin_bank refused: %s", why);
    return refuse(L, "undecodable");
  }
  const String cents(bank.amountCents, bank.amountLength);
  const String padded = cents.length() < 3 ? String("00").substring(0, 3 - cents.length()) + cents : cents;
  sAmount = "$" + padded.substring(0, padded.length() - 2) + "." + padded.substring(padded.length() - 2);
  sTo = "to   " + String(bank.payeeName, bank.payeeLength);
  sFrom = String(bank.action, bank.actionLength) + ", Nessie";
  sNote = "";
  memcpy(sBytes, BANK_PREFIX, BANK_PREFIX_LEN);
  memcpy(sBytes + BANK_PREFIX_LEN, payload, length);
  sLength = BANK_PREFIX_LEN + length;
  open(Rail::Bank);
  lua_pushboolean(L, true);
  return 1;
}

int l_poll(lua_State *L) {
  switch (sPhase) {
    case Phase::Idle: return refuse(L, "idle");
    case Phase::Pending:
      if (millis() - sOpenedAt >= VK_APPROVAL_TIMEOUT_MS) {
        finish("timeout");
        break;
      }
      lua_pushstring(L, "pending");
      return 1;
    case Phase::Done: break;
  }
  // Done: hand the result over once, then free the slot for the next begin_*.
  const char *reason = sReason;
  sPhase = Phase::Idle;
  sLength = 0;
  if (reason) return refuse(L, reason);
  lua_pushlstring(L, (const char *)sSignature, sizeof(sSignature));
  return 1;
}

const luaL_Reg FUNCTIONS[] = {
    {"pubkey", l_pubkey},
    {"address", l_address},
    {"key_location", l_key_location},
    {"time_ok", l_time_ok},
    {"sign_proof", l_sign_proof},
    {"verify_proof", l_verify_proof},
    {"sign_request", l_sign_request},
    {"begin_solana", l_begin_solana},
    {"begin_bank", l_begin_bank},
    {"poll", l_poll},
    {nullptr, nullptr},
};

}  // namespace

void openWallet(lua_State *L) {
  setTable(L, "wallet", FUNCTIONS, nullptr);
  lua_getfield(L, -1, "wallet");
  lua_pushboolean(L, true);
  lua_setfield(L, -2, "stub");
  lua_pop(L, 1);
}

}  // namespace bindings
