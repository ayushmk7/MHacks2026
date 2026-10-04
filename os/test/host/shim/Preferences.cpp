// test/host/shim/Preferences.cpp - in-memory NVS. See Preferences.h.
#include "Preferences.h"

#include <map>
#include <vector>

int vk_host_nvs_fail_writes = 0;

namespace {

struct Entry {
  PreferenceType type;
  std::vector<uint8_t> data;
};
using Namespace = std::map<std::string, Entry>;

// Function-local so that it exists before any static object that uses Preferences.
std::map<std::string, Namespace> &store() {
  static std::map<std::string, Namespace> s;
  return s;
}

bool nameOk(const char *name) {
  if (name == nullptr) return false;
  const size_t n = strlen(name);
  return n >= 1 && n <= 15;                 // NVS_KEY_NAME_MAX_SIZE - 1
}

// True if this write must fail; counts a positive vk_host_nvs_fail_writes down.
bool injectedFailure() {
  if (vk_host_nvs_fail_writes > 0) { vk_host_nvs_fail_writes--; return true; }
  return vk_host_nvs_fail_writes < 0;
}

}  // namespace

void vk_host_nvs_reset() {
  store().clear();
  vk_host_nvs_fail_writes = 0;
}

Preferences::Preferences() : started_(false), readOnly_(false) {}
Preferences::~Preferences() { end(); }

bool Preferences::begin(const char *name, bool readOnly, const char *partition_label) {
  (void)partition_label;
  if (started_ || !nameOk(name)) return false;
  if (readOnly && store().find(name) == store().end()) return false;    // nvs_open(READONLY): ESP_ERR_NVS_NOT_FOUND
  if (!readOnly) store()[name];                                         // nvs_open(READWRITE) creates the namespace
  namespace_ = name;
  readOnly_ = readOnly;
  started_ = true;
  return true;
}

void Preferences::end() { started_ = false; }

bool Preferences::clear() {
  if (!started_ || readOnly_) return false;
  store()[namespace_].clear();
  return true;
}

bool Preferences::remove(const char *key) {
  if (!started_ || readOnly_ || !nameOk(key)) return false;
  return store()[namespace_].erase(key) == 1;
}

size_t Preferences::put(const char *key, PreferenceType type, const void *data, size_t len) {
  if (!started_ || readOnly_ || !nameOk(key) || (data == nullptr && len != 0)) return 0;
  if (injectedFailure()) return 0;
  Entry &entry = store()[namespace_][key];
  entry.type = type;
  entry.data.assign((const uint8_t *)data, (const uint8_t *)data + len);
  return len;
}

bool Preferences::get(const char *key, PreferenceType type, void *out, size_t len) {
  if (!started_ || !nameOk(key)) return false;
  auto ns = store().find(namespace_);
  if (ns == store().end()) return false;
  auto it = ns->second.find(key);
  if (it == ns->second.end() || it->second.type != type || it->second.data.size() != len) return false;
  memcpy(out, it->second.data.data(), len);
  return true;
}

size_t Preferences::putChar(const char *key, int8_t value) { return put(key, PT_I8, &value, sizeof value); }
size_t Preferences::putUChar(const char *key, uint8_t value) { return put(key, PT_U8, &value, sizeof value); }
size_t Preferences::putShort(const char *key, int16_t value) { return put(key, PT_I16, &value, sizeof value); }
size_t Preferences::putUShort(const char *key, uint16_t value) { return put(key, PT_U16, &value, sizeof value); }
size_t Preferences::putInt(const char *key, int32_t value) { return put(key, PT_I32, &value, sizeof value); }
size_t Preferences::putUInt(const char *key, uint32_t value) { return put(key, PT_U32, &value, sizeof value); }
size_t Preferences::putLong(const char *key, int32_t value) { return putInt(key, value); }
size_t Preferences::putULong(const char *key, uint32_t value) { return putUInt(key, value); }
size_t Preferences::putLong64(const char *key, int64_t value) { return put(key, PT_I64, &value, sizeof value); }
size_t Preferences::putULong64(const char *key, uint64_t value) { return put(key, PT_U64, &value, sizeof value); }
size_t Preferences::putBool(const char *key, bool value) { return putUChar(key, value ? 1 : 0); }

size_t Preferences::putString(const char *key, const char *value) {
  if (value == nullptr) return 0;
  // Stored without the NUL; the return value is the string's length, as in the core.
  const size_t len = strlen(value);
  if (len == 0) {                       // an empty string is a valid value: store it, report "0 bytes" like the core
    put(key, PT_STR, value, 0);
    return 0;
  }
  return put(key, PT_STR, value, len);
}
size_t Preferences::putString(const char *key, String value) { return putString(key, value.c_str()); }

size_t Preferences::putBytes(const char *key, const void *value, size_t len) {
  if (value == nullptr || len == 0) return 0;
  return put(key, PT_BLOB, value, len);
}

bool Preferences::isKey(const char *key) { return getType(key) != PT_INVALID; }

PreferenceType Preferences::getType(const char *key) {
  if (!started_ || !nameOk(key)) return PT_INVALID;
  auto ns = store().find(namespace_);
  if (ns == store().end()) return PT_INVALID;
  auto it = ns->second.find(key);
  return it == ns->second.end() ? PT_INVALID : it->second.type;
}

int8_t Preferences::getChar(const char *key, int8_t defaultValue) { int8_t v; return get(key, PT_I8, &v, sizeof v) ? v : defaultValue; }
uint8_t Preferences::getUChar(const char *key, uint8_t defaultValue) { uint8_t v; return get(key, PT_U8, &v, sizeof v) ? v : defaultValue; }
int16_t Preferences::getShort(const char *key, int16_t defaultValue) { int16_t v; return get(key, PT_I16, &v, sizeof v) ? v : defaultValue; }
uint16_t Preferences::getUShort(const char *key, uint16_t defaultValue) { uint16_t v; return get(key, PT_U16, &v, sizeof v) ? v : defaultValue; }
int32_t Preferences::getInt(const char *key, int32_t defaultValue) { int32_t v; return get(key, PT_I32, &v, sizeof v) ? v : defaultValue; }
uint32_t Preferences::getUInt(const char *key, uint32_t defaultValue) { uint32_t v; return get(key, PT_U32, &v, sizeof v) ? v : defaultValue; }
int32_t Preferences::getLong(const char *key, int32_t defaultValue) { return getInt(key, defaultValue); }
uint32_t Preferences::getULong(const char *key, uint32_t defaultValue) { return getUInt(key, defaultValue); }
int64_t Preferences::getLong64(const char *key, int64_t defaultValue) { int64_t v; return get(key, PT_I64, &v, sizeof v) ? v : defaultValue; }
uint64_t Preferences::getULong64(const char *key, uint64_t defaultValue) { uint64_t v; return get(key, PT_U64, &v, sizeof v) ? v : defaultValue; }
bool Preferences::getBool(const char *key, bool defaultValue) { return getUChar(key, defaultValue ? 1 : 0) == 1; }

size_t Preferences::getString(const char *key, char *value, size_t maxLen) {
  if (!started_ || !nameOk(key) || value == nullptr) return 0;
  auto ns = store().find(namespace_);
  if (ns == store().end()) return 0;
  auto it = ns->second.find(key);
  if (it == ns->second.end() || it->second.type != PT_STR) return 0;
  const size_t len = it->second.data.size();
  if (len + 1 > maxLen) return 0;
  if (len) memcpy(value, it->second.data.data(), len);
  value[len] = '\0';
  return len + 1;
}

String Preferences::getString(const char *key, String defaultValue) {
  if (!started_ || !nameOk(key)) return defaultValue;
  auto ns = store().find(namespace_);
  if (ns == store().end()) return defaultValue;
  auto it = ns->second.find(key);
  if (it == ns->second.end() || it->second.type != PT_STR) return defaultValue;
  return String(std::string(it->second.data.begin(), it->second.data.end()));
}

size_t Preferences::getBytesLength(const char *key) {
  if (!started_ || !nameOk(key)) return 0;
  auto ns = store().find(namespace_);
  if (ns == store().end()) return 0;
  auto it = ns->second.find(key);
  if (it == ns->second.end() || it->second.type != PT_BLOB) return 0;
  return it->second.data.size();
}

size_t Preferences::getBytes(const char *key, void *buf, size_t maxLen) {
  const size_t len = getBytesLength(key);
  if (len == 0 || buf == nullptr || len > maxLen) return 0;
  memcpy(buf, store()[namespace_][key].data.data(), len);
  return len;
}

size_t Preferences::freeEntries() { return 1000; }
