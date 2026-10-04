// src/vk/features/history/history.h
// The signature log: a ring of 128 fixed records in /vk/history.bin (wallet/stores.md, "History").
// Three kinds of row:
//   approval  one per approval outcome (signed, cancelled, ...), written by a VK_ON_APPROVAL listener
//   auto      signatures the badge made with no screen (pay-req, pay-proof, contact, store-reg, and
//             any future auto domain), reported by signRaw() through a VK_ON_SIGN listener, queued
//             in RAM and written in batches of up to 8 per row when the loop is idle
//   received  an incoming payment the firmware checked against its transaction (record_received)
// Apps read it only through badge.wallet.history and write a received row only through
// badge.wallet.record_received (lua_history.cpp).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../../wallet/approval.h"   // ApprovalOutcome, Reason
#include "../../wallet/signer.h"     // SignEvent, vk_token_t

namespace vk::history {

// One signature of an auto row: when, and the first 16 bytes of SHA-256(prefix || bytes), the
// message the key signed.
struct AutoItem { uint32_t time; uint8_t digest[16]; };
constexpr size_t AUTO_ITEMS_PER_ROW = 8;

// The record of stores.md, unpacked. Every string is NUL-terminated.
struct Entry {
  uint32_t time;             // unix seconds; 0 if the clock had no source. Auto row: its newest item's
  char domain[12];           // "solana", "bank", "confirm", "pay-proof", ...
  uint8_t outcome;           // OUTCOME_* below
  uint8_t reason;            // vk_reason_t
  uint8_t severity;          // 0 green, 1 amber, 2 red
  bool dev;                  // flags bit 0: a dev-build override was used
  bool automatic;            // flags bit 1: an auto row (the payment fields below are zero)
  bool has_req_id;           // flags bit 2: req_id holds the request this row pays or was paid for
  uint64_t amount;           // raw units; 0 if not a payment
  uint8_t decimals;
  char symbol[9];
  uint8_t recipient[32];     // the other party: the record's device key, else the destination
                             // account (approval); the payer's key (received); else zeros
  char recipient_name[33];
  char app_id[16];           // truncated to 15 characters (display only)
  uint8_t req_id[8];         // zeros unless has_req_id
  uint8_t sig[64];           // the signature made (approval, signed) or the transaction's (received)
  uint8_t auto_count;        // auto row: 1..8 signatures in `items`; 0 otherwise
  AutoItem items[AUTO_ITEMS_PER_ROW];
};

size_t count();
bool at(size_t newestFirstIndex, Entry &out);

// ---- Added by WP24 (history.cpp). The two declarations above are the spec's. ----

constexpr char FILE_PATH[] = "/vk/history.bin";
constexpr char BAD_FILE_PATH[] = "/vk/history.bin.bad";
constexpr char DIR_PATH[] = "/vk";

constexpr size_t RING_CAPACITY = 128;  // records in the ring
constexpr size_t HEADER_SIZE = 12;     // magic[4] "VKH1", version u16, count u16, head u16, 2 bytes padding
constexpr size_t RECORD_SIZE = 192;
constexpr uint16_t FORMAT_VERSION = 2; // 2: auto and received rows, req_id. A version 1 file is read as
                                       // it is and becomes version 2 on its next append (stores.md)

// The `outcome` byte of a record. Prefixed constants rather than an enum class, so that none of the
// words can collide with a macro of the Arduino core (as DISABLED and CHANGE do).
constexpr uint8_t OUTCOME_SIGNED = 1;
constexpr uint8_t OUTCOME_CANCELLED = 2;
constexpr uint8_t OUTCOME_TIMEOUT = 3;
constexpr uint8_t OUTCOME_BLOCKED = 4;
constexpr uint8_t OUTCOME_FAILED = 5;
constexpr uint8_t OUTCOME_APPROVED = 6;   // a confirmation that is not a signature
constexpr uint8_t OUTCOME_RECEIVED = 7;   // an incoming payment (version 2)

// "signed", "cancelled", "timeout", "blocked", "failed", "approved", "received" (the strings of
// lua-api.md); "?" for any other value.
const char *outcomeName(uint8_t outcome);

// Which of the three kinds a row is: auto (flag), received (outcome), else approval.
enum class Kind : uint8_t { APPROVAL, AUTO, RECEIVED };
Kind kindOf(const Entry &entry);
const char *kindName(Kind kind);          // "approval", "auto", "received"

// The file's header, read and checked once, so that a reader of many records (wallet.history) does
// not read it again for each one. The fields are only meaningful after open() returned true.
struct Cursor { uint16_t count; uint16_t head; uint16_t version; };
bool open(Cursor &out);                                        // false: no file, or not a valid history file
bool at(const Cursor &cursor, size_t newestFirstIndex, Entry &out);

// What the approval listener does, in two steps so the host suite can drive each.
// entryFor: the record for one approval outcome. `unix_s` is the time to store (0 = no clock source).
// append: writes it as the newest record. Creates /vk/ and the file when missing; a file with a wrong
// magic or an unknown version is renamed to history.bin.bad and a new one started. False when a
// write failed: the records already in the file are still there.
Entry entryFor(const vk::wallet::ApprovalOutcome &outcome, uint32_t unix_s);
bool append(const Entry &entry);

// The time written into each record: unix seconds, or 0 when the clock has no source. On the badge
// it reads vk::clock; in a host suite it is null (records get time 0) until the test sets it.
extern uint32_t (*unixTime)();

// ---- Auto signatures (version 2): the VK_ON_SIGN listener and its queue ----
//
// The listener runs inside signRaw() and only copies: domain, outcome, time and a 16-byte digest go
// into a RAM queue. The queue is written (flushPending) by the feature's service when the loop is
// idle, before an approval row or a received row (so the ring stays in signing order), and at once
// if it is full. Guarantee: an auto signature is in the file within FLUSH_MAX_AGE_MS of being made
// (plus the write itself) unless storage fails; a power cut loses at most the queued entries.

constexpr size_t QUEUE_CAPACITY = 32;
constexpr uint32_t FLUSH_QUIET_MS = 2000;     // write once no auto signature was made for this long
constexpr uint32_t FLUSH_MAX_AGE_MS = 10000;  // or once the oldest queued one is this old
constexpr size_t FLUSH_HIGH_WATER = 24;       // or once this many are queued
constexpr uint32_t FLUSH_RETRY_MS = 5000;     // after a failed write, wait this long before the next try

void onSign(const vk::wallet::SignEvent &event);   // the VK_ON_SIGN listener (button domains are ignored:
                                                   // their approval row records them, with the outcome)
size_t pendingCount();
bool flushDue(uint32_t nowMs);                     // the policy above; false while a retry is waiting
bool flushPending();                               // writes every queued entry; false if a write failed
                                                   // (what was not written stays queued)
size_t droppedCount();                             // entries lost because the queue was full and
                                                   // could not be written (logged)

// The loop time the queue uses (millis() on the badge; null in a host suite = 0, until set).
extern uint32_t (*millisNow)();

// ---- Received payments (version 2) ----

struct Received {
  uint8_t payer[32];         // the transaction's fee payer: the paying badge's key
  uint64_t amount;           // raw units
  uint8_t decimals;
  char symbol[9];
  uint8_t req_id[8];
  uint8_t sig[64];           // the transaction's signature (its id on chain)
  char app_id[16];
};
enum class ReceiveResult : uint8_t { WRITTEN, ALREADY, FAILED };
// Writes a received row. ALREADY (nothing written) when a received row with that signature is in
// the ring, so an app may call it again after a retry. FAILED when the file could not be written.
ReceiveResult recordReceived(const Received &payment, uint32_t unix_s);

// ---- The daily total (checks.md, "Daily limit") ----

// For each token, the raw units of signed approval rows with an amount in that symbol and decimals
// whose time vk_day_counts(time, now) accepts. Also counts signed payments whose row could not be
// written this boot. False when the file exists but cannot be read, or when more payments failed to
// be logged than the RAM list holds. No file: true with every total 0.
bool spentWithin(const vk_token_t *tokens, size_t count, uint32_t now, uint64_t out[VK_MAX_TOKENS]);

// ---- Reset ----

// The VK_ON_RESET listener: removes the file (and .bad), empties the queue and the unlogged list.
void eraseAll();

}  // namespace vk::history
