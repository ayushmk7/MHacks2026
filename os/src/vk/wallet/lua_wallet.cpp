// badge.wallet (identity, config, begin, poll) and badge.codec (platform/lua-api.md).
//
// Nothing here can sign. wallet.begin hands bytes to vk::wallet::begin(), which decodes them and
// opens the firmware approval; wallet.poll reads the result the approval engine stored. The app
// never holds a key and never sees the screen it is asking for.
//
// Two rules every binding in this file keeps:
//   - luaL_error and an out-of-memory error leave by longjmp and skip C++ destructors, so no
//     heap-owning object (an Arduino String) is alive across a Lua call that can raise.
//   - Large scratch space comes from the Lua heap (luaL_Buffer, lua_newuserdatauv), not from the
//     loop task's stack and not from a static buffer: it is counted against the app's memory cap
//     and is freed by the collector whatever happens.
#include "lua_wallet.h"

#include <Arduino.h>

#include <string.h>

#include "../../lua_sdk/lua_runtime.h"   // runtime::extendDeadline, runtime::currentApp
#include "../core/clock.h"
#include "../core/config.h"
#include "../host/lua_registry.h"
#include "crypto.h"            // vk_verify_c
#include "pure/sol.h"
#include "signer.h"

namespace vk::wallet {

// ---------------------------------------------------------------------------
// Shared with the bindings in feature folders (lua_wallet.h)
// ---------------------------------------------------------------------------

int luaRefuse(lua_State *L, Reason reason) {
  lua_pushnil(L);
  lua_pushstring(L, reasonName(reason));
  return 2;
}

namespace {

const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const char kHex[] = "0123456789abcdef";

// One verification or signature inside a Lua callback (signing.md, "Crypto backend").
constexpr uint32_t kCryptoBudgetMs = 2500;

}  // namespace

size_t base64Length(size_t len) { return (len + 2) / 3 * 4; }

void base64Encode(const uint8_t *in, size_t len, char *out) {
  size_t i = 0;
  for (; i + 3 <= len; i += 3) {
    const uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | (uint32_t)in[i + 2];
    *out++ = kBase64[v >> 18];
    *out++ = kBase64[(v >> 12) & 63];
    *out++ = kBase64[(v >> 6) & 63];
    *out++ = kBase64[v & 63];
  }
  if (len - i == 1) {
    const uint32_t v = (uint32_t)in[i] << 16;
    *out++ = kBase64[v >> 18];
    *out++ = kBase64[(v >> 12) & 63];
    *out++ = '=';
    *out++ = '=';
  } else if (len - i == 2) {
    const uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8;
    *out++ = kBase64[v >> 18];
    *out++ = kBase64[(v >> 12) & 63];
    *out++ = kBase64[(v >> 6) & 63];
    *out++ = '=';
  }
}

// ---------------------------------------------------------------------------
// luaBegin: the ctx parsing shared by wallet.begin, begin_solana, begin_bank
// ---------------------------------------------------------------------------

namespace {

// Reads ctx.<key>. Absent gives nullptr; a value that is not a string raises a Lua error.
// The value is left on the stack on purpose: the pointer returned is into the Lua string, and the
// stack slot keeps that string alive until vk::wallet::begin() has read it (a ctx table with an
// __index metamethod could otherwise hand out a string nothing else refers to).
const uint8_t *ctxField(lua_State *L, int ctxIndex, const char *key, size_t *len) {
  *len = 0;
  const int type = lua_getfield(L, ctxIndex, key);
  if (type == LUA_TNIL) return nullptr;
  if (type != LUA_TSTRING) {
    luaL_error(L, "ctx.%s must be a string", key);
    return nullptr;
  }
  return (const uint8_t *)lua_tolstring(L, -1, len);
}

}  // namespace

int luaBegin(lua_State *L, const char *domain, int bytesIndex, int ctxIndex) {
  bytesIndex = lua_absindex(L, bytesIndex);
  ctxIndex = lua_absindex(L, ctxIndex);

  size_t len = 0;
  const uint8_t *bytes = (const uint8_t *)luaL_checklstring(L, bytesIndex, &len);

  Ctx ctx;
  if (!lua_isnoneornil(L, ctxIndex)) {
    luaL_checktype(L, ctxIndex, LUA_TTABLE);
    luaL_checkstack(L, 5, "wallet.begin");   // three ctx values, then the results

    size_t recordLen = 0, sigLen = 0, reqLen = 0;
    const uint8_t *record = ctxField(L, ctxIndex, "record", &recordLen);
    const uint8_t *recordSig = ctxField(L, ctxIndex, "record_sig", &sigLen);
    const uint8_t *req = ctxField(L, ctxIndex, "req", &reqLen);

    // The signer's Ctx has no length for the signature: it is 64 bytes or it is not passed on.
    if (recordSig != nullptr && sigLen != 64) return luaRefuse(L, VK_BAD_ARG);

    // An empty string is "not supplied", stated the one way the check chain reads it.
    if (record != nullptr && recordLen != 0) { ctx.record = record; ctx.record_len = recordLen; }
    ctx.record_sig = recordSig;
    if (req != nullptr && reqLen != 0) { ctx.req = req; ctx.req_len = reqLen; }
  }

  // The decoder runs inside this callback, and a software Ed25519 verification can take about a
  // second (signing.md, finding F7): one extension per signed item it will verify, plus one more.
  unsigned extensions = 1;
  if (ctx.record != nullptr && ctx.record_sig != nullptr) ++extensions;
  if (ctx.req != nullptr) ++extensions;
  for (unsigned i = 0; i < extensions; ++i) ::runtime::extendDeadline(kCryptoBudgetMs);

  // begin() copies the bytes and reads ctx only while it runs; nothing is kept from the Lua state.
  const Reason reason = vk::wallet::begin(domain, bytes, len, ctx, ::runtime::currentApp().c_str());
  if (reason != VK_OK) return luaRefuse(L, reason);
  lua_pushboolean(L, 1);
  return 1;
}

// ---------------------------------------------------------------------------
// luaCheckPayment: the payee's check of a fetched transaction
// ---------------------------------------------------------------------------

int luaRefuseDetail(lua_State *L, Reason reason, const char *detail) {
  lua_pushnil(L);
  lua_pushstring(L, reasonName(reason));
  lua_pushstring(L, detail);
  return 3;
}

namespace {

// expected.<key> of the table at `index`: nullptr when absent and optional; a value that is not a
// string (a missing required one included) raises a Lua error, as build_transfer's fields do. The
// value stays on the stack, so the pointer stays valid.
const char *expectField(lua_State *L, int index, const char *key, bool required, size_t *len) {
  *len = 0;
  const int type = lua_getfield(L, index, key);
  if (type == LUA_TNIL && !required) return nullptr;
  if (type != LUA_TSTRING) {
    luaL_error(L, "expected.%s must be a string", key);
    return nullptr;
  }
  return lua_tolstring(L, -1, len);
}

// Base58 text of exactly n bytes (32 or 64). Text with a NUL inside it is not base58.
bool decodeBase58(const char *text, size_t len, uint8_t *out, size_t n) {
  return text != nullptr && strlen(text) == len && sol_b58_decode(text, out, n) == 0;
}

}  // namespace

int luaCheckPayment(lua_State *L, int txIndex, int expectIndex, CheckedPayment &out) {
  txIndex = lua_absindex(L, txIndex);
  expectIndex = lua_absindex(L, expectIndex);
  size_t txLen = 0;
  const uint8_t *tx = (const uint8_t *)luaL_checklstring(L, txIndex, &txLen);
  luaL_checktype(L, expectIndex, LUA_TTABLE);
  luaL_checkstack(L, 8, "check payment");   // six fields, then the results

  size_t amountLen = 0, reqLen = 0, symbolLen = 0, toLen = 0, payerLen = 0, sigLen = 0;
  const char *amountText = expectField(L, expectIndex, "amount", true, &amountLen);
  const char *reqText = expectField(L, expectIndex, "req_id", true, &reqLen);
  const char *symbolText = expectField(L, expectIndex, "symbol", false, &symbolLen);
  const char *toText = expectField(L, expectIndex, "to", false, &toLen);
  const char *payerText = expectField(L, expectIndex, "payer", false, &payerLen);
  const char *sigText = expectField(L, expectIndex, "sig", false, &sigLen);

  memset(&out, 0, sizeof out);
  // The token: by symbol, or the first row of the table (as build_transfer).
  vk_token_t tokens[VK_MAX_TOKENS];
  const size_t tokenCount = vk::config::tokens(tokens);
  const vk_token_t *token = nullptr;
  if (symbolText == nullptr) {
    if (tokenCount == 0) return luaRefuseDetail(L, VK_UNSUPPORTED, "token");
    token = &tokens[0];
  } else {
    for (size_t i = 0; i < tokenCount && token == nullptr; ++i) {
      if (strlen(tokens[i].symbol) == symbolLen && memcmp(tokens[i].symbol, symbolText, symbolLen) == 0) token = &tokens[i];
    }
    if (token == nullptr) return luaRefuseDetail(L, VK_BAD_ARG, "symbol");
  }
  out.token = *token;

  // What the app supplied, checked before any default is looked up.
  uint64_t amount = 0;
  if (strlen(amountText) != amountLen || sol_parse_amount(amountText, token->decimals, &amount) != 0 || amount == 0) {
    return luaRefuseDetail(L, VK_BAD_ARG, "amount");
  }
  if (vk_req_id_parse(reqText, reqLen, out.req_id) != 0) return luaRefuseDetail(L, VK_BAD_ARG, "req_id");
  uint8_t to[32], payer[32], sig[64];
  if (toText != nullptr && !decodeBase58(toText, toLen, to, sizeof to)) return luaRefuseDetail(L, VK_BAD_ARG, "to");
  if (payerText != nullptr && !decodeBase58(payerText, payerLen, payer, sizeof payer)) return luaRefuseDetail(L, VK_BAD_ARG, "payer");
  if (sigText != nullptr) {
    // The 64 raw bytes a RESULT frame's ref carries, or the signature in base58.
    if (sigLen == sizeof sig) memcpy(sig, sigText, sizeof sig);
    else if (!decodeBase58(sigText, sigLen, sig, sizeof sig)) return luaRefuseDetail(L, VK_BAD_ARG, "sig");
  }
  // Default recipient: this badge's own token account for the token, as the balance feature knows it.
  if (toText == nullptr) {
    TokenInfo info{};
    if (tokenInfoLookup == nullptr || !tokenInfoLookup(token->mint, info) || !info.account_known) {
      return luaRefuseDetail(L, VK_UNSUPPORTED, "to");
    }
    memcpy(to, info.account, sizeof to);
  }

  vk_pay_expect_t expect;
  memset(&expect, 0, sizeof expect);
  expect.to = to;
  expect.mint = token->mint;
  expect.decimals = token->decimals;
  expect.amount = amount;
  expect.req_id = out.req_id;
  expect.payer = payerText != nullptr ? payer : nullptr;
  expect.sig = sigText != nullptr ? sig : nullptr;
  expect.verify = vk_verify_c;
  ::runtime::extendDeadline(kCryptoBudgetMs);       // the payer's signature over the message
  const vk_pay_err_t err = vk_payment_verify(tx, txLen, &expect, &out.seen);
  if (err != VK_PAY_OK) return luaRefuseDetail(L, vk_pay_reason(err), vk_pay_err_name(err));
  return 0;
}

namespace {

// wallet.verify_payment(tx, expected) -> true, {payer, sig} | nil, reason, detail
// The payee's check before it shows PAID. `tx` is the raw wire transaction the RPC node returned.
int l_verify_payment(lua_State *L) {
  CheckedPayment paid;
  if (int n = luaCheckPayment(L, 1, 2, paid)) return n;
  char text[SOL_B58_SIG_MAX];
  lua_pushboolean(L, 1);
  lua_createtable(L, 0, 2);
  lua_pushlstring(L, text, sol_b58_encode(paid.seen.transfer.fee_payer, 32, text, sizeof text));
  lua_setfield(L, -2, "payer");
  lua_pushlstring(L, text, sol_b58_encode(paid.seen.sig, 64, text, sizeof text));
  lua_setfield(L, -2, "sig");
  return 2;
}

// ---------------------------------------------------------------------------
// badge.wallet: identity (no permission)
// ---------------------------------------------------------------------------

// wallet.pubkey() -> 32-byte string ("" when the badge has no identity; key_location() is then "none")
int l_pubkey(lua_State *L) {
  const uint8_t *key = publicKey();
  lua_pushlstring(L, key ? (const char *)key : "", key ? 32 : 0);
  return 1;
}

// wallet.address() -> base58 string ("" when the badge has no identity)
int l_address(lua_State *L) {
  char text[SOL_B58_PUBKEY_MAX];
  const uint8_t *key = publicKey();
  const size_t n = key ? sol_b58_encode(key, 32, text, sizeof text) : 0;
  lua_pushlstring(L, text, n);
  return 1;
}

// wallet.key_location() -> "se050" | "software" | "none". Always the true location (signing.md, "Key").
int l_key_location(lua_State *L) {
  lua_pushstring(L, keyLocation());
  return 1;
}

// wallet.provisioned() -> boolean
int l_provisioned(lua_State *L) {
  lua_pushboolean(L, vk::config::provisioned());
  return 1;
}

// wallet.time_ok() -> boolean: the clock has a trusted source
int l_time_ok(lua_State *L) {
  lua_pushboolean(L, vk::clock::ok());
  return 1;
}

// wallet.time() -> unix seconds from the firmware clock, or nil when the clock has no trusted
// source (the same clock the header and the checks read; os.time() is the raw system time).
// Unix seconds are 32 bits unsigned and a Lua integer here is 32 bits signed: a value past 2038
// is pushed as a float.
int l_time(lua_State *L) {
  if (!vk::clock::ok()) {
    lua_pushnil(L);
    return 1;
  }
  const uint32_t seconds = vk::clock::now();
  if (seconds <= (uint32_t)LUA_MAXINTEGER) lua_pushinteger(L, (lua_Integer)seconds);
  else lua_pushnumber(L, (lua_Number)seconds);
  return 1;
}

// wallet.tokens() -> array of {symbol, mint, decimals, cap, max}; mint base58, cap and max strings
// in display units ("100.00"; a cap or max of zero, meaning none, reads "0.00").
int l_tokens(lua_State *L) {
  vk_token_t table[VK_MAX_TOKENS];
  const size_t count = vk::config::tokens(table);

  lua_createtable(L, (int)count, 0);
  for (size_t i = 0; i < count; ++i) {
    const vk_token_t &token = table[i];
    char text[SOL_B58_PUBKEY_MAX];   // also holds an amount: a u64 is at most 20 digits and a point

    lua_createtable(L, 0, 5);
    lua_pushstring(L, token.symbol);
    lua_setfield(L, -2, "symbol");
    lua_pushlstring(L, text, sol_b58_encode(token.mint, 32, text, sizeof text));
    lua_setfield(L, -2, "mint");
    lua_pushinteger(L, token.decimals);
    lua_setfield(L, -2, "decimals");
    lua_pushlstring(L, text, sol_format_amount(token.cap, token.decimals, text, sizeof text));
    lua_setfield(L, -2, "cap");
    lua_pushlstring(L, text, sol_format_amount(token.max, token.decimals, text, sizeof text));
    lua_setfield(L, -2, "max");
    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  return 1;
}

// wallet.config(name) -> the text value of a config key, or nil if no such key is registered.
// All config values are public. A key with no value and no default reads "".
int l_config(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  if (vk::config::find(name) == nullptr) {
    lua_pushnil(L);
    return 1;
  }
  // The store returns a String. It is read twice, each time inside its own scope, so that none is
  // alive while the Lua buffer is allocated (which can raise). The store is cached, there is one
  // task, and nothing can change the value between the two reads.
  size_t length = 0;
  {
    const String value = vk::config::text(name);
    length = value.length();
  }
  luaL_Buffer buffer;
  char *out = luaL_buffinitsize(L, &buffer, length);
  size_t copied = 0;
  {
    const String value = vk::config::text(name);
    copied = value.length() < length ? value.length() : length;
    memcpy(out, value.c_str(), copied);
  }
  luaL_pushresultsize(&buffer, copied);
  return 1;
}

// ---------------------------------------------------------------------------
// badge.wallet: begin and poll (permission "sign")
// ---------------------------------------------------------------------------

// wallet.begin(domain, bytes, [ctx]) -> true | nil, reason
int l_begin(lua_State *L) {
  const char *domain = luaL_checkstring(L, 1);
  return luaBegin(L, domain, 2, 3);
}

// wallet.poll() -> "pending" | 64-byte signature | nil, reason. A result is returned once; with
// nothing begun the reason is "idle".
int l_poll(lua_State *L) {
  uint8_t sig[64];
  Reason reason = VK_IDLE;
  switch (vk::wallet::poll(sig, reason)) {
    case Poll::PENDING:
      lua_pushliteral(L, "pending");
      return 1;
    case Poll::SIGNED:
      lua_pushlstring(L, (const char *)sig, sizeof sig);
      return 1;
    case Poll::FAILED:
      // A failed approval always carries its cause; VK_OK here would read as success to the app.
      return luaRefuse(L, reason == VK_OK ? VK_SIGN_FAILED : reason);
    case Poll::IDLE:
      break;
  }
  return luaRefuse(L, VK_IDLE);
}

// ---------------------------------------------------------------------------
// badge.codec (no permission). Encoders always return a string; decoders return nil on bad input.
// ---------------------------------------------------------------------------

int base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// Strict: a multiple of four characters, '=' only as the last one or two, unused bits zero. So
// every byte string has exactly one text form. `out` needs len / 4 * 3 bytes.
bool base64Decode(const char *in, size_t len, uint8_t *out, size_t *outLen) {
  if (len % 4 != 0) return false;
  size_t pad = 0;
  if (len != 0 && in[len - 1] == '=') pad = in[len - 2] == '=' ? 2 : 1;

  size_t n = 0;
  for (size_t i = 0; i < len; i += 4) {
    const size_t quadPad = (i + 4 == len) ? pad : 0;
    const int a = base64Value(in[i]);
    const int b = base64Value(in[i + 1]);
    const int c = quadPad == 2 ? 0 : base64Value(in[i + 2]);
    const int d = quadPad >= 1 ? 0 : base64Value(in[i + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0) return false;
    if (quadPad == 2 && (b & 15) != 0) return false;
    if (quadPad == 1 && (c & 3) != 0) return false;
    const uint32_t v = (uint32_t)a << 18 | (uint32_t)b << 12 | (uint32_t)c << 6 | (uint32_t)d;
    out[n++] = (uint8_t)(v >> 16);
    if (quadPad < 2) out[n++] = (uint8_t)(v >> 8);
    if (quadPad < 1) out[n++] = (uint8_t)v;
  }
  *outLen = n;
  return true;
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// codec.b64enc(bytes) -> string
int l_b64enc(lua_State *L) {
  size_t len = 0;
  const uint8_t *in = (const uint8_t *)luaL_checklstring(L, 1, &len);
  const size_t outLen = base64Length(len);
  luaL_Buffer buffer;
  char *out = luaL_buffinitsize(L, &buffer, outLen);
  base64Encode(in, len, out);
  luaL_pushresultsize(&buffer, outLen);
  return 1;
}

// codec.b64dec(text) -> bytes | nil
int l_b64dec(lua_State *L) {
  size_t len = 0;
  const char *in = luaL_checklstring(L, 1, &len);
  if (len == 0) {
    lua_pushliteral(L, "");
    return 1;
  }
  // Scratch on the Lua heap, then one copy into the result: nothing to undo when the text is bad.
  uint8_t *out = (uint8_t *)lua_newuserdatauv(L, len / 4 * 3 + 1, 0);
  size_t outLen = 0;
  if (!base64Decode(in, len, out, &outLen)) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushlstring(L, (const char *)out, outLen);
  return 1;
}

// codec.b58enc(bytes) -> string. At most 64 bytes (a signature); longer input gives nil.
int l_b58enc(lua_State *L) {
  size_t len = 0;
  const uint8_t *in = (const uint8_t *)luaL_checklstring(L, 1, &len);
  if (len > SOL_SIG_LEN) {
    lua_pushnil(L);
    return 1;
  }
  char text[SOL_B58_SIG_MAX];
  const size_t n = sol_b58_encode(in, len, text, sizeof text);
  if (n == 0 && len != 0) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushlstring(L, text, n);
  return 1;
}

// codec.b58dec(text) -> bytes | nil. At most 64 bytes of output.
int l_b58dec(lua_State *L) {
  size_t len = 0;
  const char *in = luaL_checklstring(L, 1, &len);
  if (len == 0) {
    lua_pushliteral(L, "");
    return 1;
  }
  uint8_t out[SOL_SIG_LEN];
  // sol_b58_decode reads a C string, so text with a NUL inside it is bad input.
  if (len <= SOL_B58_SIG_MAX - 1 && strlen(in) == len) {
    // sol_b58_decode wants the byte count, which the text does not state. Each leading '1' is one
    // zero byte; the remaining `rest` digits (the first of them not zero) are a number v with
    // 58^(rest-1) <= v < 58^rest, so its byte count u satisfies (rest-1)*0.7322 < u < rest*0.7322 + 1
    // (log 58 / log 256 = 0.7322). Exactly one count in that range decodes.
    size_t zeros = 0;
    while (zeros < len && in[zeros] == '1') ++zeros;
    const size_t rest = len - zeros;
    const size_t lowest = zeros + (rest > 0 ? (rest - 1) * 73 / 100 : 0);
    size_t highest = zeros + (rest > 0 ? rest * 74 / 100 + 1 : 0);
    if (highest > sizeof out) highest = sizeof out;
    for (size_t n = lowest; n <= highest; ++n) {
      if (sol_b58_decode(in, out, n) == 0) {
        lua_pushlstring(L, (const char *)out, n);
        return 1;
      }
    }
  }
  lua_pushnil(L);
  return 1;
}

// codec.hex(bytes) -> lower-case hex
int l_hex(lua_State *L) {
  size_t len = 0;
  const uint8_t *in = (const uint8_t *)luaL_checklstring(L, 1, &len);
  luaL_Buffer buffer;
  char *out = luaL_buffinitsize(L, &buffer, len * 2);
  for (size_t i = 0; i < len; ++i) {
    out[2 * i] = kHex[in[i] >> 4];
    out[2 * i + 1] = kHex[in[i] & 15];
  }
  luaL_pushresultsize(&buffer, len * 2);
  return 1;
}

// codec.unhex(text) -> bytes | nil. Accepts upper and lower case.
int l_unhex(lua_State *L) {
  size_t len = 0;
  const char *in = luaL_checklstring(L, 1, &len);
  if (len % 2 != 0) {
    lua_pushnil(L);
    return 1;
  }
  if (len == 0) {
    lua_pushliteral(L, "");
    return 1;
  }
  uint8_t *out = (uint8_t *)lua_newuserdatauv(L, len / 2, 0);
  for (size_t i = 0; i < len / 2; ++i) {
    const int high = hexValue(in[2 * i]);
    const int low = hexValue(in[2 * i + 1]);
    if (high < 0 || low < 0) {
      lua_pushnil(L);
      return 1;
    }
    out[i] = (uint8_t)(high << 4 | low);
  }
  lua_pushlstring(L, (const char *)out, len / 2);
  return 1;
}

// ---------------------------------------------------------------------------
// Registration (execution plan 5.3). One line per function.
// ---------------------------------------------------------------------------

VK_LUA_FUNCTION(pubkey, "wallet", "pubkey", nullptr, l_pubkey);
VK_LUA_FUNCTION(address, "wallet", "address", nullptr, l_address);
VK_LUA_FUNCTION(key_location, "wallet", "key_location", nullptr, l_key_location);
VK_LUA_FUNCTION(provisioned, "wallet", "provisioned", nullptr, l_provisioned);
VK_LUA_FUNCTION(time_ok, "wallet", "time_ok", nullptr, l_time_ok);
VK_LUA_FUNCTION(time, "wallet", "time", nullptr, l_time);
VK_LUA_FUNCTION(tokens, "wallet", "tokens", nullptr, l_tokens);
VK_LUA_FUNCTION(config, "wallet", "config", nullptr, l_config);

VK_LUA_FUNCTION(verify_payment, "wallet", "verify_payment", nullptr, l_verify_payment);

VK_LUA_FUNCTION(begin, "wallet", "begin", "sign", l_begin);
VK_LUA_FUNCTION(poll, "wallet", "poll", "sign", l_poll);

VK_LUA_FUNCTION(b64enc, "codec", "b64enc", nullptr, l_b64enc);
VK_LUA_FUNCTION(b64dec, "codec", "b64dec", nullptr, l_b64dec);
VK_LUA_FUNCTION(b58enc, "codec", "b58enc", nullptr, l_b58enc);
VK_LUA_FUNCTION(b58dec, "codec", "b58dec", nullptr, l_b58dec);
VK_LUA_FUNCTION(hex, "codec", "hex", nullptr, l_hex);
VK_LUA_FUNCTION(unhex, "codec", "unhex", nullptr, l_unhex);

}  // namespace

}  // namespace vk::wallet
