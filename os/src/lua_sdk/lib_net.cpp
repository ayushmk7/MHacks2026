/*
  badge.wifi and badge.http.

  wifi.connect() returns immediately - joining takes seconds and blocking the
  main loop for that long would stall the display, the buttons and every other
  radio. Poll wifi.connected() from on_update() instead.

  http.get/post DO block, because there is no sane non-blocking shape for them
  in a callback-driven script. They extend the runtime's deadline by their own
  timeout so a legitimately slow request is not killed as a runaway loop, and
  the timeout is capped at 10s. The transport is picked by net_route: real Wi-Fi
  if there is any, otherwise a phone bridging over BLE.

  DELIBERATE CONVENIENCE LIE: wifi.connected() is true when the badge has *a
  route*, which includes a phone bridge with no Wi-Fi at all. wifi.ssid() then
  reads "phone-bridge" and wifi.status() "bridged via phone". Every app already
  written gates its HTTP on wifi.connected(), and none of them know the bridge
  exists; making the gate mean "can I make a request" is what lets them work
  unchanged. wifi_mgr::connected() already tells a comparable lie for SoftAP
  mode. wifi.ip() and wifi.rssi() are NOT faked - they report the real radio.
*/
#include <Arduino.h>
#include <WiFi.h>

#include "../net/net_route.h"
#include "../net/wifi_mgr.h"
#include "../settings.h"
#include "lua_bindings.h"
#include "lua_runtime.h"

extern "C" {
#include "../lua/lauxlib.h"
#include "../lua/lua.h"
}

namespace bindings {
namespace {

// -- wifi --------------------------------------------------------------------

int l_connect(lua_State *L) {
  const char *ssid = luaL_checkstring(L, 1);
  const char *password = luaL_optstring(L, 2, "");
  // Apps do not get to overwrite the user's saved network.
  lua_pushboolean(L, wifi_mgr::connect(String(ssid), String(password), false));
  return 1;
}

// wifi.connect_enterprise{ ssid=, username=, password=, ca=, domain=, method=,
//                          identity=, phase2= }
//
// A table rather than eight positional arguments, because at a call site
// `wifi.connect_enterprise{ssid="DefCon", username=u, password=p, ca="defcon.pem",
// domain="wifireg.defcon.org"}` is readable and the positional form is not.
int l_connect_enterprise(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);

  auto field = [&](const char *key, const char *fallback = "") -> String {
    lua_getfield(L, 1, key);
    const char *value = lua_isstring(L, -1) ? lua_tostring(L, -1) : fallback;
    const String out(value);
    lua_pop(L, 1);
    return out;
  };

  // luaL_error longjmps and skips C++ destructors, so no heap-owning String
  // (ssid, or the Enterprise config's Strings) may be live across it. Compute
  // everything inside a scope, capture the outcome as plain scalars, let the
  // Strings destruct, and only then raise or push results.
  const char *errmsg = nullptr;
  bool ok = false;
  bool caPresent = false;
  {
    const String ssid = field("ssid");

    wifi_mgr::Enterprise config;
    config.method = wifi_mgr::eapFromString(field("method", "peap"));
    config.identity = field("identity");
    config.username = field("username");
    config.password = field("password");
    config.caCertName = field("ca");
    config.domain = field("domain");
    config.ttlsPhase2 = field("phase2", "mschapv2");

    if (ssid.length() == 0) {
      errmsg = "connect_enterprise: ssid is required";
    } else if (!config.valid()) {
      errmsg = "connect_enterprise: username is required for PEAP and TTLS";
    } else {
      // As with wifi.connect(), an app does not get to overwrite the user's
      // saved network - it can join one for its own purposes, not repoint the
      // badge.
      ok = wifi_mgr::connectEnterprise(ssid, config, false);
      caPresent = config.caCertName.length() > 0;
    }
  }  // ssid and config are destroyed here, before any longjmp below.

  if (errmsg != nullptr) return luaL_error(L, "%s", errmsg);

  lua_pushboolean(L, ok);
  if (!ok) {
    // The usual cause is a CA name that is not installed, and returning a
    // reason saves the app author a trip to the console.
    lua_pushstring(L, caPresent ? "association refused (is the CA certificate installed?)"
                                : "association refused");
    return 2;
  }
  return 1;
}

int l_enterprise(lua_State *L) {
  lua_pushboolean(L, wifi_mgr::usingEnterprise());
  return 1;
}

int l_disconnect(lua_State *L) {
  (void)L;
  wifi_mgr::disconnect();
  return 0;
}

// "Do I have a route?", not "is the station associated?" - see the header note.
int l_connected(lua_State *L) {
  lua_pushboolean(L, net_route::available());
  return 1;
}

int l_status(lua_State *L) {
  lua_pushstring(L, net_route::statusText());
  return 1;
}

int l_ip(lua_State *L) {
  // Not faked when bridged: the badge really has no address of its own, and an
  // app that shows one would be showing a fiction it could act on.
  lua_pushstring(L, wifi_mgr::ip().toString().c_str());
  return 1;
}

int l_ssid(lua_State *L) {
  lua_pushstring(L, net_route::ssid().c_str());
  return 1;
}

int l_rssi(lua_State *L) {
  lua_pushinteger(L, wifi_mgr::rssi());
  return 1;
}

int l_mac(lua_State *L) {
  // Lowercased to match badge.espnow: peers()[i].mac and the mac handed to
  // on_espnow() both come from espnow_mgr::macToString(), which formats with
  // %02x, while Arduino's WiFi.macAddress() returns uppercase. An app comparing
  // its own address against an ESP-NOW sender - the obvious way to filter out
  // your own broadcasts - would silently never match. One convention, and it is
  // the one apps actually compare against.
  String mac = wifi_mgr::macAddress();
  mac.toLowerCase();
  lua_pushstring(L, mac.c_str());
  return 1;
}

int l_channel(lua_State *L) {
  lua_pushinteger(L, wifi_mgr::channel());
  return 1;
}

// wifi.scan() kicks off an async scan; wifi.scanning() reports progress and
// wifi.networks() returns the results once it is done.
int l_scan(lua_State *L) {
  lua_pushboolean(L, wifi_mgr::startScan());
  return 1;
}

int l_scanning(lua_State *L) {
  lua_pushboolean(L, wifi_mgr::scanning());
  return 1;
}

int l_networks(lua_State *L) {
  const int count = wifi_mgr::scanResultCount();
  lua_createtable(L, count, 0);
  for (int i = 0; i < count; ++i) {
    lua_createtable(L, 0, 3);
    lua_pushstring(L, wifi_mgr::scanSsid(i).c_str());
    lua_setfield(L, -2, "ssid");
    lua_pushinteger(L, wifi_mgr::scanRssi(i));
    lua_setfield(L, -2, "rssi");
    lua_pushboolean(L, wifi_mgr::scanEncrypted(i));
    lua_setfield(L, -2, "encrypted");
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

int l_hotspot(lua_State *L) {
  const char *password = luaL_optstring(L, 1, "");
  lua_pushboolean(L, wifi_mgr::startAccessPoint(String(password)));
  return 1;
}

const luaL_Reg WIFI_FUNCTIONS[] = {
    {"connect", l_connect},
    {"connect_enterprise", l_connect_enterprise},
    {"enterprise", l_enterprise},
    {"disconnect", l_disconnect}, {"connected", l_connected},
    {"status", l_status},     {"ip", l_ip},                 {"ssid", l_ssid},
    {"rssi", l_rssi},         {"mac", l_mac},               {"channel", l_channel},
    {"scan", l_scan},         {"scanning", l_scanning},     {"networks", l_networks},
    {"hotspot", l_hotspot},   {nullptr, nullptr},
};

// -- http --------------------------------------------------------------------

// Shared by get and post. Returns status, body on success; nil, message on a
// transport failure.
int request(lua_State *L, const char *method, bool hasBody) {
  const char *url = luaL_checkstring(L, 1);

  int bodyArg = 2;
  size_t bodyLength = 0;
  const char *body = nullptr;
  if (hasBody) {
    body = luaL_optlstring(L, 2, "", &bodyLength);
    bodyArg = 3;
  }
  const char *contentType = luaL_optstring(L, bodyArg, "text/plain");
  const uint32_t timeoutMs =
      (uint32_t)constrain((int)luaL_optinteger(L, bodyArg + 1, 5000), 100, 10000);

  // No route at all - neither Wi-Fi nor a phone bridge. The message is the one
  // this binding has always returned, because apps in the wild compare against
  // it verbatim.
  if (!net_route::available()) {
    lua_pushnil(L);
    lua_pushstring(L, "wifi not connected");
    return 2;
  }

  // net_route blocks for up to timeoutMs (plus the route's slack) and extends
  // the callback deadline itself by what it intends to spend; without that the
  // instruction hook would fire mid-request and kill the app for waiting on the
  // network.
  //
  // `response` is still live across the lua_push* calls below, which can raise
  // on an allocation failure and longjmp past its destructor. That is the same
  // trade the previous HTTPClient version made with `payload`, and the same one
  // every other binding here makes; the alternative is a second full copy of the
  // body just to be able to free the first.
  const net_route::Response response =
      net_route::request(method, String(url), hasBody ? String(body, (unsigned int)bodyLength)
                                                      : String(),
                         String(contentType), timeoutMs);

  if (!response.ok) {
    lua_pushnil(L);
    lua_pushstring(L, response.err.c_str());
    return 2;
  }

  lua_pushinteger(L, response.status);
  lua_pushlstring(L, response.body.c_str(), response.body.length());
  return 2;
}

int l_http_get(lua_State *L) { return request(L, "GET", false); }
int l_http_post(lua_State *L) { return request(L, "POST", true); }

const luaL_Reg HTTP_FUNCTIONS[] = {
    {"get", l_http_get}, {"post", l_http_post}, {nullptr, nullptr},
};

}  // namespace

void openNet(lua_State *L) {
  setTable(L, "wifi", WIFI_FUNCTIONS, nullptr);
  setTable(L, "http", HTTP_FUNCTIONS, nullptr);
}

}  // namespace bindings
