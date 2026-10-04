// Permissions, WP01 stub: everything is granted and every launch is allowed, as in upstream.
// The manifest check, the grant slots and consent arrive in WP30.
#include "permissions.h"

namespace vk::host {

bool granted(const char *permission) {
  (void)permission;
  return true;
}

bool preLaunch(const String &appId, String &error) {
  (void)appId; (void)error;
  return true;
}

void promotePending() {}

}  // namespace vk::host
