// First-run consent store (app-host.md, "Consent").
// Header only until consent.cpp is written (WP30).
#pragma once

#include <Arduino.h>

namespace vk::host::consent {
uint32_t hashPermissions(const String &permissions);   // FNV-1a of the sorted, comma-joined list: order in app.ini does not matter
bool has(const String &appId, uint32_t hash);
bool save(const String &appId, uint32_t hash);
void eraseAll();                                       // the VK_ON_RESET listener
size_t count();
bool at(size_t index, String &appIdOut, uint32_t &hashOut);   // count() and at() let the Wallet app list stored consent
}
