// src/vk/host/permissions.cpp
// Permissions: the six names the app host owns, the grant slots, the pre-launch check and first-run
// consent (app-host.md, "Permissions" and "Consent"; hook H8a).
//
// Two slots. preLaunch() works out what the app being launched is granted and puts it in the
// PENDING slot. Upstream's launch() then stops the old app, which clears the ACTIVE slot (the
// VK_ON_APP_STOP listener below), and only then builds the new one: vk::lua::open (hook H7) or
// native::start calls promotePending(), which makes the pending set the active one. granted() reads
// the active slot and never asks upstream which app is current (upstream sets that after the
// Lua bindings are opened).
//
// One exception: while a native app object exists (native::active()), granted() answers from that
// app's BADGE_APP line instead of the slot. Hook H8b calls the stop listeners, which clear the
// slot, before the app's on_stop() and destructor run; read from the slot, those two would be "no
// app", which is granted everything.
#include "permissions.h"

#include <LittleFS.h>
#include <string.h>

#include "../../apps/app_store.h"
#include "../../badge_log.h"
#include "../../lua_sdk/lua_runtime.h"
#include "../sdk/badge_sdk.hpp"      // badge::NativeApp: a native app's manifest is its BADGE_APP line
#include "../vk_build.h"             // VK_API_VERSION
#include "../wallet/approval.h"
#include "consent.h"
#include "lifecycle.h"
#include "manifest.h"
#include "native.h"

// The table in app-host.md. `request`, `history` and `contacts` are registered by their features.
VK_PERMISSION(sign, "sign", "request payments", true, nullptr);
VK_PERMISSION(net, "net", "use the network", false, "wifi,http");
VK_PERMISSION(espnow, "espnow", "talk to nearby badges", false, "espnow");
VK_PERMISSION(ble, "ble", "use Bluetooth", false, "ble");
VK_PERMISSION(mic, "mic", "use the microphone", false, "mic");
VK_PERMISSION(storage, "storage", "store files", false, "storage");

namespace vk::host {

namespace {

constexpr size_t APP_ID_MAX = 32;    // upstream's limit; consent and the approval request store 33 bytes

struct Grant {
  bool valid = false;        // false: no app (active slot), or nothing prepared (pending slot)
  bool native = false;       // a native app always has `espnow`
  String permissions;        // comma-separated registered names, each once
};

Grant sPending;
Grant sActive;

// The consent confirmation that is on the screen, for onConsent().
String sAskedApp;
uint32_t sAskedHash = 0;

// An approval that could not be written to the consent file still lets that one launch through.
String sOnceApp;
uint32_t sOnceHash = 0;

// True when the comma-separated `list` has the item `name` (`length` characters). Spaces around an
// item are not part of it (a BADGE_APP list is read as written).
bool listHas(const char *list, const char *name, size_t length) {
  if (list == nullptr || length == 0) return false;
  const char *at = list;
  for (;;) {
    const char *end = strchr(at, ',');
    const char *item = at;
    size_t itemLength = end ? (size_t)(end - at) : strlen(at);
    while (itemLength > 0 && *item == ' ') { ++item; --itemLength; }
    while (itemLength > 0 && item[itemLength - 1] == ' ') --itemLength;
    if (itemLength == length && memcmp(item, name, length) == 0) return true;
    if (end == nullptr) return false;
    at = end + 1;
  }
}

const Permission *findPermission(const char *name, size_t length) {
  for (const Permission *p = Permission::first(); p; p = p->next()) {
    if (p->name && strlen(p->name) == length && memcmp(p->name, name, length) == 0) return p;
  }
  return nullptr;
}

const badge::NativeApp *findNative(const String &appId) {
  for (const badge::NativeApp *app = badge::NativeApp::first(); app; app = app->next()) {
    if (app->id && appId == app->id) return app;
  }
  return nullptr;
}

// Checks every name in `list` against the registry and collects the granted set: each registered
// name once, in the order of the list. False with `unknown` set to the first name nobody registered.
bool collect(const String &list, String &grantedOut, String &unknown) {
  grantedOut = "";
  const char *at = list.c_str();
  for (;;) {
    const char *end = strchr(at, ',');
    size_t length = end ? (size_t)(end - at) : strlen(at);
    const char *name = at;
    while (length > 0 && *name == ' ') { ++name; --length; }             // a BADGE_APP list may have spaces
    while (length > 0 && name[length - 1] == ' ') --length;
    if (length > 0) {
      if (findPermission(name, length) == nullptr) {
        unknown = String(name).substring(0, length);
        return false;
      }
      if (!listHas(grantedOut.c_str(), name, length)) {
        if (grantedOut.length()) grantedOut += ',';
        grantedOut.concat(name, length);
      }
    }
    if (end == nullptr) return true;
    at = end + 1;
  }
}

void onConsent(bool approved, void *) {
  const String appId = sAskedApp;
  sAskedApp = "";
  if (!approved || appId.length() == 0) return;          // rejected or timed out: nothing happens
  if (!consent::save(appId, sAskedHash)) {
    badge_log::tagf("vk", "consent for '%s' could not be stored; allowed for this launch only", appId.c_str());
    sOnceApp = appId;
    sOnceHash = sAskedHash;
  }
  runtime::requestLaunch(appId);
}

// The "Allow app" confirmation: one line per consent permission the app asks for.
bool askConsent(const String &appId, const String &grantedList, uint32_t hash) {
  using namespace vk::wallet;
  app_store::Info info;
  String name = appId;
  if (app_store::byId(appId, info) && info.name.length()) name = info.name;

  ApprovalRequest request{};
  strlcpy(request.title, "Allow app", sizeof request.title);
  strlcpy(request.headline, "NEW PERMISSIONS", sizeof request.headline);
  strlcpy(request.big, name.c_str(), sizeof request.big);
  const size_t maxLines = sizeof request.lines / sizeof request.lines[0];
  const char *at = grantedList.c_str();
  for (;;) {
    const char *end = strchr(at, ',');
    const size_t length = end ? (size_t)(end - at) : strlen(at);
    const Permission *p = findPermission(at, length);
    if (p && p->consent && request.line_count < maxLines) {
      ApprovalLine &line = request.lines[request.line_count++];
      strlcpy(line.label, "May", sizeof line.label);
      strlcpy(line.value, p->label ? p->label : p->name, sizeof line.value);
    }
    if (end == nullptr) break;
    at = end + 1;
  }
  request.severity = Severity::AMBER;
  request.select = SelectRule::HOLD;

  if (!approval::confirm(request, onConsent, nullptr)) return false;
  sAskedApp = appId;
  sAskedHash = hash;
  return true;
}

bool refuse(const String &appId, String &error, const String &why) {
  error = why;
  // Upstream shows `error` on its error screen but logs it only when another app is running.
  badge_log::tagf("vk", "launch of '%s' refused: %s", appId.c_str(), why.c_str());
  return false;
}

void onAppStop(const char *) { sActive = Grant(); }

VK_ON_APP_STOP(permissions, onAppStop);

}  // namespace

bool granted(const char *permission) {
  if (permission == nullptr || permission[0] == '\0') return true;   // nothing to hold
  if (native::active()) {
    // A native app object exists: from before its on_start() until after its destructor. Its
    // BADGE_APP line decides, whatever the slot says (it is already cleared during on_stop()).
    if (strcmp(permission, "espnow") == 0) return true;
    return listHas(native::permissions(), permission, strlen(permission));
  }
  if (!sActive.valid) return true;                                   // no app: a firmware caller
  if (sActive.native && strcmp(permission, "espnow") == 0) return true;
  return listHas(sActive.permissions.c_str(), permission, strlen(permission));
}

bool preLaunch(const String &appId, String &error) {
  error = "";
  if (appId.length() > APP_ID_MAX) return refuse(appId, error, "app id is longer than 32 characters");

  // The manifest: the BADGE_APP line of a native app, app.ini of a Lua app.
  const badge::NativeApp *nativeApp = findNative(appId);
  manifest::Extra extra;
  if (nativeApp) {
    // The built-in app always wins: H8a starts the native app for this id whatever is on the
    // filesystem, so a pushed folder with a native id can never run and can never block the
    // built-in one (anyone who can push could otherwise disable `inbox` or `wallet_settings`).
    // The folder is looked for on the filesystem only to say so in the log: app_store::exists()
    // is true for every native id.
    if (app_store::mounted() && LittleFS.exists(app_store::directory(appId))) {
      badge_log::tagf("vk", "ignoring pushed app '%s': the id belongs to a built-in app", appId.c_str());
    }
    extra.permissions = nativeApp->permissions ? nativeApp->permissions : "";
  } else {
    if (!manifest::load(appId, extra)) return refuse(appId, error, "bad min_api in app.ini");
    if (extra.min_api > (uint32_t)VK_API_VERSION) {
      return refuse(appId, error, "needs a newer BadgeOS (API " + String((unsigned long)extra.min_api) + ")");
    }
  }

  String grantedList;
  String unknown;
  if (!collect(extra.permissions, grantedList, unknown)) {
    return refuse(appId, error, "unknown permission: " + unknown);   // a typo must not silently grant nothing
  }

  // First-run consent, for Lua apps only: native apps were reviewed and compiled in.
  if (!nativeApp) {
    bool needsConsent = false;
    for (const Permission *p = Permission::first(); p; p = p->next()) {
      if (p->consent && p->name && listHas(grantedList.c_str(), p->name, strlen(p->name))) needsConsent = true;
    }
    if (needsConsent) {
      const uint32_t hash = consent::hashPermissions(grantedList);
      const bool once = sOnceApp.length() && sOnceApp == appId && sOnceHash == hash;
      sOnceApp = "";
      if (!once && !consent::has(appId, hash)) {
        if (askConsent(appId, grantedList, hash)) {
          badge_log::tagf("vk", "consent asked for '%s' (%s)", appId.c_str(), grantedList.c_str());
          return false;                // empty error: no error screen; onConsent() launches it again
        }
        return refuse(appId, error, "this app needs approval, and the approval screen is busy");
      }
    }
  }

  sPending.valid = true;
  sPending.native = nativeApp != nullptr;
  sPending.permissions = grantedList;
  return true;
}

void promotePending() {
  // An app is starting. If nothing was prepared for it (no preLaunch came first), it gets nothing.
  sActive.valid = true;
  sActive.native = sPending.valid && sPending.native;
  sActive.permissions = sPending.valid ? sPending.permissions : String();
  sPending = Grant();
}

}  // namespace vk::host
