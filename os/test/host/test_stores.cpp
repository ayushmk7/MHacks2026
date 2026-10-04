// LINK: src/vk/features/history/history.cpp
// test_stores - the history ring file (docs/os/wallet/stores.md, "History") on the in-memory file
// layer of the shim: layout, round trip, newest-first order, wrap at 128, bad-file recovery, failing
// writes, the approval listener. Also the balance feature's reply scanner (ui/ui.md, "Balance"),
// which is a pure function in balance.h and needs no further source.
#include <Arduino.h>

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "../../src/vk/features/balance/balance.h"
#include "../../src/vk/features/history/history.h"
#include "vk_host_fileio.h"

using namespace vk::history;
using vk::wallet::ApprovalListener;
using vk::wallet::ApprovalOutcome;
using vk::wallet::ApprovalRequest;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// ---- helpers --------------------------------------------------------------------------------

static const vk::fileio::Ops *io() { return vk::fileio::ops; }

static void fresh() {
  vk_host_fileio_install();
  vk_host_fileio_reset();
  unixTime = nullptr;
}

static std::vector<uint8_t> fileBytes(const char *path) {
  std::vector<uint8_t> out;
  const long size = io()->size(path);
  if (size <= 0) return out;
  out.resize((size_t)size);
  if (!io()->read(path, 0, out.data(), out.size())) out.clear();
  return out;
}

// An entry that is told apart by `n` in every field a test looks at.
static Entry numbered(uint32_t n) {
  Entry e;
  memset(&e, 0, sizeof e);
  e.time = 1700000000u + n;
  strlcpy(e.domain, "solana", sizeof e.domain);
  e.outcome = OUTCOME_SIGNED;
  e.amount = n;
  e.decimals = 2;
  strlcpy(e.symbol, "HACK", sizeof e.symbol);
  snprintf(e.app_id, sizeof e.app_id, "app%u", (unsigned)n);
  return e;
}

static bool sameEntry(const Entry &a, const Entry &b) {
  return a.time == b.time && strcmp(a.domain, b.domain) == 0 && a.outcome == b.outcome && a.reason == b.reason &&
         a.severity == b.severity && a.dev == b.dev && a.amount == b.amount && a.decimals == b.decimals &&
         strcmp(a.symbol, b.symbol) == 0 && memcmp(a.recipient, b.recipient, 32) == 0 &&
         strcmp(a.recipient_name, b.recipient_name) == 0 && strcmp(a.app_id, b.app_id) == 0 &&
         memcmp(a.sig, b.sig, 64) == 0;
}

// The registered approval listeners, called as the engine calls them on entering RESULT.
static int notifyListeners(const ApprovalOutcome &outcome) {
  int called = 0;
  for (ApprovalListener *l = vk::Registered<ApprovalListener>::first(); l; l = l->vk::Registered<ApprovalListener>::next()) {
    if (l->fn) { l->fn(outcome); called++; }
  }
  return called;
}

// A payment request as the solana decoder and the engine leave it.
static ApprovalRequest paymentRequest() {
  ApprovalRequest r;
  memset(&r, 0, sizeof r);
  strlcpy(r.title, "Pay", sizeof r.title);
  strlcpy(r.headline, "VERIFIED - PRESENT", sizeof r.headline);
  r.severity = vk::wallet::Severity::GREEN;
  r.select = vk::wallet::SelectRule::PRESS;
  for (int i = 0; i < 32; ++i) r.recipient[i] = (uint8_t)(0xA0 + i);
  strlcpy(r.recipient_name, "MHacks Merch", sizeof r.recipient_name);
  r.amount = 1000;
  r.decimals = 2;
  strlcpy(r.symbol, "HACK", sizeof r.symbol);
  strlcpy(r.app_id, "checktest", sizeof r.app_id);
  strlcpy(r.domain, "solana", sizeof r.domain);
  return r;
}

// A file layer that passes everything to the shim's but fails chosen writeAt calls.
static const vk::fileio::Ops *realOps = nullptr;
static int writeAtCalls = 0;
static int failWriteAtCall = 0;                 // 1-based; 0 = none
static bool countingWriteAt(const char *path, size_t offset, const uint8_t *data, size_t len) {
  writeAtCalls++;
  if (writeAtCalls == failWriteAtCall) return false;
  return realOps->writeAt(path, offset, data, len);
}
static vk::fileio::Ops wrappedOps;
static void installCountingOps(int failCall) {
  vk_host_fileio_install();
  realOps = vk::fileio::ops;
  wrappedOps = *realOps;
  wrappedOps.writeAt = countingWriteAt;
  writeAtCalls = 0;
  failWriteAtCall = failCall;
  vk::fileio::ops = &wrappedOps;
}

static uint32_t fakeTime() { return 1759500000u; }

// ---- history: file creation -------------------------------------------------------------------

static void test_missing_dir_is_created() {
  fresh();
  CHECK(!io()->exists(DIR_PATH));
  CHECK(count() == 0);
  Entry e;
  CHECK(!at(0, e));
  Cursor c;
  CHECK(!open(c));
  CHECK(!io()->exists(DIR_PATH));              // reading creates nothing

  CHECK(append(numbered(1)));
  CHECK(io()->exists(DIR_PATH));
  CHECK(io()->exists(FILE_PATH));
  CHECK(io()->size(FILE_PATH) == (long)(HEADER_SIZE + RECORD_SIZE));
  CHECK(count() == 1);

  // /vk/ already there (another store made it), no file yet.
  fresh();
  CHECK(io()->makeDir(DIR_PATH));
  CHECK(append(numbered(2)));
  CHECK(count() == 1);
}

// ---- history: one record, every field, and the bytes on disk ------------------------------------

static void test_round_trip_every_field() {
  fresh();
  Entry e;
  memset(&e, 0, sizeof e);
  e.time = 0x65A1B2C3u;
  strlcpy(e.domain, "elevenchars", sizeof e.domain);                         // 11: fills the field
  e.outcome = OUTCOME_BLOCKED;
  e.reason = (uint8_t)VK_MISMATCH;
  e.severity = 2;
  e.dev = true;
  e.amount = 0x0102030405060708ull;
  e.decimals = 9;
  strlcpy(e.symbol, "ABCDEFGH", sizeof e.symbol);                            // 8: fills the field
  for (int i = 0; i < 32; ++i) e.recipient[i] = (uint8_t)(i + 1);
  strlcpy(e.recipient_name, "A name of thirty-two characters!", sizeof e.recipient_name);   // 32
  strlcpy(e.app_id, "twenty-three-char-app-i", sizeof e.app_id);             // 23
  for (int i = 0; i < 64; ++i) e.sig[i] = (uint8_t)(0xC0 ^ i);
  CHECK(strlen(e.domain) == 11 && strlen(e.symbol) == 8 && strlen(e.recipient_name) == 32 && strlen(e.app_id) == 23);

  CHECK(append(e));
  CHECK(count() == 1);
  Entry back;
  memset(&back, 0xEE, sizeof back);
  CHECK(at(0, back));
  CHECK(sameEntry(e, back));
  CHECK(back.time == 0x65A1B2C3u && back.outcome == OUTCOME_BLOCKED && back.reason == VK_MISMATCH);
  CHECK(back.severity == 2 && back.dev && back.amount == 0x0102030405060708ull && back.decimals == 9);
  CHECK(strcmp(back.domain, "elevenchars") == 0 && strcmp(back.symbol, "ABCDEFGH") == 0);
  CHECK(strcmp(back.recipient_name, "A name of thirty-two characters!") == 0);
  CHECK(strcmp(back.app_id, "twenty-three-char-app-i") == 0);
  CHECK(!at(1, back));

  // The layout of stores.md, byte for byte.
  const std::vector<uint8_t> f = fileBytes(FILE_PATH);
  CHECK(f.size() == 12 + 192);
  if (f.size() == 12 + 192) {
    CHECK(memcmp(f.data(), "VKH1", 4) == 0);
    CHECK(f[4] == 1 && f[5] == 0);                       // version u16
    CHECK(f[6] == 1 && f[7] == 0);                       // count u16
    CHECK(f[8] == 1 && f[9] == 0);                       // head u16
    CHECK(f[10] == 0 && f[11] == 0);                     // padding
    const uint8_t *r = f.data() + 12;
    CHECK(r[0] == 0xC3 && r[1] == 0xB2 && r[2] == 0xA1 && r[3] == 0x65);   // time, little-endian
    CHECK(memcmp(r + 4, "elevenchars\0", 12) == 0);
    CHECK(r[16] == 4);                                   // outcome: blocked
    CHECK(r[17] == 7);                                   // reason: mismatch
    CHECK(r[18] == 2);                                   // severity: red
    CHECK(r[19] == 1);                                   // flags: dev
    const uint8_t amountLe[8] = {8, 7, 6, 5, 4, 3, 2, 1};
    CHECK(memcmp(r + 20, amountLe, 8) == 0);
    CHECK(r[28] == 9);
    CHECK(memcmp(r + 29, "ABCDEFGH\0", 9) == 0);
    CHECK(memcmp(r + 38, e.recipient, 32) == 0);
    CHECK(memcmp(r + 70, "A name of thirty-two characters!\0", 33) == 0);
    CHECK(memcmp(r + 103, "twenty-three-char-app-i\0", 24) == 0);
    CHECK(memcmp(r + 127, e.sig, 64) == 0);
    CHECK(r[191] == 0);
  }

  // A second, short record: its text fields are NUL-padded, not left over from anything.
  Entry small = numbered(7);
  small.dev = false;
  CHECK(append(small));
  const std::vector<uint8_t> g = fileBytes(FILE_PATH);
  CHECK(g.size() == 12 + 2 * 192);
  if (g.size() == 12 + 2 * 192) {
    CHECK(g[6] == 2 && g[8] == 2);
    const uint8_t *r = g.data() + 12 + 192;
    const uint8_t domainField[12] = {'s', 'o', 'l', 'a', 'n', 'a', 0, 0, 0, 0, 0, 0};
    CHECK(memcmp(r + 4, domainField, 12) == 0);
    CHECK(r[19] == 0);
    bool sigZero = true;
    for (int i = 0; i < 64; ++i) sigZero = sigZero && r[127 + i] == 0;
    CHECK(sigZero);
  }
  CHECK(at(0, back) && sameEntry(back, small));
  CHECK(at(1, back) && sameEntry(back, e));
}

static void test_outcome_names() {
  CHECK(strcmp(outcomeName(1), "signed") == 0);
  CHECK(strcmp(outcomeName(2), "cancelled") == 0);
  CHECK(strcmp(outcomeName(3), "timeout") == 0);
  CHECK(strcmp(outcomeName(4), "blocked") == 0);
  CHECK(strcmp(outcomeName(5), "failed") == 0);
  CHECK(strcmp(outcomeName(6), "approved") == 0);
  CHECK(strcmp(outcomeName(0), "?") == 0);
  CHECK(strcmp(outcomeName(7), "?") == 0);
  CHECK(RING_CAPACITY == 128 && HEADER_SIZE == 12 && RECORD_SIZE == 192);
}

// ---- history: order and the ring ----------------------------------------------------------------

static void test_newest_first() {
  fresh();
  for (uint32_t n = 1; n <= 5; ++n) CHECK(append(numbered(n)));
  CHECK(count() == 5);
  Entry e;
  for (size_t i = 0; i < 5; ++i) {
    CHECK(at(i, e));
    CHECK(e.amount == 5 - i);
  }
  CHECK(!at(5, e));
  CHECK(!at(1000000, e));

  Cursor c;
  CHECK(open(c));
  CHECK(c.count == 5 && c.head == 5);
  CHECK(at(c, 0, e) && e.amount == 5);
  CHECK(at(c, 4, e) && e.amount == 1);
  CHECK(!at(c, 5, e));
}

static void test_ring_wrap() {
  fresh();
  for (uint32_t n = 1; n <= 128; ++n) CHECK(append(numbered(n)));
  CHECK(count() == 128);
  const long fullSize = (long)(12 + 128 * 192);
  CHECK(io()->size(FILE_PATH) == fullSize);
  Entry e;
  CHECK(at(0, e) && e.amount == 128);
  CHECK(at(127, e) && e.amount == 1);
  Cursor c;
  CHECK(open(c) && c.count == 128 && c.head == 0);

  // The 129th replaces the oldest; the count stays 128 and the file does not grow.
  CHECK(append(numbered(129)));
  CHECK(count() == 128);
  CHECK(io()->size(FILE_PATH) == fullSize);
  CHECK(open(c) && c.count == 128 && c.head == 1);
  CHECK(at(0, e) && e.amount == 129);
  CHECK(at(1, e) && e.amount == 128);
  CHECK(at(127, e) && e.amount == 2);          // 1 is gone
  CHECK(!at(128, e));
  bool ordered = true;
  for (size_t i = 0; i < 128; ++i) ordered = ordered && at(i, e) && e.amount == 129 - i;
  CHECK(ordered);

  // Well past a second lap.
  for (uint32_t n = 130; n <= 300; ++n) CHECK(append(numbered(n)));
  CHECK(count() == 128);
  CHECK(io()->size(FILE_PATH) == fullSize);
  ordered = true;
  for (size_t i = 0; i < 128; ++i) {
    ordered = ordered && at(i, e) && e.amount == 300 - i && e.time == 1700000000u + 300 - (uint32_t)i;
  }
  CHECK(ordered);
  CHECK(open(c) && c.head == 300 % 128);
}

// ---- history: a file that is not a history file ---------------------------------------------------

static void test_bad_magic_recovery() {
  fresh();
  CHECK(io()->makeDir(DIR_PATH));
  const uint8_t junk[40] = {'N', 'O', 'P', 'E', 1, 0, 3, 0, 3, 0, 0, 0, 9, 9, 9};
  CHECK(io()->writeAll(FILE_PATH, junk, sizeof junk));
  CHECK(count() == 0);
  Entry e;
  CHECK(!at(0, e));
  CHECK(!io()->exists(BAD_FILE_PATH));         // reading renames nothing

  CHECK(append(numbered(1)));
  CHECK(io()->exists(BAD_FILE_PATH));
  const std::vector<uint8_t> bad = fileBytes(BAD_FILE_PATH);
  CHECK(bad.size() == sizeof junk && memcmp(bad.data(), junk, sizeof junk) == 0);
  CHECK(count() == 1);
  CHECK(io()->size(FILE_PATH) == 12 + 192);
  CHECK(at(0, e) && e.amount == 1);

  // A wrong version is treated the same, and replaces the earlier .bad file.
  std::vector<uint8_t> v2 = fileBytes(FILE_PATH);
  v2[4] = 2;
  CHECK(io()->writeAll(FILE_PATH, v2.data(), v2.size()));
  CHECK(count() == 0);
  CHECK(append(numbered(2)));
  CHECK(count() == 1);
  CHECK(at(0, e) && e.amount == 2);
  const std::vector<uint8_t> bad2 = fileBytes(BAD_FILE_PATH);
  CHECK(bad2 == v2);

  // A header that claims more records than the file holds.
  std::vector<uint8_t> shortFile = fileBytes(FILE_PATH);
  shortFile[6] = 5;
  shortFile[8] = 5;
  CHECK(io()->writeAll(FILE_PATH, shortFile.data(), shortFile.size()));
  CHECK(count() == 0);
  CHECK(append(numbered(3)));
  CHECK(count() == 1 && at(0, e) && e.amount == 3);

  // Impossible header values: count above 128; head out of step with count.
  std::vector<uint8_t> tooMany(12 + 200 * 192, 0);
  memcpy(tooMany.data(), "VKH1", 4);
  tooMany[4] = 1;
  tooMany[6] = 200;
  tooMany[8] = 0;
  CHECK(io()->writeAll(FILE_PATH, tooMany.data(), tooMany.size()));
  CHECK(count() == 0);
  std::vector<uint8_t> outOfStep = fileBytes(BAD_FILE_PATH);    // the short file from above, valid size for 1 record
  outOfStep[6] = 1;
  outOfStep[8] = 0;
  CHECK(io()->writeAll(FILE_PATH, outOfStep.data(), outOfStep.size()));
  CHECK(count() == 0);

  // Shorter than a header, and empty.
  CHECK(io()->writeAll(FILE_PATH, (const uint8_t *)"VKH1", 4));
  CHECK(count() == 0);
  CHECK(append(numbered(4)));
  CHECK(count() == 1 && at(0, e) && e.amount == 4);
  CHECK(io()->writeAll(FILE_PATH, nullptr, 0));
  CHECK(count() == 0);
  CHECK(append(numbered(5)));
  CHECK(count() == 1 && at(0, e) && e.amount == 5);

  // The rename itself fails: the bad file is overwritten and history works again.
  CHECK(io()->writeAll(FILE_PATH, junk, sizeof junk));
  CHECK(io()->removeFile(BAD_FILE_PATH));
  vk_host_fileio_fail_writes = 1;
  CHECK(append(numbered(6)));
  CHECK(!io()->exists(BAD_FILE_PATH));
  CHECK(count() == 1 && at(0, e) && e.amount == 6);
}

// ---- history: failing writes (review focus 3) -----------------------------------------------------

static void test_failed_write() {
  fresh();
  for (uint32_t n = 1; n <= 3; ++n) CHECK(append(numbered(n)));
  const std::vector<uint8_t> before = fileBytes(FILE_PATH);

  // The record write fails: false, and the file is byte for byte what it was.
  vk_host_fileio_fail_writes = 1;
  CHECK(!append(numbered(4)));
  CHECK(vk_host_fileio_fail_writes == 0);
  CHECK(fileBytes(FILE_PATH) == before);
  CHECK(count() == 3);
  Entry e;
  CHECK(at(0, e) && e.amount == 3);

  // The store works again afterwards.
  CHECK(append(numbered(5)));
  CHECK(count() == 4);
  CHECK(at(0, e) && e.amount == 5);
  CHECK(at(1, e) && e.amount == 3);

  // Every write fails (a full filesystem): false each time, nothing lost.
  const std::vector<uint8_t> four = fileBytes(FILE_PATH);
  vk_host_fileio_fail_writes = -1;
  CHECK(!append(numbered(6)));
  CHECK(!append(numbered(7)));
  vk_host_fileio_fail_writes = 0;
  CHECK(fileBytes(FILE_PATH) == four);
  CHECK(count() == 4);

  // The record is written but the header write fails: false, and the header still describes the
  // four records that were there.
  installCountingOps(2);
  CHECK(!append(numbered(8)));
  CHECK(writeAtCalls == 2);
  vk_host_fileio_install();
  CHECK(count() == 4);
  CHECK(at(0, e) && e.amount == 5);
  CHECK(at(3, e) && e.amount == 1);
  CHECK(append(numbered(9)));                  // takes the slot of the uncounted record
  CHECK(count() == 5);
  CHECK(io()->size(FILE_PATH) == 12 + 5 * 192);
  CHECK(at(0, e) && e.amount == 9);
  CHECK(at(1, e) && e.amount == 5);

  // No file yet and the first write fails: false, no half-made file is mistaken for history.
  fresh();
  vk_host_fileio_fail_writes = 1;
  CHECK(!append(numbered(1)));
  CHECK(count() == 0);
  CHECK(append(numbered(2)));
  CHECK(count() == 1);
  CHECK(at(0, e) && e.amount == 2);

  // The header of a new file is written but its first record is not.
  fresh();
  installCountingOps(1);
  CHECK(!append(numbered(1)));
  vk_host_fileio_install();
  CHECK(count() == 0);
  CHECK(io()->size(FILE_PATH) == 12);
  CHECK(append(numbered(2)));
  CHECK(count() == 1 && at(0, e) && e.amount == 2);

  // A full ring and a failing write: still 128, still in order.
  fresh();
  for (uint32_t n = 1; n <= 130; ++n) CHECK(append(numbered(n)));
  const std::vector<uint8_t> full = fileBytes(FILE_PATH);
  vk_host_fileio_fail_writes = 1;
  CHECK(!append(numbered(131)));
  CHECK(fileBytes(FILE_PATH) == full);
  CHECK(count() == 128 && at(0, e) && e.amount == 130);

  // No file layer at all.
  vk::fileio::ops = nullptr;
  CHECK(!append(numbered(1)));
  CHECK(count() == 0);
  CHECK(!at(0, e));
  vk_host_fileio_install();
}

// ---- history: the approval listener ---------------------------------------------------------------

static void test_listener() {
  fresh();
  unixTime = fakeTime;
  ApprovalRequest request = paymentRequest();
  uint8_t sig[64];
  for (int i = 0; i < 64; ++i) sig[i] = (uint8_t)(i * 3 + 1);
  static const uint8_t zeros[64] = {0};
  Entry e;

  // 1 signed
  ApprovalOutcome outcome = {&request, true, VK_OK, sig};
  CHECK(notifyListeners(outcome) == 1);        // exactly one listener is linked: history's
  CHECK(count() == 1);
  CHECK(at(0, e));
  CHECK(e.outcome == OUTCOME_SIGNED && e.reason == VK_OK && e.time == 1759500000u);
  CHECK(strcmp(e.domain, "solana") == 0 && e.severity == 0 && !e.dev);
  CHECK(e.amount == 1000 && e.decimals == 2 && strcmp(e.symbol, "HACK") == 0);
  CHECK(memcmp(e.recipient, request.recipient, 32) == 0);
  CHECK(strcmp(e.recipient_name, "MHacks Merch") == 0 && strcmp(e.app_id, "checktest") == 0);
  CHECK(memcmp(e.sig, sig, 64) == 0);

  // 2 cancelled; no signature is stored
  request.severity = vk::wallet::Severity::AMBER;
  outcome = {&request, false, VK_CANCELLED, nullptr};
  notifyListeners(outcome);
  CHECK(at(0, e) && e.outcome == OUTCOME_CANCELLED && e.reason == VK_CANCELLED && e.severity == 1);
  CHECK(memcmp(e.sig, zeros, 64) == 0);

  // 3 timeout
  outcome = {&request, false, VK_TIMEOUT, nullptr};
  notifyListeners(outcome);
  CHECK(at(0, e) && e.outcome == OUTCOME_TIMEOUT && e.reason == VK_TIMEOUT);

  // 4 blocked: a red request reports its own reason, however it was closed
  request.severity = vk::wallet::Severity::RED;
  request.red_reason = VK_MISMATCH;
  outcome = {&request, false, VK_MISMATCH, nullptr};
  notifyListeners(outcome);
  CHECK(at(0, e) && e.outcome == OUTCOME_BLOCKED && e.reason == VK_MISMATCH && e.severity == 2 && !e.dev);
  request.red_reason = VK_UNDECODABLE;
  outcome = {&request, false, VK_UNDECODABLE, nullptr};
  notifyListeners(outcome);
  CHECK(at(0, e) && e.outcome == OUTCOME_BLOCKED && e.reason == VK_UNDECODABLE);

  // 5 failed
  request.severity = vk::wallet::Severity::GREEN;
  outcome = {&request, false, VK_SIGN_FAILED, nullptr};
  notifyListeners(outcome);
  CHECK(at(0, e) && e.outcome == OUTCOME_FAILED && e.reason == VK_SIGN_FAILED);
  CHECK(memcmp(e.sig, zeros, 64) == 0);

  // 1 signed under the dev override: red, signed, dev flag
  request.severity = vk::wallet::Severity::RED;
  request.red_reason = VK_UNVERIFIED;
  request.dev_override = true;
  outcome = {&request, true, VK_OK, sig};
  notifyListeners(outcome);
  CHECK(at(0, e) && e.outcome == OUTCOME_SIGNED && e.severity == 2 && e.dev);
  // ... and the same screen cancelled: blocked, and the override was not used
  outcome = {&request, false, VK_UNVERIFIED, nullptr};
  notifyListeners(outcome);
  CHECK(at(0, e) && e.outcome == OUTCOME_BLOCKED && e.reason == VK_UNVERIFIED && !e.dev);

  // 6 approved: a confirmation, which is not a payment. No clock: time 0.
  unixTime = nullptr;
  ApprovalRequest confirmation;
  memset(&confirmation, 0, sizeof confirmation);
  strlcpy(confirmation.title, "Change setting", sizeof confirmation.title);
  confirmation.severity = vk::wallet::Severity::AMBER;
  strlcpy(confirmation.domain, "confirm", sizeof confirmation.domain);
  outcome = {&confirmation, true, VK_OK, nullptr};
  notifyListeners(outcome);
  CHECK(at(0, e) && e.outcome == OUTCOME_APPROVED && e.time == 0 && strcmp(e.domain, "confirm") == 0);
  CHECK(e.amount == 0 && e.symbol[0] == 0 && e.app_id[0] == 0 && e.recipient_name[0] == 0);
  CHECK(memcmp(e.recipient, zeros, 32) == 0 && memcmp(e.sig, zeros, 64) == 0);
  // A refused confirmation is "cancelled" with domain confirm.
  outcome = {&confirmation, false, VK_CANCELLED, nullptr};
  notifyListeners(outcome);
  CHECK(at(0, e) && e.outcome == OUTCOME_CANCELLED && strcmp(e.domain, "confirm") == 0);
  CHECK(count() == 10);

  // An app id of 32 characters is cut to 23.
  ApprovalRequest longId = paymentRequest();
  strlcpy(longId.app_id, "abcdefghijklmnopqrstuvwxyz012345", sizeof longId.app_id);
  CHECK(strlen(longId.app_id) == 32);
  outcome = {&longId, true, VK_OK, sig};
  notifyListeners(outcome);
  CHECK(at(0, e) && strcmp(e.app_id, "abcdefghijklmnopqrstuvw") == 0);
  CHECK(count() == 11);

  // An outcome with no request writes nothing.
  outcome = {nullptr, false, VK_CANCELLED, nullptr};
  notifyListeners(outcome);
  CHECK(count() == 11);

  // A failing file layer: the listener returns, nothing is written, nothing is lost.
  const std::vector<uint8_t> before = fileBytes(FILE_PATH);
  vk_host_fileio_fail_writes = -1;
  outcome = {&request, true, VK_OK, sig};
  notifyListeners(outcome);
  vk_host_fileio_fail_writes = 0;
  CHECK(fileBytes(FILE_PATH) == before);
  CHECK(count() == 11);

  // First boot, no /vk/: the listener creates it.
  fresh();
  outcome = {&request, true, VK_OK, sig};
  notifyListeners(outcome);
  CHECK(io()->exists(DIR_PATH));
  CHECK(count() == 1);
  CHECK(at(0, e) && e.time == 0);

  // entryFor on its own.
  const Entry direct = entryFor(outcome, 42);
  CHECK(direct.time == 42 && direct.outcome == OUTCOME_SIGNED && direct.dev);
}

// ---- balance: the reply scanner -------------------------------------------------------------------

// 32 bytes 1..32 in base58, and the token program id, both checked against sol_b58 below.
static std::string b58(const uint8_t *bytes) {
  char text[SOL_B58_PUBKEY_MAX];
  const size_t n = sol_b58_encode(bytes, 32, text, sizeof text);
  return std::string(text, n);
}

static bool scanText(const std::string &reply, uint8_t account[32], uint64_t &raw) {
  return vk::balance::scanReply(reply.c_str(), reply.size(), account, raw);
}

static void test_balance_scan() {
  uint8_t first[32], second[32], owner[32];
  for (int i = 0; i < 32; ++i) { first[i] = (uint8_t)(i + 1); second[i] = (uint8_t)(0xF0 - i); owner[i] = (uint8_t)(0x40 + i); }
  const std::string A = b58(first), B = b58(second), OWNER = b58(owner);
  const std::string TOKEN_PROGRAM = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA";

  auto item = [&](const std::string &pubkey, const std::string &amount, bool pubkeyFirst) {
    const std::string account =
        "\"account\":{\"data\":{\"parsed\":{\"info\":{\"isNative\":false,\"mint\":\"" + TOKEN_PROGRAM +
        "\",\"owner\":\"" + OWNER + "\",\"state\":\"initialized\",\"tokenAmount\":{\"amount\":\"" + amount +
        "\",\"decimals\":2,\"uiAmount\":142.5,\"uiAmountString\":\"142.5\"}},\"type\":\"account\"},"
        "\"program\":\"spl-token\",\"space\":165},\"executable\":false,\"lamports\":2039280,\"owner\":\"" +
        TOKEN_PROGRAM + "\",\"rentEpoch\":18446744073709551615,\"space\":165}";
    const std::string key = "\"pubkey\":\"" + pubkey + "\"";
    return pubkeyFirst ? "{" + key + "," + account + "}" : "{" + account + "," + key + "}";
  };
  auto reply = [](const std::string &items) {
    return "{\"jsonrpc\":\"2.0\",\"result\":{\"context\":{\"apiVersion\":\"2.1.0\",\"slot\":341197933},\"value\":[" +
           items + "]},\"id\":1}";
  };

  uint8_t account[32];
  uint64_t raw = 0;

  // One account, either field order.
  memset(account, 0, sizeof account);
  CHECK(scanText(reply(item(A, "14250", true)), account, raw));
  CHECK(memcmp(account, first, 32) == 0 && raw == 14250);
  memset(account, 0, sizeof account);
  raw = 0;
  CHECK(scanText(reply(item(A, "14250", false)), account, raw));
  CHECK(memcmp(account, first, 32) == 0 && raw == 14250);

  // Two accounts: the first one is kept, in either order.
  CHECK(scanText(reply(item(A, "100", true) + "," + item(B, "999", true)), account, raw));
  CHECK(memcmp(account, first, 32) == 0 && raw == 100);
  CHECK(scanText(reply(item(B, "999", false) + "," + item(A, "100", false)), account, raw));
  CHECK(memcmp(account, second, 32) == 0 && raw == 999);

  // Zero, the largest u64, and blanks around the colon.
  CHECK(scanText(reply(item(A, "0", true)), account, raw) && raw == 0);
  CHECK(scanText(reply(item(A, "18446744073709551615", true)), account, raw) && raw == UINT64_MAX);
  CHECK(scanText("{ \"pubkey\" :\n \"" + A + "\", \"amount\"\t: \"5\" }", account, raw));
  CHECK(memcmp(account, first, 32) == 0 && raw == 5);

  // Ignored replies leave the outputs alone.
  memset(account, 0x55, sizeof account);
  raw = 77;
  const uint8_t untouched[32] = {0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
                                 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55};
  CHECK(!scanText(reply(""), account, raw));                                              // no token account
  CHECK(!scanText("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32602,\"message\":\"Invalid param\"},\"id\":1}", account, raw));
  CHECK(!scanText("", account, raw));
  CHECK(!scanText("<html>502 Bad Gateway</html>", account, raw));
  CHECK(!vk::balance::scanReply(nullptr, 0, account, raw));
  CHECK(!scanText("{\"pubkey\":\"" + A + "\"}", account, raw));                           // no amount
  CHECK(!scanText("{\"amount\":\"5\"}", account, raw));                                   // no pubkey
  CHECK(!scanText(reply(item(A, "18446744073709551616", true)), account, raw));           // one above u64
  CHECK(!scanText(reply(item(A, "184467440737095516150", true)), account, raw));          // 21 digits
  CHECK(!scanText(reply(item(A, "12.5", true)), account, raw));                           // not raw units
  CHECK(!scanText(reply(item(A, "-1", true)), account, raw));
  CHECK(!scanText(reply(item(A, "", true)), account, raw));
  CHECK(!scanText(reply(item("not-base58-0OIl", "5", true)), account, raw));
  CHECK(!scanText(reply(item(A.substr(0, 20), "5", true)), account, raw));                // decodes to fewer than 32 bytes
  CHECK(!scanText(reply(item(A + A, "5", true)), account, raw));                          // too long for an address
  CHECK(!scanText("{\"pubkey\":\"" + A + "\",\"amount\":5}", account, raw));              // a number, not a string
  CHECK(!scanText("{\"pubkey\":\"" + A + "\",\"uiAmount\":\"5\",\"tokenAmount\":\"5\"}", account, raw));
  const std::string whole = reply(item(A, "14250", true));
  CHECK(!scanText(whole.substr(0, whole.find(A) + 10), account, raw));                    // cut inside the address
  CHECK(!scanText(whole.substr(0, whole.find("14250") + 2), account, raw));               // cut inside the amount
  CHECK(memcmp(account, untouched, 32) == 0 && raw == 77);

  // A length shorter than the text is respected (the body is not assumed to be NUL-terminated there).
  CHECK(!vk::balance::scanReply(whole.c_str(), whole.find("14250"), account, raw));
  CHECK(vk::balance::scanReply(whole.c_str(), whole.size(), account, raw) && raw == 14250);
}

int main() {
  test_missing_dir_is_created();
  test_round_trip_every_field();
  test_outcome_names();
  test_newest_first();
  test_ring_wrap();
  test_bad_magic_recovery();
  test_failed_write();
  test_listener();
  test_balance_scan();

  if (fails) {
    printf("%d stores checks FAILED\n", fails);
    return 1;
  }
  printf("all stores tests passed\n");
  return 0;
}
