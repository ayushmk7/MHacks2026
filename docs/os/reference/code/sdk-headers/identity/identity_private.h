// src/identity/identity_private.h
// May be included ONLY by identity/identity.cpp and wallet/wallet.cpp.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace wallet { class Gate; }

namespace identity {

// Passkey: a SignToken can be created only inside wallet::Gate.
// The constructor is user-provided (an empty body, not "= default"). A class with a user-provided
// constructor is not an aggregate in any C++ standard, so both `SignToken{}` and `SignToken()` need
// the private constructor and fail to compile outside the gate. With "= default" the class would be
// an aggregate under C++14 and C++17 and `SignToken{}` would compile anywhere.
// Holds for C++11, C++14, C++17, C++20 and C++23; no assumption about the Arduino core's -std flag.
class SignToken {
  SignToken() {}
  SignToken(const SignToken &) = delete;
  SignToken &operator=(const SignToken &) = delete;
  friend class wallet::Gate;
};

// Ed25519 over `length` bytes with the badge key, 64 bytes into `out`. SE050 or software, per source().
// False if the identity is not ready or the key backend failed.
bool signGated(const SignToken &, const uint8_t *message, size_t length, uint8_t out[64]);

}  // namespace identity
