// The native app runtime (app-host.md, "Native runtime"; native-apps.md).
//
// A native app is one BADGE_APP(...) line: a badge::NativeApp that registers itself. This file is
// what upstream's runtime and app list call through hooks H8a to H8f and H11: it lists the
// registered apps, creates the one that is launched, forwards the callbacks to it and deletes it
// when it stops. At most one app object exists at a time, and every launch makes a new one.
#include "native.h"

#include <string.h>

#include "../../badge_log.h"
#include "../../lua_sdk/lua_runtime.h"
#include "../sdk/badge_sdk.hpp"
#include "lifecycle.h"
#include "permissions.h"

namespace vk::host::native {

namespace {

const badge::NativeApp *sEntry = nullptr;   // the registry row of the running app
badge::App *sApp = nullptr;                 // its object; non-null from before on_start() until after its destructor
bool sStopping = false;

const badge::NativeApp *firstEntry() { return Registered<badge::NativeApp>::first(); }
const badge::NativeApp *nextEntry(const badge::NativeApp *entry) { return entry->Registered<badge::NativeApp>::next(); }

const char *idOf(const badge::NativeApp *entry) { return entry->id != nullptr ? entry->id : ""; }

// The registry's own order is the link order, which is not defined. The app list is shown to the
// user, so it is ordered by id: the same build always lists the same order.
bool before(const badge::NativeApp *a, const badge::NativeApp *b) {
  const int order = strcmp(idOf(a), idOf(b));
  return order < 0 || (order == 0 && a < b);
}

// The app at `index` of the list ordered by id, or nullptr.
const badge::NativeApp *entryAt(size_t index) {
  for (const badge::NativeApp *entry = firstEntry(); entry; entry = nextEntry(entry)) {
    size_t rank = 0;
    for (const badge::NativeApp *other = firstEntry(); other; other = nextEntry(other)) {
      if (other != entry && before(other, entry)) ++rank;
    }
    if (rank == index) return entry;
  }
  return nullptr;
}

// Two apps registered under one id: the first of the ordered list is the one that runs.
const badge::NativeApp *find(const String &id) {
  if (id.length() == 0) return nullptr;
  const badge::NativeApp *found = nullptr;
  for (const badge::NativeApp *entry = firstEntry(); entry; entry = nextEntry(entry)) {
    if (id != idOf(entry)) continue;
    if (found == nullptr || before(entry, found)) found = entry;
  }
  return found;
}

// Every field of Info is set: the struct has no initialisers (hook H11).
void fill(const badge::NativeApp *entry, app_store::Info &out) {
  out.id = idOf(entry);
  out.name = (entry->name != nullptr && entry->name[0] != '\0') ? entry->name : idOf(entry);
  out.version = entry->version != nullptr ? entry->version : "";
  out.author = "";
  out.description = "";
  out.entry = "";
  out.sizeBytes = 0;
}

}  // namespace

size_t count() {
  size_t n = 0;
  for (const badge::NativeApp *entry = firstEntry(); entry; entry = nextEntry(entry)) ++n;
  return n;
}

bool infoAt(size_t index, app_store::Info &out) {
  const badge::NativeApp *entry = entryAt(index);
  if (entry == nullptr) return false;
  fill(entry, out);
  return true;
}

bool infoById(const String &id, app_store::Info &out) {
  const badge::NativeApp *entry = find(id);
  if (entry == nullptr) return false;
  fill(entry, out);
  return true;
}

bool exists(const String &id) { return find(id) != nullptr; }

// Hook H8a. Upstream's launch() has already stopped the app that was running and set the current
// app id, so the id is right for anything on_start() asks the wallet core.
bool start(const String &id) {
  const badge::NativeApp *entry = find(id);
  if (entry == nullptr || entry->create == nullptr) return false;
  if (sApp != nullptr) stop();   // never two objects; upstream stops the old app first, so this does not happen

  // The grant slot of the app being launched becomes the active one before any of its code runs.
  vk::host::promotePending();

  badge::App *app = entry->create();
  if (app == nullptr) {
    badge_log::tagf("vk", "native app '%s' could not be created", idOf(entry));
    // The launch is over and no stop() will follow: tell the listeners, so that nothing (the
    // grant slot just promoted) stays held for an app that never ran.
    vk::host::onAppStopping(id);
    return false;
  }

  sEntry = entry;
  sApp = app;
  badge_log::tagf("vk", "native app '%s' started", idOf(entry));
  app->on_start();
  return true;
}

// Hook H8b. The app is still the active one, with its permissions, while on_stop() and its
// destructor run.
void stop() {
  if (sApp == nullptr || sStopping) return;
  sStopping = true;
  sApp->on_stop();
  delete sApp;
  badge_log::tagf("vk", "native app '%s' stopped", idOf(sEntry));
  sApp = nullptr;
  sEntry = nullptr;
  sStopping = false;
}

bool active() { return sApp != nullptr; }

// Hook H8d: one frame.
void update(float dt) {
  badge::App *const app = sApp;
  if (app == nullptr || sStopping) return;
  app->on_update(dt);
  if (sApp != app) return;
  app->on_draw();
}

// Hook H8e.
void button(uint8_t key, bool pressed) {
  if (sApp == nullptr || sStopping) return;
  sApp->on_button(key, pressed);
}

// Hook H8f. The router has already checked the `espnow` grant.
void espnow(const uint8_t *mac, const uint8_t *data, size_t length, int8_t rssi) {
  if (sApp == nullptr || sStopping || mac == nullptr || data == nullptr) return;
  sApp->on_espnow(mac, data, length, rssi);
}

const char *permissions() {
  if (sEntry == nullptr || sEntry->permissions == nullptr) return "";
  return sEntry->permissions;
}

}  // namespace vk::host::native

// Declared in sdk/badge_sdk.hpp: asks the host to stop the running app at the end of the frame.
void badge::exit() { runtime::requestStop(); }
