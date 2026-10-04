// src/native_apps/selftest/suite_network.cpp
// NETWORK: only meaningful when the badge is joined to a network as a station; otherwise every
// row is "--" ("not joined").
//
//   link       the address and the signal
//   listener   GET <listener_url>/health (integration/backend.md); 200 is OK, 404 is "--" (the
//              route is not there yet: the listener answered), no answer is FAIL
//   rpc        JSON-RPC getHealth to rpc_url; "result":"ok" is OK
//   time       where the clock came from: SNTP is OK, the floor or nothing yet is "--"
//
// The two requests block the loop (net_route::request is the one HTTP call the firmware has), so
// each gets its own frame, is preceded by a frame that shows the row running, and has a hard
// timeout of REQUEST_TIMEOUT_MS: the balance feature's 3 s. No URL is printed (the values are
// public, but long); nothing secret is.
#include <Arduino.h>

#include <stdio.h>
#include <string.h>

#include "../../net/net_route.h"
#include "../../net/wifi_mgr.h"
#include "../../vk/core/clock.h"
#include "../../vk/core/config.h"
#include "selftest.h"
#include "shared.h"
#include "suites.h"

namespace selftest {
namespace {

constexpr uint32_t REQUEST_TIMEOUT_MS = 3000;

bool notJoined(Ctx &c) {
  if (joined()) return false;
  c.finish(State::Skip, "not joined");
  return true;
}

void checkLink(Ctx &c) {
  if (notJoined(c)) return;
  c.finish(State::Ok, "%s %ddBm", wifi_mgr::ip().toString().c_str(), wifi_mgr::rssi());
}

// One request, on the frame after the one that shows the row running. False on the first call.
bool requestFrame(Ctx &c) {
  if (c.step == 0) {
    c.step = 1;
    c.redraw = true;
    return false;
  }
  return true;
}

void checkListener(Ctx &c) {
  if (notJoined(c)) return;
  String url = vk::config::text("listener_url");
  if (url.length() == 0) {
    c.finish(State::Skip, "no listener_url");
    return;
  }
  if (!requestFrame(c)) return;
  while (url.endsWith("/")) url.remove(url.length() - 1);
  const uint32_t before = millis();
  const net_route::Response r = net_route::request("GET", url + "/health", "", "", REQUEST_TIMEOUT_MS);
  const unsigned long ms = millis() - before;
  if (!r.ok && r.status == 0) c.finish(State::Fail, "no answer (%.20s)", r.err.c_str());
  else if (r.status == 200) c.finish(State::Ok, "200 in %lu ms", ms);
  else if (r.status == 404) c.finish(State::Skip, "answers, no /health (404)");
  else c.finish(State::Fail, "status %d in %lu ms", r.status, ms);
}

void checkRpc(Ctx &c) {
  if (notJoined(c)) return;
  const String url = vk::config::text("rpc_url");
  if (url.length() == 0) {
    c.finish(State::Skip, "no rpc_url");
    return;
  }
  if (!requestFrame(c)) return;
  const uint32_t before = millis();
  const net_route::Response r = net_route::request(
      "POST", url, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getHealth\"}", "application/json", REQUEST_TIMEOUT_MS);
  const unsigned long ms = millis() - before;
  if (!r.ok && r.status == 0) c.finish(State::Fail, "no answer (%.20s)", r.err.c_str());
  else if (r.status != 200) c.finish(State::Fail, "status %d", r.status);
  else if (r.body.indexOf("\"result\":\"ok\"") >= 0) c.finish(State::Ok, "healthy, %lu ms", ms);
  else c.finish(State::Fail, "unhealthy, %lu ms", ms);
}

void checkTime(Ctx &c) {
  if (notJoined(c)) return;
  switch (vk::clock::source()) {
    case vk::clock::Source::SNTP: {
      const uint32_t t = vk::clock::now();
      c.finish(State::Ok, "sntp %02u:%02u:%02u UTC", (unsigned)((t / 3600) % 24), (unsigned)((t / 60) % 60), (unsigned)(t % 60));
      return;
    }
    case vk::clock::Source::FLOOR:
      c.finish(State::Skip, "floor only, no sntp yet");
      return;
    default:
      c.finish(State::Skip, "no sntp yet");
      return;
  }
}

const Check TABLE[] = {
    {"link", "LINK", Kind::Auto, Profile::Any, checkLink, nullptr, 0},
    {"listener", "LISTENER", Kind::Auto, Profile::Any, checkListener, nullptr, 0},
    {"rpc", "RPC NODE", Kind::Auto, Profile::Any, checkRpc, nullptr, 0},
    {"time", "TIME SYNC", Kind::Auto, Profile::Any, checkTime, nullptr, 0},
};

}  // namespace

const Suite NETWORK = {
    "network", "NETWORK", Profile::Any, TABLE, sizeof TABLE / sizeof TABLE[0], nullptr, nullptr, nullptr, false,
};

}  // namespace selftest
