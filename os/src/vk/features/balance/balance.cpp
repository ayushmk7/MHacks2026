// src/vk/features/balance/balance.cpp
// The balance poller and the tokenInfoLookup it publishes (ui/ui.md, "Balance").
//
// Only the default (first) token of the token table is tracked. What is stored belongs to the mint it
// was fetched for: if the token table changes so that another mint comes first, the stored values
// count as unknown until the next fetch.
//
// The service's poll does not block the loop. The request runs on a small background task (the one
// task BadgeOS starts; ui.md says why) and the loop only hands it a job and picks up the answer: a
// pass costs a few microseconds whether the node answers in 200 ms or never. After a failed poll the
// next one waits twice as long, up to `balance_max_s`. Everything the rest of the firmware reads (the
// stored balance, the log, the repaint request) is written by the loop, never by the task.
//
// wallet.refresh_balance (lua_balance.cpp) still makes one blocking fetch: an app that asks for a
// balance is waiting for it anyway.
//
// No host suite links this file (it needs the network); the reply scanner, the back-off and the
// freshness rule are in balance.h and are covered by test/host/test_stores.cpp and test_balance.cpp.
#include "balance.h"

#include <Arduino.h>
#include <HTTPClient.h>

#include <atomic>

#include "../../../badge_log.h"
#include "../../../net/net_route.h"
#include "../../../net/wifi_mgr.h"
#include "../../core/config.h"
#include "../../core/service.h"
#include "../../host/home.h"
#include "../../ui/repaint.h"
#include "../../wallet/approval.h"
#include "../../wallet/signer.h"

namespace vk::balance {

namespace {

VK_CONFIG_KEY(balance_poll_s, "balance_poll_s", vk::config::Type::U32, "15", vk::config::F_NONE, 0, 3600,
              "balance poll period in seconds; 0 disables");
VK_CONFIG_KEY(balance_max_s, "balance_max_s", vk::config::Type::U32, "600", vk::config::F_NONE, 15, 3600,
              "longest wait between balance polls after failed ones, seconds");

// How often the service looks at its conditions. Between two looks update() costs one millis().
constexpr uint32_t CHECK_EVERY_MS = 500;

// The background task. A TLS handshake needs most of this stack; it runs on the core the radios use,
// so the loop's core is not shared, at the lowest priority above idle.
constexpr uint32_t TASK_STACK_BYTES = 12288;
constexpr UBaseType_t TASK_PRIORITY = 1;
constexpr BaseType_t TASK_CORE = 0;

bool sStored = false;          // a fetch has succeeded
uint8_t sMint[32];             // the mint it was made for
uint64_t sRaw = 0;
uint8_t sAccount[32];
uint32_t sStoredAtMs = 0;      // when the stored balance was fetched

bool sAttempted = false;       // the service has made at least one poll
uint32_t sNextPollMs = 0;      // when the service may poll again
uint32_t sFailures = 0;        // consecutive failed polls of the service
uint32_t sLastCheckMs = 0;

// ---- the job handed to the task ----------------------------------------------------------------
//
// sPhase says who owns sJob and sResult: the loop while IDLE and DONE, the task while RUNNING.
enum Phase : uint8_t { IDLE, RUNNING, DONE };
std::atomic<uint8_t> sPhase{IDLE};
TaskHandle_t sTask = nullptr;
bool sTaskFailed = false;      // the task could not be created: poll in the loop, with the back-off

struct Request {
  String url;
  String body;
  uint32_t timeoutMs = FETCH_TIMEOUT_MS;
  uint8_t mint[32];
};

struct Answer {
  bool ok = false;
  int status = 0;
  String body;
  String err;
  uint32_t tookMs = 0;
};

Request sJob;
Answer sResult;

// The default token: the first entry of the token table.
bool defaultToken(vk_token_t &out) {
  vk_token_t table[VK_MAX_TOKENS];
  if (vk::config::tokens(table) == 0) return false;
  out = table[0];
  return true;
}

// Joined to a network. connected() alone is also true in hotspot mode.
bool joined() { return ::wifi_mgr::mode() == ::wifi_mgr::Mode::Station && ::wifi_mgr::connected(); }

// vk::wallet::tokenInfoLookup. True only for the default token's mint.
bool lookup(const uint8_t mint[32], vk::wallet::TokenInfo &out) {
  vk_token_t token;
  if (mint == nullptr || !defaultToken(token) || memcmp(mint, token.mint, 32) != 0) return false;
  memset(&out, 0, sizeof out);
  if (sStored && memcmp(sMint, token.mint, 32) == 0) {
    out.balance_known = true;
    out.raw = sRaw;
    out.account_known = true;
    memcpy(out.account, sAccount, sizeof out.account);
  }
  return true;
}

// The request for the default token, built in the loop (it reads the config).
vk::wallet::Reason prepare(Request &out, uint32_t timeoutMs) {
  vk_token_t token;
  if (!defaultToken(token)) return VK_NOT_PROVISIONED;
  const String url = vk::config::text("rpc_url");
  const String owner = vk::wallet::addressBase58();
  if (url.length() == 0 || owner.length() == 0) return VK_NOT_PROVISIONED;
  char mint[SOL_B58_PUBKEY_MAX];
  if (sol_b58_encode(token.mint, sizeof token.mint, mint, sizeof mint) == 0) return VK_NOT_PROVISIONED;

  out.url = url;
  out.body = "";
  out.body.reserve(200);
  out.body += "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTokenAccountsByOwner\",\"params\":[\"";
  out.body += owner;
  out.body += "\",{\"mint\":\"";
  out.body += mint;
  out.body += "\"},{\"encoding\":\"jsonParsed\"}]}";
  out.timeoutMs = timeoutMs;
  memcpy(out.mint, token.mint, sizeof out.mint);
  return VK_OK;
}

// What came back, in the loop: log it and store the balance. Same reasons as fetch().
vk::wallet::Reason apply(const Request &request, const Answer &answer) {
  if (!answer.ok) {
    ::badge_log::tagf("bal", "fetch failed after %lu ms: %s", (unsigned long)answer.tookMs, answer.err.c_str());
    return VK_TIMEOUT;
  }
  ::badge_log::tagf("bal", "fetch %lu ms", (unsigned long)answer.tookMs);   // measurement M5

  uint8_t account[32];
  uint64_t value = 0;
  if (answer.status != 200 || !scanReply(answer.body.c_str(), answer.body.length(), account, value)) {
    return VK_UNSUPPORTED;                                                 // the reply is ignored
  }

  const bool changed = !sStored || value != sRaw || memcmp(account, sAccount, sizeof account) != 0 ||
                       memcmp(request.mint, sMint, sizeof sMint) != 0;
  sStored = true;
  sRaw = value;
  memcpy(sAccount, account, sizeof sAccount);
  memcpy(sMint, request.mint, sizeof sMint);
  sStoredAtMs = millis();
  sFailures = 0;                                                           // whoever fetched it, it is fresh now
  if (changed) vk::ui::requestShellRepaint();
  return VK_OK;
}

// ---- the background task -----------------------------------------------------------------------
//
// It does one thing: the HTTP request of sJob into sResult, the way net_route's Wi-Fi route makes it
// (HTTPClient; for https, TLS without a pinned certificate, as before). It logs nothing and touches
// nothing else: badge_log, the config and the stored balance belong to the loop. It never uses the
// phone bridge, whose pump belongs to the loop: the service polls only while joined to Wi-Fi.
void taskMain(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (sPhase.load(std::memory_order_acquire) != RUNNING) continue;
    Answer answer;
    const uint32_t started = millis();
    {
      HTTPClient http;
      http.setTimeout(sJob.timeoutMs);
      http.setConnectTimeout(sJob.timeoutMs);
      if (!http.begin(sJob.url)) {
        answer.err = "bad url";
      } else {
        http.addHeader("Content-Type", "application/json");
        const int status = http.POST((uint8_t *)sJob.body.c_str(), sJob.body.length());
        if (status <= 0) {
          answer.err = HTTPClient::errorToString(status);
        } else {
          answer.body = http.getString();
          answer.ok = true;
          answer.status = status;
        }
        http.end();
      }
    }
    answer.tookMs = millis() - started;
    sResult = answer;
    sPhase.store(DONE, std::memory_order_release);
  }
}

bool startTask() {
  if (sTask != nullptr) return true;
  if (sTaskFailed) return false;
  if (xTaskCreatePinnedToCore(taskMain, "vk_balance", TASK_STACK_BYTES, nullptr, TASK_PRIORITY, &sTask,
                              TASK_CORE) != pdPASS) {
    sTask = nullptr;
    sTaskFailed = true;
    ::badge_log::tagf("bal", "could not start the poll task: polling in the loop");
    return false;
  }
  return true;
}

// After a poll of the service, whatever its result.
void scheduleNext(vk::wallet::Reason result, uint32_t period_s) {
  if (result != VK_OK) ++sFailures;
  const uint32_t wait = retryDelayMs(period_s, sFailures, vk::config::u32("balance_max_s"));
  if (result != VK_OK && sFailures > 1) {
    ::badge_log::tagf("bal", "%lu polls failed in a row: next in %lu s", (unsigned long)sFailures,
                      (unsigned long)(wait / 1000));
  }
  sAttempted = true;
  sNextPollMs = millis() + wait;
}

void serviceBegin() { vk::wallet::tokenInfoLookup = lookup; }

void serviceUpdate() {
  // An answer from the task is picked up on the first pass after it lands, whatever else holds.
  if (sPhase.load(std::memory_order_acquire) == DONE) {
    const vk::wallet::Reason result = apply(sJob, sResult);
    sResult = Answer();
    sPhase.store(IDLE, std::memory_order_release);
    scheduleNext(result, vk::config::u32("balance_poll_s"));
  }

  const uint32_t now = millis();
  if ((uint32_t)(now - sLastCheckMs) < CHECK_EVERY_MS) return;
  sLastCheckMs = now;

  const uint32_t period_s = vk::config::u32("balance_poll_s");
  if (period_s == 0) return;                                              // 0 disables the poll
  if (sPhase.load(std::memory_order_acquire) != IDLE) return;             // one poll at a time
  if (sAttempted && (int32_t)(now - sNextPollMs) < 0) return;
  if (!vk::host::idle() || vk::wallet::approval::active()) return;        // an app fetches for itself
  if (!joined()) return;                                                  // Wi-Fi down: skip silently

  const vk::wallet::Reason ready = prepare(sJob, FETCH_TIMEOUT_MS);
  if (ready != VK_OK) {                                                   // unprovisioned: nothing to ask
    sAttempted = true;
    sNextPollMs = now + period_s * 1000UL;
    return;
  }
  if (startTask()) {
    sPhase.store(RUNNING, std::memory_order_release);
    xTaskNotifyGive(sTask);
    return;
  }
  // No task: one blocking fetch, as before, but with the back-off after failures.
  scheduleNext(fetch(FETCH_TIMEOUT_MS), period_s);
}

VK_SERVICE(balance, serviceBegin, serviceUpdate);

}  // namespace

bool known() {
  vk_token_t token;
  return sStored && defaultToken(token) && memcmp(sMint, token.mint, 32) == 0;
}

uint64_t raw() { return known() ? sRaw : 0; }

bool tokenAccount(uint8_t out[32]) {
  if (!known()) return false;
  memcpy(out, sAccount, sizeof sAccount);
  return true;
}

Status status() {
  const uint32_t now = millis();
  const bool have = known();
  Status s;
  s.ageMs = have ? (uint32_t)(now - sStoredAtMs) : 0;
  s.failures = sFailures;
  s.polling = sPhase.load(std::memory_order_acquire) != IDLE;
  s.nextPollInMs = (!s.polling && sAttempted && (int32_t)(sNextPollMs - now) > 0) ? sNextPollMs - now : 0;
  s.freshness = freshness(joined(), have, s.ageMs, sFailures, vk::config::u32("balance_poll_s"));
  return s;
}

// One blocking fetch in the loop (wallet.refresh_balance, and the service when it has no task).
vk::wallet::Reason fetch(uint32_t timeoutMs) {
  Request request;
  const vk::wallet::Reason ready = prepare(request, timeoutMs);
  if (ready != VK_OK) return ready;
  if (!::net_route::available()) return VK_TIMEOUT;                        // no route: nothing is sent, nothing is logged

  const uint32_t started = millis();
  const ::net_route::Response response =
      ::net_route::request("POST", request.url, request.body, String("application/json"), timeoutMs);
  Answer answer;
  answer.ok = response.ok;
  answer.status = response.status;
  answer.body = response.body;
  answer.err = response.err;
  answer.tookMs = millis() - started;
  return apply(request, answer);
}

bool refresh(uint32_t timeoutMs) { return fetch(timeoutMs) == VK_OK; }

}  // namespace vk::balance
