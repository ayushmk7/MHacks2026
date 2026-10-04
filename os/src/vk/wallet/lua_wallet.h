// The Lua side of the wallet core (platform/lua-api.md): what every wallet.begin* binding shares.
// The bindings themselves are in lua_wallet.cpp (badge.wallet identity, begin, poll; badge.codec)
// and in the feature folders (begin_solana, begin_bank, ...). Features may not include each other,
// so the one piece they need in common lives here, in the wallet core.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "reason.h"

extern "C" {
#include "../../lua/lua.h"
#include "../../lua/lauxlib.h"
}

namespace vk::wallet {

// The body of wallet.begin(domain, bytes, [ctx]) and of every wallet.begin_<domain>(bytes, [ctx]).
// `bytesIndex` and `ctxIndex` are the (positive) Lua stack positions of the bytes and of the
// optional ctx table {record=, record_sig=, req=}. It extends the callback deadline for the
// verifications the decoder will make, calls vk::wallet::begin() for the running app, pushes
// `true` or `nil, reason`, and returns the number of Lua results:
//   static int l_begin_solana(lua_State *L) { return vk::wallet::luaBegin(L, "solana", 1, 2); }
// A wrong type (bytes not a string, ctx not a table, a ctx field not a string) raises a Lua error;
// a record_sig that is not exactly 64 bytes is refused with bad_arg.
int luaBegin(lua_State *L, const char *domain, int bytesIndex, int ctxIndex);

// ---- Added with the bindings (WP13), for bindings in feature folders ----

// Pushes `nil, "<reason>"` and returns 2: `return luaRefuse(L, VK_BAD_ARG);`.
int luaRefuse(lua_State *L, Reason reason);

// Standard base64 with padding (the alphabet of RFC 4648, section 4).
// base64Encode writes exactly base64Length(len) characters to `out` and no terminating NUL.
size_t base64Length(size_t len);
void base64Encode(const uint8_t *in, size_t len, char *out);

}  // namespace vk::wallet
