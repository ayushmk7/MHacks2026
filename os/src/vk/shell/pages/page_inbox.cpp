// Settings row `inbox` (shell.md, "Wallet and Inbox"): an action row that launches the native
// Inbox app. Its value is the number of waiting notifications. It never becomes a screen.

#include "../page.h"

#include "../../../apps/app_store.h"
#include "../../../hal/leds.h"
#include "../../../lua_sdk/lua_runtime.h"
#include "../../host/notify.h"

namespace {
constexpr const char *APP_ID = "inbox";

void rowValue(char *out, size_t cap) {
  if (cap == 0) return;
  out[0] = '\0';
  if (!app_store::exists(APP_ID)) {
    snprintf(out, cap, "absent");   // the app is not compiled in
    return;
  }
  const size_t waiting = vk::host::notify::count();
  if (waiting > 0) snprintf(out, cap, "%u", (unsigned)waiting);
}

void rowAction() {
  if (!app_store::exists(APP_ID)) return;
  ::leds::stopAnimation();
  runtime::requestLaunch(APP_ID);
}
}  // namespace

VK_SETTINGS_ACTION(inbox, "inbox", 110, "Inbox", rowValue, rowAction);
