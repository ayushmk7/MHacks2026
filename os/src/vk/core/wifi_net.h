// src/vk/core/wifi_net.h
// Saved Wi-Fi networks and the join state machine (docs/os/ui/shell.md, "Wi-Fi";
// docs/os/platform/config.md, "Wi-Fi"). The radio itself stays upstream's: src/net/wifi_mgr.
//
// Saved networks. Upstream remembers one network (its `sysconf` settings: what VKWIFI, JOINWIFI and
// the web page write, and what it joins at boot). This module keeps a list of MAX_SAVED in its own
// NVS namespace, `vkwifi`, newest first, and keeps the two in step:
//   - whatever lands in upstream's slot is copied to the front of the list within SYNC_MS, so a
//     network set over USB or from the web page is in the same list as one typed on the badge;
//   - the front of the list is written to upstream's slot, so upstream's join at boot is the
//     network joined last.
// A stored password never leaves this file: no accessor returns it, nothing logs it, and it is not
// a config key (VKGET prints config values).
//
// Joining. join() and joinSaved() start one attempt with a hard timeout (config key
// `wifi_join_s`) and end in JOINED, WRONG_PASSWORD, NOT_FOUND or TIMEOUT; the caller polls
// joinState(). A network is saved when, and only when, it was joined. joinBest() is the quiet
// one, for "Wi-Fi on": it joins the strongest saved network in range, or keeps trying the newest.
//
// Auto-join (config key `wifi_autojoin`). With two or more saved networks, when the badge wants
// to be on a network (station mode) and has not been for AUTOJOIN_AFTER_MS, the service does what
// joinBest() does, at most once every AUTOJOIN_RETRY_MS, and only while no app runs and no
// approval is open. With one saved network there is nothing to choose: upstream keeps retrying it.
#pragma once

#include <Arduino.h>

namespace vk::wifi {

constexpr size_t MAX_SAVED = 4;
constexpr size_t SSID_LEN_MAX = 32;      // 802.11: a network name is 1..32 bytes
constexpr size_t PASS_LEN_MIN = 8;       // WPA2 passphrase: 8..63 characters
constexpr size_t PASS_LEN_MAX = 63;

// ---- saved networks, newest first ----
size_t savedCount();
const char *savedSsid(size_t index);                      // "" past the end
bool isSaved(const char *ssid);
bool savedIsOpen(const char *ssid);                       // saved with no password
// To the front of the list (replacing an entry of the same name; the oldest drops off a full
// list) and into upstream's slot. False when the name or the password has a length no network
// accepts, or NVS refused the write.
bool remember(const char *ssid, const char *password);
bool forget(const char *ssid);                            // false when it was not saved

// ---- joining ----
enum class Join : uint8_t { IDLE, CONNECTING, JOINED, WRONG_PASSWORD, NOT_FOUND, TIMEOUT };

bool join(const char *ssid, const char *password);        // typed credentials; "" = an open network
bool joinSaved(const char *ssid);                         // with the stored password
void joinBest();                                          // quiet: no result, see above
Join joinState();
const char *joinStateName();                              // "idle", "connecting", "joined", "wrong_password", "not_found", "timeout"
const char *joinSsid();                                   // the network of the attempt; "" when IDLE
uint32_t joinElapsedMs();                                 // since the attempt began
uint32_t joinTimeoutMs();
void joinCancel();                                        // CONNECTING: stop and turn Wi-Fi off. Then IDLE
void joinDismiss();                                       // the result was read: IDLE (Wi-Fi is left as it is)

// The user is choosing a network: no automatic join or scan for the next HOLD_MS. The Wi-Fi
// screens call it on every pass.
void hold();

// ---- pure, host-tested ----
enum class Fail : uint8_t { OTHER, NOT_FOUND, AUTH };
// What an 802.11 disconnect reason says about a join. `decisive` (may be nullptr) is set when one
// such event settles it; otherwise two do, because the driver retries those by itself.
Fail classify(uint8_t reason, bool *decisive);

#ifdef VK_HOST_TEST
// Host-test seams (test/host/test_wifi_net.cpp). The radio and upstream's settings are these
// variables; hostReboot() forgets what is in RAM, as a reboot does, and keeps the shim's NVS.
struct HostRadio {
  String slotSsid, slotPassword;     // upstream's saved network
  bool slotEnterprise = false;
  bool station = false;              // wifi_mgr::mode() == Station
  bool up = false;                   // ... and connected
  String ssid;                       // the network it is connected to
  String connectSsid, connectPassword;   // the last wifi_mgr::connect()
  unsigned connects = 0, disconnects = 0, scans = 0;
  bool scanning = false;
  int scanCount = 0;
  String scanSsid[8];
  int scanRssi[8] = {0};
  bool idle = true;
};
extern HostRadio hostRadio;
void hostReboot();
void hostBegin();                    // the service's begin
void hostUpdate();                   // the service's update: one loop pass
void hostDisconnectEvent(uint8_t reason);   // what the Wi-Fi event task reports
#endif

}  // namespace vk::wifi
