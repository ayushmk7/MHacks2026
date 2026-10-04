// Notifications, WP01 stub: posts are dropped and the inbox is always empty. The inbox arrives in WP32.
#include "notify.h"

namespace vk::host::notify {

void post(const char *title, const char *body, const char *app_id) {
  (void)title; (void)body; (void)app_id;
}

size_t count() { return 0; }

const Note *at(size_t index) {
  (void)index;
  return nullptr;
}

void remove(size_t index) { (void)index; }
void clear() {}

}  // namespace vk::host::notify
