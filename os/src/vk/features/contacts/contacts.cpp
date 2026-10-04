// src/vk/features/contacts/contacts.cpp
// The contacts file and the contact swap (wallet/stores.md, "Contacts"; protocol/espnow.md,
// "CONTACT_HELLO", "CONTACT_CARD").
//
// File: 8-byte header (magic "VKC1", version u16, count u16), then `count` records of 72 bytes,
// all little-endian, in the order the contacts were added. Every change rewrites the whole file:
// the new contents go to contacts.bin.tmp, which is then renamed over contacts.bin, so a write
// that fails leaves the old file as it was. A file with a wrong magic or version is renamed to
// contacts.bin.bad on the next write; until then readers report no contacts and change nothing.
//
// Host-test seam (test/host/test_contacts.cpp): every file call goes through vk::fileio::ops;
// time, random bytes, the signer, the verifier and the inbox go through `hooks`; the log is behind
// #ifndef VK_HOST_TEST.
#include "contacts.h"

#include <stdio.h>
#include <string.h>

#include "../../core/config.h"   // VK_ON_RESET
#include "../../core/fileio.h"

#ifndef VK_HOST_TEST
#include <Arduino.h>

#include "../../../badge_log.h"
#include "../../core/clock.h"
#include "../../host/notify.h"
#include "../../wallet/crypto.h"
#include "../../wallet/signer.h"
#define VK_CONTACT_LOG(...) ::badge_log::tagf("contact", __VA_ARGS__)
#else
#define VK_CONTACT_LOG(...) do { } while (0)
#endif

namespace vk::contacts {

// ---------------------------------------------------------------------------
// Firmware glue: the real hooks
// ---------------------------------------------------------------------------

#ifndef VK_HOST_TEST
namespace {

// The app a "Contact saved" note opens (apps.md, Contacts).
constexpr char NOTE_APP_ID[] = "contacts";

// accept() writes "Saved <name>" (a name is at most 32 characters) into a note's body.
static_assert(sizeof(vk::host::notify::Note::body) >= sizeof("Saved ") + VK_NAME_MAX, "notify::Note::body is too small");

uint32_t fwNowMs() { return (uint32_t)millis(); }
uint32_t fwUnixNow() { return vk::clock::ok() ? vk::clock::now() : 0; }
void fwNotify(const char *title, const char *body) { vk::host::notify::post(title, body, NOTE_APP_ID); }

}  // namespace

// Filled by constant initialisation, so the hooks are in place before the signing-domain
// validator or any Lua binding can run, whatever the order of the registries.
Hooks hooks = {
    fwNowMs,
    fwUnixNow,
    vk::wallet::publicKey,
    vk::wallet::randomBytes,
    vk::wallet::signAuto,
    vk_verify_c,
    fwNotify,
};
#else
Hooks hooks = {};
#endif

namespace {

// ---------------------------------------------------------------------------
// Hook wrappers: a null hook means "not available"
// ---------------------------------------------------------------------------

uint32_t nowMs() { return hooks.nowMs ? hooks.nowMs() : 0; }
uint32_t unixNow() { return hooks.unixNow ? hooks.unixNow() : 0; }
const uint8_t *ownKey() { return hooks.ownKey ? hooks.ownKey() : nullptr; }

// ---------------------------------------------------------------------------
// The file
// ---------------------------------------------------------------------------

const uint8_t MAGIC[4] = {'V', 'K', 'C', '1'};

// Record layout (stores.md): offset of each field.
constexpr size_t OFF_PUBKEY = 0;        // 32
constexpr size_t OFF_NAME = 32;         // 33, NUL-padded
constexpr size_t OFF_FLAGS = 65;        // 1, reserved, 0
constexpr size_t OFF_ADDED = 66;        // u32
                                        // 70: two bytes of padding
constexpr size_t NAME_SIZE = 33;

static_assert(OFF_ADDED + 4 + 2 == RECORD_SIZE, "the contact record is 72 bytes");
static_assert(sizeof(Entry::name) == NAME_SIZE, "Entry::name is the record's name field");
static_assert(NAME_SIZE == VK_NAME_MAX + 1, "a stored name holds a frame's name");

// The whole file, as it is read before a change and written after it. One buffer, not on the
// stack: there is one task, and no function here is re-entered.
uint8_t image[HEADER_SIZE + CAPACITY * RECORD_SIZE];

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

// The name field: at most 32 characters of `text`, the rest zeros.
void putName(uint8_t *at, const char *text) {
  memset(at, 0, NAME_SIZE);
  memcpy(at, text, strnlen(text, NAME_SIZE - 1));
}

size_t recordOffset(size_t index) { return HEADER_SIZE + index * RECORD_SIZE; }
uint8_t *recordAt(size_t index) { return image + recordOffset(index); }

void unpackRecord(const uint8_t in[RECORD_SIZE], Entry &entry) {
  memcpy(entry.pubkey, in + OFF_PUBKEY, sizeof entry.pubkey);
  memcpy(entry.name, in + OFF_NAME, NAME_SIZE);
  entry.name[NAME_SIZE - 1] = '\0';
  entry.added = getU32(in + OFF_ADDED);
}

enum class Probe : uint8_t { Absent, Invalid, ReadError, Valid };

// What is at FILE_PATH. Valid: a contacts file whose header is consistent with its size; `count`
// is then its number of records. ReadError: there is a file, but it could not be read just now
// (it is not judged, so a writer must not replace it).
Probe probe(uint16_t &count) {
  count = 0;
  const vk::fileio::Ops *io = vk::fileio::ops;
  const long size = io->size(FILE_PATH);
  if (size < 0) return Probe::Absent;
  if (size < (long)HEADER_SIZE) return Probe::Invalid;
  uint8_t header[HEADER_SIZE];
  if (!io->read(FILE_PATH, 0, header, sizeof header)) return Probe::ReadError;
  if (memcmp(header, MAGIC, sizeof MAGIC) != 0 || getU16(header + 4) != FORMAT_VERSION) return Probe::Invalid;
  const uint16_t stored = getU16(header + 6);
  if (stored > CAPACITY || (size_t)size < recordOffset(stored)) return Probe::Invalid;
  count = stored;
  return Probe::Valid;
}

// Reads the file into `image` for a change. True when `image` holds `count` records to build on:
// those of a valid file, or none when there is no file or only one that is not a contacts file
// (which is first put aside as contacts.bin.bad, replacing an earlier .bad). False when the file
// could not be read: nothing may be written then.
bool loadForChange(uint16_t &count) {
  const vk::fileio::Ops *io = vk::fileio::ops;
  switch (probe(count)) {
    case Probe::Valid:
      return io->read(FILE_PATH, 0, image, recordOffset(count));
    case Probe::ReadError:
      return false;
    case Probe::Invalid:
      if (io->exists(BAD_FILE_PATH)) io->removeFile(BAD_FILE_PATH);
      if (io->renameFile(FILE_PATH, BAD_FILE_PATH)) {
        VK_CONTACT_LOG("bad contacts file renamed to %s", BAD_FILE_PATH);
      } else {
        VK_CONTACT_LOG("bad contacts file could not be renamed; overwriting it");
      }
      return true;
    case Probe::Absent:
      return true;
  }
  return false;
}

// Writes `image` with `count` records: to the .tmp file, then renamed over the original.
bool commit(uint16_t count) {
  const vk::fileio::Ops *io = vk::fileio::ops;
  memcpy(image, MAGIC, sizeof MAGIC);
  putU16(image + 4, FORMAT_VERSION);
  putU16(image + 6, count);
  io->makeDir(DIR_PATH);                 // succeeds if it is already there; if it fails, so does writeAll
  if (!io->writeAll(TMP_FILE_PATH, image, recordOffset(count))) {
    io->removeFile(TMP_FILE_PATH);       // a partly written .tmp, if any
    VK_CONTACT_LOG("write failed (%s)", TMP_FILE_PATH);
    return false;
  }
  if (!io->renameFile(TMP_FILE_PATH, FILE_PATH)) {
    io->removeFile(TMP_FILE_PATH);
    VK_CONTACT_LOG("write failed (rename to %s)", FILE_PATH);
    return false;
  }
  return true;
}

// upsert(), also giving back the record as it is now stored.
bool store(const uint8_t pubkey[32], const char *name, Entry *stored) {
  if (vk::fileio::ops == nullptr || pubkey == nullptr || name == nullptr) return false;
  uint16_t count = 0;
  if (!loadForChange(count)) return false;

  uint8_t *record = nullptr;
  bool changed = true;
  for (size_t i = 0; i < count && record == nullptr; ++i) {
    if (memcmp(recordAt(i) + OFF_PUBKEY, pubkey, 32) == 0) record = recordAt(i);
  }
  if (record != nullptr) {
    // A second card from the same key updates the name, and nothing else.
    uint8_t field[NAME_SIZE];
    putName(field, name);
    changed = memcmp(record + OFF_NAME, field, NAME_SIZE) != 0;
    memcpy(record + OFF_NAME, field, NAME_SIZE);
  } else {
    if (count == CAPACITY) {             // full: the oldest record (the first) goes
      memmove(recordAt(0), recordAt(1), (CAPACITY - 1) * RECORD_SIZE);
      count--;
    }
    record = recordAt(count);
    memset(record, 0, RECORD_SIZE);
    memcpy(record + OFF_PUBKEY, pubkey, 32);
    putName(record + OFF_NAME, name);
    record[OFF_FLAGS] = 0;
    putU32(record + OFF_ADDED, unixNow());
    count++;
  }
  if (stored != nullptr) unpackRecord(record, *stored);
  return changed ? commit(count) : true;   // the same name again: the file already says so
}

// ---------------------------------------------------------------------------
// The swap nonce
// ---------------------------------------------------------------------------

// 16 random bytes that a card must carry to be accepted. Valid for 60 s from when it was drawn.
// (millis() wraps after 49.7 days: a nonce that neither hello() nor accept() looked at for that
// long would read as fresh for 60 s more. A badge is not up that long without either.)
struct Swap {
  bool valid;
  uint8_t nonce[16];
  uint32_t made_ms;
};
Swap nonceState = {};

bool nonceCurrent() { return nonceState.valid && (uint32_t)(nowMs() - nonceState.made_ms) < NONCE_LIFETIME_MS; }

// Copies a name as a frame carries it: 1..32 characters of printable ASCII (the codec's rule).
bool takeName(const char *name, char out[VK_NAME_MAX + 1], uint8_t &length) {
  if (name == nullptr) return false;
  const size_t n = strnlen(name, VK_NAME_MAX + 1);
  if (n < 1 || n > VK_NAME_MAX) return false;
  for (size_t i = 0; i < n; ++i) {
    if (name[i] < 0x20 || name[i] > 0x7E) return false;
  }
  memcpy(out, name, n);
  out[n] = '\0';
  length = (uint8_t)n;
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// The store
// ---------------------------------------------------------------------------

bool open(Cursor &out) {
  if (vk::fileio::ops == nullptr) return false;
  uint16_t count = 0;
  if (probe(count) != Probe::Valid) return false;
  out.count = count;
  return true;
}

size_t count() {
  Cursor cursor;
  return open(cursor) ? cursor.count : 0;
}

bool at(const Cursor &cursor, size_t index, Entry &out) {
  const vk::fileio::Ops *io = vk::fileio::ops;
  if (io == nullptr || index >= cursor.count) return false;
  uint8_t record[RECORD_SIZE];
  if (!io->read(FILE_PATH, recordOffset(index), record, sizeof record)) return false;
  unpackRecord(record, out);
  return true;
}

bool at(size_t index, Entry &out) {
  Cursor cursor;
  return open(cursor) && at(cursor, index, out);
}

bool upsert(const uint8_t pubkey[32], const char *name) { return store(pubkey, name, nullptr); }

bool remove(const uint8_t pubkey[32]) {
  if (vk::fileio::ops == nullptr || pubkey == nullptr) return false;
  uint16_t count = 0;
  if (probe(count) != Probe::Valid || count == 0) return false;   // nothing to remove, nothing to repair
  if (!vk::fileio::ops->read(FILE_PATH, 0, image, recordOffset(count))) return false;
  for (size_t i = 0; i < count; ++i) {
    if (memcmp(recordAt(i) + OFF_PUBKEY, pubkey, 32) != 0) continue;
    memmove(recordAt(i), recordAt(i + 1), (count - 1 - i) * RECORD_SIZE);
    return commit((uint16_t)(count - 1));
  }
  return false;
}

// ---------------------------------------------------------------------------
// The swap
// ---------------------------------------------------------------------------

size_t cleanName(const char *raw, char out[VK_NAME_MAX + 1]) {
  size_t n = 0;
  if (raw != nullptr) {
    for (; n < VK_NAME_MAX && raw[n] != '\0'; ++n) {
      const char c = raw[n];
      out[n] = (c >= 0x20 && c <= 0x7E) ? c : '?';
    }
  }
  out[n] = '\0';
  return n;
}

vk::wallet::Reason hello(const char *name, uint8_t out[VK_HELLO_MAX_LEN], size_t &outLen) {
  outLen = 0;
  const uint8_t *own = ownKey();
  if (own == nullptr) return VK_SIGN_FAILED;                 // no identity to announce

  vk_hello_t frame;
  memset(&frame, 0, sizeof frame);
  if (!takeName(name, frame.name, frame.name_len)) return VK_BAD_ARG;

  if (!nonceCurrent()) {                                     // none yet, used up, or 60 s old
    if (hooks.randomBytes == nullptr) return VK_SIGN_FAILED;
    hooks.randomBytes(nonceState.nonce, sizeof nonceState.nonce);
    nonceState.made_ms = nowMs();
    nonceState.valid = true;
  }
  memcpy(frame.pubkey, own, 32);
  memcpy(frame.nonce, nonceState.nonce, sizeof frame.nonce);
  const size_t n = vk_hello_build(&frame, out, VK_HELLO_MAX_LEN);
  if (n == 0) return VK_BAD_ARG;
  outLen = n;
  return VK_OK;
}

vk::wallet::Reason card(const uint8_t *helloFrame, size_t helloLen, const char *name,
                        uint8_t out[VK_CARD_MAX_LEN], size_t &outLen) {
  outLen = 0;
  vk_hello_t peer;
  if (helloFrame == nullptr || vk_hello_parse(helloFrame, helloLen, &peer) != 0) return VK_BAD_ARG;

  vk_card_t made;
  memset(&made, 0, sizeof made);
  if (!takeName(name, made.name, made.name_len)) return VK_BAD_ARG;
  memcpy(made.peer_pubkey, peer.pubkey, 32);
  memcpy(made.peer_nonce, peer.nonce, 16);

  const uint8_t *own = ownKey();
  if (own == nullptr || hooks.signAuto == nullptr) return VK_SIGN_FAILED;
  memcpy(made.pubkey, own, 32);

  uint8_t bytes[VK_CARD_SIGNED_MAX];
  const size_t signedLen = vk_card_signed_bytes(&made, bytes);
  if (signedLen == 0) return VK_BAD_ARG;

  // The domain's validator checks these same bytes again, then "contact:" ‖ bytes is signed.
  if (hooks.signAuto("contact", bytes, signedLen, made.sig) != VK_OK) return VK_SIGN_FAILED;
  const size_t n = vk_card_build(&made, out, VK_CARD_MAX_LEN);
  if (n == 0) return VK_BAD_ARG;
  outLen = n;
  return VK_OK;
}

vk::wallet::Reason accept(const uint8_t *cardFrame, size_t cardLen, Entry &saved) {
  vk_card_t got;
  if (cardFrame == nullptr || vk_card_parse(cardFrame, cardLen, &got) != 0) return VK_BAD_ARG;

  // 1. The card was made for this badge.
  const uint8_t *own = ownKey();
  if (own == nullptr || memcmp(got.peer_pubkey, own, 32) != 0) return VK_MISMATCH;

  // 2. It answers this badge's current swap nonce.
  if (!nonceCurrent()) {
    nonceState.valid = false;            // once too old, it stays refused
    return VK_EXPIRED;
  }
  if (memcmp(got.peer_nonce, nonceState.nonce, sizeof nonceState.nonce) != 0) return VK_EXPIRED;

  // 3. Its owner signed it: "contact:" ‖ peer_nonce ‖ peer_pubkey ‖ pubkey ‖ name_len ‖ name.
  constexpr size_t PREFIX_LEN = sizeof(VK_PREFIX_CONTACT) - 1;
  uint8_t message[PREFIX_LEN + VK_CARD_SIGNED_MAX];
  memcpy(message, VK_PREFIX_CONTACT, PREFIX_LEN);
  const size_t signedLen = vk_card_signed_bytes(&got, message + PREFIX_LEN);
  if (signedLen == 0 || hooks.verify == nullptr ||
      hooks.verify(message, PREFIX_LEN + signedLen, got.sig, got.pubkey) != 1) {
    return VK_BAD_PROOF;
  }

  // The card is good. If it cannot be saved the nonce stays, so the same card can be tried again.
  if (!store(got.pubkey, got.name, &saved)) return VK_UNSUPPORTED;

  nonceState.valid = false;              // rotated: the next hello() draws a new nonce
  VK_CONTACT_LOG("saved %s", saved.name);
  if (hooks.notify != nullptr) {
    char body[40];                       // notify::Note::body; "Saved " and 32 characters fit
    snprintf(body, sizeof body, "Saved %s", saved.name);
    hooks.notify("Contact saved", body);
  }
  return VK_OK;
}

bool validCardBytes(const uint8_t *bytes, size_t len, const uint8_t *own) {
  constexpr size_t FIXED = 16 + 32 + 32 + 1;        // everything before the name
  if (bytes == nullptr || own == nullptr || len <= FIXED || len > VK_CARD_SIGNED_MAX) return false;
  vk_card_t made;
  memset(&made, 0, sizeof made);
  memcpy(made.peer_nonce, bytes, 16);
  memcpy(made.peer_pubkey, bytes + 16, 32);
  memcpy(made.pubkey, bytes + 48, 32);
  made.name_len = bytes[80];
  if (made.name_len < 1 || made.name_len > VK_NAME_MAX || len != FIXED + made.name_len) return false;
  memcpy(made.name, bytes + FIXED, made.name_len);
  // The codec builds the same bytes from the fields only when the name is printable ASCII.
  uint8_t again[VK_CARD_SIGNED_MAX];
  if (vk_card_signed_bytes(&made, again) != len || memcmp(again, bytes, len) != 0) return false;
  return memcmp(made.pubkey, own, 32) == 0;
}

void reset() { nonceState = Swap{}; }

void eraseAll() {
  reset();
  const vk::fileio::Ops *io = vk::fileio::ops;
  if (io == nullptr) return;
  io->removeFile(FILE_PATH);
  io->removeFile(TMP_FILE_PATH);
  io->removeFile(BAD_FILE_PATH);
  VK_CONTACT_LOG("contacts erased");
}

namespace {
VK_ON_RESET(contacts, eraseAll);   // VKRESET erases the contacts with the wallet config (stores.md)
}

}  // namespace vk::contacts
