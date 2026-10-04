// src/native_apps/selftest/suite_stores.cpp
// STORES: the filesystem and the three stores of wallet/stores.md.
//
//   fs         mounted, used and free space
//   probe      a small file written, read back and deleted through vk::fileio (as HARDWARE storage)
//   history    /vk/history.bin   header magic, version, count within capacity, size holds the count
//   contacts   /vk/contacts.bin  the same
//   consent    /vk/consent.bin   the same, and the consent store's own count agrees
//
// A missing file is OK ("none yet": a store is created by its first write). A file the store would
// read as empty (wrong magic or version, a count it cannot hold) is FAIL: the firmware recovers by
// renaming it on the next write, but whatever it held is lost to the badge. A leftover <name>.bad
// is mentioned in the value.
//
// Only the 8-byte header is read, through the one file layer, read-only. The stores' own headers
// belong to their features, which a native app may not include (native-apps.md, rule 9): the paths
// and formats below are the ones stores.md publishes.
#include <Arduino.h>

#include <stdio.h>
#include <string.h>

#include "../../apps/app_store.h"
#include "../../vk/core/fileio.h"
#include "../../vk/host/consent.h"
#include "selftest.h"
#include "shared.h"
#include "suites.h"

namespace selftest {
namespace {

struct StoreFormat {
  const char *path;
  char magic[5];
  uint16_t version;
  size_t headerSize;                     // the bytes before the first record
  size_t recordSize;
  size_t capacity;
};

// wallet/stores.md: "History", "Contacts", "Consent".
const StoreFormat HISTORY = {"/vk/history.bin", "VKH1", 1, 12, 192, 128};
const StoreFormat CONTACTS = {"/vk/contacts.bin", "VKC1", 1, 8, 72, 64};
const StoreFormat CONSENT = {"/vk/consent.bin", "VKP1", 1, 8, 40, 32};

uint16_t getU16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// The header of one store. Returns the state; `count` is the number of records when it is OK.
State readStore(const StoreFormat &f, size_t &count, char *value, size_t cap) {
  count = 0;
  const vk::fileio::Ops *io = vk::fileio::ops;
  if (io == nullptr) {
    snprintf(value, cap, "no file layer");
    return State::Fail;
  }
  char bad[40];
  snprintf(bad, sizeof bad, "%s.bad", f.path);
  const char *badNote = io->exists(bad) ? ", .bad kept" : "";
  if (!io->exists(f.path)) {
    snprintf(value, cap, "none yet%s", badNote);
    return State::Ok;
  }
  const long size = io->size(f.path);
  uint8_t header[8];
  if (size < (long)f.headerSize || !io->read(f.path, 0, header, sizeof header)) {
    snprintf(value, cap, "unreadable, %ld bytes", size);
    return State::Fail;
  }
  if (memcmp(header, f.magic, 4) != 0) {
    snprintf(value, cap, "wrong magic");
    return State::Fail;
  }
  if (getU16(header + 4) != f.version) {
    snprintf(value, cap, "version %u, expected %u", (unsigned)getU16(header + 4), (unsigned)f.version);
    return State::Fail;
  }
  count = getU16(header + 6);
  if (count > f.capacity) {
    snprintf(value, cap, "count %u over %u", (unsigned)count, (unsigned)f.capacity);
    return State::Fail;
  }
  if ((size_t)size < f.headerSize + count * f.recordSize) {
    snprintf(value, cap, "%u entries, file short", (unsigned)count);
    return State::Fail;
  }
  snprintf(value, cap, "%u of %u entries%s", (unsigned)count, (unsigned)f.capacity, badNote);
  return State::Ok;
}

void checkFs(Ctx &c) {
  if (!app_store::mounted()) {
    c.finish(State::Fail, "not mounted");
    return;
  }
  const size_t total = app_store::totalBytes(), used = app_store::usedBytes();
  const size_t freeBytes = total > used ? total - used : 0;
  c.finish(freeBytes > 0 ? State::Ok : State::Fail, "%luK free, %luK used of %luK", (unsigned long)(freeBytes / 1024),
           (unsigned long)(used / 1024), (unsigned long)(total / 1024));
}

void checkProbe(Ctx &c) {
  char value[ROW_VALUE_MAX];
  const State state = storageProbe(value, sizeof value);
  c.finish(state, "%s", value);
}

void checkStore(Ctx &c, const StoreFormat &f) {
  char value[ROW_VALUE_MAX];
  size_t count;
  const State state = readStore(f, count, value, sizeof value);
  c.finish(state, "%s", value);
}

void checkHistory(Ctx &c) { checkStore(c, HISTORY); }
void checkContacts(Ctx &c) { checkStore(c, CONTACTS); }

// The consent store keeps its file in RAM once read: the two counts must agree.
void checkConsent(Ctx &c) {
  char value[ROW_VALUE_MAX];
  size_t count;
  const State state = readStore(CONSENT, count, value, sizeof value);
  const size_t inMemory = vk::host::consent::count();
  if (state == State::Ok && inMemory != count) c.finish(State::Fail, "file %u, store %u", (unsigned)count, (unsigned)inMemory);
  else c.finish(state, "%s", value);
}

const Check TABLE[] = {
    {"fs", "FILESYSTEM", Kind::Auto, Profile::Any, checkFs, nullptr, 0},
    {"probe", "WRITE READ DELETE", Kind::Auto, Profile::Any, checkProbe, nullptr, 0},
    {"history", "HISTORY", Kind::Auto, Profile::Any, checkHistory, nullptr, 0},
    {"contacts", "CONTACTS", Kind::Auto, Profile::Any, checkContacts, nullptr, 0},
    {"consent", "CONSENT", Kind::Auto, Profile::Any, checkConsent, nullptr, 0},
};

}  // namespace

const Suite STORES = {
    "stores", "STORES", Profile::Any, TABLE, sizeof TABLE / sizeof TABLE[0], nullptr, nullptr, nullptr, false,
};

}  // namespace selftest
