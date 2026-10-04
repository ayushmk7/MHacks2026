// src/native_apps/selftest/suite_checks.cpp
// CHECKS: the payment check chain on embedded vectors, on the badge itself. One row per case of
// checks_cases.cpp (the cases are listed there); a verdict other than the expected one is FAIL.
// One case per frame: a case verifies at most two signatures (about 18 ms each with Monocypher).
#include <stdio.h>

#include "../../vk/wallet/crypto.h"    // vk_verify_c
#include "checks_cases.h"
#include "selftest.h"
#include "suites.h"

namespace selftest {
namespace {

void runCase(Ctx &c) {
  char value[ROW_VALUE_MAX];
  const bool same = cases::run(c.index, vk_verify_c, value, sizeof value);
  c.finish(same ? State::Ok : State::Fail, "%s", value);
}

size_t caseRows() { return cases::count(); }

void caseName(size_t index, char *name, size_t nameCap, char *label, size_t labelCap) {
  snprintf(name, nameCap, "%s", cases::name(index));
  snprintf(label, labelCap, "%s", cases::label(index));
}

const Check TABLE[] = {
    {"", "", Kind::Auto, Profile::Any, runCase, nullptr, 0},     // every row: one case
};

}  // namespace

const Suite CHECKS = {
    "checks", "CHECKS", Profile::Any, TABLE, 1, caseRows, caseName, nullptr, false,
};

}  // namespace selftest
