// test/host/shim/Preferences.h
// Host-test stand-in for the ESP32 core's <Preferences.h>: NVS kept in memory.
//
// One store is shared by every Preferences object, keyed by namespace, as on the badge. It
// follows the real limits and failures that code can trip over:
//   - namespace and key names are 1..15 characters; a longer or empty one fails;
//   - begin(name, true) (read-only) fails when the namespace has never been written;
//   - put*() returns the number of bytes stored, 0 on failure (not begun, read-only, bad key,
//     or a failure injected with vk_host_nvs_fail_writes);
//   - a getter returns its default when the key is missing or holds another type.
//
// Test hooks:
//   vk_host_nvs_reset()         erase every namespace and clear vk_host_nvs_fail_writes
//   vk_host_nvs_fail_writes     > 0: the next N put*() calls return 0 and store nothing (it counts down);
//                               < 0: every put*() fails until it is set back to 0
#pragma once

#include <Arduino.h>

void vk_host_nvs_reset();
extern int vk_host_nvs_fail_writes;

typedef enum { PT_I8, PT_U8, PT_I16, PT_U16, PT_I32, PT_U32, PT_I64, PT_U64, PT_STR, PT_BLOB, PT_INVALID } PreferenceType;

class Preferences {
 public:
  Preferences();
  ~Preferences();

  bool begin(const char *name, bool readOnly = false, const char *partition_label = nullptr);
  void end();

  bool clear();                       // removes every key of this namespace
  bool remove(const char *key);

  size_t putChar(const char *key, int8_t value);
  size_t putUChar(const char *key, uint8_t value);
  size_t putShort(const char *key, int16_t value);
  size_t putUShort(const char *key, uint16_t value);
  size_t putInt(const char *key, int32_t value);
  size_t putUInt(const char *key, uint32_t value);
  size_t putLong(const char *key, int32_t value);
  size_t putULong(const char *key, uint32_t value);
  size_t putLong64(const char *key, int64_t value);
  size_t putULong64(const char *key, uint64_t value);
  size_t putBool(const char *key, bool value);
  size_t putString(const char *key, const char *value);
  size_t putString(const char *key, String value);
  size_t putBytes(const char *key, const void *value, size_t len);

  bool isKey(const char *key);
  PreferenceType getType(const char *key);

  int8_t getChar(const char *key, int8_t defaultValue = 0);
  uint8_t getUChar(const char *key, uint8_t defaultValue = 0);
  int16_t getShort(const char *key, int16_t defaultValue = 0);
  uint16_t getUShort(const char *key, uint16_t defaultValue = 0);
  int32_t getInt(const char *key, int32_t defaultValue = 0);
  uint32_t getUInt(const char *key, uint32_t defaultValue = 0);
  int32_t getLong(const char *key, int32_t defaultValue = 0);
  uint32_t getULong(const char *key, uint32_t defaultValue = 0);
  int64_t getLong64(const char *key, int64_t defaultValue = 0);
  uint64_t getULong64(const char *key, uint64_t defaultValue = 0);
  bool getBool(const char *key, bool defaultValue = false);
  size_t getString(const char *key, char *value, size_t maxLen);     // bytes copied incl. the NUL; 0 on failure
  String getString(const char *key, String defaultValue = String());
  size_t getBytesLength(const char *key);
  size_t getBytes(const char *key, void *buf, size_t maxLen);        // 0 if missing or maxLen is too small

  size_t freeEntries();

 private:
  size_t put(const char *key, PreferenceType type, const void *data, size_t len);
  bool get(const char *key, PreferenceType type, void *out, size_t len);
  bool started_;
  bool readOnly_;
  std::string namespace_;
};
