// test_balance - the balance poller's pure scheduling (src/vk/features/balance/balance.h; ui/ui.md,
// "Balance"): the wait before the next poll after failures (exponential back-off, capped) and the
// freshness a screen may show next to the balance. No source beyond the header: the poll itself needs
// the network. Run: test/host/run.sh test_balance
//
// What is checked:
//   1. back-off: the period after a success, doubling per failure, the cap, a cap below the period,
//      no overflow after many failures, period 0;
//   2. freshness: offline without a network, none before the first balance, fresh, stale after a
//      failure or when the balance is older than three periods, a poll period of 0.
#include <Arduino.h>

#include <stdio.h>

#include "../../src/vk/features/balance/balance.h"

using namespace vk::balance;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void testBackoff() {
  CHECK(retryDelayMs(15, 0, 600) == 15000u);       // after a success: the period
  CHECK(retryDelayMs(15, 1, 600) == 30000u);       // one failure: twice
  CHECK(retryDelayMs(15, 2, 600) == 60000u);
  CHECK(retryDelayMs(15, 3, 600) == 120000u);
  CHECK(retryDelayMs(15, 5, 600) == 480000u);
  CHECK(retryDelayMs(15, 6, 600) == 600000u);      // capped
  CHECK(retryDelayMs(15, 40, 600) == 600000u);     // no overflow
  CHECK(retryDelayMs(15, 0xFFFFFFFFu, 600) == 600000u);
  CHECK(retryDelayMs(900, 1, 600) == 900000u);     // a cap below the period: never faster than the period
  CHECK(retryDelayMs(3600, 30, 3600) == 3600000u);
  CHECK(retryDelayMs(0, 3, 600) == 0u);            // 0 is "poll off"; the caller does not poll
}

static void testFreshness() {
  // joined, known, ageMs, failures, period_s
  CHECK(freshness(false, true, 1000, 0, 15) == Freshness::OFFLINE);
  CHECK(freshness(false, false, 0, 0, 15) == Freshness::OFFLINE);
  CHECK(freshness(true, false, 0, 0, 15) == Freshness::NONE);
  CHECK(freshness(true, false, 0, 3, 15) == Freshness::NONE);      // never had one: nothing to call stale
  CHECK(freshness(true, true, 1000, 0, 15) == Freshness::FRESH);
  CHECK(freshness(true, true, 45000, 0, 15) == Freshness::FRESH);  // three periods
  CHECK(freshness(true, true, 45001, 0, 15) == Freshness::STALE);  // older than three periods
  CHECK(freshness(true, true, 1000, 1, 15) == Freshness::STALE);   // the last poll failed
  CHECK(freshness(true, true, 9000000, 0, 0) == Freshness::FRESH); // poll off: age alone says nothing
  CHECK(freshness(true, true, 1000, 2, 0) == Freshness::STALE);
  CHECK(strcmp(freshnessName(Freshness::NONE), "none") == 0);
  CHECK(strcmp(freshnessName(Freshness::FRESH), "fresh") == 0);
  CHECK(strcmp(freshnessName(Freshness::STALE), "stale") == 0);
  CHECK(strcmp(freshnessName(Freshness::OFFLINE), "offline") == 0);
}

int main() {
  testBackoff();
  testFreshness();
  if (fails) {
    printf("%d failure(s)\n", fails);
    return 1;
  }
  printf("all balance tests passed\n");
  return 0;
}
