// Permissions (app-host.md, "Permissions").
#pragma once

#include <Arduino.h>

#include "../core/registry.h"

namespace vk::host {
struct Permission : Registered<Permission> {
  const char *name;             // as written in app.ini
  const char *label;            // shown to the user: "request payments"
  bool consent;                 // true: the user must approve on first launch
  const char *upstream_tables;  // comma-separated upstream badge.<table> names this gates, or nullptr
  Permission(const char *n, const char *l, bool c, const char *t) : name(n), label(l), consent(c), upstream_tables(t) {}
};
#define VK_PERMISSION(ident, name, label, consent, upstream_tables) \
  static vk::host::Permission vk_permission_##ident(name, label, consent, upstream_tables)

bool granted(const char *permission);                  // for the active app; true when no app is active (firmware callers)
bool preLaunch(const String &appId, String &error);    // hook H8a
void promotePending();                                 // pending grant slot -> active; called by vk::lua::open and native::start
}
