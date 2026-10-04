// The app.ini keys Badge OS adds (app-host.md, "Manifest").
// Header only until manifest.cpp is written (WP30).
#pragma once

#include <Arduino.h>

namespace vk::host::manifest {
struct Extra { String permissions; uint32_t min_api = 1; };
bool load(const String &appId, Extra &out);     // reads /apps/<id>/app.ini via app_store::readFile
}
