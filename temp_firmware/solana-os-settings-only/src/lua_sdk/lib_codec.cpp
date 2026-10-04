/*
  badge.codec - byte encodings, the same names and rules as BadgeOS's
  (docs/os/platform/lua-api.md, "badge.codec" on the badge-os branch), so the
  harness apps run unchanged on this fork and on BadgeOS.

    codec.b64enc(bytes) / codec.b64dec(text)   standard alphabet, padded; dec is
                                               strict (no whitespace, unused bits
                                               zero) and returns nil on bad input
    codec.b58enc(bytes) / codec.b58dec(text)   at most 64 bytes; nil otherwise
    codec.hex(bytes)    / codec.unhex(text)    lower-case out; either case in

  The empty string encodes and decodes to the empty string in all three.
*/
#include <Arduino.h>

#include "../identity/identity.h"
#include "lua_bindings.h"

extern "C" {
#include "../lua/lauxlib.h"
#include "../lua/lua.h"
}

namespace bindings {
namespace {

constexpr size_t MAX_B58_BYTES = 64;
const char B58[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

int pushNil(lua_State *L) {
  lua_pushnil(L);
  return 1;
}

int l_b64enc(lua_State *L) {
  size_t length = 0;
  const char *bytes = luaL_checklstring(L, 1, &length);
  const String text = identity::base64Encode((const uint8_t *)bytes, length);
  lua_pushlstring(L, text.c_str(), text.length());
  return 1;
}

int b64value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

int l_b64dec(lua_State *L) {
  size_t length = 0;
  const char *text = luaL_checklstring(L, 1, &length);
  if (length % 4 != 0) return pushNil(L);
  luaL_Buffer out;
  luaL_buffinit(L, &out);
  for (size_t i = 0; i < length; i += 4) {
    const bool last = i + 4 == length;
    const int pad = last ? (text[i + 3] == '=') + (text[i + 2] == '=') : 0;
    if (pad == 1 && text[i + 2] == '=') return pushNil(L);  // "x=y=" style
    int v[4];
    for (int k = 0; k < 4 - pad; ++k) {
      v[k] = b64value(text[i + k]);
      if (v[k] < 0) return pushNil(L);
    }
    if (pad >= 1 && (v[4 - pad - 1] & (pad == 1 ? 0x03 : 0x0f))) return pushNil(L);  // unused bits set
    const uint32_t n = (v[0] << 18) | (v[1] << 12) | ((pad < 2 ? v[2] : 0) << 6) | (pad < 1 ? v[3] : 0);
    luaL_addchar(&out, (char)(n >> 16));
    if (pad < 2) luaL_addchar(&out, (char)((n >> 8) & 0xff));
    if (pad < 1) luaL_addchar(&out, (char)(n & 0xff));
  }
  luaL_pushresult(&out);
  return 1;
}

int l_b58enc(lua_State *L) {
  size_t length = 0;
  const char *bytes = luaL_checklstring(L, 1, &length);
  if (length > MAX_B58_BYTES) return pushNil(L);
  if (length == 0) {
    lua_pushliteral(L, "");
    return 1;
  }
  const String text = identity::base58Encode((const uint8_t *)bytes, length);
  lua_pushlstring(L, text.c_str(), text.length());
  return 1;
}

int l_b58dec(lua_State *L) {
  size_t length = 0;
  const char *text = luaL_checklstring(L, 1, &length);
  if (length == 0) {
    lua_pushliteral(L, "");
    return 1;
  }
  if (length > 90) return pushNil(L);  // 64 bytes are at most 88 characters
  uint8_t bytes[MAX_B58_BYTES] = {0};  // big-endian, right-aligned
  size_t used = 0;                     // significant bytes so far
  size_t zeros = 0;
  while (zeros < length && text[zeros] == '1') ++zeros;
  for (size_t i = 0; i < length; ++i) {
    const char *at = strchr(B58, text[i]);
    if (!at || !*at) return pushNil(L);
    uint32_t carry = (uint32_t)(at - B58);
    for (size_t j = 0; j < MAX_B58_BYTES && (carry || j < used); ++j) {
      uint8_t &b = bytes[MAX_B58_BYTES - 1 - j];
      carry += 58u * b;
      b = carry & 0xff;
      carry >>= 8;
      if (j + 1 > used && (b || carry)) used = j + 1;
    }
    if (carry) return pushNil(L);
  }
  if (zeros + used > MAX_B58_BYTES) return pushNil(L);
  luaL_Buffer out;
  luaL_buffinit(L, &out);
  for (size_t i = 0; i < zeros; ++i) luaL_addchar(&out, '\0');
  luaL_addlstring(&out, (const char *)bytes + MAX_B58_BYTES - used, used);
  luaL_pushresult(&out);
  return 1;
}

int l_hex(lua_State *L) {
  static const char DIGITS[] = "0123456789abcdef";
  size_t length = 0;
  const uint8_t *bytes = (const uint8_t *)luaL_checklstring(L, 1, &length);
  luaL_Buffer out;
  luaL_buffinit(L, &out);
  for (size_t i = 0; i < length; ++i) {
    luaL_addchar(&out, DIGITS[bytes[i] >> 4]);
    luaL_addchar(&out, DIGITS[bytes[i] & 0x0f]);
  }
  luaL_pushresult(&out);
  return 1;
}

int nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int l_unhex(lua_State *L) {
  size_t length = 0;
  const char *text = luaL_checklstring(L, 1, &length);
  if (length % 2 != 0) return pushNil(L);
  luaL_Buffer out;
  luaL_buffinit(L, &out);
  for (size_t i = 0; i < length; i += 2) {
    const int hi = nibble(text[i]), lo = nibble(text[i + 1]);
    if (hi < 0 || lo < 0) return pushNil(L);
    luaL_addchar(&out, (char)((hi << 4) | lo));
  }
  luaL_pushresult(&out);
  return 1;
}

const luaL_Reg FUNCTIONS[] = {
    {"b64enc", l_b64enc}, {"b64dec", l_b64dec}, {"b58enc", l_b58enc},
    {"b58dec", l_b58dec}, {"hex", l_hex},       {"unhex", l_unhex},
    {nullptr, nullptr},
};

}  // namespace

void openCodec(lua_State *L) { setTable(L, "codec", FUNCTIONS, nullptr); }

}  // namespace bindings
