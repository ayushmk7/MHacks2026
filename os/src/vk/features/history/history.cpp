// src/vk/features/history/history.cpp
// The history ring file and the approval listener that fills it (wallet/stores.md, "History").
//
// File: 12-byte header (magic "VKH1", version u16, count u16, head u16, 2 bytes padding), then up to
// 128 records of 192 bytes, all little-endian. A new record goes into slot `head`; records are added
// in slot order, so while the ring is not full the file ends exactly where the next record starts.
// The record is written first and the header second. If the record write fails nothing has changed.
// If only the header write fails, the header still describes the records that were there before;
// the slot just written is not counted (in a full ring it is the oldest record's slot, so the oldest
// entry then shows the new record's contents until the next write).
//
// Host-test seam (test/host/test_stores.cpp): every file call goes through vk::fileio::ops, the time
// comes through the `unixTime` pointer, and the log and the clock are behind #ifndef VK_HOST_TEST.
#include "history.h"

#include <string.h>

#include "../../core/fileio.h"

#ifndef VK_HOST_TEST
#include <Arduino.h>

#include "../../../badge_log.h"
#include "../../core/clock.h"
#include "../../vk_build.h"
#define VK_HISTORY_LOG(...) ::badge_log::tagf("vk", __VA_ARGS__)
#else
#define VK_HISTORY_LOG(...) do { } while (0)
#endif

namespace vk::history {

namespace {

const uint8_t MAGIC[4] = {'V', 'K', 'H', '1'};

// Record layout (stores.md): offset of each field.
constexpr size_t OFF_TIME = 0;          // u32
constexpr size_t OFF_DOMAIN = 4;        // 12
constexpr size_t OFF_OUTCOME = 16;      // 1
constexpr size_t OFF_REASON = 17;       // 1
constexpr size_t OFF_SEVERITY = 18;     // 1
constexpr size_t OFF_FLAGS = 19;        // 1; bit 0 = dev override used
constexpr size_t OFF_AMOUNT = 20;       // u64
constexpr size_t OFF_DECIMALS = 28;     // 1
constexpr size_t OFF_SYMBOL = 29;       // 9
constexpr size_t OFF_RECIPIENT = 38;    // 32
constexpr size_t OFF_NAME = 70;         // 33
constexpr size_t OFF_APP = 103;         // 24
constexpr size_t OFF_SIG = 127;         // 64
                                        // 191: one byte of padding
constexpr uint8_t FLAG_DEV = 0x01;

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

void putU64(uint8_t *at, uint64_t value) {
  for (size_t i = 0; i < 8; ++i) at[i] = (uint8_t)(value >> (8 * i));
}

uint64_t getU64(const uint8_t *at) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) value |= (uint64_t)at[i] << (8 * i);
  return value;
}

// A NUL-padded text field of `size` bytes: at most size-1 characters of `text`, the rest zeros.
void putText(uint8_t *at, size_t size, const char *text) {
  memset(at, 0, size);
  const size_t length = strnlen(text, size - 1);
  memcpy(at, text, length);
}

// Reads a text field of `size` bytes into `out` of the same size; always NUL-terminated.
void getText(const uint8_t *at, size_t size, char *out) {
  memcpy(out, at, size);
  out[size - 1] = '\0';
}

void packRecord(const Entry &entry, uint8_t out[RECORD_SIZE]) {
  memset(out, 0, RECORD_SIZE);
  putU32(out + OFF_TIME, entry.time);
  putText(out + OFF_DOMAIN, sizeof entry.domain, entry.domain);
  out[OFF_OUTCOME] = entry.outcome;
  out[OFF_REASON] = entry.reason;
  out[OFF_SEVERITY] = entry.severity;
  out[OFF_FLAGS] = entry.dev ? FLAG_DEV : 0;
  putU64(out + OFF_AMOUNT, entry.amount);
  out[OFF_DECIMALS] = entry.decimals;
  putText(out + OFF_SYMBOL, sizeof entry.symbol, entry.symbol);
  memcpy(out + OFF_RECIPIENT, entry.recipient, sizeof entry.recipient);
  putText(out + OFF_NAME, sizeof entry.recipient_name, entry.recipient_name);
  putText(out + OFF_APP, sizeof entry.app_id, entry.app_id);
  memcpy(out + OFF_SIG, entry.sig, sizeof entry.sig);
}

void unpackRecord(const uint8_t in[RECORD_SIZE], Entry &entry) {
  entry.time = getU32(in + OFF_TIME);
  getText(in + OFF_DOMAIN, sizeof entry.domain, entry.domain);
  entry.outcome = in[OFF_OUTCOME];
  entry.reason = in[OFF_REASON];
  entry.severity = in[OFF_SEVERITY];
  entry.dev = (in[OFF_FLAGS] & FLAG_DEV) != 0;
  entry.amount = getU64(in + OFF_AMOUNT);
  entry.decimals = in[OFF_DECIMALS];
  getText(in + OFF_SYMBOL, sizeof entry.symbol, entry.symbol);
  memcpy(entry.recipient, in + OFF_RECIPIENT, sizeof entry.recipient);
  getText(in + OFF_NAME, sizeof entry.recipient_name, entry.recipient_name);
  getText(in + OFF_APP, sizeof entry.app_id, entry.app_id);
  memcpy(entry.sig, in + OFF_SIG, sizeof entry.sig);
}

void packHeader(const Cursor &cursor, uint8_t out[HEADER_SIZE]) {
  memset(out, 0, HEADER_SIZE);
  memcpy(out, MAGIC, sizeof MAGIC);
  putU16(out + 4, FORMAT_VERSION);
  putU16(out + 6, cursor.count);
  putU16(out + 8, cursor.head);
}

size_t recordOffset(size_t slot) { return HEADER_SIZE + slot * RECORD_SIZE; }

// Makes sure /vk/ exists and starts an empty history file. A file already at that path is one that
// open() refused: it is kept as history.bin.bad (replacing an earlier .bad).
bool startNewFile() {
  const vk::fileio::Ops *io = vk::fileio::ops;
  io->makeDir(DIR_PATH);                 // succeeds if it is already there; if it fails, so does writeAll
  if (io->exists(FILE_PATH)) {
    if (io->exists(BAD_FILE_PATH)) io->removeFile(BAD_FILE_PATH);
    if (io->renameFile(FILE_PATH, BAD_FILE_PATH)) {
      VK_HISTORY_LOG("history: bad file renamed to %s", BAD_FILE_PATH);
    } else {
      VK_HISTORY_LOG("history: bad file could not be renamed; overwriting it");
    }
  }
  uint8_t header[HEADER_SIZE];
  packHeader(Cursor{0, 0}, header);
  return io->writeAll(FILE_PATH, header, sizeof header);
}

void onApproval(const vk::wallet::ApprovalOutcome &outcome) {
  if (outcome.request == nullptr) return;
  const Entry entry = entryFor(outcome, unixTime ? unixTime() : 0);
#if !defined(VK_HOST_TEST) && VK_PROFILE_DEV
  const uint32_t startedAt = millis();
#endif
  // A failed write is logged and ignored: the signature, if any, was already made and stays valid.
  if (!append(entry)) VK_HISTORY_LOG("history: write failed (%s %s)", entry.domain, outcomeName(entry.outcome));
#if !defined(VK_HOST_TEST) && VK_PROFILE_DEV
  // The cost of one append (stores.md, "History": it grows with the file). Dev profile only.
  VK_HISTORY_LOG("history write %lu ms", (unsigned long)(millis() - startedAt));
#endif
}

VK_ON_APPROVAL(history, onApproval);

#ifndef VK_HOST_TEST
uint32_t clockTime() { return vk::clock::ok() ? vk::clock::now() : 0; }
#endif

}  // namespace

#ifndef VK_HOST_TEST
uint32_t (*unixTime)() = clockTime;
#else
uint32_t (*unixTime)() = nullptr;
#endif

const char *outcomeName(uint8_t outcome) {
  switch (outcome) {
    case OUTCOME_SIGNED:    return "signed";
    case OUTCOME_CANCELLED: return "cancelled";
    case OUTCOME_TIMEOUT:   return "timeout";
    case OUTCOME_BLOCKED:   return "blocked";
    case OUTCOME_FAILED:    return "failed";
    case OUTCOME_APPROVED:  return "approved";
    default:                return "?";
  }
}

bool open(Cursor &out) {
  const vk::fileio::Ops *io = vk::fileio::ops;
  if (io == nullptr) return false;
  const long size = io->size(FILE_PATH);
  if (size < (long)HEADER_SIZE) return false;
  uint8_t header[HEADER_SIZE];
  if (!io->read(FILE_PATH, 0, header, sizeof header)) return false;
  if (memcmp(header, MAGIC, sizeof MAGIC) != 0 || getU16(header + 4) != FORMAT_VERSION) return false;
  const uint16_t count = getU16(header + 6);
  const uint16_t head = getU16(header + 8);
  if (count > RING_CAPACITY || head >= RING_CAPACITY) return false;
  if (count < RING_CAPACITY && head != count) return false;           // not yet wrapped: the next slot is `count`
  if ((size_t)size < recordOffset(count)) return false;               // shorter than its own count says
  out.count = count;
  out.head = head;
  return true;
}

size_t count() {
  Cursor cursor;
  return open(cursor) ? cursor.count : 0;
}

bool at(const Cursor &cursor, size_t newestFirstIndex, Entry &out) {
  const vk::fileio::Ops *io = vk::fileio::ops;
  if (io == nullptr || newestFirstIndex >= cursor.count) return false;
  // The newest record is the one before `head`.
  const size_t slot = (cursor.head + RING_CAPACITY - 1 - newestFirstIndex) % RING_CAPACITY;
  uint8_t record[RECORD_SIZE];
  if (!io->read(FILE_PATH, recordOffset(slot), record, sizeof record)) return false;
  unpackRecord(record, out);
  return true;
}

bool at(size_t newestFirstIndex, Entry &out) {
  Cursor cursor;
  return open(cursor) && at(cursor, newestFirstIndex, out);
}

Entry entryFor(const vk::wallet::ApprovalOutcome &outcome, uint32_t unix_s) {
  Entry entry;
  memset(&entry, 0, sizeof entry);
  entry.time = unix_s;
  entry.reason = (uint8_t)outcome.reason;
  if (outcome.approved) {
    entry.outcome = outcome.sig ? OUTCOME_SIGNED : OUTCOME_APPROVED;
  } else {
    // The same split as the result band of the approval screen.
    switch (outcome.reason) {
      case VK_CANCELLED:   entry.outcome = OUTCOME_CANCELLED; break;
      case VK_TIMEOUT:     entry.outcome = OUTCOME_TIMEOUT; break;
      case VK_SIGN_FAILED: entry.outcome = OUTCOME_FAILED; break;
      default:             entry.outcome = OUTCOME_BLOCKED; break;   // a red request: the reason says why
    }
  }
  if (outcome.approved && outcome.sig) memcpy(entry.sig, outcome.sig, sizeof entry.sig);

  const vk::wallet::ApprovalRequest *request = outcome.request;
  if (request == nullptr) return entry;
  // The engine names every confirmation "confirm"; a request with no domain can only be one.
  strlcpy(entry.domain, request->domain[0] ? request->domain : "confirm", sizeof entry.domain);
  entry.severity = (uint8_t)request->severity;
  entry.dev = request->dev_override && outcome.approved;   // the override was used only if it led to a signature
  entry.amount = request->amount;
  entry.decimals = request->decimals;
  strlcpy(entry.symbol, request->symbol, sizeof entry.symbol);
  memcpy(entry.recipient, request->recipient, sizeof entry.recipient);
  strlcpy(entry.recipient_name, request->recipient_name, sizeof entry.recipient_name);
  strlcpy(entry.app_id, request->app_id, sizeof entry.app_id);   // 23 characters at most
  return entry;
}

bool append(const Entry &entry) {
  const vk::fileio::Ops *io = vk::fileio::ops;
  if (io == nullptr) return false;

  Cursor cursor;
  if (!open(cursor)) {                   // no file yet, or one that is not a history file
    if (!startNewFile()) return false;
    cursor = Cursor{0, 0};
  }

  uint8_t record[RECORD_SIZE];
  packRecord(entry, record);
  if (!io->writeAt(FILE_PATH, recordOffset(cursor.head), record, sizeof record)) return false;

  cursor.head = (uint16_t)((cursor.head + 1) % RING_CAPACITY);
  if (cursor.count < RING_CAPACITY) cursor.count++;
  uint8_t header[HEADER_SIZE];
  packHeader(cursor, header);
  return io->writeAt(FILE_PATH, 0, header, sizeof header);
}

}  // namespace vk::history
