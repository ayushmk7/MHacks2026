// Lua bindings of the solana_pay feature (platform/lua-api.md, "badge.wallet: payments"):
//   wallet.begin_solana(msg, [ctx])    open the firmware approval for a transfer message
//   wallet.build_transfer{...}         build that message (apps cannot pack a u64)
//   wallet.wire_tx(sig, msg)           the signed transaction, base64, for sendTransaction
//   wallet.check_record(record, sig)   what a registry record says, for list UIs
// All four need the permission "sign".
//
// Nothing here is trusted by the approval: begin_solana hands over bytes, and the domain's decoder
// (domain_solana.cpp) decodes them and verifies the record again itself.
//
// As in wallet/lua_wallet.cpp: no heap-owning C++ object is alive across a Lua call that can
// raise, and large scratch space comes from the Lua heap, not from the loop task's stack.
#include <Arduino.h>

#include <string.h>

#include "../../../lua_sdk/lua_runtime.h"   // runtime::extendDeadline
#include "../../core/clock.h"
#include "../../core/config.h"
#include "../../host/lua_registry.h"
#include "../../wallet/crypto.h"            // vk_verify_c
#include "../../wallet/lua_wallet.h"        // luaBegin, luaRefuse, base64Encode
#include "../../wallet/pure/sol.h"
#include "../../wallet/pure/vk_record.h"
#include "../../wallet/signer.h"            // publicKey, tokenInfoLookup

namespace {

using vk::wallet::luaRefuse;

// wallet.begin_solana(msg, [ctx]) -> true | nil, reason
int l_begin_solana(lua_State *L) { return vk::wallet::luaBegin(L, "solana", 1, 2); }

// ---------------------------------------------------------------------------
// wallet.build_transfer
// ---------------------------------------------------------------------------

// Reads opts.<key> of the table at stack position 1. An absent optional field gives nullptr.
// Anything but a string (a missing required field included) raises a Lua error: amounts and
// addresses are strings, never Lua numbers. The value stays on the stack so the pointer stays valid.
const char *transferField(lua_State *L, const char *key, bool required, size_t *len) {
  *len = 0;
  const int type = lua_getfield(L, 1, key);
  if (type == LUA_TNIL && !required) return nullptr;
  if (type != LUA_TSTRING) {
    luaL_error(L, "build_transfer: %s must be a string", key);
    return nullptr;
  }
  return lua_tolstring(L, -1, len);
}

// Base58 text of exactly 32 bytes. Text with a NUL inside it is not base58.
bool decodeKey32(const char *text, size_t len, uint8_t out[32]) {
  return text != nullptr && strlen(text) == len && sol_b58_decode(text, out, 32) == 0;
}

// wallet.build_transfer{destination=, amount=, blockhash=, [symbol=], [source=], [memo=]}
//   -> message bytes | nil, "bad_arg" | nil, "unsupported"
// The payer and transfer authority is always this badge. `symbol` defaults to the first token of
// the provisioned table; `source` defaults to this badge's token account for that token, which the
// balance feature learns from the RPC node. "unsupported" means a default could not be resolved.
int l_build_transfer(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_settop(L, 1);
  luaL_checkstack(L, 10, "build_transfer");   // six fields, the scratch block, the results

  size_t destinationLen = 0, amountLen = 0, blockhashLen = 0, symbolLen = 0, sourceLen = 0, memoLen = 0;
  const char *destinationText = transferField(L, "destination", true, &destinationLen);
  const char *amountText = transferField(L, "amount", true, &amountLen);
  const char *blockhashText = transferField(L, "blockhash", true, &blockhashLen);
  const char *symbolText = transferField(L, "symbol", false, &symbolLen);
  const char *sourceText = transferField(L, "source", false, &sourceLen);
  const char *memoText = transferField(L, "memo", false, &memoLen);

  // The token: by symbol, or the first row of the table.
  vk_token_t tokens[VK_MAX_TOKENS];
  const size_t tokenCount = vk::config::tokens(tokens);
  const vk_token_t *token = nullptr;
  if (symbolText == nullptr) {
    if (tokenCount == 0) return luaRefuse(L, VK_UNSUPPORTED);   // no token table: no default token
    token = &tokens[0];
  } else {
    for (size_t i = 0; i < tokenCount && token == nullptr; ++i) {
      if (strlen(tokens[i].symbol) == symbolLen && memcmp(tokens[i].symbol, symbolText, symbolLen) == 0) token = &tokens[i];
    }
    if (token == nullptr) return luaRefuse(L, VK_BAD_ARG);
  }

  // What the app supplied, checked before any default is looked up.
  uint8_t destination[32], blockhash[32], source[32];
  uint64_t amount = 0;
  if (!decodeKey32(destinationText, destinationLen, destination)) return luaRefuse(L, VK_BAD_ARG);
  if (!decodeKey32(blockhashText, blockhashLen, blockhash)) return luaRefuse(L, VK_BAD_ARG);
  if (strlen(amountText) != amountLen || sol_parse_amount(amountText, token->decimals, &amount) != 0 || amount == 0) {
    return luaRefuse(L, VK_BAD_ARG);
  }
  if (sourceText != nullptr && !decodeKey32(sourceText, sourceLen, source)) return luaRefuse(L, VK_BAD_ARG);

  // Defaults.
  if (sourceText == nullptr) {
    vk::wallet::TokenInfo info{};
    if (vk::wallet::tokenInfoLookup == nullptr || !vk::wallet::tokenInfoLookup(token->mint, info) || !info.account_known) {
      return luaRefuse(L, VK_UNSUPPORTED);
    }
    memcpy(source, info.account, sizeof source);
  }
  const uint8_t *payer = vk::wallet::publicKey();
  if (payer == nullptr) return luaRefuse(L, VK_UNSUPPORTED);     // no identity: nothing to pay from

  // The builder refuses what the decoder would refuse: source == destination, a memo that is not
  // valid UTF-8, a message over SOL_TX_MSG_MAX.
  uint8_t *out = (uint8_t *)lua_newuserdatauv(L, SOL_TX_MSG_MAX, 0);
  const size_t n = sol_tx_build_transfer(payer, source, destination, token->mint, blockhash, amount, token->decimals,
                                         memoLen != 0 ? (const uint8_t *)memoText : nullptr, memoLen,
                                         out, SOL_TX_MSG_MAX);
  if (n == 0) return luaRefuse(L, VK_BAD_ARG);
  lua_pushlstring(L, (const char *)out, n);
  return 1;
}

// ---------------------------------------------------------------------------
// wallet.wire_tx
// ---------------------------------------------------------------------------

// wallet.wire_tx(sig, msg) -> base64 of 0x01 ‖ sig ‖ msg | nil, "bad_arg"
// 0x01 is the compact-u16 signature count: the messages this badge signs have one signer.
int l_wire_tx(lua_State *L) {
  size_t sigLen = 0, msgLen = 0;
  const char *sig = luaL_checklstring(L, 1, &sigLen);
  const char *msg = luaL_checklstring(L, 2, &msgLen);
  if (sigLen != SOL_SIG_LEN || msgLen == 0 || msgLen > SOL_TX_MSG_MAX) return luaRefuse(L, VK_BAD_ARG);

  const size_t rawLen = 1 + SOL_SIG_LEN + msgLen;
  uint8_t *raw = (uint8_t *)lua_newuserdatauv(L, rawLen, 0);
  raw[0] = 0x01;
  memcpy(raw + 1, sig, SOL_SIG_LEN);
  memcpy(raw + 1 + SOL_SIG_LEN, msg, msgLen);

  const size_t textLen = vk::wallet::base64Length(rawLen);
  luaL_Buffer buffer;
  char *text = luaL_buffinitsize(L, &buffer, textLen);
  vk::wallet::base64Encode(raw, rawLen, text);
  luaL_pushresultsize(&buffer, textLen);
  return 1;
}

// ---------------------------------------------------------------------------
// wallet.check_record
// ---------------------------------------------------------------------------

void setBase58(lua_State *L, const char *field, const uint8_t key[32]) {
  char text[SOL_B58_PUBKEY_MAX];
  lua_pushlstring(L, text, sol_b58_encode(key, 32, text, sizeof text));
  lua_setfield(L, -2, field);
}

// Unix seconds are 32 bits unsigned and a Lua integer here is 32 bits signed: a value past 2038
// is pushed as a float (inexact, but still ordered correctly against any other time).
void setSeconds(lua_State *L, const char *field, uint32_t seconds) {
  if (seconds <= (uint32_t)LUA_MAXINTEGER) lua_pushinteger(L, (lua_Integer)seconds);
  else lua_pushnumber(L, (lua_Number)seconds);
  lua_setfield(L, -2, field);
}

// wallet.check_record(record, sig)
//   -> {ok, reason, display_name, device_pubkey, kind, solana_wallet, solana_ata, expiry, status, issued_at}
// Always a table. `ok` is true only if the issuer signature is valid, the status is active and,
// when the clock has a trusted source, the record has not expired; `reason` is then "ok", else
// "unverified", "revoked" or "expired". The record's own fields are present only when the issuer
// signature is valid: a name the issuer did not sign is never handed to an app as a record's name.
// solana_wallet and solana_ata are absent when the record has none. For list UIs only: this neither
// checks the record's freshness nor raises the clock; the approval verifies the record again itself.
int l_check_record(lua_State *L) {
  size_t recordLen = 0, sigLen = 0;
  const uint8_t *record = (const uint8_t *)luaL_checklstring(L, 1, &recordLen);
  const uint8_t *sig = (const uint8_t *)luaL_checklstring(L, 2, &sigLen);

  vk_record_t parsed;
  uint8_t issuer[32];
  bool verified = false;
  vk_reason_t reason = VK_UNVERIFIED;
  // The cheap tests first; an unprovisioned badge has no issuer key, so no record can verify.
  if (sigLen == SOL_SIG_LEN && recordLen != 0 && recordLen <= VK_RECORD_MAX &&
      vk_record_parse(record, recordLen, &parsed) == 0 && vk::config::key32("issuer_key", issuer)) {
    ::runtime::extendDeadline(2500);     // one Ed25519 verification inside a Lua callback
    verified = vk_record_verify(record, recordLen, sig, issuer, vk_verify_c) == 1;
  }
  if (verified) {
    if (parsed.status != VK_STATUS_ACTIVE) reason = VK_REVOKED;
    else if (vk::clock::ok() && !(parsed.expiry > vk::clock::now())) reason = VK_EXPIRED;
    else reason = VK_OK;
  }

  lua_createtable(L, 0, verified ? 10 : 2);
  lua_pushboolean(L, reason == VK_OK);
  lua_setfield(L, -2, "ok");
  lua_pushstring(L, vk::wallet::reasonName(reason));
  lua_setfield(L, -2, "reason");
  if (!verified) return 1;

  lua_pushstring(L, parsed.display_name);
  lua_setfield(L, -2, "display_name");
  setBase58(L, "device_pubkey", parsed.device_pubkey);
  lua_pushstring(L, parsed.kind == VK_KIND_MERCHANT ? "merchant" : "person");
  lua_setfield(L, -2, "kind");
  if (parsed.has_solana) {
    setBase58(L, "solana_wallet", parsed.solana_wallet);
    setBase58(L, "solana_ata", parsed.solana_ata);
  }
  setSeconds(L, "expiry", parsed.expiry);
  lua_pushstring(L, parsed.status == VK_STATUS_ACTIVE ? "active" : "revoked");
  lua_setfield(L, -2, "status");
  setSeconds(L, "issued_at", parsed.issued_at);
  return 1;
}

// ---------------------------------------------------------------------------
// Registration (execution plan 5.3). One line per function.
// ---------------------------------------------------------------------------

VK_LUA_FUNCTION(begin_solana, "wallet", "begin_solana", "sign", l_begin_solana);
VK_LUA_FUNCTION(build_transfer, "wallet", "build_transfer", "sign", l_build_transfer);
VK_LUA_FUNCTION(wire_tx, "wallet", "wire_tx", "sign", l_wire_tx);
VK_LUA_FUNCTION(check_record, "wallet", "check_record", "sign", l_check_record);

}  // namespace
