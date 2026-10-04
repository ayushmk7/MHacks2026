// Home service, WP01 stub: upstream's launcher stays in charge. The service arrives in WP37.
#include "home.h"

#include "../../lua_sdk/lua_runtime.h"

namespace vk::host {

void showShell() {}

// "The badge is idle": until the launcher is itself an app, that is simply "no app is running".
bool idle() { return !runtime::running(); }

}  // namespace vk::host
