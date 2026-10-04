// src/vk/core/wifi_net.cpp
// Saved Wi-Fi networks, the join state machine and auto-join (wifi_net.h; docs/os/ui/shell.md,
// "Wi-Fi"). The radio is upstream's wifi_mgr and the one saved network of upstream's settings;
// every call to either is in the "radio" block below, which the host test replaces with variables.
//
// No password is logged or returned by anything in this file.
#include "wifi_net.h"

#include <Preferences.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "service.h"

#ifndef VK_HOST_TEST
#include <WiFi.h>

#include "../../badge_log.h"
#include "../../net/wifi_mgr.h"
#include "../../settings.h"
#include "../host/home.h"
#include "../vk.h"
#endif

namespace vk::wifi {

namespace {

// ---- tunables -------------------------------------------------------------------------------------
const char NVS_NAMESPACE[] = "vkwifi";            // keys s0..s3 (names) and p0..p3 (passwords), newest first
constexpr uint32_t SYNC_MS = 2000;                // how often upstream's saved network is looked at
constexpr uint32_t HOLD_MS = 5000;                // hold(): no automatic join for this long
constexpr uint32_t AUTOJOIN_AFTER_MS = 15000;     // off the network this long before the first automatic look
constexpr uint32_t AUTOJOIN_RETRY_MS = 60000;     // and this long between two looks
constexpr uint32_t BEST_SCAN_TIMEOUT_MS = 10000;  // a scan that has not ended by then is given up
constexpr uint8_t EVENTS_TO_DECIDE = 2;           // disconnect events of one kind that settle a join
constexpr uint8_t EVENTS_CAP = 200;               // the counters stop here (they are one byte)
constexpr size_t PSK_HEX_LEN = 64;                // upstream's slot may hold a raw key: 64 hex digits

// 802.11 / ESP-IDF disconnect reasons (esp_wifi_types_generic.h), by value so that classify() needs
// no radio header on the host.
constexpr uint8_t REASON_MIC_FAILURE = 14;
constexpr uint8_t REASON_4WAY_HANDSHAKE_TIMEOUT = 15;
constexpr uint8_t REASON_NO_AP_FOUND = 201;
constexpr uint8_t REASON_AUTH_FAIL = 202;
constexpr uint8_t REASON_HANDSHAKE_TIMEOUT = 204;
constexpr uint8_t REASON_NO_AP_COMPATIBLE_SECURITY = 210;
constexpr uint8_t REASON_NO_AP_IN_AUTHMODE = 211;
constexpr uint8_t REASON_NO_AP_IN_RSSI = 212;
#ifndef VK_HOST_TEST
static_assert(REASON_MIC_FAILURE == WIFI_REASON_MIC_FAILURE && REASON_4WAY_HANDSHAKE_TIMEOUT == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT &&
              REASON_NO_AP_FOUND == WIFI_REASON_NO_AP_FOUND && REASON_AUTH_FAIL == WIFI_REASON_AUTH_FAIL &&
              REASON_HANDSHAKE_TIMEOUT == WIFI_REASON_HANDSHAKE_TIMEOUT &&
              REASON_NO_AP_COMPATIBLE_SECURITY == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY &&
              REASON_NO_AP_IN_AUTHMODE == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD &&
              REASON_NO_AP_IN_RSSI == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD,
              "the disconnect reasons moved: update the constants above");
#endif

}  // namespace

VK_CONFIG_KEY(wifi_join_s, "wifi_join_s", vk::config::Type::U32, "15", vk::config::F_NONE, 5, 60,
              "seconds before a Wi-Fi join started on the badge gives up");
VK_CONFIG_KEY(wifi_autojoin, "wifi_autojoin", vk::config::Type::U32, "1", vk::config::F_NONE, 0, 1,
              "1: when off the network, join the strongest saved Wi-Fi network; 0: never");

#ifdef VK_HOST_TEST
HostRadio hostRadio;
#endif

namespace {

// ---- the radio and upstream's settings ----------------------------------------------------------------
#ifndef VK_HOST_TEST
String slotSsid() { return ::settings::wifiSsid(); }
String slotPassword() { return ::settings::wifiPassword(); }
bool slotEnterprise() { return ::settings::wifiIsEnterprise(); }
bool slotSet(const char *ssid, const char *password) { return ::settings::setWifiCredentials(ssid, password); }
void slotForget() { ::settings::forgetWifi(); }

bool radioStation() { return ::wifi_mgr::mode() == ::wifi_mgr::Mode::Station; }
bool radioUp() { return radioStation() && ::wifi_mgr::connected(); }
String radioSsid() { return ::wifi_mgr::ssid(); }
void radioConnect(const char *ssid, const char *password) { ::wifi_mgr::connect(ssid, password, false); }
void radioOff() { ::wifi_mgr::disconnect(); }
bool radioScanStart() { return ::wifi_mgr::startScan(); }
bool radioScanning() { return ::wifi_mgr::scanning(); }
int radioScanCount() { return ::wifi_mgr::scanResultCount(); }
String radioScanSsid(int i) { return ::wifi_mgr::scanSsid(i); }
int radioScanRssi(int i) { return ::wifi_mgr::scanRssi(i); }
// No app is running and no approval is open: nobody is using ESP-NOW for a payment right now.
bool quiet() { return vk::host::idle() && !vk::modalActive(); }

void logf(const char *format, ...) __attribute__((format(printf, 1, 2)));
void logf(const char *format, ...) {
  char line[96];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof line, format, args);
  va_end(args);
  badge_log::tagf("wifi", "%s", line);
}
#else
String slotSsid() { return hostRadio.slotSsid; }
String slotPassword() { return hostRadio.slotPassword; }
bool slotEnterprise() { return hostRadio.slotEnterprise; }
bool slotSet(const char *ssid, const char *password) {
  hostRadio.slotSsid = ssid;
  hostRadio.slotPassword = password;
  hostRadio.slotEnterprise = false;
  return true;
}
void slotForget() {
  hostRadio.slotSsid = "";
  hostRadio.slotPassword = "";
  hostRadio.slotEnterprise = false;
}

bool radioStation() { return hostRadio.station; }
bool radioUp() { return hostRadio.station && hostRadio.up; }
String radioSsid() { return hostRadio.ssid; }
void radioConnect(const char *ssid, const char *password) {
  hostRadio.connectSsid = ssid;
  hostRadio.connectPassword = password;
  hostRadio.station = true;
  hostRadio.up = false;
  ++hostRadio.connects;
}
void radioOff() {
  hostRadio.station = false;
  hostRadio.up = false;
  hostRadio.ssid = "";
  ++hostRadio.disconnects;
}
bool radioScanStart() {
  if (hostRadio.station && !hostRadio.up) return false;   // as the driver: no scan while it is connecting
  hostRadio.scanning = true;
  ++hostRadio.scans;
  return true;
}
bool radioScanning() { return hostRadio.scanning; }
int radioScanCount() { return hostRadio.scanCount; }
String radioScanSsid(int i) { return hostRadio.scanSsid[i]; }
int radioScanRssi(int i) { return hostRadio.scanRssi[i]; }
bool quiet() { return hostRadio.idle; }
void logf(const char *, ...) {}
#endif

// ---- the saved list -------------------------------------------------------------------------------------
struct Saved {
  char ssid[SSID_LEN_MAX + 1];
  char pass[PSK_HEX_LEN + 1];
};

Saved sSaved[MAX_SAVED];
size_t sCount = 0;
bool sLoaded = false;
bool sOpen = false;                // the NVS namespace is open
Preferences sPrefs;

void wipe(char *buffer, size_t size) {
  volatile char *p = buffer;
  for (size_t i = 0; i < size; ++i) p[i] = 0;
}

bool validSsid(const char *ssid) {
  const size_t n = ssid != nullptr ? strlen(ssid) : 0;
  return n >= 1 && n <= SSID_LEN_MAX;
}

// Empty (an open network), a passphrase, or a raw key as upstream's slot may hold it.
bool validPassword(const char *password) {
  const size_t n = password != nullptr ? strlen(password) : 0;
  return n == 0 || (n >= PASS_LEN_MIN && n <= PSK_HEX_LEN);
}

void slotKey(char *out, size_t cap, char kind, size_t index) { snprintf(out, cap, "%c%u", kind, (unsigned)index); }

void loadList() {
  sCount = 0;
  wipe((char *)sSaved, sizeof sSaved);
  if (!sOpen) sOpen = sPrefs.begin(NVS_NAMESPACE, false);
  sLoaded = true;
  if (!sOpen) return;
  for (size_t i = 0; i < MAX_SAVED; ++i) {
    char key[4];
    Saved entry = {};
    slotKey(key, sizeof key, 's', i);
    if (!sPrefs.isKey(key) || sPrefs.getString(key, entry.ssid, sizeof entry.ssid) == 0) break;
    slotKey(key, sizeof key, 'p', i);
    if (sPrefs.isKey(key)) sPrefs.getString(key, entry.pass, sizeof entry.pass);
    if (!validSsid(entry.ssid) || !validPassword(entry.pass)) break;
    sSaved[sCount++] = entry;
  }
}

void ensure() {
  if (!sLoaded) loadList();
}

// Writes `value` under `key` unless it is already there. putString reports 0 bytes for an empty
// string whether or not it was written, so an empty value is checked by reading it back.
bool putText(const char *key, const char *value) {
  char stored[PSK_HEX_LEN + 1];
  if (sPrefs.isKey(key) && sPrefs.getString(key, stored, sizeof stored) > 0 && strcmp(stored, value) == 0) {
    wipe(stored, sizeof stored);
    return true;
  }
  const size_t written = sPrefs.putString(key, value);
  if (value[0] != '\0') return written == strlen(value);
  const bool ok = sPrefs.isKey(key) && sPrefs.getString(key, stored, sizeof stored) == 1;
  return ok;
}

bool storeList() {
  if (!sOpen) return false;
  bool ok = true;
  for (size_t i = 0; i < MAX_SAVED; ++i) {
    char name[4], pass[4];
    slotKey(name, sizeof name, 's', i);
    slotKey(pass, sizeof pass, 'p', i);
    if (i < sCount) {
      ok = putText(name, sSaved[i].ssid) && ok;
      ok = putText(pass, sSaved[i].pass) && ok;
    } else {
      if (sPrefs.isKey(name)) sPrefs.remove(name);
      if (sPrefs.isKey(pass)) sPrefs.remove(pass);
    }
  }
  return ok;
}

int indexOf(const char *ssid) {
  if (ssid == nullptr) return -1;
  for (size_t i = 0; i < sCount; ++i) {
    if (strcmp(sSaved[i].ssid, ssid) == 0) return (int)i;
  }
  return -1;
}

// The list only. On a failed write the list is read back, so RAM never claims more than NVS holds.
bool rememberLocal(const char *ssid, const char *password) {
  ensure();
  if (!validSsid(ssid) || !validPassword(password)) return false;
  Saved entry = {};
  strlcpy(entry.ssid, ssid, sizeof entry.ssid);
  strlcpy(entry.pass, password != nullptr ? password : "", sizeof entry.pass);

  const int old = indexOf(ssid);
  size_t last = old >= 0 ? (size_t)old : (sCount < MAX_SAVED ? sCount : MAX_SAVED - 1);
  for (size_t i = last; i > 0; --i) sSaved[i] = sSaved[i - 1];
  sSaved[0] = entry;
  if (old < 0 && sCount < MAX_SAVED) ++sCount;
  wipe(entry.pass, sizeof entry.pass);

  if (storeList()) return true;
  logf("could not store the saved networks");
  loadList();
  return false;
}

// Upstream's slot follows the front of the list.
void mirrorFront() {
  if (sCount == 0) return;
  if (!slotEnterprise() && slotSsid() == sSaved[0].ssid && slotPassword() == sSaved[0].pass) return;
  slotSet(sSaved[0].ssid, sSaved[0].pass);
}

// A network that VKWIFI, JOINWIFI or the web page saved goes to the front of the list.
void importSlot() {
  ensure();
  if (slotEnterprise()) return;                    // an enterprise profile is upstream's alone
  const String ssid = slotSsid();
  if (ssid.length() == 0) return;
  const String password = slotPassword();
  if (sCount > 0 && ssid == sSaved[0].ssid && password == sSaved[0].pass) return;
  if (rememberLocal(ssid.c_str(), password.c_str())) logf("'%s' is in the saved networks (%u)", ssid.c_str(), (unsigned)sCount);
}

// ---- joining ------------------------------------------------------------------------------------------
Join sJoin = Join::IDLE;
char sJoinSsid[SSID_LEN_MAX + 1] = "";
char sJoinPass[PSK_HEX_LEN + 1] = "";
uint32_t sJoinStarted = 0;
// Written by the Wi-Fi event task, read by the loop: one byte each, so no lock.
volatile uint8_t sAuthEvents = 0;
volatile uint8_t sMissEvents = 0;

bool sBestScan = false;            // a scan for the strongest saved network is running
uint32_t sBestStarted = 0;
uint32_t sDownSince = 0;           // station mode without a network since then; 0 = not counting
bool sAutoRan = false;             // an automatic look has run since the network was last up
uint32_t sHoldUntil = 0;
uint32_t sLastSync = 0;

uint32_t stamp(uint32_t now) { return now != 0 ? now : 1; }   // 0 means "not set"

void noteDisconnect(uint8_t reason) {
  bool decisive = false;
  const Fail fail = classify(reason, &decisive);
  const uint8_t weight = decisive ? EVENTS_TO_DECIDE : 1;
  if (fail == Fail::AUTH && sAuthEvents < EVENTS_CAP) sAuthEvents = (uint8_t)(sAuthEvents + weight);
  if (fail == Fail::NOT_FOUND && sMissEvents < EVENTS_CAP) sMissEvents = (uint8_t)(sMissEvents + weight);
}

#ifndef VK_HOST_TEST
void onStaDisconnected(arduino_event_t *event) {
  if (event != nullptr) noteDisconnect(event->event_info.wifi_sta_disconnected.reason);
}
#endif

void startConnect(const char *ssid, const char *password) {
  // From a clean radio: a link that is still up must not be mistaken for the new one, and the
  // disconnect events of a join that was under way must not be counted against this one.
  if (radioStation()) radioOff();
  sBestScan = false;
  strlcpy(sJoinSsid, ssid, sizeof sJoinSsid);
  strlcpy(sJoinPass, password != nullptr ? password : "", sizeof sJoinPass);
  sAuthEvents = 0;
  sMissEvents = 0;
  sJoinStarted = millis();
  sJoin = Join::CONNECTING;
  radioConnect(sJoinSsid, sJoinPass);
}

void finishJoin(Join result) {
  sJoin = result;
  if (result == Join::JOINED) {
    const bool saved = remember(sJoinSsid, sJoinPass);
    logf("joined '%s'%s", sJoinSsid, saved ? "" : " (not saved)");
  } else {
    radioOff();                                    // or the driver keeps retrying a join that failed
    logf("join '%s': %s", sJoinSsid, joinStateName());
  }
  wipe(sJoinPass, sizeof sJoinPass);
}

void updateJoin(uint32_t now) {
  if (sJoin != Join::CONNECTING) return;
  if (radioUp() && radioSsid() == sJoinSsid) {
    finishJoin(Join::JOINED);
  } else if (sAuthEvents >= EVENTS_TO_DECIDE) {
    finishJoin(Join::WRONG_PASSWORD);
  } else if (sMissEvents >= EVENTS_TO_DECIDE) {
    finishJoin(Join::NOT_FOUND);
  } else if (now - sJoinStarted >= joinTimeoutMs()) {
    finishJoin(sAuthEvents ? Join::WRONG_PASSWORD : (sMissEvents ? Join::NOT_FOUND : Join::TIMEOUT));
  }
}

// The quiet join: the radio goes for a saved network and nothing reports a result.
void connectSaved(size_t index) { radioConnect(sSaved[index].ssid, sSaved[index].pass); }

void startBestScan(uint32_t now) {
  if (!radioScanStart()) {
    radioOff();                                    // a radio busy with a join refuses to scan
    if (!radioScanStart()) {
      connectSaved(0);
      return;
    }
  }
  sBestScan = true;
  sBestStarted = now;
}

void updateBestScan(uint32_t now) {
  if (!sBestScan) return;
  if (radioScanning()) {
    if (now - sBestStarted < BEST_SCAN_TIMEOUT_MS) return;
    sBestScan = false;
    connectSaved(0);
    return;
  }
  sBestScan = false;
  int best = -1, bestRssi = 0;
  const int found = radioScanCount();
  for (int i = 0; i < found; ++i) {
    const int saved = indexOf(radioScanSsid(i).c_str());
    if (saved < 0) continue;
    const int rssi = radioScanRssi(i);
    if (best < 0 || rssi > bestRssi) {
      best = saved;
      bestRssi = rssi;
    }
  }
  // None in range: keep trying the newest, as upstream does with its one network.
  connectSaved(best >= 0 ? (size_t)best : 0);
}

void updateAuto(uint32_t now) {
  if (sJoin == Join::CONNECTING || sBestScan) return;
  if (!radioStation() || radioUp()) {              // off, a hotspot, or on a network: nothing to do
    sDownSince = 0;
    if (radioUp()) sAutoRan = false;
    return;
  }
  if (sDownSince == 0) {
    sDownSince = stamp(now);
    return;
  }
  if (now - sDownSince < (sAutoRan ? AUTOJOIN_RETRY_MS : AUTOJOIN_AFTER_MS)) return;
  if (sCount < 2) return;                          // one network: upstream is already retrying it
  if (sHoldUntil != 0 && (int32_t)(now - sHoldUntil) < 0) return;
  if (!quiet() || vk::config::u32("wifi_autojoin") == 0) return;
  sDownSince = stamp(now);
  sAutoRan = true;
  logf("off the network: looking for a saved one");
  startBestScan(now);
}

void serviceBegin() {
  loadList();
  importSlot();
  if (sCount > 0 && !slotEnterprise() && slotSsid().length() == 0) mirrorFront();
#ifndef VK_HOST_TEST
  WiFi.onEvent(onStaDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  badge_log::tagf("wifi", "saved networks: %u", (unsigned)sCount);
#endif
}

void serviceUpdate() {
  const uint32_t now = millis();
  if (now - sLastSync >= SYNC_MS) {
    sLastSync = now;
    importSlot();
  }
  updateJoin(now);
  updateBestScan(now);
  updateAuto(now);
}

VK_SERVICE(wifi_net, serviceBegin, serviceUpdate);

}  // namespace

// ---- saved networks -------------------------------------------------------------------------------------

size_t savedCount() {
  ensure();
  return sCount;
}

const char *savedSsid(size_t index) {
  ensure();
  return index < sCount ? sSaved[index].ssid : "";
}

bool isSaved(const char *ssid) {
  ensure();
  return indexOf(ssid) >= 0;
}

bool savedIsOpen(const char *ssid) {
  ensure();
  const int index = indexOf(ssid);
  return index >= 0 && sSaved[index].pass[0] == '\0';
}

bool remember(const char *ssid, const char *password) {
  if (!rememberLocal(ssid, password)) return false;
  mirrorFront();
  return true;
}

bool forget(const char *ssid) {
  ensure();
  const int index = indexOf(ssid);
  if (index < 0) return false;
  for (size_t i = (size_t)index; i + 1 < sCount; ++i) sSaved[i] = sSaved[i + 1];
  --sCount;
  wipe((char *)&sSaved[sCount], sizeof sSaved[sCount]);
  if (!storeList()) logf("could not store the saved networks");
  // Upstream's slot must not bring the network back at the next look or the next boot.
  if (!slotEnterprise() && slotSsid() == ssid) {
    if (sCount > 0) {
      mirrorFront();
    } else {
      slotForget();
    }
  }
  return true;
}

// ---- joining ----------------------------------------------------------------------------------------------

Fail classify(uint8_t reason, bool *decisive) {
  bool one = false;
  Fail fail = Fail::OTHER;
  switch (reason) {
    case REASON_NO_AP_FOUND:
      fail = Fail::NOT_FOUND;
      break;
    case REASON_NO_AP_IN_RSSI:
      fail = Fail::NOT_FOUND;
      one = true;
      break;
    case REASON_4WAY_HANDSHAKE_TIMEOUT:            // what a wrong WPA2 passphrase usually gives
    case REASON_HANDSHAKE_TIMEOUT:
    case REASON_MIC_FAILURE:
      fail = Fail::AUTH;
      break;
    case REASON_AUTH_FAIL:
    case REASON_NO_AP_COMPATIBLE_SECURITY:         // a password for an open network, or none for a secured one
    case REASON_NO_AP_IN_AUTHMODE:
      fail = Fail::AUTH;
      one = true;                                  // the driver does not retry these
      break;
    default:
      break;
  }
  if (decisive != nullptr) *decisive = one;
  return fail;
}

bool join(const char *ssid, const char *password) {
  ensure();
  if (!validSsid(ssid) || !validPassword(password)) return false;
  startConnect(ssid, password);
  return true;
}

bool joinSaved(const char *ssid) {
  ensure();
  const int index = indexOf(ssid);
  if (index < 0) return false;
  startConnect(sSaved[index].ssid, sSaved[index].pass);
  return true;
}

void joinBest() {
  ensure();
  if (sJoin == Join::CONNECTING || sBestScan || sCount == 0 || radioUp()) return;
  if (sCount == 1) {
    connectSaved(0);
  } else {
    startBestScan(millis());
  }
}

Join joinState() { return sJoin; }

const char *joinStateName() {
  switch (sJoin) {
    case Join::IDLE:           return "idle";
    case Join::CONNECTING:     return "connecting";
    case Join::JOINED:         return "joined";
    case Join::WRONG_PASSWORD: return "wrong_password";
    case Join::NOT_FOUND:      return "not_found";
    case Join::TIMEOUT:        return "timeout";
  }
  return "?";
}

const char *joinSsid() { return sJoin == Join::IDLE ? "" : sJoinSsid; }

uint32_t joinElapsedMs() { return sJoin == Join::CONNECTING ? (uint32_t)millis() - sJoinStarted : 0; }

uint32_t joinTimeoutMs() { return vk::config::u32("wifi_join_s") * 1000; }

void joinCancel() {
  if (sJoin == Join::CONNECTING) {
    radioOff();
    logf("join '%s': stopped", sJoinSsid);
  }
  joinDismiss();
}

void joinDismiss() {
  wipe(sJoinPass, sizeof sJoinPass);
  sJoinSsid[0] = '\0';
  sJoin = Join::IDLE;
}

void hold() {
  sHoldUntil = stamp((uint32_t)millis() + HOLD_MS);
}

#ifdef VK_HOST_TEST
void hostReboot() {
  if (sOpen) sPrefs.end();
  sOpen = false;
  sLoaded = false;
  sCount = 0;
  wipe((char *)sSaved, sizeof sSaved);
  joinDismiss();
  sBestScan = false;
  sDownSince = 0;
  sAutoRan = false;
  sHoldUntil = 0;
  sLastSync = 0;
  sAuthEvents = 0;
  sMissEvents = 0;
}
void hostBegin() { serviceBegin(); }
void hostUpdate() { serviceUpdate(); }
void hostDisconnectEvent(uint8_t reason) { noteDisconnect(reason); }
#endif

}  // namespace vk::wifi
