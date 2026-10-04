// App lifecycle events (app-host.md, "Lifecycle events").
#pragma once

#include <Arduino.h>

#include "../core/registry.h"

namespace vk::host {
struct AppStopListener : Registered<AppStopListener> {
  void (*fn)(const char *appId);                 // appId may be "" (upstream calls stop() with no app running)
  explicit AppStopListener(void (*f)(const char *)) : fn(f) {}
};
#define VK_ON_APP_STOP(ident, fn) static vk::host::AppStopListener vk_on_app_stop_##ident(fn)

void onAppStopping(const String &appId);         // hook H8b: calls every listener
bool luaPaused();                                // hook H19: true while the approval is active
}
