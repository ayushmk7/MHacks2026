// src/vk/core/config.h
// Config store (platform/config.md, Config store). NVS namespace "vkconf".
#pragma once

#include <Arduino.h>

#include "../wallet/pure/vk_checks.h"   // vk_token_t, VK_MAX_TOKENS
#include "registry.h"

namespace vk::config {

enum class Type : uint8_t { STR, U32, KEY32, TOKENS };
enum : uint8_t { F_NONE = 0, F_SECURE = 1, F_REQUIRED = 2 };

struct ConfigKey : Registered<ConfigKey> {
  const char *name;            // NVS key, <= 15 chars
  Type type;
  const char *def;             // default as text; nullptr = no default
  uint8_t flags;
  uint32_t min, max;           // U32: allowed range. STR: min and max length. Others: unused
  const char *help;            // one line, shown by VKKEYS
  ConfigKey(const char *n, Type t, const char *d, uint8_t f, uint32_t mn, uint32_t mx, const char *h)
      : name(n), type(t), def(d), flags(f), min(mn), max(mx), help(h) {}
};
#define VK_CONFIG_KEY(ident, name, type, def, flags, min, max, help) \
  static vk::config::ConfigKey vk_config_##ident{name, type, def, flags, min, max, help}

void begin();                                   // open NVS, load `provisioned`
bool provisioned();

String  text(const char *name);                 // stored text, or the default, or ""
uint32_t u32(const char *name);
bool    key32(const char *name, uint8_t out[32]);            // base58 text -> 32 bytes
size_t  tokens(vk_token_t out[VK_MAX_TOKENS]);               // parsed token table

enum class SetResult : uint8_t { OK, PENDING, UNKNOWN_KEY, INVALID, UNAVAILABLE, STORAGE };
// Validates by type and range. Unprovisioned: writes. Provisioned: non-secure keys write;
// secure keys open a firmware confirmation and return PENDING (the write happens on approval).
SetResult set(const char *name, const char *value);

bool commit(String &missing);                   // all REQUIRED keys set -> provisioned = 1
void requestReset();                            // firmware confirmation, then erase the namespace and run every reset listener

// Set at boot by the approval engine. Raises the "Change setting" confirmation and calls done(approved).
// Null until the engine exists: a secure change then returns SetResult::UNAVAILABLE.
extern bool (*confirmChange)(const char *key, const char *oldText, const char *newText, void (*done)(bool approved));

struct ResetListener : Registered<ResetListener> { void (*fn)(); explicit ResetListener(void (*f)()) : fn(f) {} };
#define VK_ON_RESET(ident, fn) static vk::config::ResetListener vk_on_reset_##ident(fn)

// ---- Added by WP10 (core/config.cpp). Nothing above this line was changed. ----

// The registered key with that name, or nullptr.
const ConfigKey *find(const char *name);

// Text parsers (platform/config.md, "Type text forms" and "Token table"). Pure functions: no NVS,
// no state; host-tested in test/host/test_config.cpp. Each returns true when `text` is a valid
// value; an out-parameter may be nullptr and is written only on success.
bool parseStr(const char *text, uint32_t minLen, uint32_t maxLen);            // printable ASCII, length in range
bool parseU32(const char *text, uint32_t lowest, uint32_t highest, uint32_t *out);   // decimal digits only, value in range
bool parseKey32(const char *text, uint8_t out[32]);                           // base58 of exactly 32 bytes
bool parseTokens(const char *text, vk_token_t out[VK_MAX_TOKENS], size_t *count);   // 1..VK_MAX_TOKENS entries
bool validate(const ConfigKey &key, const char *text);                        // by the key's type and range

#ifdef VK_HOST_TEST
// Host-test seams. hostReboot() forgets everything held in RAM, as a reboot does (the in-memory NVS
// of the shim is kept): the next call opens the namespace again. The three strings are what
// VKWIFI and VKAUTOSTART would have passed to upstream (wifi_mgr::connect, settings::setAutostartApp).
void hostReboot();
extern String hostWifiSsid, hostWifiPassword, hostAutostart;
extern unsigned hostRepaints;      // how many times the shell repaint was requested
#endif

}  // namespace vk::config
