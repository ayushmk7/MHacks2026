// LINK: src/vk/host/consent.cpp
// test_consent - the first-run consent store (docs/os/platform/app-host.md, "Consent";
// wallet/stores.md, "Consent") on the in-memory file layer of the shim: the permission-list hash,
// the file layout, save / has / replace, oldest replaced at 33, eraseAll and the reset listener,
// bad-file recovery, failing writes, and that reading costs no file access after the first call.
#include <Arduino.h>

#include <stdio.h>
#include <string.h>

#include <vector>

#include "../../src/vk/core/config.h"
#include "../../src/vk/host/consent.h"
#include "vk_host_fileio.h"

using namespace vk::host::consent;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static const char FILE_PATH[] = "/vk/consent.bin";
static const char TMP_PATH[] = "/vk/consent.bin.tmp";
static const char BAD_PATH[] = "/vk/consent.bin.bad";

// ---- helpers --------------------------------------------------------------------------------

static const vk::fileio::Ops *io() { return vk::fileio::ops; }

// An empty filesystem and a store that has read nothing yet.
static void fresh() {
  vk_host_fileio_install();
  vk_host_fileio_reset();
  hostReboot();
}

static std::vector<uint8_t> fileBytes(const char *path) {
  std::vector<uint8_t> out;
  const long size = io()->size(path);
  if (size <= 0) return out;
  out.resize((size_t)size);
  if (!io()->read(path, 0, out.data(), out.size())) out.clear();
  return out;
}

// Writes a file behind the store's back, as if it had been there at boot.
static void putFile(const char *path, const std::vector<uint8_t> &bytes) {
  io()->makeDir("/vk");
  CHECK(io()->writeAll(path, bytes.data(), bytes.size()));
  hostReboot();
}

static String numbered(unsigned n) {
  char id[16];
  snprintf(id, sizeof id, "app%u", n);
  return String(id);
}

// FNV-1a over a string, written out here independently of the code under test.
static uint32_t fnv1a(const char *text) {
  uint32_t hash = 2166136261u;
  for (const char *c = text; *c; ++c) hash = (hash ^ (uint8_t)*c) * 16777619u;
  return hash;
}

// ---- the hash -------------------------------------------------------------------------------

static void test_hash() {
  // FNV-1a of the sorted, comma-joined list.
  CHECK(hashPermissions("sign") == fnv1a("sign"));
  CHECK(hashPermissions("sign,net") == fnv1a("net,sign"));
  CHECK(hashPermissions("sign,net,espnow") == fnv1a("espnow,net,sign"));
  CHECK(hashPermissions("") == 2166136261u);
  CHECK(fnv1a("a") == 0xE40C292Cu);                       // the published FNV-1a test value

  // The order in app.ini does not matter.
  const uint32_t three = hashPermissions("sign,net,espnow");
  CHECK(hashPermissions("net,sign,espnow") == three);
  CHECK(hashPermissions("espnow,net,sign") == three);
  CHECK(hashPermissions("espnow,sign,net") == three);

  // Neither do spaces, empty items or a repeated name.
  CHECK(hashPermissions(" sign , net,,espnow,") == three);
  CHECK(hashPermissions("sign,net,espnow,sign") == three);
  CHECK(hashPermissions(",") == hashPermissions(""));

  // A changed, added or removed name does.
  CHECK(hashPermissions("sign,net,espnov") != three);
  CHECK(hashPermissions("sign,net") != three);
  CHECK(hashPermissions("sign,net,espnow,mic") != three);
  CHECK(hashPermissions("sign") != hashPermissions("request"));
  CHECK(hashPermissions("sign") != hashPermissions(""));
  CHECK(hashPermissions("Sign") != hashPermissions("sign"));
  // The comma is part of what is hashed: two names are not their concatenation.
  CHECK(hashPermissions("ab,c") != hashPermissions("a,bc"));
  CHECK(hashPermissions("a,b") != hashPermissions("ab"));
}

// ---- save, has, count, at -------------------------------------------------------------------

static void test_empty() {
  fresh();
  String id = "unchanged";
  uint32_t hash = 7;
  CHECK(count() == 0);
  CHECK(!has("pay", 1));
  CHECK(!at(0, id, hash));
  CHECK(id == "unchanged" && hash == 7);
  CHECK(!io()->exists(FILE_PATH));                        // reading creates nothing
  CHECK(!io()->exists("/vk"));
}

static void test_save_and_has() {
  fresh();
  const uint32_t signHash = hashPermissions("sign");
  CHECK(save("needsign", signHash));                      // /vk/ did not exist: save creates it
  CHECK(count() == 1);
  CHECK(has("needsign", signHash));
  CHECK(!has("needsign", signHash + 1));                  // a changed permission list asks again (T-APP3)
  CHECK(!has("needsig", signHash));                       // a prefix of the id is another app
  CHECK(!has("needsign2", signHash));
  CHECK(!has("", signHash));
  CHECK(!io()->exists(TMP_PATH));                         // the temporary file was renamed away

  CHECK(save("pay", 0xDEADBEEFu));
  CHECK(save("game", 0));
  CHECK(count() == 3);
  CHECK(has("needsign", signHash) && has("pay", 0xDEADBEEFu) && has("game", 0));

  // The same after a reboot: it is the file that holds them.
  hostReboot();
  CHECK(count() == 3);
  CHECK(has("needsign", signHash) && has("pay", 0xDEADBEEFu) && has("game", 0));
  CHECK(!has("pay", 0xDEADBEEEu));

  // at(): oldest first.
  String id;
  uint32_t hash = 0;
  CHECK(at(0, id, hash) && id == "needsign" && hash == signHash);
  CHECK(at(1, id, hash) && id == "pay" && hash == 0xDEADBEEFu);
  CHECK(at(2, id, hash) && id == "game" && hash == 0);
  CHECK(!at(3, id, hash));

  // The file: 8-byte header, 40-byte records, little-endian (stores.md).
  const std::vector<uint8_t> bytes = fileBytes(FILE_PATH);
  CHECK(bytes.size() == 8 + 3 * 40);
  if (bytes.size() == 8 + 3 * 40) {
    CHECK(memcmp(bytes.data(), "VKP1", 4) == 0);
    CHECK(bytes[4] == 1 && bytes[5] == 0);                // version
    CHECK(bytes[6] == 3 && bytes[7] == 0);                // count
    const uint8_t *second = bytes.data() + 8 + 40;
    CHECK(memcmp(second, "pay", 3) == 0);
    bool padded = true;
    for (size_t i = 3; i < 36; ++i) padded = padded && second[i] == 0;   // the id's NUL padding and 3 bytes
    CHECK(padded);
    CHECK(second[36] == 0xEF && second[37] == 0xBE && second[38] == 0xAD && second[39] == 0xDE);
  }

  // Ids: 1 to 32 characters; the full 32 are stored and compared.
  const String long32 = "abcdefghijklmnopqrstuvwxyz012345";
  const String other32 = "abcdefghijklmnopqrstuvwxyz012346";
  CHECK(long32.length() == 32);
  CHECK(save(long32, 5));
  CHECK(has(long32, 5));
  CHECK(!has(other32, 5));
  CHECK(!has("abcdefghijklmnopqrstuvwxyz01234", 5));
  CHECK(at(3, id, hash) && id == long32 && hash == 5);
  CHECK(!save(long32 + "6", 5));                          // 33 characters
  CHECK(!has(long32 + "6", 5));
  CHECK(!save("", 5));
  CHECK(count() == 4);
  hostReboot();
  CHECK(count() == 4 && has(long32, 5) && !has(other32, 5));
}

static void test_replace_same_id() {
  fresh();
  CHECK(save("first", 1));
  CHECK(save("needsign", 10));
  CHECK(save("last", 3));
  CHECK(save("needsign", 20));                            // the permission list changed and was approved again
  CHECK(count() == 3);
  CHECK(has("needsign", 20));
  CHECK(!has("needsign", 10));
  CHECK(has("first", 1) && has("last", 3));

  // The renewed entry is now the newest one.
  String id;
  uint32_t hash = 0;
  CHECK(at(0, id, hash) && id == "first");
  CHECK(at(1, id, hash) && id == "last");
  CHECK(at(2, id, hash) && id == "needsign" && hash == 20);

  CHECK(save("needsign", 20));                            // saving the same again changes nothing
  CHECK(count() == 3 && has("needsign", 20));

  hostReboot();
  CHECK(count() == 3 && has("needsign", 20) && !has("needsign", 10) && has("first", 1) && has("last", 3));
  CHECK(at(2, id, hash) && id == "needsign" && hash == 20);
}

static void test_oldest_replaced() {
  fresh();
  for (unsigned n = 0; n < 32; ++n) CHECK(save(numbered(n), n));
  CHECK(count() == 32);
  for (unsigned n = 0; n < 32; ++n) CHECK(has(numbered(n), n));
  CHECK(fileBytes(FILE_PATH).size() == 8 + 32 * 40);

  // Replacing an app that is already there in a full file drops nothing else.
  CHECK(save(numbered(5), 500));
  CHECK(count() == 32);
  CHECK(has(numbered(0), 0) && has(numbered(5), 500) && has(numbered(31), 31));

  // The 33rd app replaces the oldest.
  CHECK(save(numbered(32), 32));
  CHECK(count() == 32);
  CHECK(!has(numbered(0), 0));
  CHECK(has(numbered(1), 1));
  CHECK(has(numbered(32), 32));
  CHECK(has(numbered(5), 500));
  CHECK(fileBytes(FILE_PATH).size() == 8 + 32 * 40);

  // And the next one the next oldest; app5 was renewed, so it outlives its neighbours.
  for (unsigned n = 33; n < 40; ++n) CHECK(save(numbered(n), n));
  CHECK(count() == 32);
  for (unsigned n = 1; n <= 8; ++n) {
    if (n == 5) continue;
    CHECK(!has(numbered(n), n));
  }
  CHECK(has(numbered(5), 500));
  CHECK(has(numbered(9), 9));
  CHECK(has(numbered(39), 39));

  String id;
  uint32_t hash = 0;
  CHECK(at(0, id, hash) && id == numbered(9));
  CHECK(at(31, id, hash) && id == numbered(39) && hash == 39);

  hostReboot();
  CHECK(count() == 32);
  CHECK(!has(numbered(8), 8) && has(numbered(5), 500) && has(numbered(9), 9) && has(numbered(39), 39));
  CHECK(at(0, id, hash) && id == numbered(9));
  CHECK(at(31, id, hash) && id == numbered(39) && hash == 39);
}

// ---- eraseAll -------------------------------------------------------------------------------

static void test_erase_all() {
  fresh();
  eraseAll();                                             // nothing there: no harm
  CHECK(count() == 0);

  CHECK(save("needsign", 1));
  CHECK(save("pay", 2));
  eraseAll();
  CHECK(count() == 0);
  CHECK(!has("needsign", 1) && !has("pay", 2));
  CHECK(!io()->exists(FILE_PATH));
  hostReboot();
  CHECK(count() == 0 && !has("needsign", 1));
  CHECK(save("pay", 3));                                  // and the store works again
  CHECK(count() == 1 && has("pay", 3));

  // VKRESET reaches it through a VK_ON_RESET listener: exactly one is registered by consent.cpp.
  size_t listeners = 0;
  for (auto *l = vk::config::ResetListener::first(); l; l = l->next()) {
    listeners++;
    if (l->fn) l->fn();
  }
  CHECK(listeners == 1);
  CHECK(count() == 0 && !has("pay", 3));

  // A set-aside bad file goes too.
  putFile(FILE_PATH, {'j', 'u', 'n', 'k'});
  CHECK(save("pay", 4));
  CHECK(io()->exists(BAD_PATH));
  eraseAll();
  CHECK(!io()->exists(FILE_PATH) && !io()->exists(BAD_PATH) && !io()->exists(TMP_PATH));
}

// ---- bad files ------------------------------------------------------------------------------

// A valid file with one record, as bytes.
static std::vector<uint8_t> oneRecordFile() {
  fresh();
  CHECK(save("needsign", 0x11223344u));
  return fileBytes(FILE_PATH);
}

static void expectRecovery(const std::vector<uint8_t> &badBytes, const char *what) {
  fresh();
  putFile(FILE_PATH, badBytes);
  const bool readsEmpty = count() == 0 && !has("needsign", 0x11223344u);
  String id;
  uint32_t hash = 0;
  const bool noEntry = !at(0, id, hash);
  const bool untouched = fileBytes(FILE_PATH) == badBytes && !io()->exists(BAD_PATH);   // reading changes nothing
  const bool saved = save("pay", 9);
  bool works = count() == 1 && has("pay", 9) && !has("needsign", 0x11223344u);
  hostReboot();
  works = works && count() == 1 && has("pay", 9) && !has("needsign", 0x11223344u);
  const bool keptAside = fileBytes(BAD_PATH) == badBytes;
  const bool newFile = fileBytes(FILE_PATH).size() == 8 + 40;
  if (!(readsEmpty && noEntry && untouched && saved && works && keptAside && newFile)) {
    printf("FAIL bad file (%s): empty=%d noEntry=%d untouched=%d saved=%d works=%d aside=%d new=%d\n", what,
           readsEmpty, noEntry, untouched, saved, works, keptAside, newFile);
    fails++;
  }
}

static void test_bad_file_recovery() {
  const std::vector<uint8_t> good = oneRecordFile();
  CHECK(good.size() == 8 + 40);
  if (good.size() != 8 + 40) return;

  std::vector<uint8_t> bytes = good;
  bytes[3] = '2';                                         // "VKP2"
  expectRecovery(bytes, "magic");

  bytes = good;
  memcpy(bytes.data(), "VKH1", 4);                        // the history file's magic
  expectRecovery(bytes, "another store's magic");

  bytes = good;
  bytes[4] = 2;                                           // version 2
  expectRecovery(bytes, "version");

  bytes = good;
  bytes[6] = 33;                                          // count above the capacity
  bytes.resize(8 + 33 * 40, 0);
  expectRecovery(bytes, "count 33");

  bytes = good;
  bytes[6] = 2;                                           // count says two records, the file holds one
  expectRecovery(bytes, "shorter than its count");

  expectRecovery(std::vector<uint8_t>(good.begin(), good.begin() + 5), "truncated header");
  expectRecovery({'x'}, "one byte");
  expectRecovery(std::vector<uint8_t>(4000, 0xAA), "garbage longer than a full file");

  // An earlier .bad file is replaced by the newer one.
  fresh();
  putFile(BAD_PATH, {'o', 'l', 'd'});
  putFile(FILE_PATH, {'n', 'e', 'w', 'e', 'r'});
  CHECK(save("pay", 1));
  CHECK(fileBytes(BAD_PATH) == std::vector<uint8_t>({'n', 'e', 'w', 'e', 'r'}));
  CHECK(has("pay", 1));

  // Bytes after the counted records are ignored and dropped by the next save.
  bytes = good;
  bytes.resize(8 + 40 + 13, 0x55);
  fresh();
  putFile(FILE_PATH, bytes);
  CHECK(count() == 1 && has("needsign", 0x11223344u));
  CHECK(save("pay", 2));
  CHECK(count() == 2 && has("needsign", 0x11223344u) && has("pay", 2));
  CHECK(fileBytes(FILE_PATH).size() == 8 + 2 * 40);
  CHECK(!io()->exists(BAD_PATH));

  // An id that fills all 33 bytes (no NUL) cannot have been written by save(); it matches nothing
  // and at() still returns a terminated string.
  bytes = good;
  memset(bytes.data() + 8, 'a', 33);
  fresh();
  putFile(FILE_PATH, bytes);
  CHECK(count() == 1);
  CHECK(!has("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0x11223344u));
  String id;
  uint32_t hash = 0;
  CHECK(at(0, id, hash) && id.length() == 32 && hash == 0x11223344u);
}

// ---- failing writes -------------------------------------------------------------------------

// The file layer with a rename that always refuses; everything else is the in-memory one.
static const vk::fileio::Ops *sMemory = nullptr;
static bool refuseRename(const char *, const char *) { return false; }

static void test_failing_writes() {
  // The first save: nothing is created.
  fresh();
  vk_host_fileio_fail_writes = 1;
  CHECK(!save("pay", 1));
  CHECK(count() == 0 && !has("pay", 1));
  CHECK(!io()->exists(FILE_PATH) && !io()->exists(TMP_PATH));
  CHECK(save("pay", 1));                                  // the next one goes through
  CHECK(has("pay", 1));

  // A later save: false, and what was stored is still there, unchanged, before and after a reboot.
  CHECK(save("game", 2));
  const std::vector<uint8_t> before = fileBytes(FILE_PATH);
  vk_host_fileio_fail_writes = 1;
  CHECK(!save("needsign", 3));
  CHECK(fileBytes(FILE_PATH) == before);
  CHECK(count() == 2 && has("pay", 1) && has("game", 2) && !has("needsign", 3));
  CHECK(!io()->exists(TMP_PATH));
  hostReboot();
  CHECK(count() == 2 && has("pay", 1) && has("game", 2) && !has("needsign", 3));

  vk_host_fileio_fail_writes = 1;
  CHECK(!save("pay", 99));                                // replacing an entry: the old hash stays
  CHECK(has("pay", 1) && !has("pay", 99));
  CHECK(fileBytes(FILE_PATH) == before);

  // Every write and rename failing.
  vk_host_fileio_fail_writes = -1;
  CHECK(!save("needsign", 3));
  CHECK(!save("pay", 99));
  vk_host_fileio_fail_writes = 0;
  CHECK(fileBytes(FILE_PATH) == before);
  CHECK(count() == 2 && has("pay", 1) && has("game", 2));
  CHECK(save("needsign", 3));
  CHECK(count() == 3 && has("needsign", 3));

  // A full file whose 33rd save fails keeps its oldest entry.
  fresh();
  for (unsigned n = 0; n < 32; ++n) CHECK(save(numbered(n), n));
  vk_host_fileio_fail_writes = 1;
  CHECK(!save(numbered(32), 32));
  CHECK(count() == 32 && has(numbered(0), 0) && !has(numbered(32), 32));

  // A bad file and a failing save: false, and the next save recovers.
  fresh();
  putFile(FILE_PATH, {'j', 'u', 'n', 'k'});
  vk_host_fileio_fail_writes = -1;
  CHECK(!save("pay", 1));
  vk_host_fileio_fail_writes = 0;
  CHECK(count() == 0);
  CHECK(save("pay", 1));
  CHECK(count() == 1 && has("pay", 1));

  // Only the rename refuses (a filesystem that will not rename over a file): the store is written
  // in place and the temporary file is removed.
  fresh();
  CHECK(save("pay", 1));
  sMemory = vk::fileio::ops;
  vk::fileio::Ops noRename = *sMemory;
  noRename.renameFile = refuseRename;
  vk::fileio::ops = &noRename;
  CHECK(save("game", 2));
  CHECK(count() == 2 && has("pay", 1) && has("game", 2));
  CHECK(!io()->exists(TMP_PATH));
  vk_host_fileio_install();
  hostReboot();
  CHECK(count() == 2 && has("pay", 1) && has("game", 2));

  // No file layer at all.
  vk::fileio::ops = nullptr;
  CHECK(!save("pay", 1));
  CHECK(count() == 0 && !has("pay", 1));
  eraseAll();
  vk_host_fileio_install();
  CHECK(count() == 2);                                    // the files were not touched meanwhile
}

// ---- reads are served from RAM ----------------------------------------------------------------

static int sReads = 0;
static bool countingRead(const char *path, size_t offset, uint8_t *out, size_t len) {
  sReads++;
  return sMemory->read(path, offset, out, len);
}
static long countingSize(const char *path) {
  sReads++;
  return sMemory->size(path);
}

static void test_reads_are_cached() {
  fresh();
  CHECK(save("pay", 1));
  CHECK(save("game", 2));
  hostReboot();

  sMemory = vk::fileio::ops;
  vk::fileio::Ops counting = *sMemory;
  counting.read = countingRead;
  counting.size = countingSize;
  vk::fileio::ops = &counting;
  sReads = 0;

  CHECK(count() == 2);                                    // the first call after a boot reads the file
  CHECK(sReads > 0);
  sReads = 0;
  String id;
  uint32_t hash = 0;
  for (int frame = 0; frame < 50; ++frame) {              // the Wallet app's page, drawn every frame
    const size_t entries = count();
    for (size_t i = 0; i < entries; ++i) CHECK(at(i, id, hash));
    CHECK(has("pay", 1) && !has("pay", 2) && !has("nobody", 1));
  }
  CHECK(sReads == 0);

  CHECK(save("needsign", 3));                             // a save keeps the copy in step
  sReads = 0;
  CHECK(count() == 3 && has("needsign", 3) && at(2, id, hash) && id == "needsign");
  CHECK(sReads == 0);

  eraseAll();                                             // and so does erasing
  CHECK(count() == 0 && !has("pay", 1));
  vk_host_fileio_install();

  // No file at all is remembered too: an empty store is not looked for again on every launch.
  fresh();
  sMemory = vk::fileio::ops;
  vk::fileio::ops = &counting;
  CHECK(count() == 0);
  sReads = 0;
  CHECK(count() == 0 && !has("pay", 1));
  CHECK(sReads == 0);
  vk_host_fileio_install();
}

int main() {
  test_hash();
  test_empty();
  test_save_and_has();
  test_replace_same_id();
  test_oldest_replaced();
  test_erase_all();
  test_bad_file_recovery();
  test_failing_writes();
  test_reads_are_cached();

  if (fails) {
    printf("%d consent checks failed\n", fails);
    return 1;
  }
  printf("all consent tests passed\n");
  return 0;
}
