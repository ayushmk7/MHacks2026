// The native app runtime (app-host.md, "Native runtime").
#pragma once

#include <Arduino.h>

#include "../../apps/app_store.h"   // app_store::Info

namespace vk::host::native {
size_t count();
bool infoAt(size_t index, app_store::Info &out);       // for hook H11
bool infoById(const String &id, app_store::Info &out);
bool exists(const String &id);
bool start(const String &id);     // constructs the app object and calls on_start()
void stop();                      // on_stop(), then destroys the object
bool active();
void update(float dt);            // on_update(dt), on_draw()
void button(uint8_t key, bool pressed);
void espnow(const uint8_t *mac, const uint8_t *data, size_t length, int8_t rssi);
const char *permissions();        // of the active app; "" when none
const char *launcherKeys(const String &id);   // the optional last BADGE_APP argument; "" when none
}
