// LINK: src/vk/core/wifi_net.cpp src/vk/core/config.cpp
// test_wifi_net - saved Wi-Fi networks, the join state machine and auto-join
// (src/vk/core/wifi_net.h; docs/os/ui/shell.md, "Wi-Fi"). The radio and upstream's one saved
// network are the variables of vk::wifi::hostRadio; NVS is the shim's. Run: test/host/run.sh test_wifi_net
//
// What is checked:
//   1. classify(): which disconnect reasons mean "wrong password" and "not found", and which settle
//      a join with one event;
//   2. the saved list: newest first, one entry per name, four at most, forget, lengths no network
//      accepts, a reboot, a failed NVS write;
//   3. upstream's slot follows the front of the list, and a network that lands in the slot
//      (VKWIFI, the web page) is copied to the front of the list;
//   4. join(): JOINED saves, WRONG_PASSWORD / NOT_FOUND / TIMEOUT do not and turn the radio off;
//      the hard timeout is config key wifi_join_s; cancel and dismiss;
//   5. joinBest(): the strongest saved network in range, else the newest; no scan with one network;
//   6. auto-join: only with two or more networks, only after AUTOJOIN_AFTER_MS off the network,
//      only while the badge is idle, not while hold() is in force, not with wifi_autojoin 0.
#include <Arduino.h>
#include <Preferences.h>

#include "../../src/vk/core/config.h"
#include "../../src/vk/core/wifi_net.h"

using namespace vk::wifi;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint32_t sNow = 0;

// A fresh badge: empty NVS, nothing in RAM, the radio off.
static void fresh() {
  vk_host_nvs_reset();
  vk::config::hostReboot();
  hostReboot();
  hostRadio = HostRadio();
  sNow = 1000;
  vk_host_set_millis(sNow);
  hostBegin();
}

// One loop pass `ms` later.
static void pass(uint32_t ms) {
  sNow += ms;
  vk_host_set_millis(sNow);
  hostUpdate();
}

static bool listIs(const char *a, const char *b = "", const char *c = "", const char *d = "") {
  const char *want[MAX_SAVED] = {a, b, c, d};
  size_t count = 0;
  while (count < MAX_SAVED && want[count][0] != '\0') ++count;
  if (savedCount() != count) return false;
  for (size_t i = 0; i < MAX_SAVED; ++i) {
    if (strcmp(savedSsid(i), want[i]) != 0) return false;
  }
  return true;
}

static void testClassify() {
  bool one = true;
  CHECK(classify(201, &one) == Fail::NOT_FOUND && !one);        // NO_AP_FOUND: the driver tries again
  CHECK(classify(212, &one) == Fail::NOT_FOUND && one);
  CHECK(classify(15, &one) == Fail::AUTH && !one);              // 4WAY_HANDSHAKE_TIMEOUT
  CHECK(classify(204, &one) == Fail::AUTH && !one);
  CHECK(classify(14, &one) == Fail::AUTH && !one);
  CHECK(classify(202, &one) == Fail::AUTH && one);              // AUTH_FAIL
  CHECK(classify(210, &one) == Fail::AUTH && one);
  CHECK(classify(211, &one) == Fail::AUTH && one);
  CHECK(classify(8, &one) == Fail::OTHER && !one);              // ASSOC_LEAVE: our own disconnect
  CHECK(classify(2, &one) == Fail::OTHER && !one);              // AUTH_EXPIRE: says nothing
  CHECK(classify(0, nullptr) == Fail::OTHER);
  CHECK(classify(200, nullptr) == Fail::OTHER);
}

static void testList() {
  fresh();
  CHECK(savedCount() == 0);
  CHECK(strcmp(savedSsid(0), "") == 0);
  CHECK(!isSaved("home"));
  CHECK(!forget("home"));

  CHECK(remember("home", "password-1"));
  CHECK(remember("venue", ""));                                 // an open network
  CHECK(remember("phone", "password-3"));
  CHECK(listIs("phone", "venue", "home"));
  CHECK(isSaved("venue") && savedIsOpen("venue") && !savedIsOpen("home") && !savedIsOpen("nobody"));

  // The same name again: one entry, at the front.
  CHECK(remember("home", "password-1b"));
  CHECK(listIs("home", "phone", "venue"));
  CHECK(remember("home", "password-1b"));
  CHECK(listIs("home", "phone", "venue"));

  // Four at most: the oldest drops off.
  CHECK(remember("cafe", "password-4"));
  CHECK(listIs("cafe", "home", "phone", "venue"));
  CHECK(remember("hotel", "password-5"));
  CHECK(listIs("hotel", "cafe", "home", "phone"));
  CHECK(!isSaved("venue"));
  CHECK(strcmp(savedSsid(4), "") == 0 && strcmp(savedSsid(99), "") == 0);

  // Lengths no network accepts.
  CHECK(!remember("", "password-x"));
  CHECK(!remember(nullptr, "password-x"));
  CHECK(!remember("123456789012345678901234567890123", "password-x"));       // 33
  CHECK(remember("12345678901234567890123456789012", "password-x"));         // 32
  CHECK(!remember("short", "1234567"));                         // 7
  CHECK(remember("short", "12345678"));                         // 8
  CHECK(remember("long", "123456789012345678901234567890123456789012345678901234567890123"));    // 63
  CHECK(!remember("longer", "12345678901234567890123456789012345678901234567890123456789012345"));   // 65
  CHECK(listIs("long", "short", "12345678901234567890123456789012", "hotel"));

  // A reboot keeps the list, in order, with its passwords (joinSaved hands them to the radio).
  hostReboot();
  hostRadio = HostRadio();
  hostBegin();
  CHECK(listIs("long", "short", "12345678901234567890123456789012", "hotel"));
  CHECK(joinSaved("short"));
  CHECK(hostRadio.connectSsid == "short" && hostRadio.connectPassword == "12345678");
  joinCancel();

  // Forget: from the middle, the front, the end.
  CHECK(forget("short"));
  CHECK(listIs("long", "12345678901234567890123456789012", "hotel"));
  CHECK(forget("long"));
  CHECK(forget("hotel"));
  CHECK(listIs("12345678901234567890123456789012"));
  CHECK(forget("12345678901234567890123456789012"));
  CHECK(savedCount() == 0);
  hostReboot();
  hostBegin();
  CHECK(savedCount() == 0);

  // NVS full: the call says so and RAM holds what NVS holds.
  fresh();
  CHECK(remember("home", "password-1"));
  vk_host_nvs_fail_writes = -1;
  CHECK(!remember("venue", "password-2"));
  vk_host_nvs_fail_writes = 0;
  CHECK(listIs("home"));
  hostReboot();
  hostBegin();
  CHECK(listIs("home"));
}

static void testSlot() {
  // The front of the list is upstream's saved network.
  fresh();
  CHECK(remember("home", "password-1"));
  CHECK(hostRadio.slotSsid == "home" && hostRadio.slotPassword == "password-1");
  CHECK(remember("venue", ""));
  CHECK(hostRadio.slotSsid == "venue" && hostRadio.slotPassword == "");
  // Forgetting the front moves the slot to the next one; forgetting another leaves it.
  CHECK(remember("phone", "password-3"));
  CHECK(forget("venue"));
  CHECK(hostRadio.slotSsid == "phone");
  CHECK(forget("phone"));
  CHECK(hostRadio.slotSsid == "home" && hostRadio.slotPassword == "password-1");
  CHECK(forget("home"));
  CHECK(hostRadio.slotSsid == "" && hostRadio.slotPassword == "");

  // A network saved through upstream (VKWIFI, JOINWIFI, the web page) reaches the list.
  fresh();
  CHECK(remember("home", "password-1"));
  hostRadio.slotSsid = "My Hotspot";
  hostRadio.slotPassword = "hunter2hunter2";
  pass(100);
  CHECK(listIs("home"));                                        // not looked at yet
  pass(2000);
  CHECK(listIs("My Hotspot", "home"));
  pass(2000);
  pass(2000);
  CHECK(listIs("My Hotspot", "home"));                          // once
  // The same network with another password replaces its entry.
  hostRadio.slotPassword = "hunter3hunter3";
  pass(2000);
  CHECK(listIs("My Hotspot", "home"));
  CHECK(joinSaved("My Hotspot") && hostRadio.connectPassword == "hunter3hunter3");
  joinCancel();
  // At boot too, before the first pass.
  fresh();
  hostReboot();
  hostRadio.slotSsid = "boot-net";
  hostRadio.slotPassword = "";
  hostBegin();
  CHECK(listIs("boot-net"));

  // An enterprise profile in the slot is upstream's alone: not copied, not overwritten by forget.
  fresh();
  hostRadio.slotSsid = "eduroam";
  hostRadio.slotEnterprise = true;
  pass(2000);
  CHECK(savedCount() == 0);
  CHECK(remember("home", "password-1"));                        // joining a network from the badge takes the slot
  CHECK(hostRadio.slotSsid == "home" && !hostRadio.slotEnterprise);
  hostRadio.slotSsid = "eduroam";
  hostRadio.slotEnterprise = true;
  CHECK(forget("home"));
  CHECK(hostRadio.slotSsid == "eduroam" && hostRadio.slotEnterprise);

  // A list with an empty slot (upstream's settings were erased): the slot is filled again at boot.
  fresh();
  CHECK(remember("home", "password-1"));
  hostReboot();
  hostRadio = HostRadio();
  hostBegin();
  CHECK(hostRadio.slotSsid == "home" && hostRadio.slotPassword == "password-1");

  // A slot no network could have (a name of 40 bytes) is ignored, pass after pass.
  fresh();
  hostRadio.slotSsid = "1234567890123456789012345678901234567890";
  pass(2000);
  pass(2000);
  CHECK(savedCount() == 0);
}

static void testJoin() {
  // Joined: saved, at the front, in the slot.
  fresh();
  CHECK(joinState() == Join::IDLE && strcmp(joinSsid(), "") == 0 && strcmp(joinStateName(), "idle") == 0);
  CHECK(join("venue", "password-9"));
  CHECK(joinState() == Join::CONNECTING && strcmp(joinSsid(), "venue") == 0);
  CHECK(hostRadio.connects == 1 && hostRadio.connectSsid == "venue" && hostRadio.connectPassword == "password-9");
  CHECK(joinTimeoutMs() == 15000);
  pass(500);
  CHECK(joinState() == Join::CONNECTING && joinElapsedMs() == 500);
  CHECK(savedCount() == 0);                                     // nothing is saved before it works
  hostRadio.up = true;
  hostRadio.ssid = "other";                                     // up, but on another network: not ours
  pass(100);
  CHECK(joinState() == Join::CONNECTING);
  hostRadio.ssid = "venue";
  pass(100);
  CHECK(joinState() == Join::JOINED && strcmp(joinStateName(), "joined") == 0);
  CHECK(listIs("venue") && hostRadio.slotSsid == "venue" && hostRadio.slotPassword == "password-9");
  CHECK(hostRadio.disconnects == 0 && joinElapsedMs() == 0);
  joinDismiss();
  CHECK(joinState() == Join::IDLE && strcmp(joinSsid(), "") == 0);
  CHECK(hostRadio.up);                                          // dismiss leaves the radio alone

  // A new join while on a network starts from a clean radio.
  CHECK(join("phone", "password-3"));
  CHECK(hostRadio.disconnects == 1 && hostRadio.connects == 2 && !hostRadio.up);

  joinCancel();
  // ... and so does one while the radio is still trying another network: its disconnect events
  // must not count against the new join.
  hostRadio.station = true;
  hostRadio.up = false;
  CHECK(join("phone", "password-3"));
  CHECK(hostRadio.disconnects == 3 && hostRadio.connects == 3);

  // Wrong password: two handshake timeouts.
  hostDisconnectEvent(15);
  pass(100);
  CHECK(joinState() == Join::CONNECTING);
  hostDisconnectEvent(15);
  pass(100);
  CHECK(joinState() == Join::WRONG_PASSWORD && strcmp(joinStateName(), "wrong_password") == 0);
  CHECK(hostRadio.disconnects == 4 && !hostRadio.station);      // the radio stops retrying
  CHECK(!isSaved("phone") && listIs("venue"));
  CHECK(strcmp(joinSsid(), "phone") == 0);                      // the result still names its network
  joinDismiss();

  // ... or one event the driver does not retry.
  CHECK(join("phone", "password-3"));
  hostDisconnectEvent(202);
  pass(100);
  CHECK(joinState() == Join::WRONG_PASSWORD);
  joinDismiss();

  // Our own disconnect from the network before, and reasons that say nothing, do not count.
  CHECK(join("phone", "password-3"));
  hostDisconnectEvent(8);
  hostDisconnectEvent(2);
  hostDisconnectEvent(2);
  pass(100);
  CHECK(joinState() == Join::CONNECTING);
  joinCancel();
  CHECK(joinState() == Join::IDLE && !hostRadio.station);

  // Not found: the driver reports it twice.
  CHECK(join("zz-nobody", ""));
  hostDisconnectEvent(201);
  pass(2500);
  CHECK(joinState() == Join::CONNECTING);
  hostDisconnectEvent(201);
  pass(2500);
  CHECK(joinState() == Join::NOT_FOUND && strcmp(joinStateName(), "not_found") == 0);
  CHECK(!hostRadio.station && !isSaved("zz-nobody"));
  joinDismiss();

  // The hard timeout, with nothing heard; with one event heard, the event names the result.
  CHECK(join("slow", "password-7"));
  pass(14900);
  CHECK(joinState() == Join::CONNECTING);
  pass(100);
  CHECK(joinState() == Join::TIMEOUT && strcmp(joinStateName(), "timeout") == 0 && !hostRadio.station);
  joinDismiss();
  CHECK(join("slow", "password-7"));
  hostDisconnectEvent(15);
  pass(15000);
  CHECK(joinState() == Join::WRONG_PASSWORD);
  joinDismiss();
  CHECK(join("slow", "password-7"));
  hostDisconnectEvent(201);
  pass(15000);
  CHECK(joinState() == Join::NOT_FOUND);
  joinDismiss();
  // Events of the attempt before do not leak into the next.
  CHECK(join("slow", "password-7"));
  pass(100);
  CHECK(joinState() == Join::CONNECTING);
  joinCancel();

  // The timeout is config key wifi_join_s.
  CHECK(vk::config::set("wifi_join_s", "5") == vk::config::SetResult::OK);
  CHECK(joinTimeoutMs() == 5000);
  CHECK(join("slow", "password-7"));
  pass(4900);
  CHECK(joinState() == Join::CONNECTING);
  pass(100);
  CHECK(joinState() == Join::TIMEOUT);
  joinDismiss();
  CHECK(vk::config::set("wifi_join_s", "4") == vk::config::SetResult::INVALID);
  CHECK(vk::config::set("wifi_join_s", "61") == vk::config::SetResult::INVALID);

  // What cannot be a network is refused before the radio is touched.
  const unsigned before = hostRadio.connects;
  CHECK(!join("", "password-1"));
  CHECK(!join("venue", "short"));
  CHECK(!join(nullptr, ""));
  CHECK(!joinSaved("never-saved"));
  CHECK(hostRadio.connects == before && joinState() == Join::IDLE);

  // A saved network: its stored password; joined, it moves to the front.
  fresh();
  CHECK(remember("home", "password-1"));
  CHECK(remember("venue", ""));
  CHECK(joinSaved("home"));
  CHECK(hostRadio.connectSsid == "home" && hostRadio.connectPassword == "password-1");
  hostRadio.up = true;
  hostRadio.ssid = "home";
  pass(100);
  CHECK(joinState() == Join::JOINED && listIs("home", "venue") && hostRadio.slotSsid == "home");
  joinDismiss();

  // Joined, but NVS is full: joined all the same, and not in the list.
  fresh();
  CHECK(join("venue", ""));
  hostRadio.up = true;
  hostRadio.ssid = "venue";
  vk_host_nvs_fail_writes = -1;
  pass(100);
  vk_host_nvs_fail_writes = 0;
  CHECK(joinState() == Join::JOINED && !isSaved("venue"));
  joinDismiss();
}

static void scanResult(int index, const char *ssid, int rssi) {
  hostRadio.scanSsid[index] = ssid;
  hostRadio.scanRssi[index] = rssi;
}

static void testBest() {
  // Nothing saved: nothing happens.
  fresh();
  joinBest();
  CHECK(hostRadio.connects == 0 && hostRadio.scans == 0);

  // One saved network: it is joined without a scan.
  CHECK(remember("home", "password-1"));
  joinBest();
  CHECK(hostRadio.scans == 0 && hostRadio.connects == 1 && hostRadio.connectSsid == "home");
  CHECK(joinState() == Join::IDLE);                             // the quiet join reports nothing

  // Three saved: a scan, then the strongest of those in range.
  fresh();
  CHECK(remember("home", "password-1"));
  CHECK(remember("venue", ""));
  CHECK(remember("phone", "password-3"));
  joinBest();
  CHECK(hostRadio.scans == 1 && hostRadio.connects == 0);
  joinBest();
  CHECK(hostRadio.scans == 1);                                  // one scan at a time
  pass(1000);
  CHECK(hostRadio.connects == 0);
  scanResult(0, "neighbour", -40);
  scanResult(1, "home", -70);
  scanResult(2, "venue", -55);
  scanResult(3, "venue", -80);                                  // a second access point of the same network
  hostRadio.scanCount = 4;
  hostRadio.scanning = false;
  pass(100);
  CHECK(hostRadio.connects == 1 && hostRadio.connectSsid == "venue" && hostRadio.connectPassword == "");
  CHECK(listIs("phone", "venue", "home"));                      // the quiet join does not reorder the list

  // None in range: the newest, which the driver keeps retrying.
  fresh();
  CHECK(remember("home", "password-1"));
  CHECK(remember("phone", "password-3"));
  joinBest();
  scanResult(0, "neighbour", -40);
  hostRadio.scanCount = 1;
  hostRadio.scanning = false;
  pass(100);
  CHECK(hostRadio.connects == 1 && hostRadio.connectSsid == "phone" && hostRadio.connectPassword == "password-3");

  // A radio busy with a join refuses to scan: the join is stopped first.
  fresh();
  CHECK(remember("home", "password-1"));
  CHECK(remember("phone", "password-3"));
  hostRadio.station = true;                                     // connecting, not up
  joinBest();
  CHECK(hostRadio.disconnects == 1 && hostRadio.scans == 1);
  // A scan that never ends is given up for the newest network.
  pass(9900);
  CHECK(hostRadio.connects == 0);
  pass(100);
  CHECK(hostRadio.connects == 1 && hostRadio.connectSsid == "phone");

  // On a network already: nothing to do.
  fresh();
  CHECK(remember("home", "password-1"));
  hostRadio.station = true;
  hostRadio.up = true;
  joinBest();
  CHECK(hostRadio.connects == 0 && hostRadio.scans == 0);
}

static void testAuto() {
  // Two saved networks, station mode, no network: one look after 15 s, the next 60 s later.
  fresh();
  CHECK(remember("home", "password-1"));
  CHECK(remember("phone", "password-3"));
  hostRadio.station = true;
  pass(100);                                                    // the clock starts on the first pass that sees it down
  pass(14800);
  CHECK(hostRadio.scans == 0);
  pass(200);
  CHECK(hostRadio.scans == 1 && hostRadio.disconnects == 1);    // the stalled join is stopped, then the scan
  scanResult(0, "home", -60);
  hostRadio.scanCount = 1;
  hostRadio.scanning = false;
  pass(100);
  CHECK(hostRadio.connects == 1 && hostRadio.connectSsid == "home");
  pass(100);                                                    // 200 ms after the look began
  pass(30000);
  CHECK(hostRadio.scans == 1);
  pass(29700);                                                  // 59.9 s after it began
  CHECK(hostRadio.scans == 1);
  pass(200);
  CHECK(hostRadio.scans == 2);
  hostRadio.scanning = false;
  pass(100);
  // Up: the count starts again from 15 s at the next drop.
  hostRadio.up = true;
  hostRadio.ssid = "home";
  pass(120000);
  CHECK(hostRadio.scans == 2);
  hostRadio.up = false;
  pass(100);
  pass(14800);
  CHECK(hostRadio.scans == 2);
  pass(200);
  CHECK(hostRadio.scans == 3);

  // One saved network: never (upstream retries it by itself).
  fresh();
  CHECK(remember("home", "password-1"));
  hostRadio.station = true;
  for (int i = 0; i < 10; ++i) pass(20000);
  CHECK(hostRadio.scans == 0 && hostRadio.disconnects == 0 && hostRadio.connects == 0);

  // Wi-Fi off, or a hotspot: never.
  fresh();
  CHECK(remember("home", "password-1"));
  CHECK(remember("phone", "password-3"));
  for (int i = 0; i < 10; ++i) pass(20000);
  CHECK(hostRadio.scans == 0 && hostRadio.connects == 0);

  // An app is running or an approval is open: not now; as soon as the badge is idle again.
  hostRadio.station = true;
  hostRadio.idle = false;
  pass(100);
  pass(60000);
  CHECK(hostRadio.scans == 0);
  hostRadio.idle = true;
  pass(100);
  CHECK(hostRadio.scans == 1);

  // The user is on the Wi-Fi screens (hold() every pass): not now.
  fresh();
  CHECK(remember("home", "password-1"));
  CHECK(remember("phone", "password-3"));
  hostRadio.station = true;
  pass(100);
  for (int i = 0; i < 40; ++i) { hold(); pass(1000); }
  CHECK(hostRadio.scans == 0);
  pass(3900);                                                   // 4.9 s after the last hold()
  CHECK(hostRadio.scans == 0);
  pass(200);
  CHECK(hostRadio.scans == 1);

  // Switched off with the config key.
  fresh();
  CHECK(remember("home", "password-1"));
  CHECK(remember("phone", "password-3"));
  CHECK(vk::config::set("wifi_autojoin", "0") == vk::config::SetResult::OK);
  hostRadio.station = true;
  for (int i = 0; i < 10; ++i) pass(20000);
  CHECK(hostRadio.scans == 0);
  CHECK(vk::config::set("wifi_autojoin", "1") == vk::config::SetResult::OK);
  pass(100);
  CHECK(hostRadio.scans == 1);
  CHECK(vk::config::set("wifi_autojoin", "2") == vk::config::SetResult::INVALID);

  // A join the user started is never interrupted by a look.
  fresh();
  CHECK(remember("home", "password-1"));
  CHECK(remember("phone", "password-3"));
  CHECK(vk::config::set("wifi_join_s", "60") == vk::config::SetResult::OK);
  CHECK(join("venue", "password-9"));
  pass(100);
  pass(30000);
  CHECK(hostRadio.scans == 0 && joinState() == Join::CONNECTING);
}

int main() {
  testClassify();
  testList();
  testSlot();
  testJoin();
  testBest();
  testAuto();
  if (fails) {
    printf("%d wifi_net checks failed\n", fails);
    return 1;
  }
  printf("all wifi_net tests passed\n");
  return 0;
}
