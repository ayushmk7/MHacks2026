// src/native_apps/selftest/suites.cpp
// The suites, in the order of the TESTS menu. RUN ALL follows them on the menu and runs them in
// this order. A new suite is a new suite_<name>.cpp, its line in suites.h and its line here.
#include "suites.h"

namespace selftest {

const Suite *const SUITES[] = {
    &HARDWARE, &WALLET, &CHECKS, &APPROVALS, &STORES, &RADIO, &NETWORK, &APPS,
};
const size_t SUITE_COUNT = sizeof SUITES / sizeof SUITES[0];

}  // namespace selftest
