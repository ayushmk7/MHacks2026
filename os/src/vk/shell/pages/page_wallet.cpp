// Settings row `wallet` (shell.md, "Wallet and Inbox"): an action row that launches the native
// Wallet app. It never becomes a screen; the app exits to the launcher, like every app.

#include "../page.h"

#include "../../../apps/app_store.h"
#include "../../../hal/leds.h"
#include "../../../lua_sdk/lua_runtime.h"
#include "../../core/config.h"

namespace {
constexpr const char *APP_ID = "wallet_settings";

void rowValue(char *out, size_t cap) {
  if (!app_store::exists(APP_ID)) {
    snprintf(out, cap, "absent");   // the app is not compiled in
    return;
  }
  snprintf(out, cap, "%s", vk::config::provisioned() ? "provisioned" : "setup needed");
}

void rowAction() {
  if (!app_store::exists(APP_ID)) return;
  ::leds::stopAnimation();
  runtime::requestLaunch(APP_ID);
}
}  // namespace

VK_SETTINGS_ACTION(wallet, "wallet", 100, "Wallet", rowValue, rowAction);
