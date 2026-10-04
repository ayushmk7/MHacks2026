// src/vk/features/balance/lua_balance.cpp
// badge.wallet.balance, token_account and refresh_balance (platform/lua-api.md, "identity" and
// "balance"). Only the default (first) token is tracked: any other symbol gives nil.
#include <Arduino.h>

#include "balance.h"

#include "../../../lua_sdk/lua_runtime.h"   // runtime::extendDeadline
#include "../../core/config.h"
#include "../../host/lua_registry.h"

namespace vk::balance {

namespace {

// The request's connect and its read each get the 3 s timeout, plus upstream's own slack.
constexpr uint32_t REFRESH_DEADLINE_MS = 7000;

// The default token when the optional symbol argument at `index` is absent, nil or its symbol.
// False for any other symbol, and when there is no token table.
bool tokenForArg(lua_State *L, int index, vk_token_t &out) {
  const char *symbol = luaL_optstring(L, index, nullptr);   // a wrong type raises a Lua error
  vk_token_t table[VK_MAX_TOKENS];
  if (vk::config::tokens(table) == 0) return false;
  if (symbol != nullptr && strcmp(symbol, table[0].symbol) != 0) return false;
  out = table[0];
  return true;
}

// wallet.balance([symbol]) -> the last known balance as a string in display units, or nil.
int l_balance(lua_State *L) {
  vk_token_t token;
  char text[24];
  if (!tokenForArg(L, 1, token) || !known() ||
      sol_format_amount(raw(), token.decimals, text, sizeof text) == 0) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushstring(L, text);
  return 1;
}

// wallet.token_account([symbol]) -> this badge's token account, base58, or nil.
int l_token_account(lua_State *L) {
  vk_token_t token;
  uint8_t account[32];
  char text[SOL_B58_PUBKEY_MAX];
  if (!tokenForArg(L, 1, token) || !tokenAccount(account) ||
      sol_b58_encode(account, sizeof account, text, sizeof text) == 0) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushstring(L, text);
  return 1;
}

// wallet.refresh_balance() -> true, or nil, reason. Blocks for one fetch.
int l_refresh_balance(lua_State *L) {
  ::runtime::extendDeadline(REFRESH_DEADLINE_MS);
  const vk::wallet::Reason reason = fetch(FETCH_TIMEOUT_MS);
  if (reason != VK_OK) {
    lua_pushnil(L);
    lua_pushstring(L, vk::wallet::reasonName(reason));
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

VK_LUA_FUNCTION(balance, "wallet", "balance", nullptr, l_balance);
VK_LUA_FUNCTION(token_account, "wallet", "token_account", nullptr, l_token_account);
VK_LUA_FUNCTION(refresh_balance, "wallet", "refresh_balance", "net", l_refresh_balance);

}  // namespace

}  // namespace vk::balance
