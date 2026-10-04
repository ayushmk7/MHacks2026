// First-run consent store (app-host.md, "Consent").
#pragma once

#include <Arduino.h>

namespace vk::host::consent {
uint32_t hashPermissions(const String &permissions);   // FNV-1a of the sorted, comma-joined list: order in app.ini does not matter
bool has(const String &appId, uint32_t hash);
bool save(const String &appId, uint32_t hash);
void eraseAll();                                       // the VK_ON_RESET listener
size_t count();
bool at(size_t index, String &appIdOut, uint32_t &hashOut);   // count() and at() let the Wallet app list stored consent

// ---- Added with consent.cpp (WP30). Nothing above this line was changed. ----
// The file is read once and kept in RAM: has(), count() and at() cost no file access after the
// first call. at() lists the entries oldest first. save() refuses an empty id and one longer than
// 32 characters.

#ifdef VK_HOST_TEST
// Host-test seam. Forgets the copy in RAM, as a reboot does: the next call reads the file again.
void hostReboot();
#endif
}
