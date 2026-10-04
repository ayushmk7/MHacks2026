// Settings page `espnow` (docs/os/ui/shell.md, "ESP-NOW"): the radar. Every badge in range that
// runs BadgeOS on the same channel, with its signal and the time since it was last heard.
// Deleting this file removes the page and nothing else.
#include "../page.h"

#include "../../../net/espnow_mgr.h"
#include "../../../settings.h"

namespace {
using namespace vk::shell;
namespace receipt = vk::ui::receipt;
namespace tk = vk::ui::theme;

constexpr int PEERS_Y = 110;
constexpr int PEERS_VISIBLE = 6;      // rows at 110 .. 200

void espnowValue(char *out, size_t cap) {
  if (!espnow_mgr::enabled()) {
    snprintf(out, cap, "off");
    return;
  }
  const unsigned peers = (unsigned)espnow_mgr::peerCount();
  snprintf(out, cap, "%u peer%s", peers, peers == 1 ? "" : "s");
}

void espnowUpdate() {
  if (back()) return;                                  // CANCEL

  if (buttons::pressed(BTN_A)) {
    if (espnow_mgr::enabled()) {
      espnow_mgr::end();
      settings::setEspnowEnabledAtBoot(false);
    } else {
      espnow_mgr::begin(settings::espnowChannel());
      settings::setEspnowEnabledAtBoot(true);
    }
    repaint();
    return;
  }

  // Changing channel means tearing ESP-NOW down and bringing it back up: an initialised peer set
  // cannot move across channels.
  int channel = settings::espnowChannel();
  if (buttons::pressed(BTN_LEFT)) --channel;
  if (buttons::pressed(BTN_RIGHT)) ++channel;
  if (channel != settings::espnowChannel() && channel >= 1 && channel <= 13) {
    settings::setEspnowChannel((uint8_t)channel);
    if (espnow_mgr::enabled()) {
      espnow_mgr::end();
      espnow_mgr::begin((uint8_t)channel);
    }
    repaint();
  }
}

void espnowDraw() {
  frame("ESP-NOW", "SELECT on/off  LEFT/RIGHT channel");

  const bool enabled = espnow_mgr::enabled();
  const size_t peers = espnow_mgr::peerCount();
  char value[16];
  receipt::row(X0, X1, 48, "STATE", onOff(enabled), false, onOffColor(enabled));
  snprintf(value, sizeof(value), "%u", (unsigned)espnow_mgr::channel());
  receipt::row(X0, X1, 66, "CHANNEL", value);
  snprintf(value, sizeof(value), "%u", (unsigned)peers);
  receipt::row(X0, X1, 84, "PEERS", value);
  receipt::rule(100);

  if (!enabled) {
    textCentered(160, 130, "Press SELECT to turn ESP-NOW on", tk::SUB);
    return;
  }
  if (peers == 0) {
    textCentered(160, 130, "Listening for other badges...", tk::SUB);
    textCentered(160, 144, "They must be on the same channel", tk::SUB);
    return;
  }

  const uint32_t now = millis();
  for (int i = 0; i < PEERS_VISIBLE && (size_t)i < peers; ++i) {
    const espnow_mgr::Peer *peer = espnow_mgr::peerAt((size_t)i);
    if (peer == nullptr) continue;
    char name[sizeof(peer->name) + 1];
    memcpy(name, peer->name, sizeof(peer->name));
    name[sizeof(peer->name)] = '\0';
    char detail[40];
    snprintf(detail, sizeof(detail), "%ddBm %lus", (int)peer->rssi,
             (unsigned long)((now - peer->lastSeenMs) / 1000));
    receipt::row(X0, X1, PEERS_Y + i * ROW_PITCH, name, detail);
  }
}
}  // namespace

VK_SETTINGS_PAGE(espnow, "espnow", 40, "ESP-NOW", espnowValue, nullptr, espnowUpdate, espnowDraw, 250);
