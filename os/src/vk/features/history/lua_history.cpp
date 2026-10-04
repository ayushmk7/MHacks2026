// src/vk/features/history/lua_history.cpp
// badge.wallet.history([max]), badge.wallet.record_received(tx, expected) and the permission that
// gates both (platform/lua-api.md, "history").
#include <Arduino.h>

#include "history.h"

#include "../../../lua_sdk/lua_runtime.h"   // runtime::extendDeadline
#include "../../host/lua_registry.h"
#include "../../host/permissions.h"
#include "../../wallet/lua_wallet.h"        // luaCheckPayment, luaRefuseDetail
#include "../../wallet/pure/sol.h"
#include "../../wallet/pure/vk_payment.h"   // vk_req_memo
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

void pushHex(lua_State *L, const char *field, const uint8_t *bytes, size_t length) {
  static const char kHexDigits[] = "0123456789abcdef";
  char text[2 * 16 + 1];
  if (length > 16) length = 16;
  for (size_t i = 0; i < length; ++i) {
    text[2 * i] = kHexDigits[bytes[i] >> 4];
    text[2 * i + 1] = kHexDigits[bytes[i] & 0x0F];
  }
  lua_pushlstring(L, text, 2 * length);
  lua_setfield(L, -2, field);
}

// {kind, time, domain, outcome, reason, amount, symbol, name, address, app, sig, dev, [req_id],
//  [count, items]} for one entry.
void pushEntry(lua_State *L, const Entry &entry) {
  lua_createtable(L, 0, 15);

  setString(L, "kind", kindName(kindOf(entry)));

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

  // `sig` stays nil unless a signature was made (approval) or a transaction was received.
  if ((entry.outcome == OUTCOME_SIGNED && !entry.automatic) || entry.outcome == OUTCOME_RECEIVED) {
    char sig[SOL_B58_SIG_MAX];
    if (sol_b58_encode(entry.sig, sizeof entry.sig, sig, sizeof sig) != 0) setString(L, "sig", sig);
  }

  lua_pushboolean(L, entry.dev);
  lua_setfield(L, -2, "dev");

  if (entry.has_req_id) {
    char id[VK_REQ_MEMO_LEN + 1];
    vk_req_memo(entry.req_id, id);
    setString(L, "req_id", id);
  }

  // An auto row: how many signatures, and each one's time and digest (32 hex characters).
  if (entry.automatic) {
    lua_pushinteger(L, (lua_Integer)entry.auto_count);
    lua_setfield(L, -2, "count");
    lua_createtable(L, entry.auto_count, 0);
    for (size_t i = 0; i < entry.auto_count; ++i) {
      lua_createtable(L, 0, 2);
      lua_pushinteger(L, (lua_Integer)entry.items[i].time);
      lua_setfield(L, -2, "time");
      pushHex(L, "digest", entry.items[i].digest, sizeof entry.items[i].digest);
      lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }
    lua_setfield(L, -2, "items");
  }
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
  luaL_checkstack(L, 8, "wallet.history");
  lua_Integer made = 0;
  for (size_t i = 0; i < cursor.count && made < wanted; ++i) {
    Entry entry;
    if (!at(cursor, i, entry)) break;                // a read failed: return what was read so far
    pushEntry(L, entry);
    lua_rawseti(L, -2, ++made);
  }
  return 1;
}

// wallet.record_received(tx, expected) -> true | nil, reason, detail
// The payee's record of a payment it was paid. The firmware runs the same check as
// wallet.verify_payment (the payer's signature included) and writes a received row from the
// transaction's own bytes: payer, amount, token, request id and signature are never taken from the
// app. "unsupported", "write" when the file could not be written. Calling it again for the same
// transaction writes nothing and returns true.
int l_record_received(lua_State *L) {
  vk::wallet::CheckedPayment paid;
  if (int n = vk::wallet::luaCheckPayment(L, 1, 2, paid)) return n;

  Received row;
  memset(&row, 0, sizeof row);
  memcpy(row.payer, paid.seen.transfer.fee_payer, sizeof row.payer);
  row.amount = paid.seen.transfer.amount;
  row.decimals = paid.token.decimals;
  strlcpy(row.symbol, paid.token.symbol, sizeof row.symbol);
  memcpy(row.req_id, paid.req_id, sizeof row.req_id);
  memcpy(row.sig, paid.seen.sig, sizeof row.sig);
  {
    // A String may not live across a Lua call that can raise; none follows inside this block.
    const String app = ::runtime::currentApp();
    strlcpy(row.app_id, app.c_str(), sizeof row.app_id);
  }
  ::runtime::extendDeadline(READ_EXTRA_MS);           // a scan for a duplicate, then one append
  if (recordReceived(row, unixTime ? unixTime() : 0) == ReceiveResult::FAILED) {
    return vk::wallet::luaRefuseDetail(L, VK_UNSUPPORTED, "write");
  }
  lua_pushboolean(L, 1);
  return 1;
}

VK_PERMISSION(history, "history", "read and add to payment history", false, nullptr);
VK_LUA_FUNCTION(history, "wallet", "history", "history", l_history);
VK_LUA_FUNCTION(record_received, "wallet", "record_received", "history", l_record_received);

}  // namespace

}  // namespace vk::history
