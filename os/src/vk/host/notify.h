// Notification inbox (app-host.md, "Notifications").
#pragma once

#include <Arduino.h>

namespace vk::host::notify {
struct Note { char title[24]; char body[40]; char app_id[33]; uint32_t at_ms; };
void post(const char *title, const char *body, const char *app_id);   // identical title+body within 10 s is ignored
size_t count();
const Note *at(size_t index);     // newest first
void remove(size_t index);
void clear();
}
