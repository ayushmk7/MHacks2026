// src/vk/host/consent.cpp
// The first-run consent store (app-host.md, "Consent"; wallet/stores.md, "Consent").
//
// File /vk/consent.bin: an 8-byte header (magic "VKP1", version u16, count u16), then up to 32
// records of 40 bytes: app_id[33] NUL-padded, 3 bytes of padding, hash u32. All little-endian.
// Records are kept oldest first. Saving an app that is already listed drops its old record and adds
// the new one at the end; saving a new app into a full file drops the first (oldest) record.
//
// The whole file is at most 1288 bytes. It is read once and kept in RAM, so has(), count() and at()
// cost no file access after the first call (the Wallet app lists the entries on every frame); only
// this file writes it. A save writes it whole: to /vk/consent.bin.tmp, which is then renamed over
// the original (stores.md, common rules). A file with a wrong magic or version reads as empty and
// is renamed to /vk/consent.bin.bad by the next save.
//
// Host-test seam (test/host/test_consent.cpp): every file call goes through vk::fileio::ops; the
// log is behind #ifndef VK_HOST_TEST; hostReboot() forgets the copy in RAM.
#include "consent.h"

#include <string.h>

#include <algorithm>
#include <vector>

#include "../core/config.h"   // VK_ON_RESET
#include "../core/fileio.h"

#ifndef VK_HOST_TEST
#include "../../badge_log.h"
#define VK_CONSENT_LOG(...) ::badge_log::tagf("vk", __VA_ARGS__)
#else
#define VK_CONSENT_LOG(...) do { } while (0)
#endif

namespace vk::host::consent {

namespace {

const char DIR_PATH[] = "/vk";
const char FILE_PATH[] = "/vk/consent.bin";
const char TMP_FILE_PATH[] = "/vk/consent.bin.tmp";
const char BAD_FILE_PATH[] = "/vk/consent.bin.bad";

const uint8_t MAGIC[4] = {'V', 'K', 'P', '1'};
constexpr uint16_t FORMAT_VERSION = 1;
constexpr size_t HEADER_SIZE = 8;
constexpr size_t RECORD_SIZE = 40;
constexpr size_t CAPACITY = 32;
constexpr size_t ID_SIZE = 33;                 // 32 characters and a NUL
constexpr size_t OFF_HASH = 36;                // after app_id[33] and 3 bytes of padding
constexpr size_t IMAGE_SIZE = HEADER_SIZE + CAPACITY * RECORD_SIZE;

// The file as it is on disk, once sLoaded is true: sCount valid records, or sBad when a file is
// there but is not a consent file (it then counts as empty).
uint8_t sImage[IMAGE_SIZE];
bool sLoaded = false;
size_t sCount = 0;
bool sBad = false;

void putU16(uint8_t *at, uint16_t value) {
  at[0] = (uint8_t)(value & 0xFF);
  at[1] = (uint8_t)(value >> 8);
}

uint16_t getU16(const uint8_t *at) { return (uint16_t)(at[0] | ((uint16_t)at[1] << 8)); }

void putU32(uint8_t *at, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) at[i] = (uint8_t)(value >> (8 * i));
}

uint32_t getU32(const uint8_t *at) {
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i) value |= (uint32_t)at[i] << (8 * i);
  return value;
}

uint8_t *record(size_t index) { return sImage + HEADER_SIZE + index * RECORD_SIZE; }

// Reads the file into sImage and returns its record count. 0 with `bad` true when a file is there
// but is not a consent file; 0 with `bad` false when there is no file.
size_t readImage(const vk::fileio::Ops *io, bool &bad) {
  bad = false;
  const long size = io->size(FILE_PATH);
  if (size < 0) return 0;                                    // no file yet
  bad = true;
  if (size < (long)HEADER_SIZE || size > (long)IMAGE_SIZE) return 0;
  if (!io->read(FILE_PATH, 0, sImage, (size_t)size)) return 0;
  if (memcmp(sImage, MAGIC, sizeof MAGIC) != 0 || getU16(sImage + 4) != FORMAT_VERSION) return 0;
  const size_t count = getU16(sImage + 6);
  if (count > CAPACITY || (size_t)size < HEADER_SIZE + count * RECORD_SIZE) return 0;
  bad = false;
  return count;
}

// The record count, reading the file on the first call only. Sets *bad as readImage() does.
size_t loadImage(bool *bad) {
  if (bad) *bad = false;
  const vk::fileio::Ops *io = vk::fileio::ops;
  if (io == nullptr) return 0;
  if (!sLoaded) {
    sCount = readImage(io, sBad);
    sLoaded = true;
  }
  if (bad) *bad = sBad;
  return sCount;
}

// True when the record's id is exactly `appId` (1 to 32 characters).
bool recordIs(size_t index, const String &appId) {
  const uint8_t *id = record(index);
  const size_t length = appId.length();
  return length < ID_SIZE && memcmp(id, appId.c_str(), length) == 0 && id[length] == 0;
}

bool validId(const String &appId) { return appId.length() > 0 && appId.length() < ID_SIZE; }

// Writes sImage with `count` records: to the .tmp file, then renamed over the original. If the
// rename is refused the file is written in place instead, so consent keeps working.
bool storeImage(size_t count) {
  const vk::fileio::Ops *io = vk::fileio::ops;
  if (io == nullptr) return false;
  memcpy(sImage, MAGIC, sizeof MAGIC);
  putU16(sImage + 4, FORMAT_VERSION);
  putU16(sImage + 6, (uint16_t)count);
  const size_t length = HEADER_SIZE + count * RECORD_SIZE;

  io->makeDir(DIR_PATH);                 // succeeds if it is already there; if it fails, so does writeAll
  if (!io->writeAll(TMP_FILE_PATH, sImage, length)) {
    io->removeFile(TMP_FILE_PATH);       // whatever part of it was written
    return false;
  }
  if (io->renameFile(TMP_FILE_PATH, FILE_PATH)) return true;
  VK_CONSENT_LOG("consent: rename refused; writing %s in place", FILE_PATH);
  const bool written = io->writeAll(FILE_PATH, sImage, length);
  io->removeFile(TMP_FILE_PATH);
  return written;
}

}  // namespace

uint32_t hashPermissions(const String &permissions) {
  // The names, sorted, each one once: neither the order nor a repeat in app.ini changes the hash.
  std::vector<String> names;
  const unsigned int length = permissions.length();
  unsigned int at = 0;
  while (at < length) {
    int end = permissions.indexOf(',', at);
    if (end < 0) end = (int)length;
    String name = permissions.substring(at, (unsigned int)end);
    name.trim();
    if (name.length()) names.push_back(name);
    at = (unsigned int)end + 1;
  }
  std::sort(names.begin(), names.end(),
            [](const String &a, const String &b) { return strcmp(a.c_str(), b.c_str()) < 0; });

  // FNV-1a, 32 bits, over the names joined with commas.
  uint32_t hash = 2166136261u;
  bool first = true;
  for (size_t i = 0; i < names.size(); ++i) {
    if (i > 0 && names[i] == names[i - 1]) continue;
    if (!first) hash = (hash ^ (uint8_t)',') * 16777619u;
    first = false;
    for (const char *c = names[i].c_str(); *c; ++c) hash = (hash ^ (uint8_t)*c) * 16777619u;
  }
  return hash;
}

bool has(const String &appId, uint32_t hash) {
  if (!validId(appId)) return false;
  const size_t count = loadImage(nullptr);
  for (size_t i = 0; i < count; ++i) {
    if (recordIs(i, appId)) return getU32(record(i) + OFF_HASH) == hash;
  }
  return false;
}

bool save(const String &appId, uint32_t hash) {
  const vk::fileio::Ops *io = vk::fileio::ops;
  if (io == nullptr || !validId(appId)) return false;

  bool bad = false;
  size_t count = loadImage(&bad);
  if (bad) {                             // not a consent file: keep it aside and start an empty one
    if (io->exists(BAD_FILE_PATH)) io->removeFile(BAD_FILE_PATH);
    if (io->renameFile(FILE_PATH, BAD_FILE_PATH)) {
      VK_CONSENT_LOG("consent: bad file renamed to %s", BAD_FILE_PATH);
    } else {
      VK_CONSENT_LOG("consent: bad file could not be renamed; overwriting it");
    }
  }

  // Drop this app's old record, or the oldest one if the file is full; the records after it move up.
  size_t drop = count;
  for (size_t i = 0; i < count; ++i) {
    if (recordIs(i, appId)) { drop = i; break; }
  }
  if (drop == count && count == CAPACITY) drop = 0;
  if (drop < count) {
    memmove(record(drop), record(drop + 1), (count - drop - 1) * RECORD_SIZE);
    --count;
  }

  uint8_t *slot = record(count);
  memset(slot, 0, RECORD_SIZE);
  memcpy(slot, appId.c_str(), appId.length());
  putU32(slot + OFF_HASH, hash);

  sLoaded = false;                       // sImage is ahead of the file until the write has succeeded
  if (!storeImage(count + 1)) return false;
  sCount = count + 1;
  sBad = false;
  sLoaded = true;
  return true;
}

void eraseAll() {
  const vk::fileio::Ops *io = vk::fileio::ops;
  if (io == nullptr) return;
  io->removeFile(FILE_PATH);
  io->removeFile(TMP_FILE_PATH);
  io->removeFile(BAD_FILE_PATH);
  sLoaded = false;
}

size_t count() { return loadImage(nullptr); }

bool at(size_t index, String &appIdOut, uint32_t &hashOut) {
  if (index >= loadImage(nullptr)) return false;
  const uint8_t *entry = record(index);
  char id[ID_SIZE];
  memcpy(id, entry, ID_SIZE);
  id[ID_SIZE - 1] = '\0';
  appIdOut = id;
  hashOut = getU32(entry + OFF_HASH);
  return true;
}

#ifdef VK_HOST_TEST
void hostReboot() { sLoaded = false; }
#endif

namespace {
VK_ON_RESET(consent, eraseAll);   // VKRESET erases stored consent with the wallet config
}

}  // namespace vk::host::consent
