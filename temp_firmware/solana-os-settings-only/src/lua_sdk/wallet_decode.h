/*
  Wallet input decoders - DEV STAND-IN for A's wallet_decode (P1-A §4.2, 00 §5,
  00 §6). Plain C++ with no Arduino dependency, so tools/wallet_decode_test.cpp
  can run the R6 fuzz set (harness/fuzz-vectors.json) against it on a laptop.

  The rules mirror the reference decoders in dashboard/server/src/fuzz.js; a
  difference between the two is a bug in one of them.
*/
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace wallet_decode {

constexpr size_t MAX_MESSAGE = 1232;  // Solana packet limit
constexpr size_t REQ_FIXED = 62;      // REQ frame header..name_len (00 §5)
constexpr size_t REQ_MAX = REQ_FIXED + 32;

struct Solana {
  uint64_t amount;
  const uint8_t *source;       // 32 B, points into the message
  const uint8_t *destination;  // 32 B, points into the message
  const uint8_t *memo;         // nullptr when there is no memo
  size_t memoLength;
};

struct Bank {
  const char *action;  // "purchase" | "transfer", points into the payload
  size_t actionLength;
  const char *amountCents;
  size_t amountLength;
  const char *payeeName;
  size_t payeeLength;
};

// Each returns nullptr when the input is accepted, else a short detail string
// ("versioned", "mint", ...) for the log. Lua only ever sees the spec's reason.
const char *solana(const uint8_t *msg, size_t length, const uint8_t payer[32],
                   const uint8_t mint[32], uint8_t decimals, Solana *out);
const char *bank(const uint8_t *payload, size_t length, Bank *out);
// The unsigned REQ frame sign_request covers: header..name, payee == `self`.
const char *request(const uint8_t *frame, size_t length, const uint8_t self[32]);

// True when the bytes start with a reserved signing prefix (00 §3).
bool reservedPrefix(const uint8_t *data, size_t length);

// Base58 (Bitcoin/Solana alphabet) to exactly 32 bytes. False on anything else.
bool base58To32(const char *text, uint8_t out[32]);

}  // namespace wallet_decode
