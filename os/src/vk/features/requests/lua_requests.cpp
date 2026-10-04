// badge.wallet functions of the requests feature (platform/lua-api.md, "badge.wallet: requests" and
// the requests, challenge, presence rows of "badge.wallet: payments").
//
//   permission "request":  wallet.request_open{...}  wallet.request_close(req_id)  wallet.request_status(req_id)
//   permission "sign":     wallet.requests()  wallet.challenge(mac, req)  wallet.presence(req_id)
//
// A req_id crosses the Lua API as 16 hex characters; a MAC as "aa:bb:cc:dd:ee:ff" (the form
// upstream's badge.espnow uses); an amount as a decimal string in display units. A refusal returns
// nil, "<reason>"; a wrong argument type raises a Lua error.
//
// No Arduino String is alive across a call that can raise a Lua error (luaL_error does not run
// C++ destructors).
#include <Arduino.h>

#include <string.h>

#include "../../../lua_sdk/lua_runtime.h"   // runtime::extendDeadline, runtime::currentApp
#include "../../../settings.h"              // settings::deviceName
#include "../../core/clock.h"
#include "../../core/config.h"
#include "../../host/lua_registry.h"        // VK_LUA_FUNCTION, the Lua headers
#include "../../wallet/pure/sol.h"          // sol_parse_amount, sol_b58_encode
#include "presence.h"
#include "requests.h"

namespace {

using vk::wallet::Reason;

int refuse(lua_State *L, Reason reason) {
  lua_pushnil(L);
  lua_pushstring(L, vk_reason_name(reason));
  return 2;
}

// ---- req_id and MAC text --------------------------------------------------------------------

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Exactly 16 hex characters.
bool parseReqId(const char *text, size_t len, uint8_t out[8]) {
  if (text == nullptr || len != 16) return false;
  for (size_t i = 0; i < 8; ++i) {
    const int hi = hexNibble(text[2 * i]);
    const int lo = hexNibble(text[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

void pushReqId(lua_State *L, const uint8_t req_id[8]) {
  static const char kDigits[] = "0123456789abcdef";
  char text[16];
  for (size_t i = 0; i < 8; ++i) {
    text[2 * i] = kDigits[req_id[i] >> 4];
    text[2 * i + 1] = kDigits[req_id[i] & 0x0F];
  }
  lua_pushlstring(L, text, sizeof text);
}

// "aa:bb:cc:dd:ee:ff", either case.
bool parseMac(const char *text, size_t len, uint8_t out[6]) {
  if (text == nullptr || len != 17) return false;
  for (size_t i = 0; i < 6; ++i) {
    const int hi = hexNibble(text[3 * i]);
    const int lo = hexNibble(text[3 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    if (i < 5 && text[3 * i + 2] != ':') return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

void pushMac(lua_State *L, const uint8_t mac[6]) {
  char text[18];
  snprintf(text, sizeof text, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  lua_pushstring(L, text);
}

// ---- request_open ---------------------------------------------------------------------------

// Pushes table[key] and leaves it on the stack (so the returned pointer stays valid). Returns the
// string, or nullptr when the field is absent. Anything but a string raises a Lua error.
const char *stringField(lua_State *L, int table, const char *key, bool required) {
  const int type = lua_getfield(L, table, key);
  if (type == LUA_TSTRING) return lua_tostring(L, -1);
  if (type == LUA_TNIL && !required) return nullptr;
  luaL_error(L, "request_open: '%s' must be a string", key);
  return nullptr;
}

// The name this badge claims in a request: config `display_name`, or upstream's device name when
// that is empty. Made to fit the frame: at most 32 characters, anything not printable ASCII as '?'.
void defaultName(char out[VK_NAME_MAX + 1]) {
  String name = vk::config::text("display_name");
  if (name.length() == 0) name = settings::deviceName();
  size_t n = 0;
  for (; n < VK_NAME_MAX && n < name.length(); ++n) {
    const char c = name[n];
    out[n] = (c >= 0x20 && c <= 0x7E) ? c : '?';
  }
  out[n] = '\0';
}

// The currency and its decimals for a new request. Solana: a token of the provisioned table, by
// symbol (the first token when no symbol is given). Bank: cents, in `symbol` or USD (the bank
// payload's currency, checks.md "Bank rail").
bool resolveCurrency(uint8_t rail, const char *symbol, char currency[5], uint8_t &decimals) {
  if (rail == VK_RAIL_BANK) {
    const char *text = symbol ? symbol : "USD";
    const size_t n = strlen(text);
    if (n < 1 || n > 4) return false;
    for (size_t i = 0; i < n; ++i) {
      const char c = text[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    }
    memcpy(currency, text, n + 1);
    decimals = 2;
    return true;
  }
  vk_token_t tokens[VK_MAX_TOKENS];
  const size_t count = vk::config::tokens(tokens);
  for (size_t i = 0; i < count && i < VK_MAX_TOKENS; ++i) {
    if (symbol != nullptr && strcmp(tokens[i].symbol, symbol) != 0) continue;
    strlcpy(currency, tokens[i].symbol, 5);
    decimals = tokens[i].decimals;
    return true;
  }
  return false;
}

// wallet.request_open{amount=, [symbol=], [rail=], [name=], [ttl_s=]} -> {req_id, frame, expiry}
int l_request_open(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_settop(L, 1);

  // Argument types first: a wrong type is a Lua error whatever state the badge is in.
  // Each field stays on the stack: amount at 2, symbol at 3, rail at 4, name at 5, ttl_s at 6.
  const char *amount = stringField(L, 1, "amount", true);      // a string, never a number
  const char *symbol = stringField(L, 1, "symbol", false);
  const char *railText = stringField(L, 1, "rail", false);
  const char *name = stringField(L, 1, "name", false);
  bool hasTtl = false;
  lua_Integer ttl = 0;
  const int ttlType = lua_getfield(L, 1, "ttl_s");
  if (ttlType != LUA_TNIL) {
    if (!lua_isinteger(L, -1)) return luaL_error(L, "request_open: 'ttl_s' must be an integer");
    ttl = lua_tointeger(L, -1);
    hasTtl = true;
  }

  // Refusals, in the order of lua-api.md: not_provisioned, no_time, busy, bad_arg, sign_failed.
  const Reason blocked = vk::requests::canOpen();
  if (blocked != VK_OK) return refuse(L, blocked);

  vk::requests::OpenArgs args;
  if (railText == nullptr || strcmp(railText, "solana") == 0) args.rail = VK_RAIL_SOLANA;
  else if (strcmp(railText, "bank") == 0) args.rail = VK_RAIL_BANK;
  else return refuse(L, VK_BAD_ARG);

  char currency[5];
  uint8_t decimals = 0;
  if (!resolveCurrency(args.rail, symbol, currency, decimals)) return refuse(L, VK_BAD_ARG);
  args.currency = currency;

  // strlen(amount) is the whole string only if it holds no NUL; sol_parse_amount refuses the rest.
  if (lua_rawlen(L, 2) != strlen(amount)) return refuse(L, VK_BAD_ARG);
  if (sol_parse_amount(amount, decimals, &args.amount) != 0 || args.amount == 0) return refuse(L, VK_BAD_ARG);

  char ownName[VK_NAME_MAX + 1];
  if (name == nullptr) {
    defaultName(ownName);
    args.name = ownName;
  } else {
    if (lua_rawlen(L, 5) != strlen(name)) return refuse(L, VK_BAD_ARG);   // a NUL inside the name
    args.name = name;        // 1..32 printable ASCII, checked by openRequest
  }

  if (hasTtl) {
    // The same range as the config key req_ttl_s (platform/config.md, Keys).
    const vk::config::ConfigKey *key = vk::config::find("req_ttl_s");
    const lua_Integer lowest = key ? (lua_Integer)key->min : 1;
    const lua_Integer highest = key ? (lua_Integer)key->max : 600;
    if (ttl < lowest || ttl > highest) return refuse(L, VK_BAD_ARG);
    args.ttl_s = (uint32_t)ttl;
  }

  args.app_id = runtime::currentApp().c_str();

  runtime::extendDeadline(2500);     // one signature: about a second with a software key
  vk::requests::Opened opened;
  const Reason result = vk::requests::openRequest(args, opened);
  if (result != VK_OK) return refuse(L, result);

  lua_createtable(L, 0, 3);
  pushReqId(L, opened.req_id);
  lua_setfield(L, -2, "req_id");
  lua_pushlstring(L, (const char *)opened.frame, opened.frame_len);
  lua_setfield(L, -2, "frame");
  lua_pushinteger(L, (lua_Integer)opened.expiry);
  lua_setfield(L, -2, "expiry");
  return 1;
}

// wallet.request_close(req_id) -> boolean
int l_request_close(lua_State *L) {
  size_t len = 0;
  const char *text = luaL_checklstring(L, 1, &len);
  uint8_t req_id[8];
  lua_pushboolean(L, parseReqId(text, len, req_id) && vk::requests::closeRequest(req_id));
  return 1;
}

// wallet.request_status(req_id) -> {state, proofs} or nil
int l_request_status(lua_State *L) {
  size_t len = 0;
  const char *text = luaL_checklstring(L, 1, &len);
  uint8_t req_id[8];
  vk::requests::State state = vk::requests::State::Closed;
  uint32_t proofs = 0;
  if (!parseReqId(text, len, req_id) || !vk::requests::requestStatus(req_id, state, proofs)) {
    lua_pushnil(L);
    return 1;
  }
  lua_createtable(L, 0, 2);
  lua_pushstring(L, vk::requests::stateName(state));
  lua_setfield(L, -2, "state");
  lua_pushinteger(L, (lua_Integer)proofs);
  lua_setfield(L, -2, "proofs");
  return 1;
}

// ---- the payer's side -----------------------------------------------------------------------

// wallet.requests() -> array of {req, mac, rssi, name, amount, currency, rail, payee, req_id, age_ms, expires_in_s}
int l_requests(lua_State *L) {
  const size_t count = vk::requests::cacheCount();
  const uint32_t nowMs = (uint32_t)millis();
  const bool timed = vk::clock::ok();
  const uint32_t nowS = timed ? vk::clock::now() : 0;

  lua_createtable(L, (int)count, 0);
  for (size_t i = 0; i < count; ++i) {
    const vk::requests::Cached *c = vk::requests::cacheAt(i);
    if (c == nullptr) break;
    lua_createtable(L, 0, 11);

    lua_pushlstring(L, (const char *)c->frame, c->frame_len);
    lua_setfield(L, -2, "req");
    pushMac(L, c->mac);
    lua_setfield(L, -2, "mac");
    lua_pushinteger(L, c->rssi);
    lua_setfield(L, -2, "rssi");
    lua_pushlstring(L, c->req.name, c->req.name_len);          // the sender's claim, not a verified name
    lua_setfield(L, -2, "name");

    char amount[24];
    vk::requests::formatAmount(c->req.rail, c->req.currency, c->req.amount, amount, sizeof amount);
    lua_pushstring(L, amount);
    lua_setfield(L, -2, "amount");
    lua_pushstring(L, c->req.currency);
    lua_setfield(L, -2, "currency");
    lua_pushstring(L, c->req.rail == VK_RAIL_BANK ? "bank" : "solana");
    lua_setfield(L, -2, "rail");

    char payee[SOL_B58_PUBKEY_MAX];
    if (sol_b58_encode(c->req.payee_pubkey, 32, payee, sizeof payee) == 0) payee[0] = '\0';
    lua_pushstring(L, payee);
    lua_setfield(L, -2, "payee");
    pushReqId(L, c->req.req_id);
    lua_setfield(L, -2, "req_id");

    lua_pushinteger(L, (lua_Integer)(uint32_t)(nowMs - c->heard_ms));
    lua_setfield(L, -2, "age_ms");
    // 0 when the clock has no source: the expiry cannot be compared with anything.
    lua_pushinteger(L, (timed && c->req.expiry > nowS) ? (lua_Integer)(c->req.expiry - nowS) : 0);
    lua_setfield(L, -2, "expires_in_s");

    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  return 1;
}

// wallet.challenge(mac, req) -> true
int l_challenge(lua_State *L) {
  size_t macLen = 0, reqLen = 0;
  const char *macText = luaL_checklstring(L, 1, &macLen);
  const char *req = luaL_checklstring(L, 2, &reqLen);
  uint8_t mac[6];
  if (!parseMac(macText, macLen, mac)) return refuse(L, VK_BAD_ARG);
  const Reason result = vk::presence::challenge(mac, (const uint8_t *)req, reqLen);
  if (result != VK_OK) return refuse(L, result);
  lua_pushboolean(L, 1);
  return 1;
}

// wallet.presence(req_id) -> "none" | "pending" | "present" | "late" | "bad_sig"
int l_presence(lua_State *L) {
  size_t len = 0;
  const char *text = luaL_checklstring(L, 1, &len);
  uint8_t req_id[8];
  vk_presence_t result = VK_PRESENCE_NONE;       // an id that is not 16 hex characters has no slot
  if (parseReqId(text, len, req_id)) result = vk::presence::lookup(req_id, nullptr, nullptr);
  lua_pushstring(L, vk::presence::resultName(result));
  return 1;
}

}  // namespace

VK_LUA_FUNCTION(request_open, "wallet", "request_open", "request", l_request_open);
VK_LUA_FUNCTION(request_close, "wallet", "request_close", "request", l_request_close);
VK_LUA_FUNCTION(request_status, "wallet", "request_status", "request", l_request_status);
VK_LUA_FUNCTION(requests, "wallet", "requests", "sign", l_requests);
VK_LUA_FUNCTION(challenge, "wallet", "challenge", "sign", l_challenge);
VK_LUA_FUNCTION(presence, "wallet", "presence", "sign", l_presence);
