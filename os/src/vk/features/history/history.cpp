// src/vk/features/history/history.cpp
// The signature log (wallet/stores.md, "History"): the ring file, the approval listener, the sign
// listener and its queue, received payments, the daily total and the reset listener.
//
// File: 12-byte header (magic "VKH1", version u16, count u16, head u16, 2 bytes padding), then up to
// 128 records of 192 bytes, all little-endian. A new record goes into slot `head`; records are added
// in slot order, so while the ring is not full the file ends exactly where the next record starts.
// Records are written first and the header second. If a record write fails nothing has changed. If
// only the header write fails, the header still describes the records that were there before; the
// slots just written are not counted (in a full ring they are the oldest records' slots, so those
// entries show the new contents until the next write).
//
// Version 2 (this file) only adds to version 1: flags bits 1 (auto row) and 2 (req_id present), the
// outcome 7 (received), the auto row layout, and req_id in what were the last 8 bytes of a 24-byte
// app_id. A version 1 record sets neither new flag, so it reads correctly as a version 2 approval
// row whose app_id is its first 15 characters; a version 1 file is read as it is, and its header
// becomes version 2 on the next append. No record is rewritten.
//
// Host-test seam (test/host/test_stores.cpp): every file call goes through vk::fileio::ops, the times
// come through the `unixTime` and `millisNow` pointers, and the log, the clock, the approval engine
// and the services are behind #ifndef VK_HOST_TEST.
#include "history.h"

#include <string.h>

#include "../../core/config.h"     // VK_ON_RESET
#include "../../core/fileio.h"
#include "../../wallet/pure/sol.h"  // sol_sha256

#ifndef VK_HOST_TEST
#include <Arduino.h>

#include "../../../badge_log.h"
#include "../../core/clock.h"
#include "../../core/service.h"
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
constexpr size_t OFF_FLAGS = 19;        // 1
// approval and received rows
constexpr size_t OFF_AMOUNT = 20;       // u64
constexpr size_t OFF_DECIMALS = 28;     // 1
constexpr size_t OFF_SYMBOL = 29;       // 9
constexpr size_t OFF_RECIPIENT = 38;    // 32
constexpr size_t OFF_NAME = 70;         // 33
constexpr size_t OFF_APP = 103;         // 16 (version 1: 24)
constexpr size_t OFF_REQ_ID = 119;      // 8 (version 2)
constexpr size_t OFF_SIG = 127;         // 64
                                        // 191: one byte of padding
// auto rows (flags bit 1)
constexpr size_t OFF_AUTO_COUNT = 20;   // 1, then 3 bytes of padding
constexpr size_t OFF_AUTO_ITEMS = 24;   // 8 x (time u32, digest[16]) = 160, then 8 bytes of padding
constexpr size_t AUTO_ITEM_SIZE = 20;

constexpr uint8_t FLAG_DEV = 0x01;
constexpr uint8_t FLAG_AUTO = 0x02;
constexpr uint8_t FLAG_REQ = 0x04;

constexpr uint16_t OLDEST_READABLE_VERSION = 1;

static_assert(OFF_AUTO_ITEMS + AUTO_ITEMS_PER_ROW * AUTO_ITEM_SIZE <= RECORD_SIZE, "auto row fits a record");
static_assert(OFF_SIG + 64 + 1 == RECORD_SIZE, "record layout");

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
  out[OFF_FLAGS] = (uint8_t)((entry.dev ? FLAG_DEV : 0) | (entry.automatic ? FLAG_AUTO : 0) |
                             (!entry.automatic && entry.has_req_id ? FLAG_REQ : 0));
  if (entry.automatic) {
    const size_t n = entry.auto_count <= AUTO_ITEMS_PER_ROW ? entry.auto_count : AUTO_ITEMS_PER_ROW;
    out[OFF_AUTO_COUNT] = (uint8_t)n;
    for (size_t i = 0; i < n; ++i) {
      uint8_t *item = out + OFF_AUTO_ITEMS + i * AUTO_ITEM_SIZE;
      putU32(item, entry.items[i].time);
      memcpy(item + 4, entry.items[i].digest, sizeof entry.items[i].digest);
    }
    return;
  }
  putU64(out + OFF_AMOUNT, entry.amount);
  out[OFF_DECIMALS] = entry.decimals;
  putText(out + OFF_SYMBOL, sizeof entry.symbol, entry.symbol);
  memcpy(out + OFF_RECIPIENT, entry.recipient, sizeof entry.recipient);
  putText(out + OFF_NAME, sizeof entry.recipient_name, entry.recipient_name);
  putText(out + OFF_APP, sizeof entry.app_id, entry.app_id);
  if (entry.has_req_id) memcpy(out + OFF_REQ_ID, entry.req_id, sizeof entry.req_id);
  memcpy(out + OFF_SIG, entry.sig, sizeof entry.sig);
}

void unpackRecord(const uint8_t in[RECORD_SIZE], Entry &entry) {
  memset(&entry, 0, sizeof entry);
  entry.time = getU32(in + OFF_TIME);
  getText(in + OFF_DOMAIN, sizeof entry.domain, entry.domain);
  entry.outcome = in[OFF_OUTCOME];
  entry.reason = in[OFF_REASON];
  entry.severity = in[OFF_SEVERITY];
  const uint8_t flags = in[OFF_FLAGS];
  entry.dev = (flags & FLAG_DEV) != 0;
  entry.automatic = (flags & FLAG_AUTO) != 0;
  if (entry.automatic) {
    const uint8_t n = in[OFF_AUTO_COUNT];
    entry.auto_count = n <= AUTO_ITEMS_PER_ROW ? n : (uint8_t)AUTO_ITEMS_PER_ROW;
    for (size_t i = 0; i < entry.auto_count; ++i) {
      const uint8_t *item = in + OFF_AUTO_ITEMS + i * AUTO_ITEM_SIZE;
      entry.items[i].time = getU32(item);
      memcpy(entry.items[i].digest, item + 4, sizeof entry.items[i].digest);
    }
    return;
  }
  entry.amount = getU64(in + OFF_AMOUNT);
  entry.decimals = in[OFF_DECIMALS];
  getText(in + OFF_SYMBOL, sizeof entry.symbol, entry.symbol);
  memcpy(entry.recipient, in + OFF_RECIPIENT, sizeof entry.recipient);
  getText(in + OFF_NAME, sizeof entry.recipient_name, entry.recipient_name);
  getText(in + OFF_APP, sizeof entry.app_id, entry.app_id);     // a version 1 id: its first 15 characters
  // Version 1 never sets the flag, and its bytes here are the tail of a long app id: ignored.
  entry.has_req_id = (flags & FLAG_REQ) != 0;
  if (entry.has_req_id) memcpy(entry.req_id, in + OFF_REQ_ID, sizeof entry.req_id);
  memcpy(entry.sig, in + OFF_SIG, sizeof entry.sig);
}

void packHeader(const Cursor &cursor, uint8_t out[HEADER_SIZE]) {
  memset(out, 0, HEADER_SIZE);
  memcpy(out, MAGIC, sizeof MAGIC);
  putU16(out + 4, FORMAT_VERSION);       // a version 1 file becomes version 2 here
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
  packHeader(Cursor{0, 0, FORMAT_VERSION}, header);
  return io->writeAll(FILE_PATH, header, sizeof header);
}

// Writes `n` packed records as the newest, oldest first, then the header once. Returns how many
// records the file now counts (0 or n): a failed record write stops there and keeps the old header;
// a failed header write counts none.
size_t appendRecords(const uint8_t (*records)[RECORD_SIZE], size_t n) {
  const vk::fileio::Ops *io = vk::fileio::ops;
  if (io == nullptr || n == 0) return 0;
  Cursor cursor;
  if (!open(cursor)) {                   // no file yet, or one that is not a history file
    if (!startNewFile()) return 0;
    cursor = Cursor{0, 0, FORMAT_VERSION};
  }
  for (size_t i = 0; i < n; ++i) {
    if (!io->writeAt(FILE_PATH, recordOffset(cursor.head), records[i], RECORD_SIZE)) return 0;
    cursor.head = (uint16_t)((cursor.head + 1) % RING_CAPACITY);
    if (cursor.count < RING_CAPACITY) cursor.count++;
  }
  uint8_t header[HEADER_SIZE];
  packHeader(cursor, header);
  return io->writeAt(FILE_PATH, 0, header, sizeof header) ? n : 0;
}

// ---- the auto-signature queue ------------------------------------------------------------------

struct Pending {
  uint32_t time;
  char domain[12];
  uint8_t outcome;
  uint8_t reason;
  uint8_t digest[16];
};
Pending sQueue[QUEUE_CAPACITY];
size_t sQueued = 0;
uint32_t sFirstAtMs = 0, sLastAtMs = 0, sRetryAtMs = 0;
bool sRetryWaiting = false;
size_t sDropped = 0;

uint32_t loopMs() { return millisNow ? millisNow() : 0; }

// ---- signed payments whose approval row could not be written (counted by spentWithin) -----------

struct Unlogged { uint32_t time; uint64_t amount; uint8_t decimals; char symbol[9]; };
constexpr size_t UNLOGGED_CAPACITY = 4;
Unlogged sUnlogged[UNLOGGED_CAPACITY];
size_t sUnloggedCount = 0;
bool sUnloggedOverflow = false;

void rememberUnlogged(const Entry &entry) {
  if (sUnloggedCount >= UNLOGGED_CAPACITY) { sUnloggedOverflow = true; return; }
  Unlogged &u = sUnlogged[sUnloggedCount++];
  u.time = entry.time;
  u.amount = entry.amount;
  u.decimals = entry.decimals;
  strlcpy(u.symbol, entry.symbol, sizeof u.symbol);
}

// A signed approval row that moved an amount: what the daily limit counts. Confirmations carry no
// amount, and spentWithin keeps only rows whose symbol and decimals are a token's.
bool isPayment(const Entry &entry) {
  return !entry.automatic && entry.outcome == OUTCOME_SIGNED && entry.amount != 0;
}

void onApproval(const vk::wallet::ApprovalOutcome &outcome) {
  if (outcome.request == nullptr) return;
  const Entry entry = entryFor(outcome, unixTime ? unixTime() : 0);
#if !defined(VK_HOST_TEST) && VK_PROFILE_DEV
  const uint32_t startedAt = millis();
#endif
  // Queued auto signatures were made before this approval closed: they go first, so the ring stays
  // in signing order.
  if (sQueued != 0) flushPending();
  // A failed write is logged and ignored: the signature, if any, was already made and stays valid.
  // A signed payment that could not be logged is still counted against the daily limit.
  if (!append(entry)) {
    VK_HISTORY_LOG("history: write failed (%s %s)", entry.domain, outcomeName(entry.outcome));
    if (isPayment(entry)) rememberUnlogged(entry);
  }
#if !defined(VK_HOST_TEST) && VK_PROFILE_DEV
  // The cost of one append (stores.md, "History": it grows with the file). Dev profile only.
  VK_HISTORY_LOG("history write %lu ms", (unsigned long)(millis() - startedAt));
#endif
}

VK_ON_APPROVAL(history, onApproval);
VK_ON_SIGN(history, onSign);
VK_ON_RESET(history, eraseAll);   // VKRESET erases the log with the wallet config

#ifndef VK_HOST_TEST
uint32_t clockTime() { return vk::clock::ok() ? vk::clock::now() : 0; }
uint32_t loopMillis() { return millis(); }

bool spentLookupFn(const vk_token_t *tokens, size_t count, uint32_t now, uint64_t out[VK_MAX_TOKENS]) {
  return spentWithin(tokens, count, now, out);
}

void historyBegin() { vk::wallet::spentLookup = spentLookupFn; }

// Writes the queue when the loop is idle. Not while an approval is on the screen (its hold bar would
// stutter); the approval's own row flushes the queue first when it closes.
void historyUpdate() {
  if (sQueued == 0 || vk::wallet::approval::active()) return;
  if (!flushDue(millis())) return;
#if VK_PROFILE_DEV
  const uint32_t startedAt = millis();
  const size_t n = sQueued;
#endif
  const bool ok = flushPending();
#if VK_PROFILE_DEV
  VK_HISTORY_LOG("history auto flush %u %s %lu ms", (unsigned)n, ok ? "ok" : "failed", (unsigned long)(millis() - startedAt));
#else
  (void)ok;
#endif
}

VK_SERVICE(history, historyBegin, historyUpdate);
#endif

}  // namespace

#ifndef VK_HOST_TEST
uint32_t (*unixTime)() = clockTime;
uint32_t (*millisNow)() = loopMillis;
#else
uint32_t (*unixTime)() = nullptr;
uint32_t (*millisNow)() = nullptr;
#endif

const char *outcomeName(uint8_t outcome) {
  switch (outcome) {
    case OUTCOME_SIGNED:    return "signed";
    case OUTCOME_CANCELLED: return "cancelled";
    case OUTCOME_TIMEOUT:   return "timeout";
    case OUTCOME_BLOCKED:   return "blocked";
    case OUTCOME_FAILED:    return "failed";
    case OUTCOME_APPROVED:  return "approved";
    case OUTCOME_RECEIVED:  return "received";
    default:                return "?";
  }
}

Kind kindOf(const Entry &entry) {
  if (entry.automatic) return Kind::AUTO;
  if (entry.outcome == OUTCOME_RECEIVED) return Kind::RECEIVED;
  return Kind::APPROVAL;
}

const char *kindName(Kind kind) {
  switch (kind) {
    case Kind::AUTO:     return "auto";
    case Kind::RECEIVED: return "received";
    default:             return "approval";
  }
}

bool open(Cursor &out) {
  const vk::fileio::Ops *io = vk::fileio::ops;
  if (io == nullptr) return false;
  const long size = io->size(FILE_PATH);
  if (size < (long)HEADER_SIZE) return false;
  uint8_t header[HEADER_SIZE];
  if (!io->read(FILE_PATH, 0, header, sizeof header)) return false;
  const uint16_t version = getU16(header + 4);
  if (memcmp(header, MAGIC, sizeof MAGIC) != 0 || version < OLDEST_READABLE_VERSION || version > FORMAT_VERSION) return false;
  const uint16_t count = getU16(header + 6);
  const uint16_t head = getU16(header + 8);
  if (count > RING_CAPACITY || head >= RING_CAPACITY) return false;
  if (count < RING_CAPACITY && head != count) return false;           // not yet wrapped: the next slot is `count`
  if ((size_t)size < recordOffset(count)) return false;               // shorter than its own count says
  out.count = count;
  out.head = head;
  out.version = version;
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
  strlcpy(entry.app_id, request->app_id, sizeof entry.app_id);   // 15 characters at most
  entry.has_req_id = request->has_req_id;
  if (request->has_req_id) memcpy(entry.req_id, request->req_id, sizeof entry.req_id);
  return entry;
}

bool append(const Entry &entry) {
  uint8_t record[1][RECORD_SIZE];
  packRecord(entry, record[0]);
  return appendRecords(record, 1) == 1;
}

// ---- auto signatures ---------------------------------------------------------------------------

void onSign(const vk::wallet::SignEvent &event) {
  // A button domain's signature is recorded by its approval row, which also knows how it ended.
  if (event.domain == nullptr || event.domain->needs_button) return;
  if (sQueued == QUEUE_CAPACITY && !flushPending()) {
    // Last resort, never seen in normal use (the loop runs between signatures): the oldest is lost.
    memmove(sQueue, sQueue + 1, (QUEUE_CAPACITY - 1) * sizeof sQueue[0]);
    --sQueued;
    ++sDropped;
    VK_HISTORY_LOG("history: auto queue full, oldest entry lost (%u lost)", (unsigned)sDropped);
  }
  Pending &p = sQueue[sQueued];
  memset(&p, 0, sizeof p);
  p.time = unixTime ? unixTime() : 0;
  strlcpy(p.domain, event.domain->name, sizeof p.domain);
  p.outcome = event.result == VK_OK ? OUTCOME_SIGNED : OUTCOME_FAILED;
  p.reason = (uint8_t)event.result;
  uint8_t digest[32];
  const sol_slice_t part = {event.signed_bytes, event.signed_bytes ? event.signed_len : 0};
  sol_sha256(&part, 1, digest);
  memcpy(p.digest, digest, sizeof p.digest);
  const uint32_t now = loopMs();
  if (sQueued == 0) sFirstAtMs = now;
  sLastAtMs = now;
  ++sQueued;
}

size_t pendingCount() { return sQueued; }
size_t droppedCount() { return sDropped; }

bool flushDue(uint32_t nowMs) {
  if (sQueued == 0) return false;
  if (sRetryWaiting && (int32_t)(nowMs - sRetryAtMs) < 0) return false;
  return sQueued >= FLUSH_HIGH_WATER || nowMs - sLastAtMs >= FLUSH_QUIET_MS || nowMs - sFirstAtMs >= FLUSH_MAX_AGE_MS;
}

bool flushPending() {
  if (sQueued == 0) return true;
  // Consecutive entries of one domain and outcome share a row, up to 8 to a row. At most one row
  // per entry, so the rows fit a buffer of QUEUE_CAPACITY records; it is static (6 KB) because this
  // runs from the approval listener and from inside signRaw, where the stack is already deep.
  static uint8_t rows[QUEUE_CAPACITY][RECORD_SIZE];
  size_t rowCount = 0;
  for (size_t i = 0; i < sQueued;) {
    Entry e;
    memset(&e, 0, sizeof e);
    e.automatic = true;
    strlcpy(e.domain, sQueue[i].domain, sizeof e.domain);
    e.outcome = sQueue[i].outcome;
    e.reason = sQueue[i].reason;
    while (i < sQueued && e.auto_count < AUTO_ITEMS_PER_ROW && strcmp(sQueue[i].domain, e.domain) == 0 &&
           sQueue[i].outcome == e.outcome && sQueue[i].reason == e.reason) {
      e.items[e.auto_count].time = sQueue[i].time;
      memcpy(e.items[e.auto_count].digest, sQueue[i].digest, sizeof sQueue[i].digest);
      e.time = sQueue[i].time;                       // the row's time is its newest item's
      ++e.auto_count;
      ++i;
    }
    packRecord(e, rows[rowCount++]);
  }
  if (appendRecords(rows, rowCount) != rowCount) {
    sRetryWaiting = true;
    sRetryAtMs = loopMs() + FLUSH_RETRY_MS;
    VK_HISTORY_LOG("history: auto write failed (%u queued)", (unsigned)sQueued);
    return false;
  }
  sQueued = 0;
  sRetryWaiting = false;
  return true;
}

// ---- received payments -------------------------------------------------------------------------

ReceiveResult recordReceived(const Received &payment, uint32_t unix_s) {
  Cursor cursor;
  if (open(cursor)) {
    for (size_t i = 0; i < cursor.count; ++i) {
      Entry seen;
      if (!at(cursor, i, seen)) break;
      if (kindOf(seen) == Kind::RECEIVED && memcmp(seen.sig, payment.sig, sizeof seen.sig) == 0) return ReceiveResult::ALREADY;
    }
  }
  if (sQueued != 0) flushPending();
  Entry e;
  memset(&e, 0, sizeof e);
  e.time = unix_s;
  strlcpy(e.domain, "solana", sizeof e.domain);
  e.outcome = OUTCOME_RECEIVED;
  e.reason = VK_OK;
  e.amount = payment.amount;
  e.decimals = payment.decimals;
  strlcpy(e.symbol, payment.symbol, sizeof e.symbol);
  memcpy(e.recipient, payment.payer, sizeof e.recipient);
  strlcpy(e.app_id, payment.app_id, sizeof e.app_id);
  e.has_req_id = true;
  memcpy(e.req_id, payment.req_id, sizeof e.req_id);
  memcpy(e.sig, payment.sig, sizeof e.sig);
  if (!append(e)) {
    VK_HISTORY_LOG("history: write failed (received)");
    return ReceiveResult::FAILED;
  }
  return ReceiveResult::WRITTEN;
}

// ---- the daily total ---------------------------------------------------------------------------

bool spentWithin(const vk_token_t *tokens, size_t count, uint32_t now, uint64_t out[VK_MAX_TOKENS]) {
  for (size_t i = 0; i < VK_MAX_TOKENS; ++i) out[i] = 0;
  if (count > VK_MAX_TOKENS || (count != 0 && tokens == nullptr)) return false;
  if (sUnloggedOverflow) return false;

  auto add = [&](const char *symbol, uint8_t decimals, uint64_t amount, uint32_t time) {
    if (!vk_day_counts(time, now)) return;
    for (size_t t = 0; t < count; ++t) {
      if (tokens[t].decimals == decimals && strcmp(tokens[t].symbol, symbol) == 0) {
        out[t] = out[t] > UINT64_MAX - amount ? UINT64_MAX : out[t] + amount;   // saturate: still over any limit
        return;
      }
    }
  };
  for (size_t i = 0; i < sUnloggedCount; ++i) add(sUnlogged[i].symbol, sUnlogged[i].decimals, sUnlogged[i].amount, sUnlogged[i].time);

  const vk::fileio::Ops *io = vk::fileio::ops;
  if (io == nullptr) return false;
  Cursor cursor;
  if (!open(cursor)) return !io->exists(FILE_PATH);   // no log yet: nothing spent; a bad file: unknown
  // Read whole runs of slots at once: this runs inside begin(), before the approval opens. Static
  // for the same reason as flushPending's buffer.
  constexpr size_t CHUNK = 16;
  static uint8_t chunk[CHUNK][RECORD_SIZE];
  for (size_t first = 0; first < cursor.count; first += CHUNK) {
    const size_t n = cursor.count - first < CHUNK ? cursor.count - first : CHUNK;
    if (!io->read(FILE_PATH, recordOffset(first), chunk[0], n * RECORD_SIZE)) return false;
    for (size_t k = 0; k < n; ++k) {
      Entry e;
      unpackRecord(chunk[k], e);
      if (isPayment(e)) add(e.symbol, e.decimals, e.amount, e.time);
    }
  }
  return true;
}

// ---- reset -------------------------------------------------------------------------------------

void eraseAll() {
  sQueued = 0;
  sRetryWaiting = false;
  sDropped = 0;
  sUnloggedCount = 0;
  sUnloggedOverflow = false;
  const vk::fileio::Ops *io = vk::fileio::ops;
  if (io == nullptr) return;
  io->removeFile(FILE_PATH);
  io->removeFile(BAD_FILE_PATH);
  VK_HISTORY_LOG("history: erased");
}

}  // namespace vk::history
