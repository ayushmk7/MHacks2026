// src/vk/core/config.cpp
// WP01 stub of the config store (platform/config.md, Config store). The registry is real; nothing
// is stored yet: the accessors return each key's default, the badge is never provisioned and
// set() answers UNAVAILABLE. WP10 fills this file (NVS namespace "vkconf", the config keys,
// commands, info fields and the status item that core/config.cpp registers).
#include "config.h"

#include <stdlib.h>
#include <string.h>

namespace vk::config {
namespace {

const ConfigKey *find(const char *name) {
  if (name == nullptr) return nullptr;
  for (auto *key = ConfigKey::first(); key; key = key->next()) {
    if (strcmp(key->name, name) == 0) return key;
  }
  return nullptr;
}

}  // namespace

bool (*confirmChange)(const char *key, const char *oldText, const char *newText, void (*done)(bool approved)) = nullptr;

void begin() {}

bool provisioned() { return false; }

String text(const char *name) {
  const ConfigKey *key = find(name);
  return String((key && key->def) ? key->def : "");
}

uint32_t u32(const char *name) {
  const ConfigKey *key = find(name);
  if (key == nullptr || key->def == nullptr) return 0;
  return (uint32_t)strtoul(key->def, nullptr, 10);
}

// No KEY32 or TOKENS key has a default (platform/config.md, Keys), so with nothing stored there is
// nothing to return.
bool key32(const char *name, uint8_t out[32]) {
  (void)name;
  (void)out;
  return false;
}

size_t tokens(vk_token_t out[VK_MAX_TOKENS]) {
  (void)out;
  return 0;
}

SetResult set(const char *name, const char *value) {
  (void)name;
  (void)value;
  return SetResult::UNAVAILABLE;
}

bool commit(String &missing) {
  missing = "";
  return false;
}

void requestReset() {}

}  // namespace vk::config
