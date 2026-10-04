// src/vk/core/clock.cpp
// The clock service (wallet/checks.md, Clock). The badge has no real-time clock, and the raw system
// time says nothing about whether it is right (upstream finding F3), so this file keeps its own
// time base, unix seconds at a millis() reference, together with where that time came from:
//   SNTP   the network answered (started once, when Wi-Fi first connects as a station);
//   FLOOR  raised to the issued_at of a record whose issuer signature verified (raiseTo);
//   NONE   nothing has set it yet.
// Nothing here waits: boot never depends on the network (implementation-plan.md, review focus 4).
// The clock never goes backwards. The one exception is devSet(), the dev command VKTIME.
//
// Host tests: with VK_HOST_TEST only source(), now(), ok(), raiseTo() and devSet() are compiled, on
// the shim's fake millis(). The service, the config key and the info field are firmware only.
#include "clock.h"

#include <Arduino.h>

#ifndef VK_HOST_TEST
#include <esp_sntp.h>
#include <sys/time.h>

#include "../../badge_log.h"
#include "../../net/wifi_mgr.h"
#include "config.h"
#include "serial.h"
#include "service.h"
#include "utc_offset.h"
#endif

namespace vk::clock {
namespace {

Source sSource = Source::NONE;
uint32_t sBaseUnix = 0;   // unix seconds at sBaseMs
uint32_t sBaseMs = 0;     // the millis() reference; differences are taken modulo 2^32

uint32_t elapsedMs(uint32_t sinceMs) { return (uint32_t)millis() - sinceMs; }

}  // namespace

Source source() { return sSource; }

uint32_t now() {
  if (sSource == Source::NONE) return 0;
  return sBaseUnix + elapsedMs(sBaseMs) / 1000;
}

bool ok() { return sSource != Source::NONE; }

void raiseTo(uint32_t unix_s) {
  // "With no SNTP": once the network has set the clock, a record never moves it. Otherwise check 9
  // (issued_at <= now + 60 under SNTP) could not catch a record issued in the future.
  if (sSource == Source::SNTP) return;
  if (unix_s <= now()) return;   // now() is 0 while the source is NONE
  const bool first = sSource == Source::NONE;
  sBaseUnix = unix_s;
  sBaseMs = (uint32_t)millis();
  sSource = Source::FLOOR;
#ifndef VK_HOST_TEST
  if (first) badge_log::tagf("vk", "clock: floor %lu (from a verified record)", (unsigned long)unix_s);
#else
  (void)first;
#endif
}

#if VK_TEST_HOOKS
void devSet(uint32_t unix_s) {
  // The dev command sets the time as given, backwards too: tests move the clock both ways.
  sBaseUnix = unix_s;
  sBaseMs = (uint32_t)millis();
  sSource = Source::SNTP;
}
#endif

#ifndef VK_HOST_TEST
namespace {

VK_CONFIG_KEY(ntp_server, "ntp_server", vk::config::Type::STR, "pool.ntp.org", vk::config::F_NONE, 3, 64,
              "SNTP host");
// For display only (utcOffsetMin() below). The rule makes VKSET and the settings pages refuse
// anything but a quarter-hour offset from -12:00 to +14:00.
VK_CONFIG_KEY(utc_offset, "utc_offset", vk::config::Type::STR, "", vk::config::F_NONE, 0, 6,
              "local time shown: +HH:MM from UTC; empty = UTC");
VK_CONFIG_RULE(utc_offset, "utc_offset", vk_utc_offset_valid);

constexpr uint32_t TICK_MS = 1000;          // the sync-status poll and the rebase run once a second
constexpr uint32_t REBASE_MS = 3600000UL;   // fold elapsed time into the base hourly, long before millis() wraps

// lwIP keeps the server name by pointer, not by copy, so it lives here. 64 is the key's maximum length.
char sServer[65] = "";
bool sStarted = false;        // configTime() has been called; it is called once
uint32_t sLastTickMs = 0;
bool sLoggedCallback = false;
bool sLoggedPoll = false;

// Written by the sync callback on the lwIP task, read by the service on the loop task.
portMUX_TYPE sSyncMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool sSyncPending = false;
uint32_t sSyncUnix = 0;
uint32_t sSyncRefMs = 0;

bool usable(const struct timeval &tv) {
  return tv.tv_sec > 0 && (uint64_t)tv.tv_sec <= 0xFFFFFFFFULL;
}

// The millis() value at which the time was exactly tv.tv_sec, given that it is `tv` at atMs.
uint32_t referenceMs(const struct timeval &tv, uint32_t atMs) {
  return atMs - (uint32_t)(tv.tv_usec / 1000);
}

// The network's time: unix_s was true at millis() == refMs. The source becomes SNTP; the time
// moves only forwards (a floor or a dev time that is already later stays).
void markSntp(uint32_t unix_s, uint32_t refMs) {
  const uint32_t candidate = unix_s + elapsedMs(refMs) / 1000;
  if (sSource == Source::NONE || candidate >= now()) {
    sBaseUnix = unix_s;
    sBaseMs = refMs;
  }
  sSource = Source::SNTP;
}

// Runs on the lwIP task each time SNTP sets the system time. It only records; the loop applies it.
void onSntpSync(struct timeval *tv) {
  if (tv == nullptr || !usable(*tv)) return;
  const uint32_t refMs = referenceMs(*tv, (uint32_t)millis());
  portENTER_CRITICAL(&sSyncMux);
  sSyncUnix = (uint32_t)tv->tv_sec;
  sSyncRefMs = refMs;
  sSyncPending = true;
  portEXIT_CRITICAL(&sSyncMux);
}

void startSntp() {
  sStarted = true;
  strlcpy(sServer, vk::config::text("ntp_server").c_str(), sizeof sServer);
  if (sServer[0] == '\0') {
    badge_log::tagf("vk", "clock: ntp_server is empty; sntp not started");
    return;
  }
  sntp_set_time_sync_notification_cb(onSntpSync);
  configTime(0, 0, sServer);   // returns at once: the request and its retries run on the lwIP task
  badge_log::tagf("vk", "clock: sntp started (%s)", sServer);
}

void serviceUpdate() {
  // 1. A sync reported by the callback.
  if (sSyncPending) {
    portENTER_CRITICAL(&sSyncMux);
    const uint32_t unix_s = sSyncUnix;
    const uint32_t refMs = sSyncRefMs;
    sSyncPending = false;
    portEXIT_CRITICAL(&sSyncMux);
    markSntp(unix_s, refMs);
    if (!sLoggedCallback) {
      sLoggedCallback = true;
      badge_log::tagf("vk", "clock: sntp %lu (sync callback)", (unsigned long)now());
    }
  }

  // 2. Start SNTP the first time Wi-Fi is up as a station. Never in hotspot mode, never twice.
  if (!sStarted && wifi_mgr::mode() == wifi_mgr::Mode::Station && wifi_mgr::connected()) startSntp();

  const uint32_t ms = (uint32_t)millis();
  if ((uint32_t)(ms - sLastTickMs) < TICK_MS) return;
  sLastTickMs = ms;

  // 3. The fallback for the callback: the sync status reads COMPLETED once after each sync, and only
  //    SNTP sets it, so the system time read at that moment is the network's. After a callback this
  //    finds the same time again and changes nothing.
  if (sStarted && sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
    struct timeval tv = {};
    if (gettimeofday(&tv, nullptr) == 0 && usable(tv)) {
      markSntp((uint32_t)tv.tv_sec, referenceMs(tv, (uint32_t)millis()));
      if (!sLoggedPoll) {
        sLoggedPoll = true;
        badge_log::tagf("vk", "clock: sntp %lu (status poll)", (unsigned long)now());
      }
    }
  }

  // 4. Keep the millis() reference recent, so now() survives the 49-day wrap of millis().
  if (sSource != Source::NONE) {
    const uint32_t elapsed = elapsedMs(sBaseMs);
    if (elapsed >= REBASE_MS) {
      const uint32_t whole = elapsed / 1000;
      sBaseUnix += whole;
      sBaseMs += whole * 1000;
    }
  }
}

String infoTime() {
  switch (sSource) {
    case Source::NONE:  return String("none");
    case Source::FLOOR: return String("floor");
    case Source::SNTP:  return String("sntp");
  }
  return String("none");
}

}  // namespace

int32_t utcOffsetMin() {
  int32_t minutes = 0;
  if (!vk_utc_offset_parse(vk::config::text("utc_offset").c_str(), &minutes)) return 0;
  return minutes;
}

namespace {

VK_SERVICE(clock, nullptr, serviceUpdate);
VK_INFO_FIELD(time, "time", infoTime);

}  // namespace
#endif  // VK_HOST_TEST

}  // namespace vk::clock
