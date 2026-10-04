// src/vk/features/history/history.h
// The signature history: one fixed record per approval outcome, in a ring of 128 kept in
// /vk/history.bin (wallet/stores.md, "History"). Written by a VK_ON_APPROVAL listener; read by
// apps only through badge.wallet.history (lua_history.cpp).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../../wallet/approval.h"   // ApprovalOutcome, Reason

namespace vk::history {

// The record of stores.md, unpacked. Every string is NUL-terminated.
struct Entry {
  uint32_t time;             // unix seconds; 0 if the clock had no source
  char domain[12];           // "solana", "bank", "confirm", ...
  uint8_t outcome;           // OUTCOME_* below
  uint8_t reason;            // vk_reason_t
  uint8_t severity;          // 0 green, 1 amber, 2 red
  bool dev;                  // flags bit 0: a dev-build override was used
  uint64_t amount;           // raw units; 0 if not a payment
  uint8_t decimals;
  char symbol[9];
  uint8_t recipient[32];     // device key from the record, else the destination account, else zeros
  char recipient_name[33];
  char app_id[24];           // truncated to 23 characters (display only)
  uint8_t sig[64];           // zeros unless signed
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
constexpr uint16_t FORMAT_VERSION = 1;

// The `outcome` byte of a record. Prefixed constants rather than an enum class, so that none of the
// words can collide with a macro of the Arduino core (as DISABLED and CHANGE do).
constexpr uint8_t OUTCOME_SIGNED = 1;
constexpr uint8_t OUTCOME_CANCELLED = 2;
constexpr uint8_t OUTCOME_TIMEOUT = 3;
constexpr uint8_t OUTCOME_BLOCKED = 4;
constexpr uint8_t OUTCOME_FAILED = 5;
constexpr uint8_t OUTCOME_APPROVED = 6;   // a confirmation that is not a signature

// "signed", "cancelled", "timeout", "blocked", "failed", "approved" (the strings of lua-api.md);
// "?" for any other value.
const char *outcomeName(uint8_t outcome);

// The file's header, read and checked once, so that a reader of many records (wallet.history) does
// not read it again for each one. `count` and `head` are only meaningful after open() returned true.
struct Cursor { uint16_t count; uint16_t head; };
bool open(Cursor &out);                                        // false: no file, or not a valid history file
bool at(const Cursor &cursor, size_t newestFirstIndex, Entry &out);

// What the listener does, in two steps so the host suite can drive each.
// entryFor: the record for one approval outcome. `unix_s` is the time to store (0 = no clock source).
// append: writes it as the newest record. Creates /vk/ and the file when missing; a file with a wrong
// magic or version is renamed to history.bin.bad and a new one started. False when a write failed:
// the records already in the file are still there.
Entry entryFor(const vk::wallet::ApprovalOutcome &outcome, uint32_t unix_s);
bool append(const Entry &entry);

// The time written into each record: unix seconds, or 0 when the clock has no source. On the badge
// it reads vk::clock; in a host suite it is null (records get time 0) until the test sets it.
extern uint32_t (*unixTime)();

}  // namespace vk::history
