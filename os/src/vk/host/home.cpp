// vk::host::idle(): what the balance poll, the `notify` LED pattern and the idle LED animation ask.
#include "home.h"

#include "../../lua_sdk/lua_runtime.h"

namespace vk::host {

// No app is running, so the shell is showing.
bool idle() { return !runtime::running(); }

}  // namespace vk::host
