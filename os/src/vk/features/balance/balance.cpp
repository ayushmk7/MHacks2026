// src/vk/features/balance/balance.cpp
// The balance poller, its status item and the tokenInfoLookup it publishes (ui/ui.md, "Balance").
//
// Only the default (first) token of the token table is tracked. What is stored belongs to the mint it
// was fetched for: if the token table changes so that another mint comes first, the stored values
// count as unknown until the next fetch.
//
// No host suite links this file (it needs the network); the reply scanner is in balance.h and is
// covered by test/host/test_stores.cpp.
#include "balance.h"

#include <Arduino.h>

#include "../../../badge_log.h"
#include "../../../hal/display.h"
#include "../../../net/net_route.h"
#include "../../../net/wifi_mgr.h"
#include "../../../ui/theme.h"        // upstream's palette, ::theme::
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

// How often the service looks at its conditions. Between two looks update() costs one millis().
constexpr uint32_t CHECK_EVERY_MS = 500;

bool sStored = false;          // a fetch has succeeded
uint8_t sMint[32];             // the mint it was made for
uint64_t sRaw = 0;
uint8_t sAccount[32];

bool sAttempted = false;       // the service has made at least one fetch
uint32_t sLastAttemptMs = 0;   // when its last fetch ended
uint32_t sLastCheckMs = 0;

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

void serviceBegin() { vk::wallet::tokenInfoLookup = lookup; }

void serviceUpdate() {
  const uint32_t now = millis();
  if ((uint32_t)(now - sLastCheckMs) < CHECK_EVERY_MS) return;
  sLastCheckMs = now;

  const uint32_t period_s = vk::config::u32("balance_poll_s");
  if (period_s == 0) return;                                              // 0 disables the poll
  if (sAttempted && (uint32_t)(now - sLastAttemptMs) < period_s * 1000UL) return;
  if (!vk::host::idle() || vk::wallet::approval::active()) return;        // an app fetches for itself
  if (!joined()) return;                                                  // Wi-Fi down: skip silently

  fetch(FETCH_TIMEOUT_MS);
  // The period runs from the end of the fetch, whatever its result, so a node that does not answer
  // stalls the launcher once per period and not continuously.
  sAttempted = true;
  sLastAttemptMs = millis();
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

vk::wallet::Reason fetch(uint32_t timeoutMs) {
  vk_token_t token;
  if (!defaultToken(token)) return VK_NOT_PROVISIONED;
  const String url = vk::config::text("rpc_url");
  const String owner = vk::wallet::addressBase58();
  if (url.length() == 0 || owner.length() == 0) return VK_NOT_PROVISIONED;
  char mint[SOL_B58_PUBKEY_MAX];
  if (sol_b58_encode(token.mint, sizeof token.mint, mint, sizeof mint) == 0) return VK_NOT_PROVISIONED;
  if (!::net_route::available()) return VK_TIMEOUT;                        // no route: nothing is sent, nothing is logged

  String body;
  body.reserve(200);
  body += "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTokenAccountsByOwner\",\"params\":[\"";
  body += owner;
  body += "\",{\"mint\":\"";
  body += mint;
  body += "\"},{\"encoding\":\"jsonParsed\"}]}";

  const uint32_t started = millis();
  const ::net_route::Response response =
      ::net_route::request("POST", url, body, String("application/json"), timeoutMs);
  const unsigned long took = (unsigned long)(millis() - started);
  if (!response.ok) {
    ::badge_log::tagf("bal", "fetch failed after %lu ms: %s", took, response.err.c_str());
    return VK_TIMEOUT;
  }
  ::badge_log::tagf("bal", "fetch %lu ms", took);                          // measurement M5

  uint8_t account[32];
  uint64_t value = 0;
  if (response.status != 200 || !scanReply(response.body.c_str(), response.body.length(), account, value)) {
    return VK_UNSUPPORTED;                                                 // the reply is ignored
  }

  const bool changed = !sStored || value != sRaw || memcmp(account, sAccount, sizeof account) != 0 ||
                       memcmp(token.mint, sMint, sizeof sMint) != 0;
  sStored = true;
  sRaw = value;
  memcpy(sAccount, account, sizeof sAccount);
  memcpy(sMint, token.mint, sizeof sMint);
  if (changed) vk::ui::requestShellRepaint();
  return VK_OK;
}

bool refresh(uint32_t timeoutMs) { return fetch(timeoutMs) == VK_OK; }

}  // namespace vk::balance
