// src/native_apps/selftest/suites.h
// One line per suite; each is defined in its own suite_<name>.cpp and listed, in menu order, in
// suites.cpp.
#pragma once

#include "selftest.h"

namespace selftest {
extern const Suite HARDWARE;
extern const Suite WALLET;
extern const Suite CHECKS;
extern const Suite APPROVALS;     // dev profile only (it raises demo approvals)
extern const Suite STORES;
extern const Suite RADIO;
extern const Suite NETWORK;
extern const Suite APPS;
}  // namespace selftest
