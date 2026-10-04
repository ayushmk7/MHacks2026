// src/vk/features/contacts/lua_contacts.cpp
// badge.wallet functions of the contacts feature and the permission that gates them
// (platform/lua-api.md, "badge.wallet: contacts").
//
//   wallet.contact_hello()            -> HELLO frame bytes
//   wallet.contact_card(hello_frame)  -> CARD frame bytes
//   wallet.contact_accept(card_frame) -> {name, address}
//   wallet.contacts()                 -> array of {name, address, added}, oldest first
//   wallet.contact_remove(address)    -> boolean
//
// The frames are sent and received by the app itself with badge.espnow. A refusal returns
// nil, "<reason>"; a wrong argument type raises a Lua error.
//
// No Arduino String is alive across a call that can raise a Lua error (luaL_error does not run
// C++ destructors).
#include <Arduino.h>

#include <string.h>

#include "contacts.h"

#include "../../../lua_sdk/lua_runtime.h"   // runtime::extendDeadline
#include "../../../settings.h"              // settings::deviceName
#include "../../core/config.h"
#include "../../host/lua_registry.h"        // VK_LUA_FUNCTION, the Lua headers
#include "../../host/permissions.h"
#include "../../wallet/lua_wallet.h"        // luaRefuse
#include "../../wallet/pure/sol.h"          // sol_b58_encode, sol_b58_decode

namespace vk::contacts {

namespace {

using vk::wallet::Reason;

// One signature (contact_card) or one verification (contact_accept): about a second at worst
// with a software key (signing.md).
constexpr uint32_t CRYPTO_EXTRA_MS = 2500;

// One file read per contact (a few milliseconds each on LittleFS). 64 of them can pass upstream's
// 250 ms callback budget, so wallet.contacts asks for this much more before it starts.
constexpr uint32_t READ_EXTRA_MS = 1500;

// The name this badge gives in HELLO and CARD: config `display_name`, or upstream's device name
// when that is empty. Made to fit the frame: at most 32 characters, anything not printable ASCII
// as '?'.
void ownName(char out[VK_NAME_MAX + 1]) {
  String name = vk::config::text("display_name");
  if (name.length() == 0) name = settings::deviceName();
  cleanName(name.c_str(), out);
}

void setAddress(lua_State *L, const uint8_t pubkey[32]) {
  char address[SOL_B58_PUBKEY_MAX];
  lua_pushlstring(L, address, sol_b58_encode(pubkey, 32, address, sizeof address));
  lua_setfield(L, -2, "address");
}

// wallet.contact_hello() -> HELLO frame bytes
int l_contact_hello(lua_State *L) {
  char name[VK_NAME_MAX + 1];
  ownName(name);
  uint8_t frame[VK_HELLO_MAX_LEN];
  size_t length = 0;
  const Reason result = hello(name, frame, length);
  if (result != VK_OK) return vk::wallet::luaRefuse(L, result);
  lua_pushlstring(L, (const char *)frame, length);
  return 1;
}

// wallet.contact_card(hello_frame) -> CARD frame bytes
int l_contact_card(lua_State *L) {
  size_t helloLen = 0;
  const char *helloFrame = luaL_checklstring(L, 1, &helloLen);
  char name[VK_NAME_MAX + 1];
  ownName(name);
  ::runtime::extendDeadline(CRYPTO_EXTRA_MS);
  uint8_t frame[VK_CARD_MAX_LEN];
  size_t length = 0;
  const Reason result = card((const uint8_t *)helloFrame, helloLen, name, frame, length);
  if (result != VK_OK) return vk::wallet::luaRefuse(L, result);
  lua_pushlstring(L, (const char *)frame, length);
  return 1;
}

// wallet.contact_accept(card_frame) -> {name, address}
int l_contact_accept(lua_State *L) {
  size_t cardLen = 0;
  const char *cardFrame = luaL_checklstring(L, 1, &cardLen);
  ::runtime::extendDeadline(CRYPTO_EXTRA_MS);
  Entry saved;
  const Reason result = accept((const uint8_t *)cardFrame, cardLen, saved);
  if (result != VK_OK) return vk::wallet::luaRefuse(L, result);
  lua_createtable(L, 0, 2);
  lua_pushstring(L, saved.name);
  lua_setfield(L, -2, "name");
  setAddress(L, saved.pubkey);
  return 1;
}

// wallet.contacts() -> array of {name, address, added}
int l_contacts(lua_State *L) {
  lua_newtable(L);
  Cursor cursor;
  if (!open(cursor) || cursor.count == 0) return 1;      // no contacts yet: an empty array

  ::runtime::extendDeadline(READ_EXTRA_MS);
  luaL_checkstack(L, 4, "wallet.contacts");
  lua_Integer made = 0;
  for (size_t i = 0; i < cursor.count; ++i) {
    Entry entry;
    if (!at(cursor, i, entry)) break;                    // a read failed: return what was read so far
    lua_createtable(L, 0, 3);
    lua_pushstring(L, entry.name);
    lua_setfield(L, -2, "name");
    setAddress(L, entry.pubkey);
    // Unix seconds are 32 bits unsigned and a Lua integer here is 32 bits signed: a value past
    // 2038 is pushed as a float (lua-api.md, Conventions).
    if (entry.added <= (uint32_t)LUA_MAXINTEGER) lua_pushinteger(L, (lua_Integer)entry.added);
    else lua_pushnumber(L, (lua_Number)entry.added);
    lua_setfield(L, -2, "added");
    lua_rawseti(L, -2, ++made);
  }
  return 1;
}

// wallet.contact_remove(address) -> boolean
int l_contact_remove(lua_State *L) {
  size_t length = 0;
  const char *address = luaL_checklstring(L, 1, &length);
  uint8_t pubkey[32];
  // strlen(address) is the whole string only if it holds no NUL.
  const bool known = length == strlen(address) && sol_b58_decode(address, pubkey, sizeof pubkey) == 0 &&
                     vk::contacts::remove(pubkey);
  lua_pushboolean(L, known);
  return 1;
}

VK_PERMISSION(contacts, "contacts", "read and add contacts", false, nullptr);

VK_LUA_FUNCTION(contact_hello, "wallet", "contact_hello", "contacts", l_contact_hello);
VK_LUA_FUNCTION(contact_card, "wallet", "contact_card", "contacts", l_contact_card);
VK_LUA_FUNCTION(contact_accept, "wallet", "contact_accept", "contacts", l_contact_accept);
VK_LUA_FUNCTION(contacts, "wallet", "contacts", "contacts", l_contacts);
VK_LUA_FUNCTION(contact_remove, "wallet", "contact_remove", "contacts", l_contact_remove);

}  // namespace

}  // namespace vk::contacts
