// src/native_apps/selftest/suite_radio.cpp
// RADIO: ESP-NOW and the Wi-Fi radio (protocol/espnow.md).
//
//   espnow     on, and its channel ("--" when it is off; the rows below are then "--" too)
//   beacon     the presence beacon that lets other badges list this one
//   broadcast  one frame sent through the router to everyone; the radio queued it
//   peers      badges heard in the next 5 s ("--" when none: it depends on who is around)
//   wifi       the Wi-Fi radio's state: off, hotspot, or station and whether joined (no password)
//   channel    joined to a network with ESP-NOW on: both on the same channel, as espnow.md requires
//
// Not checked: the presence exchange (CHAL and PROOF). It needs a second badge; protocol/espnow.md
// gives no way for a badge to answer its own challenge.
//
// The broadcast is the plain text PING_TEXT, not a VK frame, so it takes no type number from the
// registry in espnow.md: a badge that hears it hands it to its running app as an app message, and
// apps ignore payloads that are not theirs.
#include <Arduino.h>

#include <stdio.h>
#include <string.h>

#include "../../net/espnow_mgr.h"
#include "../../net/wifi_mgr.h"
#include "../../vk/host/router.h"
#include "selftest.h"
#include "shared.h"
#include "suites.h"

namespace selftest {
namespace {

constexpr char PING_TEXT[] = "BADGEOS SELFTEST PING";
constexpr uint32_t LISTEN_MS = 5000;

void checkEspnow(Ctx &c) {
  if (!espnow_mgr::enabled()) c.finish(State::Skip, "off");
  else c.finish(State::Ok, "on, ch %u", (unsigned)espnow_mgr::channel());
}

void checkBeacon(Ctx &c) {
  if (!espnow_mgr::enabled()) c.finish(State::Skip, "espnow off");
  else if (espnow_mgr::beaconEnabled()) c.finish(State::Ok, "on");
  else c.finish(State::Skip, "off");
}

void checkBroadcast(Ctx &c) {
  if (!espnow_mgr::enabled()) {
    c.finish(State::Skip, "espnow off");
    return;
  }
  if (vk::host::router::send(nullptr, (const uint8_t *)PING_TEXT, sizeof PING_TEXT - 1)) {
    c.finish(State::Ok, "%u bytes queued", (unsigned)(sizeof PING_TEXT - 1));
  } else {
    c.finish(State::Fail, "send refused");
  }
}

// Step 0 notes the time; the result counts the peers whose last frame came after it.
void checkPeers(Ctx &c) {
  if (!espnow_mgr::enabled()) {
    c.finish(State::Skip, "espnow off");
    return;
  }
  if (c.step == 0) {
    c.step = 1;
    c.stepAt = c.now;
    c.redraw = true;
    return;
  }
  if (c.now - c.stepAt < LISTEN_MS) return;
  unsigned heard = 0;
  const size_t total = espnow_mgr::peerCount();
  for (size_t i = 0; i < total; ++i) {
    const espnow_mgr::Peer *peer = espnow_mgr::peerAt(i);
    if (peer != nullptr && (int32_t)(peer->lastSeenMs - c.stepAt) >= 0) ++heard;
  }
  if (heard == 0) c.finish(State::Skip, "none in 5 s, %u known", (unsigned)total);
  else c.finish(State::Ok, "%u in 5 s, %u known", heard, (unsigned)total);
}

void checkWifi(Ctx &c) {
  switch (wifi_mgr::mode()) {
    case wifi_mgr::Mode::Off:
      c.finish(State::Skip, "off");
      return;
    case wifi_mgr::Mode::AccessPoint:
      c.finish(State::Ok, "hotspot, ch %u", (unsigned)wifi_mgr::channel());
      return;
    default:
      break;
  }
  if (!wifi_mgr::connected()) c.finish(State::Skip, "station, %s", wifi_mgr::statusText());
  else c.finish(State::Ok, "%.14s %ddBm ch %u", wifi_mgr::ssid().c_str(), wifi_mgr::rssi(), (unsigned)wifi_mgr::channel());
}

void checkChannel(Ctx &c) {
  if (!espnow_mgr::enabled() || !joined()) {
    c.finish(State::Skip, "needs espnow and wi-fi");
    return;
  }
  const unsigned espnow = espnow_mgr::channel(), wifi = wifi_mgr::channel();
  if (espnow == wifi) c.finish(State::Ok, "both on ch %u", espnow);
  else c.finish(State::Fail, "espnow ch %u, wi-fi ch %u", espnow, wifi);
}

const Check TABLE[] = {
    {"espnow", "ESP-NOW", Kind::Auto, Profile::Any, checkEspnow, nullptr, 0},
    {"beacon", "BEACON", Kind::Auto, Profile::Any, checkBeacon, nullptr, 0},
    {"broadcast", "BROADCAST", Kind::Auto, Profile::Any, checkBroadcast, nullptr, 0},
    {"peers", "PEERS HEARD", Kind::Auto, Profile::Any, checkPeers, nullptr, LISTEN_MS + 3000},
    {"wifi", "WI-FI", Kind::Auto, Profile::Any, checkWifi, nullptr, 0},
    {"channel", "SAME CHANNEL", Kind::Auto, Profile::Any, checkChannel, nullptr, 0},
};

}  // namespace

const Suite RADIO = {
    "radio", "RADIO", Profile::Any, TABLE, sizeof TABLE / sizeof TABLE[0], nullptr, nullptr, nullptr, false,
};

}  // namespace selftest
