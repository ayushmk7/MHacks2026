// LINK: src/vk/features/contacts/contacts.cpp
// test_contacts - the contacts store on the in-memory file layer of the shim, and the contact swap
// driven with a fake clock, fake random bytes and real Ed25519 (two key pairs from host_ed25519).
// Spec: docs/os/wallet/stores.md ("Contacts"), docs/os/protocol/espnow.md ("CONTACT_HELLO",
// "CONTACT_CARD"), docs/os/wallet/signing.md (the domain table row "contact").
#include <Arduino.h>

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "../../src/vk/features/contacts/contacts.h"
#include "host_ed25519.h"
#include "vectors.h"
#include "vk_host_fileio.h"

using namespace vk;
using contacts::Entry;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// ---- fakes ------------------------------------------------------------------------------------
// Two badges are played by one process: badge A has the device key of vectors.h, badge B a key
// made from SEED_B. `own_key` and `own_seed` say which one the code under test is right now. The
// swap nonce inside contacts.cpp belongs to whichever badge last called hello(); card() does not
// touch it, so "B answers A's HELLO" is: become B, card(), become A again.

static const uint8_t SEED_B[32] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
                                   7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
static const uint8_t SEED_C[32] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
                                   9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
static uint8_t PUB_A[32], PUB_B[32], PUB_C[32];

static uint32_t t_ms = 0;
static uint32_t t_unix = 0;
static const uint8_t *own_key = nullptr;
static const uint8_t *own_seed = nullptr;

static uint8_t random_next = 1;
static int random_calls = 0;
static int sign_calls = 0;
static bool sign_fails = false;
static std::vector<uint8_t> last_signed;       // the bytes the code handed to signAuto("contact", ...)
static int verify_calls = 0;

struct Note { std::string title, body; };
static std::vector<Note> notes;

static uint32_t f_now_ms() { return t_ms; }
static uint32_t f_unix() { return t_unix; }
static const uint8_t *f_own_key() { return own_key; }
// Every call gives different bytes, so two nonces never repeat within a run.
static void f_random(uint8_t *out, size_t len) {
  random_calls++;
  for (size_t i = 0; i < len; i++) out[i] = (uint8_t)(random_next * 31 + i * 7 + 1);
  random_next++;
}
// What vk::wallet::signAuto does for the domain "contact": the length limits, the domain's
// validator, then prefix ‖ bytes signed with the key.
static wallet::Reason f_sign(const char *domain, const uint8_t *bytes, size_t len, uint8_t sig[64]) {
  sign_calls++;
  if (strcmp(domain, "contact") != 0) return VK_UNSUPPORTED;
  if (len == 0 || len > VK_CARD_SIGNED_MAX) return VK_TOO_LONG;
  if (!contacts::validCardBytes(bytes, len, own_key)) return VK_BAD_ARG;
  if (sign_fails || own_seed == nullptr) return VK_SIGN_FAILED;
  last_signed.assign(bytes, bytes + len);
  std::string message = VK_PREFIX_CONTACT;
  message.append((const char *)bytes, len);
  host_ed25519_sign(own_seed, (const uint8_t *)message.data(), message.size(), sig);
  return VK_OK;
}
static int f_verify(const uint8_t *msg, size_t len, const uint8_t *sig, const uint8_t *pubkey) {
  verify_calls++;
  return host_ed25519_verify(msg, len, sig, pubkey);
}
static void f_notify(const char *title, const char *body) { notes.push_back(Note{title, body}); }

static void become(const uint8_t *seed, const uint8_t *pub) {
  own_seed = seed;
  own_key = pub;
}
static void becomeA() { become(V_DEVICE_SEED, PUB_A); }
static void becomeB() { become(SEED_B, PUB_B); }

// A clean laptop "badge": no files, no nonce, badge A, the clock at a known time.
static void fresh() {
  vk_host_fileio_install();
  vk_host_fileio_reset();
  contacts::reset();
  contacts::hooks = contacts::Hooks{f_now_ms, f_unix, f_own_key, f_random, f_sign, f_verify, f_notify};
  t_ms = 100000;
  t_unix = 1790000000u;
  random_next = 1;
  random_calls = sign_calls = verify_calls = 0;
  sign_fails = false;
  last_signed.clear();
  notes.clear();
  becomeA();
}

// ---- helpers ----------------------------------------------------------------------------------

static const vk::fileio::Ops *io() { return vk::fileio::ops; }

static std::vector<uint8_t> fileBytes(const char *path) {
  std::vector<uint8_t> out;
  const long size = io()->size(path);
  if (size <= 0) return out;
  out.resize((size_t)size);
  if (!io()->read(path, 0, out.data(), out.size())) out.clear();
  return out;
}

// A key that is told apart by `n`. (Only the store sees these; no signature is made with them.)
static void numberedKey(unsigned n, uint8_t out[32]) {
  for (size_t i = 0; i < 32; i++) out[i] = (uint8_t)(n * 13 + i + 1);
  out[0] = (uint8_t)(n & 0xFF);
  out[1] = (uint8_t)(n >> 8);
}

static bool hasContact(const uint8_t pubkey[32], Entry *found = nullptr) {
  const size_t n = contacts::count();
  for (size_t i = 0; i < n; i++) {
    Entry e;
    if (contacts::at(i, e) && memcmp(e.pubkey, pubkey, 32) == 0) {
      if (found) *found = e;
      return true;
    }
  }
  return false;
}

// A's HELLO, made now (draws A's nonce if there is none).
static std::vector<uint8_t> helloOfA(const char *name = "Alice") {
  becomeA();
  uint8_t frame[VK_HELLO_MAX_LEN];
  size_t len = 0;
  CHECK(contacts::hello(name, frame, len) == VK_OK);
  return std::vector<uint8_t>(frame, frame + len);
}

// The card B makes in answer to a HELLO. Leaves the process as badge A again.
static std::vector<uint8_t> cardFromB(const std::vector<uint8_t> &hello, const char *name = "Bob") {
  becomeB();
  uint8_t frame[VK_CARD_MAX_LEN];
  size_t len = 0;
  CHECK(contacts::card(hello.data(), hello.size(), name, frame, len) == VK_OK);
  becomeA();
  return std::vector<uint8_t>(frame, frame + len);
}

// A card built and signed by hand with `seed`, for any fields: what another firmware could send.
static std::vector<uint8_t> handCard(const uint8_t *seed, const uint8_t owner[32], const uint8_t peer[32],
                                     const uint8_t nonce[16], const char *name) {
  vk_card_t c;
  memset(&c, 0, sizeof c);
  memcpy(c.peer_pubkey, peer, 32);
  memcpy(c.peer_nonce, nonce, 16);
  memcpy(c.pubkey, owner, 32);
  c.name_len = (uint8_t)strlen(name);
  memcpy(c.name, name, c.name_len);
  uint8_t message[8 + VK_CARD_SIGNED_MAX];
  memcpy(message, VK_PREFIX_CONTACT, 8);
  const size_t n = vk_card_signed_bytes(&c, message + 8);
  CHECK(n == 81u + c.name_len);
  host_ed25519_sign(seed, message, 8 + n, c.sig);
  uint8_t frame[VK_CARD_MAX_LEN];
  const size_t total = vk_card_build(&c, frame, sizeof frame);
  CHECK(total == 85u + c.name_len + 64u);
  return std::vector<uint8_t>(frame, frame + total);
}

static wallet::Reason acceptCard(const std::vector<uint8_t> &card, Entry *saved = nullptr) {
  Entry e;
  memset(&e, 0, sizeof e);
  const wallet::Reason r = contacts::accept(card.data(), card.size(), e);
  if (saved) *saved = e;
  return r;
}

// ---- the store ----------------------------------------------------------------------------------

static void test_store_empty() {
  fresh();
  Entry e;
  CHECK(contacts::count() == 0);
  CHECK(!contacts::at(0, e));
  CHECK(!contacts::remove(PUB_B));
  CHECK(!io()->exists(contacts::FILE_PATH));       // reading creates nothing
  contacts::Cursor cursor;
  CHECK(!contacts::open(cursor));
}

static void test_store_round_trip_and_layout() {
  fresh();
  CHECK(contacts::upsert(PUB_B, "Bob"));
  t_unix += 10;
  CHECK(contacts::upsert(PUB_C, "Carol the 2nd"));
  CHECK(contacts::count() == 2);

  Entry e;
  CHECK(contacts::at(0, e));                       // the order they were added: oldest first
  CHECK(memcmp(e.pubkey, PUB_B, 32) == 0 && strcmp(e.name, "Bob") == 0 && e.added == 1790000000u);
  CHECK(contacts::at(1, e));
  CHECK(memcmp(e.pubkey, PUB_C, 32) == 0 && strcmp(e.name, "Carol the 2nd") == 0 && e.added == 1790000010u);
  CHECK(!contacts::at(2, e));

  // The bytes of stores.md: 8-byte header, then 72-byte records, little-endian.
  const std::vector<uint8_t> file = fileBytes(contacts::FILE_PATH);
  CHECK(file.size() == 8 + 2 * 72);
  if (file.size() == 8 + 2 * 72) {
    CHECK(memcmp(file.data(), "VKC1", 4) == 0);
    CHECK(file[4] == 1 && file[5] == 0);           // version u16
    CHECK(file[6] == 2 && file[7] == 0);           // count u16
    const uint8_t *r = file.data() + 8;
    CHECK(memcmp(r, PUB_B, 32) == 0);              // 0: pubkey
    uint8_t name[33] = {'B', 'o', 'b'};            // 32: name, NUL-padded
    CHECK(memcmp(r + 32, name, 33) == 0);
    CHECK(r[65] == 0);                             // 65: flags
    const uint32_t added = 1790000000u;            // 66: added u32
    CHECK(r[66] == (added & 0xFF) && r[67] == ((added >> 8) & 0xFF) && r[68] == ((added >> 16) & 0xFF) &&
          r[69] == (added >> 24));
    CHECK(r[70] == 0 && r[71] == 0);               // 70: padding
    CHECK(memcmp(r + 72, PUB_C, 32) == 0);         // the second record follows at once
  }
  CHECK(!io()->exists(contacts::TMP_FILE_PATH));   // the .tmp was renamed over the original
  CHECK(io()->exists(contacts::DIR_PATH));

  // A cursor reads the header once for many records.
  contacts::Cursor cursor;
  CHECK(contacts::open(cursor) && cursor.count == 2);
  CHECK(contacts::at(cursor, 1, e) && strcmp(e.name, "Carol the 2nd") == 0);
  CHECK(!contacts::at(cursor, 2, e));
}

static void test_store_no_clock() {
  fresh();
  t_unix = 0;                                      // what the unixNow hook gives with no clock source
  CHECK(contacts::upsert(PUB_B, "Bob"));
  Entry e;
  CHECK(contacts::at(0, e) && e.added == 0);
  contacts::hooks.unixNow = nullptr;               // a null hook is the same
  CHECK(contacts::upsert(PUB_C, "Carol"));
  CHECK(contacts::at(1, e) && e.added == 0);
}

static void test_store_upsert_updates_name() {
  fresh();
  CHECK(contacts::upsert(PUB_B, "Bob"));
  CHECK(contacts::upsert(PUB_C, "Carol"));
  t_unix += 500;
  CHECK(contacts::upsert(PUB_B, "Robert"));        // the same key again: only the name changes
  CHECK(contacts::count() == 2);
  Entry e;
  CHECK(contacts::at(0, e));
  CHECK(memcmp(e.pubkey, PUB_B, 32) == 0 && strcmp(e.name, "Robert") == 0 && e.added == 1790000000u);
  CHECK(contacts::at(1, e) && strcmp(e.name, "Carol") == 0);

  // A shorter name leaves nothing of the longer one behind.
  CHECK(contacts::upsert(PUB_B, "Al"));
  const std::vector<uint8_t> file = fileBytes(contacts::FILE_PATH);
  uint8_t name[33] = {'A', 'l'};
  CHECK(file.size() == 8 + 2 * 72 && memcmp(file.data() + 8 + 32, name, 33) == 0);

  // The same name again changes nothing, and so writes nothing.
  vk_host_fileio_fail_writes = -1;
  CHECK(contacts::upsert(PUB_B, "Al"));
  CHECK(!contacts::upsert(PUB_B, "Alfred"));
  vk_host_fileio_fail_writes = 0;
  CHECK(contacts::at(0, e) && strcmp(e.name, "Al") == 0);

  // A name longer than the field is cut to 32 characters.
  CHECK(contacts::upsert(PUB_C, "0123456789012345678901234567890123456789"));
  CHECK(contacts::at(1, e) && strcmp(e.name, "01234567890123456789012345678901") == 0);

  CHECK(!contacts::upsert(nullptr, "x"));
  CHECK(!contacts::upsert(PUB_B, nullptr));
  CHECK(contacts::count() == 2);
}

static void test_store_remove() {
  fresh();
  uint8_t k[3][32];
  for (unsigned i = 0; i < 3; i++) {
    numberedKey(i, k[i]);
    char name[8];
    snprintf(name, sizeof name, "n%u", i);
    CHECK(contacts::upsert(k[i], name));
  }
  CHECK(contacts::remove(k[1]));                   // the middle one: the rest keep their order
  CHECK(contacts::count() == 2);
  Entry e;
  CHECK(contacts::at(0, e) && strcmp(e.name, "n0") == 0);
  CHECK(contacts::at(1, e) && strcmp(e.name, "n2") == 0);
  CHECK(fileBytes(contacts::FILE_PATH).size() == 8 + 2 * 72);
  CHECK(!contacts::remove(k[1]));                  // not there any more
  CHECK(!contacts::remove(nullptr));
  CHECK(contacts::remove(k[2]));                   // the last
  CHECK(contacts::remove(k[0]));                   // the only one left
  CHECK(contacts::count() == 0);
  CHECK(fileBytes(contacts::FILE_PATH).size() == 8);   // an empty, valid file
  CHECK(contacts::upsert(k[1], "back"));
  CHECK(contacts::count() == 1 && hasContact(k[1]));
}

static void test_store_oldest_replaced_at_65() {
  fresh();
  uint8_t key[32];
  for (unsigned i = 0; i < 64; i++) {
    numberedKey(i, key);
    char name[8];
    snprintf(name, sizeof name, "n%u", i);
    t_unix++;
    CHECK(contacts::upsert(key, name));
  }
  CHECK(contacts::count() == 64);
  numberedKey(0, key);
  CHECK(hasContact(key));

  // Updating a name in a full file replaces nobody.
  numberedKey(5, key);
  CHECK(contacts::upsert(key, "five"));
  CHECK(contacts::count() == 64);
  numberedKey(0, key);
  CHECK(hasContact(key));

  // The 65th contact: the oldest (n0) goes, the others move up, the new one is last.
  numberedKey(64, key);
  t_unix++;
  CHECK(contacts::upsert(key, "n64"));
  CHECK(contacts::count() == 64);
  CHECK(fileBytes(contacts::FILE_PATH).size() == 8 + 64 * 72);
  numberedKey(0, key);
  CHECK(!hasContact(key));
  Entry e;
  CHECK(contacts::at(0, e) && strcmp(e.name, "n1") == 0);
  CHECK(contacts::at(4, e) && strcmp(e.name, "five") == 0);
  CHECK(contacts::at(63, e) && strcmp(e.name, "n64") == 0 && e.added == 1790000065u);
  CHECK(!contacts::at(64, e));

  // And again: now n1 is the oldest.
  numberedKey(65, key);
  CHECK(contacts::upsert(key, "n65"));
  CHECK(contacts::count() == 64);
  CHECK(contacts::at(0, e) && strcmp(e.name, "n2") == 0);
  CHECK(contacts::at(63, e) && strcmp(e.name, "n65") == 0);
}

static void test_store_bad_file_recovery() {
  // A wrong magic: readers see no contacts and change nothing; the next write puts the file aside.
  fresh();
  const uint8_t junk[20] = {'N', 'O', 'P', 'E', 1, 0, 1, 0, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
  CHECK(io()->makeDir(contacts::DIR_PATH));
  CHECK(io()->writeAll(contacts::FILE_PATH, junk, sizeof junk));
  Entry e;
  CHECK(contacts::count() == 0);
  CHECK(!contacts::at(0, e));
  CHECK(!contacts::remove(PUB_B));
  CHECK(fileBytes(contacts::FILE_PATH) == std::vector<uint8_t>(junk, junk + sizeof junk));
  CHECK(!io()->exists(contacts::BAD_FILE_PATH));

  CHECK(contacts::upsert(PUB_B, "Bob"));
  CHECK(fileBytes(contacts::BAD_FILE_PATH) == std::vector<uint8_t>(junk, junk + sizeof junk));
  CHECK(contacts::count() == 1 && hasContact(PUB_B));
  CHECK(fileBytes(contacts::FILE_PATH).size() == 8 + 72);

  // A wrong version, with an older .bad already there: the newer bad file replaces it.
  uint8_t wrongVersion[8] = {'V', 'K', 'C', '1', 2, 0, 0, 0};
  CHECK(io()->writeAll(contacts::FILE_PATH, wrongVersion, sizeof wrongVersion));
  CHECK(contacts::count() == 0);
  CHECK(contacts::upsert(PUB_C, "Carol"));
  CHECK(fileBytes(contacts::BAD_FILE_PATH) == std::vector<uint8_t>(wrongVersion, wrongVersion + 8));
  CHECK(contacts::count() == 1 && hasContact(PUB_C) && !hasContact(PUB_B));

  // A header that claims more records than the file holds, or more than the format allows.
  uint8_t shortFile[8 + 72] = {'V', 'K', 'C', '1', 1, 0, 2, 0};
  CHECK(io()->writeAll(contacts::FILE_PATH, shortFile, sizeof shortFile));
  CHECK(contacts::count() == 0);
  uint8_t tooMany[8] = {'V', 'K', 'C', '1', 1, 0, 65, 0};
  CHECK(io()->writeAll(contacts::FILE_PATH, tooMany, sizeof tooMany));
  CHECK(contacts::count() == 0);
  // A file shorter than a header.
  CHECK(io()->writeAll(contacts::FILE_PATH, tooMany, 3));
  CHECK(contacts::count() == 0);
  CHECK(contacts::upsert(PUB_B, "Bob"));
  CHECK(contacts::count() == 1 && hasContact(PUB_B));
}

// The memory file layer with a rename that always fails (the shim's counter cannot fail the second
// write of a call and let the first through).
static const vk::fileio::Ops *real_ops = nullptr;
static int renames_refused = 0;
static bool refuseRename(const char *, const char *) { renames_refused++; return false; }

static void test_store_failing_writes() {
  fresh();
  CHECK(contacts::upsert(PUB_B, "Bob"));
  const std::vector<uint8_t> before = fileBytes(contacts::FILE_PATH);

  // The .tmp cannot be written: false, and the file is as it was.
  vk_host_fileio_fail_writes = 1;
  CHECK(!contacts::upsert(PUB_C, "Carol"));
  CHECK(vk_host_fileio_fail_writes == 0);
  CHECK(fileBytes(contacts::FILE_PATH) == before);
  CHECK(!io()->exists(contacts::TMP_FILE_PATH));
  vk_host_fileio_fail_writes = 1;
  CHECK(!contacts::remove(PUB_B));
  CHECK(fileBytes(contacts::FILE_PATH) == before);
  vk_host_fileio_fail_writes = 1;
  CHECK(!contacts::upsert(PUB_B, "Robert"));
  CHECK(fileBytes(contacts::FILE_PATH) == before);
  vk_host_fileio_fail_writes = 0;

  // The .tmp is written but cannot be renamed over the original: false, the file is as it was,
  // and no .tmp is left behind.
  real_ops = vk::fileio::ops;
  vk::fileio::Ops broken = *real_ops;
  broken.renameFile = refuseRename;
  vk::fileio::ops = &broken;
  renames_refused = 0;
  CHECK(!contacts::upsert(PUB_C, "Carol"));
  CHECK(!contacts::remove(PUB_B));
  CHECK(renames_refused == 2);
  vk_host_fileio_install();
  CHECK(fileBytes(contacts::FILE_PATH) == before);
  CHECK(!io()->exists(contacts::TMP_FILE_PATH));

  // With the file layer working again, both succeed.
  CHECK(contacts::upsert(PUB_C, "Carol"));
  CHECK(contacts::remove(PUB_B));
  CHECK(contacts::count() == 1 && hasContact(PUB_C));

  // No /vk/ and no way to make the file: false, nothing appears.
  fresh();
  vk_host_fileio_fail_writes = -1;
  CHECK(!contacts::upsert(PUB_B, "Bob"));
  vk_host_fileio_fail_writes = 0;
  CHECK(contacts::count() == 0 && !io()->exists(contacts::FILE_PATH));

  // No file layer at all.
  vk::fileio::ops = nullptr;
  Entry e;
  CHECK(contacts::count() == 0 && !contacts::at(0, e));
  CHECK(!contacts::upsert(PUB_B, "Bob") && !contacts::remove(PUB_B));
  vk_host_fileio_install();
}

// ---- names --------------------------------------------------------------------------------------

static void test_clean_name() {
  char out[VK_NAME_MAX + 1];
  CHECK(contacts::cleanName("Alice", out) == 5 && strcmp(out, "Alice") == 0);
  CHECK(contacts::cleanName("", out) == 0 && out[0] == '\0');
  CHECK(contacts::cleanName(nullptr, out) == 0 && out[0] == '\0');
  CHECK(contacts::cleanName("caf\xC3\xA9\tbar", out) == 9 && strcmp(out, "caf???bar") == 0);
  CHECK(contacts::cleanName("0123456789012345678901234567890123456789", out) == 32 &&
        strcmp(out, "01234567890123456789012345678901") == 0);
  CHECK(contacts::cleanName(" ~", out) == 2 && strcmp(out, " ~") == 0);   // both ends of printable ASCII
}

// ---- HELLO --------------------------------------------------------------------------------------

static void test_hello() {
  fresh();
  uint8_t frame[VK_HELLO_MAX_LEN];
  size_t len = 0;
  CHECK(contacts::hello("Alice", frame, len) == VK_OK);
  CHECK(len == 53 + 5);
  vk_hello_t h;
  CHECK(vk_hello_parse(frame, len, &h) == 0);
  CHECK(memcmp(h.pubkey, PUB_A, 32) == 0);
  CHECK(h.name_len == 5 && strcmp(h.name, "Alice") == 0);
  CHECK(random_calls == 1);

  // The same frame is returned until the nonce is 60 s old.
  const std::vector<uint8_t> first(frame, frame + len);
  t_ms += 59999;
  CHECK(contacts::hello("Alice", frame, len) == VK_OK);
  CHECK(std::vector<uint8_t>(frame, frame + len) == first);
  CHECK(random_calls == 1);

  // At 60 s a new nonce is drawn, and it lives 60 s from then.
  t_ms += 1;
  CHECK(contacts::hello("Alice", frame, len) == VK_OK);
  vk_hello_t h2;
  CHECK(vk_hello_parse(frame, len, &h2) == 0);
  CHECK(memcmp(h2.nonce, h.nonce, 16) != 0);
  CHECK(random_calls == 2);
  const std::vector<uint8_t> second(frame, frame + len);
  t_ms += 59999;
  CHECK(contacts::hello("Alice", frame, len) == VK_OK);
  CHECK(std::vector<uint8_t>(frame, frame + len) == second);

  // The clock of the badge wraps: the age is still right.
  fresh();
  t_ms = 0xFFFFFF00u;
  CHECK(contacts::hello("Alice", frame, len) == VK_OK);
  const std::vector<uint8_t> beforeWrap(frame, frame + len);
  t_ms += 0x200;                                   // wrapped past zero, 512 ms later
  CHECK(contacts::hello("Alice", frame, len) == VK_OK);
  CHECK(std::vector<uint8_t>(frame, frame + len) == beforeWrap);
  t_ms += 60000;
  CHECK(contacts::hello("Alice", frame, len) == VK_OK);
  CHECK(std::vector<uint8_t>(frame, frame + len) != beforeWrap);

  // Refusals: a bad name draws no nonce; no key; the longest name fits.
  fresh();
  len = 99;
  CHECK(contacts::hello("", frame, len) == VK_BAD_ARG && len == 0);
  CHECK(contacts::hello(nullptr, frame, len) == VK_BAD_ARG);
  CHECK(contacts::hello("012345678901234567890123456789012", frame, len) == VK_BAD_ARG);   // 33 characters
  CHECK(contacts::hello("tab\there", frame, len) == VK_BAD_ARG);
  CHECK(random_calls == 0);
  CHECK(contacts::hello("01234567890123456789012345678901", frame, len) == VK_OK && len == VK_HELLO_MAX_LEN);
  own_key = nullptr;
  CHECK(contacts::hello("Alice", frame, len) == VK_SIGN_FAILED && len == 0);
  becomeA();
  contacts::hooks.randomBytes = nullptr;
  CHECK(contacts::hello("Alice", frame, len) == VK_OK);          // the nonce drawn above is still current
  contacts::reset();
  CHECK(contacts::hello("Alice", frame, len) == VK_SIGN_FAILED); // no nonce and no way to draw one
}

// ---- CARD ---------------------------------------------------------------------------------------

static void test_card_signed_bytes() {
  fresh();
  const std::vector<uint8_t> hello = helloOfA();
  vk_hello_t h;
  CHECK(vk_hello_parse(hello.data(), hello.size(), &h) == 0);

  const std::vector<uint8_t> card = cardFromB(hello, "Bob");
  CHECK(card.size() == 85 + 3 + 64);
  CHECK(sign_calls == 1);
  vk_card_t c;
  CHECK(vk_card_parse(card.data(), card.size(), &c) == 0);
  CHECK(memcmp(c.peer_pubkey, PUB_A, 32) == 0);    // made for the badge that sent the HELLO
  CHECK(memcmp(c.peer_nonce, h.nonce, 16) == 0);   // and for that swap
  CHECK(memcmp(c.pubkey, PUB_B, 32) == 0);         // the card's owner
  CHECK(c.name_len == 3 && strcmp(c.name, "Bob") == 0);

  // The bytes handed to the signer are exactly vk_card_signed_bytes of the card that was sent.
  uint8_t expect[VK_CARD_SIGNED_MAX];
  const size_t n = vk_card_signed_bytes(&c, expect);
  CHECK(n == 81 + 3);
  CHECK(last_signed == std::vector<uint8_t>(expect, expect + n));
  // Laid out as the domain table says: peer_nonce ‖ peer_pubkey ‖ own_pubkey ‖ name_len ‖ name.
  CHECK(memcmp(expect, h.nonce, 16) == 0 && memcmp(expect + 16, PUB_A, 32) == 0 &&
        memcmp(expect + 48, PUB_B, 32) == 0 && expect[80] == 3 && memcmp(expect + 81, "Bob", 3) == 0);
  // And the signature in the frame is over "contact:" ‖ those bytes, by the owner's key.
  uint8_t message[8 + VK_CARD_SIGNED_MAX];
  memcpy(message, "contact:", 8);
  memcpy(message + 8, expect, n);
  CHECK(host_ed25519_verify(message, 8 + n, c.sig, PUB_B) == 1);
  CHECK(host_ed25519_verify(expect, n, c.sig, PUB_B) != 1);      // not valid without the prefix

  // The longest name: 113 signed bytes, a 181-byte frame.
  const std::vector<uint8_t> longest = cardFromB(hello, "01234567890123456789012345678901");
  CHECK(longest.size() == VK_CARD_MAX_LEN && last_signed.size() == VK_CARD_SIGNED_MAX);
}

static void test_card_refusals() {
  fresh();
  const std::vector<uint8_t> hello = helloOfA();
  uint8_t frame[VK_CARD_MAX_LEN];
  size_t len = 99;
  becomeB();

  // Not a HELLO frame: nothing is signed.
  CHECK(contacts::card(nullptr, 0, "Bob", frame, len) == VK_BAD_ARG && len == 0);
  CHECK(contacts::card(hello.data(), hello.size() - 1, "Bob", frame, len) == VK_BAD_ARG);
  std::vector<uint8_t> wrongType = hello;
  wrongType[3] = VK_T_CONTACT_CARD;
  CHECK(contacts::card(wrongType.data(), wrongType.size(), "Bob", frame, len) == VK_BAD_ARG);
  const std::vector<uint8_t> aCard = cardFromB(hello);           // a CARD is not a HELLO
  becomeB();
  const int signedSoFar = sign_calls;
  CHECK(contacts::card(aCard.data(), aCard.size(), "Bob", frame, len) == VK_BAD_ARG);
  // A bad name of our own.
  CHECK(contacts::card(hello.data(), hello.size(), "", frame, len) == VK_BAD_ARG);
  CHECK(contacts::card(hello.data(), hello.size(), nullptr, frame, len) == VK_BAD_ARG);
  CHECK(contacts::card(hello.data(), hello.size(), "new\nline", frame, len) == VK_BAD_ARG);
  CHECK(sign_calls == signedSoFar);

  // The signer refuses, or there is no key or no signer.
  sign_fails = true;
  CHECK(contacts::card(hello.data(), hello.size(), "Bob", frame, len) == VK_SIGN_FAILED && len == 0);
  sign_fails = false;
  own_key = nullptr;
  CHECK(contacts::card(hello.data(), hello.size(), "Bob", frame, len) == VK_SIGN_FAILED);
  becomeB();
  contacts::hooks.signAuto = nullptr;
  CHECK(contacts::card(hello.data(), hello.size(), "Bob", frame, len) == VK_SIGN_FAILED);
}

// The validator of the signing domain "contact" (domain_contact.cpp passes this badge's key).
static void test_domain_validator() {
  fresh();
  const std::vector<uint8_t> hello = helloOfA();
  cardFromB(hello, "Bob");
  const std::vector<uint8_t> body = last_signed;   // 84 bytes, own_pubkey = B
  CHECK(body.size() == 84);

  CHECK(contacts::validCardBytes(body.data(), body.size(), PUB_B));
  CHECK(!contacts::validCardBytes(body.data(), body.size(), PUB_A));     // own_pubkey is not this badge's key
  CHECK(!contacts::validCardBytes(body.data(), body.size(), nullptr));   // no identity
  CHECK(!contacts::validCardBytes(nullptr, body.size(), PUB_B));
  CHECK(!contacts::validCardBytes(body.data(), body.size() - 1, PUB_B)); // shorter than name_len says
  std::vector<uint8_t> longer = body;
  longer.push_back('x');
  CHECK(!contacts::validCardBytes(longer.data(), longer.size(), PUB_B)); // longer than name_len says
  CHECK(!contacts::validCardBytes(body.data(), 81, PUB_B));              // no name at all
  CHECK(!contacts::validCardBytes(body.data(), 0, PUB_B));

  std::vector<uint8_t> zeroName = body;
  zeroName[80] = 0;
  CHECK(!contacts::validCardBytes(zeroName.data(), 81, PUB_B));
  std::vector<uint8_t> control = body;
  control[82] = 0x07;
  CHECK(!contacts::validCardBytes(control.data(), control.size(), PUB_B));   // not printable ASCII
  std::vector<uint8_t> otherOwner = body;
  otherOwner[48] ^= 1;
  CHECK(!contacts::validCardBytes(otherOwner.data(), otherOwner.size(), PUB_B));

  // The longest body, and one byte more.
  std::vector<uint8_t> longest(body.begin(), body.begin() + 80);
  longest.push_back(32);
  longest.insert(longest.end(), 32, 'n');
  CHECK(longest.size() == VK_CARD_SIGNED_MAX);
  CHECK(contacts::validCardBytes(longest.data(), longest.size(), PUB_B));
  std::vector<uint8_t> over(body.begin(), body.begin() + 80);
  over.push_back(33);
  over.insert(over.end(), 33, 'n');
  CHECK(!contacts::validCardBytes(over.data(), over.size(), PUB_B));

  // Bytes of other domains never pass: a text domain's bytes are too short or do not fit the layout.
  const char text[] = "solana-badge-register:abc:123";
  CHECK(!contacts::validCardBytes((const uint8_t *)text, sizeof text - 1, PUB_B));
}

// ---- accept -------------------------------------------------------------------------------------

static void test_accept_ok() {
  fresh();
  const std::vector<uint8_t> hello = helloOfA();
  const std::vector<uint8_t> card = cardFromB(hello, "Bob");
  t_ms += 5000;
  t_unix += 5;

  Entry saved;
  CHECK(acceptCard(card, &saved) == VK_OK);
  CHECK(verify_calls == 1);
  CHECK(memcmp(saved.pubkey, PUB_B, 32) == 0 && strcmp(saved.name, "Bob") == 0 && saved.added == 1790000005u);

  // Saved, and announced in the inbox.
  Entry stored;
  CHECK(contacts::count() == 1 && hasContact(PUB_B, &stored));
  CHECK(strcmp(stored.name, "Bob") == 0 && stored.added == 1790000005u);
  CHECK(notes.size() == 1);
  if (notes.size() == 1) CHECK(notes[0].title == "Contact saved" && notes[0].body == "Saved Bob");

  // The nonce was rotated: the same card again is refused, without another verification, and
  // nothing more is saved or announced.
  CHECK(acceptCard(card) == VK_EXPIRED);
  CHECK(verify_calls == 1 && contacts::count() == 1 && notes.size() == 1);

  // The next HELLO carries a new nonce, and the old card stays refused against it.
  const std::vector<uint8_t> hello2 = helloOfA();
  CHECK(hello2 != hello);
  vk_hello_t h1, h2;
  CHECK(vk_hello_parse(hello.data(), hello.size(), &h1) == 0 && vk_hello_parse(hello2.data(), hello2.size(), &h2) == 0);
  CHECK(memcmp(h1.nonce, h2.nonce, 16) != 0);
  CHECK(acceptCard(card) == VK_EXPIRED);

  // A second swap with the same badge, under another name: the contact's name is updated.
  const std::vector<uint8_t> card2 = cardFromB(hello2, "Robert");
  t_unix += 100;
  CHECK(acceptCard(card2, &saved) == VK_OK);
  CHECK(strcmp(saved.name, "Robert") == 0 && saved.added == 1790000005u);   // `added` is the first time
  CHECK(contacts::count() == 1 && hasContact(PUB_B, &stored) && strcmp(stored.name, "Robert") == 0);
  CHECK(notes.size() == 2);
  if (notes.size() == 2) CHECK(notes[1].body == "Saved Robert");

  // The longest name fits the note's 39 characters.
  const std::vector<uint8_t> hello3 = helloOfA();
  const std::vector<uint8_t> card3 = cardFromB(hello3, "01234567890123456789012345678901");
  CHECK(acceptCard(card3) == VK_OK);
  CHECK(notes.size() == 3);
  if (notes.size() == 3) CHECK(notes[2].body == "Saved 01234567890123456789012345678901" && notes[2].body.size() <= 39);

  // No inbox: the contact is still saved.
  contacts::hooks.notify = nullptr;
  const std::vector<uint8_t> hello4 = helloOfA();
  CHECK(acceptCard(cardFromB(hello4, "Bob")) == VK_OK);
  CHECK(hasContact(PUB_B, &stored) && strcmp(stored.name, "Bob") == 0);
}

static void test_accept_not_a_card() {
  fresh();
  const std::vector<uint8_t> hello = helloOfA();
  const std::vector<uint8_t> card = cardFromB(hello);
  Entry e;
  CHECK(contacts::accept(nullptr, 0, e) == VK_BAD_ARG);
  CHECK(acceptCard(hello) == VK_BAD_ARG);                                  // a HELLO is not a CARD
  CHECK(acceptCard(std::vector<uint8_t>(card.begin(), card.end() - 1)) == VK_BAD_ARG);   // truncated
  std::vector<uint8_t> longer = card;
  longer.push_back(0);
  CHECK(acceptCard(longer) == VK_BAD_ARG);
  std::vector<uint8_t> badName = card;
  badName[85] = 0x01;                                                      // a control character in the name
  CHECK(acceptCard(badName) == VK_BAD_ARG);
  CHECK(verify_calls == 0 && contacts::count() == 0 && notes.empty());
  CHECK(acceptCard(card) == VK_OK);                                        // none of that used up the nonce
}

static void test_accept_wrong_nonce() {
  fresh();
  const std::vector<uint8_t> hello = helloOfA();
  vk_hello_t h;
  CHECK(vk_hello_parse(hello.data(), hello.size(), &h) == 0);

  // A card B signed properly, for A, but for another nonce (a swap that is not the current one).
  uint8_t other[16];
  memcpy(other, h.nonce, 16);
  other[15] ^= 0x01;
  CHECK(acceptCard(handCard(SEED_B, PUB_B, PUB_A, other, "Bob")) == VK_EXPIRED);
  CHECK(verify_calls == 0);                        // refused before the signature is looked at
  CHECK(contacts::count() == 0 && notes.empty());

  // Changing the nonce inside a good card does not help either.
  std::vector<uint8_t> card = cardFromB(hello);
  std::vector<uint8_t> edited = card;
  edited[36] ^= 0x80;
  CHECK(acceptCard(edited) == VK_EXPIRED);
  CHECK(acceptCard(card) == VK_OK);                // a refusal does not use up the nonce

  // Before any HELLO was made there is no nonce: every card is expired.
  fresh();
  const uint8_t zeros[16] = {0};
  CHECK(acceptCard(handCard(SEED_B, PUB_B, PUB_A, zeros, "Bob")) == VK_EXPIRED);
  CHECK(verify_calls == 0 && contacts::count() == 0);
}

static void test_accept_expired_after_60s() {
  fresh();
  const std::vector<uint8_t> hello = helloOfA();
  const std::vector<uint8_t> card = cardFromB(hello);

  t_ms += 60000;                                   // the nonce is 60 s old
  CHECK(acceptCard(card) == VK_EXPIRED);
  CHECK(verify_calls == 0 && contacts::count() == 0 && notes.empty());
  t_ms += 600000;
  CHECK(acceptCard(card) == VK_EXPIRED);

  // One millisecond inside the lifetime it is still good.
  fresh();
  const std::vector<uint8_t> hello2 = helloOfA();
  const std::vector<uint8_t> card2 = cardFromB(hello2);
  t_ms += 59999;
  CHECK(acceptCard(card2) == VK_OK);
  CHECK(contacts::count() == 1);
}

static void test_accept_wrong_addressee() {
  fresh();
  const std::vector<uint8_t> hello = helloOfA();
  vk_hello_t h;
  CHECK(vk_hello_parse(hello.data(), hello.size(), &h) == 0);

  // A card B made for C (with A's nonce, and a valid signature): not for this badge.
  CHECK(acceptCard(handCard(SEED_B, PUB_B, PUB_C, h.nonce, "Bob")) == VK_MISMATCH);
  CHECK(verify_calls == 0 && contacts::count() == 0 && notes.empty());

  // The card B made for A, shown to C: C refuses it. (C has its own nonce.)
  const std::vector<uint8_t> card = cardFromB(hello);
  become(SEED_C, PUB_C);
  uint8_t frame[VK_HELLO_MAX_LEN];
  size_t len = 0;
  contacts::reset();
  CHECK(contacts::hello("Carol", frame, len) == VK_OK);
  CHECK(acceptCard(card) == VK_MISMATCH);
  CHECK(contacts::count() == 0);

  // A badge with no key is nobody's addressee.
  own_key = nullptr;
  CHECK(acceptCard(card) == VK_MISMATCH);
}

static void test_accept_bad_signature() {
  fresh();
  const std::vector<uint8_t> hello = helloOfA();
  vk_hello_t h;
  CHECK(vk_hello_parse(hello.data(), hello.size(), &h) == 0);
  const std::vector<uint8_t> card = cardFromB(hello, "Bob");
  const size_t sigAt = 85 + 3;

  // One flipped bit of the signature, at either end of it.
  std::vector<uint8_t> flipped = card;
  flipped[sigAt] ^= 0x01;
  CHECK(acceptCard(flipped) == VK_BAD_PROOF);
  flipped = card;
  flipped[sigAt + 63] ^= 0x80;
  CHECK(acceptCard(flipped) == VK_BAD_PROOF);
  CHECK(verify_calls == 2);

  // A signed field changed after signing: the name, or the owner's key.
  std::vector<uint8_t> renamed = card;
  renamed[85] = 'R';
  CHECK(acceptCard(renamed) == VK_BAD_PROOF);
  std::vector<uint8_t> otherOwner = card;
  memcpy(otherOwner.data() + 52, PUB_C, 32);       // C claims B's card as its own
  CHECK(acceptCard(otherOwner) == VK_BAD_PROOF);

  // Signed by a key that is not the card's `pubkey`.
  CHECK(acceptCard(handCard(SEED_C, PUB_B, PUB_A, h.nonce, "Bob")) == VK_BAD_PROOF);

  // Signed over the right bytes but without the domain prefix (as a signature of another domain
  // would be).
  {
    vk_card_t c;
    CHECK(vk_card_parse(card.data(), card.size(), &c) == 0);
    uint8_t bytes[VK_CARD_SIGNED_MAX];
    const size_t n = vk_card_signed_bytes(&c, bytes);
    host_ed25519_sign(SEED_B, bytes, n, c.sig);
    uint8_t frame[VK_CARD_MAX_LEN];
    const size_t total = vk_card_build(&c, frame, sizeof frame);
    CHECK(acceptCard(std::vector<uint8_t>(frame, frame + total)) == VK_BAD_PROOF);
  }

  // No verifier: nothing can be accepted.
  contacts::hooks.verify = nullptr;
  CHECK(acceptCard(card) == VK_BAD_PROOF);
  contacts::hooks.verify = f_verify;

  CHECK(contacts::count() == 0 && notes.empty());
  CHECK(acceptCard(card) == VK_OK);                // a refusal does not use up the nonce
}

// espnow.md: addressee first (mismatch), then nonce (expired), then signature (bad_proof).
static void test_accept_order() {
  fresh();
  const std::vector<uint8_t> hello = helloOfA();
  vk_hello_t h;
  CHECK(vk_hello_parse(hello.data(), hello.size(), &h) == 0);
  uint8_t other[16];
  memcpy(other, h.nonce, 16);
  other[0] ^= 0xFF;

  // Wrong addressee, wrong nonce and a bad signature at once: mismatch.
  std::vector<uint8_t> all = handCard(SEED_B, PUB_B, PUB_C, other, "Bob");
  all[all.size() - 1] ^= 1;
  CHECK(acceptCard(all) == VK_MISMATCH);
  // Wrong nonce and a bad signature: expired.
  std::vector<uint8_t> two = handCard(SEED_B, PUB_B, PUB_A, other, "Bob");
  two[two.size() - 1] ^= 1;
  CHECK(acceptCard(two) == VK_EXPIRED);
  // Only the signature is bad: bad_proof.
  std::vector<uint8_t> one = handCard(SEED_B, PUB_B, PUB_A, h.nonce, "Bob");
  one[one.size() - 1] ^= 1;
  CHECK(acceptCard(one) == VK_BAD_PROOF);
  CHECK(verify_calls == 1);                        // only the last one reached the verifier
}

// A good card that cannot be written: refused, nothing announced, and the nonce is kept so the
// same card can be tried again.
static void test_accept_store_failure() {
  fresh();
  const std::vector<uint8_t> hello = helloOfA();
  const std::vector<uint8_t> card = cardFromB(hello);
  vk_host_fileio_fail_writes = -1;
  CHECK(acceptCard(card) == VK_UNSUPPORTED);
  vk_host_fileio_fail_writes = 0;
  CHECK(contacts::count() == 0 && notes.empty());
  CHECK(helloOfA() == hello);                      // not rotated
  CHECK(acceptCard(card) == VK_OK);
  CHECK(contacts::count() == 1 && notes.size() == 1);
}

// The whole swap, both ways, as two badges do it (each side: HELLO out, CARD back, accept).
static void test_swap_both_ways() {
  fresh();
  // A's HELLO is answered by B.
  const std::vector<uint8_t> helloA = helloOfA("Alice");
  const std::vector<uint8_t> cardForA = cardFromB(helloA, "Bob");
  CHECK(acceptCard(cardForA) == VK_OK);
  CHECK(hasContact(PUB_B));

  // B's side, on B's own badge: a separate file and a separate nonce.
  vk_host_fileio_reset();
  contacts::reset();
  becomeB();
  uint8_t frame[VK_HELLO_MAX_LEN];
  size_t len = 0;
  CHECK(contacts::hello("Bob", frame, len) == VK_OK);
  const std::vector<uint8_t> helloB(frame, frame + len);
  becomeA();
  uint8_t cardFrame[VK_CARD_MAX_LEN];
  size_t cardLen = 0;
  CHECK(contacts::card(helloB.data(), helloB.size(), "Alice", cardFrame, cardLen) == VK_OK);
  becomeB();
  const std::vector<uint8_t> cardForB(cardFrame, cardFrame + cardLen);
  CHECK(acceptCard(cardForA) == VK_MISMATCH);      // A's card of B is not for B
  Entry saved;
  CHECK(acceptCard(cardForB, &saved) == VK_OK);
  CHECK(memcmp(saved.pubkey, PUB_A, 32) == 0 && strcmp(saved.name, "Alice") == 0);
  CHECK(contacts::count() == 1 && hasContact(PUB_A));
  CHECK(acceptCard(cardForB) == VK_EXPIRED);       // replayed after the swap
}

int main() {
  host_ed25519_keypair(V_DEVICE_SEED, PUB_A);
  host_ed25519_keypair(SEED_B, PUB_B);
  host_ed25519_keypair(SEED_C, PUB_C);
  CHECK(memcmp(PUB_A, V_DEVICE_PUB, 32) == 0);

  test_store_empty();
  test_store_round_trip_and_layout();
  test_store_no_clock();
  test_store_upsert_updates_name();
  test_store_remove();
  test_store_oldest_replaced_at_65();
  test_store_bad_file_recovery();
  test_store_failing_writes();
  test_clean_name();
  test_hello();
  test_card_signed_bytes();
  test_card_refusals();
  test_domain_validator();
  test_accept_ok();
  test_accept_not_a_card();
  test_accept_wrong_nonce();
  test_accept_expired_after_60s();
  test_accept_wrong_addressee();
  test_accept_bad_signature();
  test_accept_order();
  test_accept_store_failure();
  test_swap_both_ways();

  if (fails) {
    printf("%d contacts checks FAILED\n", fails);
    return 1;
  }
  printf("all contacts tests passed\n");
  return 0;
}
