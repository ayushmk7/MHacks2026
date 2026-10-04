// src/vk/features/history/lua_history.cpp
// badge.wallet.history([max]) and the permission that gates it (platform/lua-api.md, "history").
#include <Arduino.h>

#include "history.h"

#include "../../../lua_sdk/lua_runtime.h"   // runtime::extendDeadline
#include "../../host/lua_registry.h"
#include "../../host/permissions.h"
#include "../../wallet/pure/sol.h"
#include "../../wallet/reason.h"

namespace vk::history {

namespace {

constexpr lua_Integer DEFAULT_MAX = 20;
constexpr lua_Integer LIMIT_MAX = 64;

// One file read per entry (a few milliseconds each on LittleFS). 64 of them can pass upstream's
// 250 ms callback budget, so the binding asks for this much more before it starts.
constexpr uint32_t READ_EXTRA_MS = 1500;

bool allZero(const uint8_t *bytes, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    if (bytes[i] != 0) return false;
  }
  return true;
}

void setString(lua_State *L, const char *field, const char *value) {
  lua_pushstring(L, value);
  lua_setfield(L, -2, field);
}

// {time, domain, outcome, reason, amount, symbol, name, address, app, sig, dev} for one entry.
void pushEntry(lua_State *L, const Entry &entry) {
  lua_createtable(L, 0, 11);

  lua_pushinteger(L, (lua_Integer)entry.time);
  lua_setfield(L, -2, "time");
  setString(L, "domain", entry.domain);
  setString(L, "outcome", outcomeName(entry.outcome));
  // The stored number is named only when it is one of the codes; a cast of anything else is not valid.
  setString(L, "reason", entry.reason < VK_REASON_COUNT ? vk::wallet::reasonName((vk::wallet::Reason)entry.reason) : "?");

  // Amounts cross the API as decimal strings in display units, never as Lua numbers.
  char amount[24];
  if (sol_format_amount(entry.amount, entry.decimals, amount, sizeof amount) == 0) strlcpy(amount, "0", sizeof amount);
  setString(L, "amount", amount);
  setString(L, "symbol", entry.symbol);
  setString(L, "name", entry.recipient_name);

  // No recipient (a confirmation, or bytes that could not be read) is stored as zeros: shown as "".
  char address[SOL_B58_PUBKEY_MAX] = "";
  if (!allZero(entry.recipient, sizeof entry.recipient)) {
    if (sol_b58_encode(entry.recipient, sizeof entry.recipient, address, sizeof address) == 0) address[0] = '\0';
  }
  setString(L, "address", address);
  setString(L, "app", entry.app_id);

  // `sig` stays nil unless a signature was made.
  if (entry.outcome == OUTCOME_SIGNED) {
    char sig[SOL_B58_SIG_MAX];
    if (sol_b58_encode(entry.sig, sizeof entry.sig, sig, sizeof sig) != 0) setString(L, "sig", sig);
  }

  lua_pushboolean(L, entry.dev);
  lua_setfield(L, -2, "dev");
}

// wallet.history([max]) -> array, newest first. `max` defaults to 20, limit 64.
int l_history(lua_State *L) {
  lua_Integer wanted = luaL_optinteger(L, 1, DEFAULT_MAX);
  if (wanted < 0) wanted = 0;
  if (wanted > LIMIT_MAX) wanted = LIMIT_MAX;

  lua_newtable(L);
  Cursor cursor;
  if (wanted == 0 || !open(cursor)) return 1;        // nothing logged yet: an empty array

  ::runtime::extendDeadline(READ_EXTRA_MS);
  luaL_checkstack(L, 4, "wallet.history");
  lua_Integer made = 0;
  for (size_t i = 0; i < cursor.count && made < wanted; ++i) {
    Entry entry;
    if (!at(cursor, i, entry)) break;                // a read failed: return what was read so far
    pushEntry(L, entry);
    lua_rawseti(L, -2, ++made);
  }
  return 1;
}

VK_PERMISSION(history, "history", "read payment history", false, nullptr);
VK_LUA_FUNCTION(history, "wallet", "history", "history", l_history);

}  // namespace

}  // namespace vk::history
