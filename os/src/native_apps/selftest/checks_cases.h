// src/native_apps/selftest/checks_cases.h
// The CHECKS suite's cases: the payment check chain (vk_check_solana, wallet/checks.md) run on
// embedded test vectors, each with the verdict it must give. Plain C++ with no Arduino include:
// the host simulation runs the same cases against the same pure C chain.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace selftest::cases {

using VerifyFn = int (*)(const uint8_t *msg, size_t len, const uint8_t *sig, const uint8_t *pubkey);

size_t count();
const char *name(size_t index);          // serial: "green", "unknown_token", ...
const char *label(size_t index);         // screen: "GREEN", "UNKNOWN TOKEN", ...

// Runs one case. True when the verdict is the expected one. `value` gets what the screen shows:
// the verdict ("red UNKNOWN TOKEN") or, when it differs, what came instead ("got amber ...").
bool run(size_t index, VerifyFn verify, char *value, size_t cap);

}  // namespace selftest::cases
