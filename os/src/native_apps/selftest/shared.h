// src/native_apps/selftest/shared.h
// The few things more than one suite uses. Device code (it is not compiled by the host simulation).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "selftest.h"

namespace selftest {

// The storage probe: the filesystem is mounted, and a small file is written, read back and deleted
// through vk::fileio. Used by HARDWARE ("storage") and STORES ("probe"). Writes the value.
State storageProbe(char *value, size_t cap);

// Published Ed25519 test vectors (RFC 8032, section 7.1, TESTS 1 to 3). Verification only.
struct Ed25519Vector {
  const char *name;
  const uint8_t *key;                    // 32 bytes
  const uint8_t *message;
  size_t length;
  const uint8_t *signature;              // 64 bytes
};
extern const Ed25519Vector RFC8032[3];
constexpr size_t RFC8032_COUNT = 3;
constexpr size_t RFC8032_HARDWARE = 1;   // TEST 2 (a one-byte message): the HARDWARE crypto check

// Wi-Fi joined as a station (connected() alone is also true in hotspot mode; ui.md, "Balance").
bool joined();

}  // namespace selftest
