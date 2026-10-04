// src/native_apps/selftest/suite_apps.cpp
// APPS: one row per installed app, Lua and native, in launcher order (app_store::at, hook H11), so
// a person can see on the badge itself which apps would not start. Per app:
//
//   native (a BADGE_APP line)   every permission is a registered name, and it has a constructor
//   Lua (/apps/<id>/)           app.ini parses (manifest::load: a bad min_api fails), every
//                               permission is a registered name, min_api <= the firmware's API
//                               version, and the entry file is there
//
// The value says what is wrong ("unknown permission: foo", "min_api 3 > 2", "no main.lua"), or the
// kind and version when nothing is. The ids come from the app store and the native registry, and
// the permission names from the permission registry: nothing is listed here. An app's row is read
// again by its id when it runs, so an install or delete meanwhile shows as "gone".
#include "../../vk/sdk/badge_sdk.hpp"

#include <stdio.h>
#include <string.h>

#include "../../apps/app_store.h"
#include "../../vk/core/fileio.h"
#include "../../vk/host/manifest.h"
#include "../../vk/host/permissions.h"
#include "../../vk/vk_build.h"          // VK_API_VERSION
#include "selftest.h"
#include "suites.h"

namespace selftest {
namespace {

// The first name in the comma-separated `list` that no feature registered; false when all are known.
bool unknownPermission(const char *list, char *out, size_t cap) {
  const char *at = list ? list : "";
  for (;;) {
    const char *end = strchr(at, ',');
    size_t length = end ? (size_t)(end - at) : strlen(at);
    const char *name = at;
    while (length > 0 && *name == ' ') { ++name; --length; }
    while (length > 0 && name[length - 1] == ' ') --length;
    if (length > 0) {
      bool known = false;
      for (const vk::host::Permission *p = vk::host::Permission::first(); p && !known; p = p->next()) {
        known = p->name && strlen(p->name) == length && memcmp(p->name, name, length) == 0;
      }
      if (!known) {
        snprintf(out, cap, "%.*s", (int)length, name);
        return true;
      }
    }
    if (end == nullptr) return false;
    at = end + 1;
  }
}

const badge::NativeApp *findNative(const char *id) {
  for (const badge::NativeApp *app = badge::NativeApp::first(); app; app = app->next()) {
    if (app->id && strcmp(app->id, id) == 0) return app;
  }
  return nullptr;
}

void checkApp(Ctx &c) {
  char unknown[24];
  const badge::NativeApp *native = findNative(c.name);
  if (native != nullptr) {
    if (unknownPermission(native->permissions, unknown, sizeof unknown)) c.finish(State::Fail, "unknown permission: %s", unknown);
    else if (native->create == nullptr) c.finish(State::Fail, "no constructor");
    else c.finish(State::Ok, "native %s", native->version ? native->version : "");
    return;
  }

  app_store::Info info;
  if (!app_store::byId(c.name, info)) {
    c.finish(State::Skip, "gone");
    return;
  }
  vk::host::manifest::Extra extra;
  if (!vk::host::manifest::load(c.name, extra)) {
    c.finish(State::Fail, "app.ini: bad min_api");
    return;
  }
  if (unknownPermission(extra.permissions.c_str(), unknown, sizeof unknown)) {
    c.finish(State::Fail, "unknown permission: %s", unknown);
    return;
  }
  if (extra.min_api > VK_API_VERSION) {
    c.finish(State::Fail, "min_api %lu > %u", (unsigned long)extra.min_api, (unsigned)VK_API_VERSION);
    return;
  }
  const String entry = info.entry.length() ? info.entry : String("main.lua");
  const String path = app_store::directory(c.name) + "/" + entry;
  const vk::fileio::Ops *io = vk::fileio::ops;
  if (io == nullptr || !io->exists(path.c_str())) {
    c.finish(State::Fail, "no %.24s", entry.c_str());
    return;
  }
  c.finish(State::Ok, "lua %s", info.version.length() ? info.version.c_str() : "-");
}

size_t appRows() { return app_store::count(); }

void appName(size_t index, char *name, size_t nameCap, char *label, size_t labelCap) {
  app_store::Info info;
  if (!app_store::at(index, info)) {
    snprintf(name, nameCap, "?");
    snprintf(label, labelCap, "?");
    return;
  }
  snprintf(name, nameCap, "%s", info.id.c_str());
  String shown = info.name.length() ? info.name : info.id;
  shown.toUpperCase();
  snprintf(label, labelCap, "%s", shown.c_str());
}

const Check TABLE[] = {
    {"", "", Kind::Auto, Profile::Any, checkApp, nullptr, 0},     // every row: one app
};

}  // namespace

const Suite APPS = {
    "apps", "APPS", Profile::Any, TABLE, 1, appRows, appName, nullptr, false,
};

}  // namespace selftest
