// LINK: src/vk/core/config.cpp
// test_config - the config store (docs/os/platform/config.md): the text parsers (TOKENS, KEY32,
// U32, STR), the store on the in-memory NVS of the shim, provisioning, the secure-change and reset
// confirmations, and the serial commands with their exact replies.
#include <Arduino.h>
#include <Preferences.h>

#include <string>
#include <vector>

#include "../../src/vk/core/config.h"
#include "../../src/vk/core/serial.h"
#include "vectors.h"

using namespace vk::config;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// ---- keys other modules own in the firmware; registered here so the store can be exercised -----
// (core/config.cpp itself registers rpc_url, listener_url and display_name.)
VK_CONFIG_KEY(issuer_key, "issuer_key", Type::KEY32, nullptr, F_SECURE | F_REQUIRED, 0, 0, "test issuer key");
VK_CONFIG_KEY(tokens, "tokens", Type::TOKENS, nullptr, F_SECURE | F_REQUIRED, 0, 0, "test token table");
VK_CONFIG_KEY(approval_tmo_s, "approval_tmo_s", Type::U32, "45", F_SECURE, 10, 120, "test approval timeout");
VK_CONFIG_KEY(req_ttl_s, "req_ttl_s", Type::U32, "60", F_NONE, 10, 600, "test request lifetime");
VK_CONFIG_KEY(home_app, "home_app", Type::STR, "launcher", F_NONE, 0, 32, "test home app");

// ---- fixtures ------------------------------------------------------------------------------------
static std::string b58(const uint8_t *bytes, size_t len) {
  char out[96];
  const size_t n = sol_b58_encode(bytes, len, out, sizeof out);
  return std::string(out, n);
}

static std::string MINT, ALT_MINT, DST;       // three distinct base58 addresses
static std::string ONE;                       // "<MINT>:2:HACK:100.00:1000.00"

static void fixtures() {
  MINT = b58(V_MINT, 32);
  ALT_MINT = b58(V_ALT_MINT, 32);
  DST = b58(V_DST, 32);
  ONE = MINT + ":2:HACK:100.00:1000.00";
}

// A fresh badge: empty NVS, nothing in RAM, no approval engine.
static void fresh() {
  vk_host_nvs_reset();
  hostReboot();
  confirmChange = nullptr;
  hostWifiSsid = "";
  hostWifiPassword = "";
  hostAutostart = "";
  hostRepaints = 0;
}

// Fake approval engine behind confirmChange.
static int cc_calls;
static bool cc_accept;
static std::string cc_key, cc_old, cc_new;
static void (*cc_done)(bool);
static bool fakeConfirm(const char *key, const char *oldText, const char *newText, void (*done)(bool approved)) {
  cc_calls++;
  if (!cc_accept) return false;
  cc_key = key;
  cc_old = oldText;
  cc_new = newText;
  cc_done = done;
  return true;
}
static void engine() {
  confirmChange = fakeConfirm;
  cc_calls = 0;
  cc_accept = true;
  cc_key = cc_old = cc_new = "";
  cc_done = nullptr;
}

static int resets_seen;
static void onReset() { resets_seen++; }
VK_ON_RESET(test_listener, onReset);

// Sets every required key and commits.
static void provision() {
  CHECK(set("issuer_key", B58_ISSUER) == SetResult::OK);
  CHECK(set("tokens", ONE.c_str()) == SetResult::OK);
  CHECK(set("rpc_url", "http://127.0.0.1:8899") == SetResult::OK);
  String missing;
  CHECK(commit(missing));
  CHECK(provisioned());
}

// Runs a registered serial command by name; returns every reply line.
static std::vector<std::string> run(const char *name, const char *args) {
  std::vector<std::string> lines;
  for (auto *command = vk::serial::SerialCommand::first(); command; command = command->next()) {
    if (strcmp(command->name, name) != 0) continue;
    command->fn(String(args), [&lines](const String &line) { lines.push_back(line.c_str()); });
    return lines;
  }
  printf("FAIL no serial command %s is registered\n", name);
  fails++;
  return lines;
}
static std::string last(const char *name, const char *args) {
  const std::vector<std::string> lines = run(name, args);
  return lines.empty() ? std::string("(no reply)") : lines.back();
}

// ---- parsers -------------------------------------------------------------------------------------
static void test_tokens_parser() {
  vk_token_t table[VK_MAX_TOKENS];
  size_t count = 99;

  // one entry
  CHECK(parseTokens(ONE.c_str(), table, &count));
  CHECK(count == 1);
  CHECK(memcmp(table[0].mint, V_MINT, 32) == 0);
  CHECK(table[0].decimals == 2);
  CHECK(strcmp(table[0].symbol, "HACK") == 0);
  CHECK(table[0].cap == 10000);
  CHECK(table[0].max == 100000);

  // three entries, in order; 0 = no cap, 0 = no max; 9 decimals; a one-character symbol
  const std::string three = ONE + "," + ALT_MINT + ":0:X:0:0," + DST + ":9:SOL9:1.5:0";
  CHECK(parseTokens(three.c_str(), table, &count));
  CHECK(count == 3);
  CHECK(memcmp(table[0].mint, V_MINT, 32) == 0 && strcmp(table[0].symbol, "HACK") == 0);
  CHECK(memcmp(table[1].mint, V_ALT_MINT, 32) == 0);
  CHECK(table[1].decimals == 0 && strcmp(table[1].symbol, "X") == 0 && table[1].cap == 0 && table[1].max == 0);
  CHECK(memcmp(table[2].mint, V_DST, 32) == 0);
  CHECK(table[2].decimals == 9 && strcmp(table[2].symbol, "SOL9") == 0);
  CHECK(table[2].cap == 1500000000ull && table[2].max == 0);

  // out-parameters are optional
  CHECK(parseTokens(ONE.c_str(), nullptr, nullptr));

  // a failed parse leaves the out-parameters alone
  count = 7;
  CHECK(!parseTokens("garbage", table, &count));
  CHECK(count == 7);

  // bad mint
  CHECK(!parseTokens("notbase58:2:HACK:100.00:1000.00", nullptr, nullptr));
  CHECK(!parseTokens((MINT.substr(0, 20) + ":2:HACK:100.00:1000.00").c_str(), nullptr, nullptr));   // too short
  CHECK(!parseTokens((MINT + "1:2:HACK:100.00:1000.00").c_str(), nullptr, nullptr));                // 33 bytes
  CHECK(!parseTokens(("0" + MINT.substr(1) + ":2:HACK:100.00:1000.00").c_str(), nullptr, nullptr)); // '0' is not base58
  CHECK(!parseTokens(":2:HACK:100.00:1000.00", nullptr, nullptr));

  // bad decimals
  CHECK(!parseTokens((MINT + ":10:HACK:100.00:1000.00").c_str(), nullptr, nullptr));
  CHECK(!parseTokens((MINT + ":x:HACK:100.00:1000.00").c_str(), nullptr, nullptr));
  CHECK(!parseTokens((MINT + "::HACK:100.00:1000.00").c_str(), nullptr, nullptr));
  CHECK(!parseTokens((MINT + ":-1:HACK:100.00:1000.00").c_str(), nullptr, nullptr));

  // bad symbol
  CHECK(!parseTokens((MINT + ":2:hack:100.00:1000.00").c_str(), nullptr, nullptr));     // lower case
  CHECK(!parseTokens((MINT + ":2:HACKS:100.00:1000.00").c_str(), nullptr, nullptr));    // 5 characters
  CHECK(!parseTokens((MINT + ":2::100.00:1000.00").c_str(), nullptr, nullptr));         // empty
  CHECK(!parseTokens((MINT + ":2:HA K:100.00:1000.00").c_str(), nullptr, nullptr));
  CHECK(!parseTokens((MINT + ":2:H$:100.00:1000.00").c_str(), nullptr, nullptr));

  // bad amounts
  CHECK(!parseTokens((MINT + ":2:HACK:100.001:1000.00").c_str(), nullptr, nullptr));    // 3 fraction digits, 2 decimals
  CHECK(!parseTokens((MINT + ":2:HACK:abc:1000.00").c_str(), nullptr, nullptr));
  CHECK(!parseTokens((MINT + ":2:HACK::1000.00").c_str(), nullptr, nullptr));
  CHECK(!parseTokens((MINT + ":2:HACK:100.00:").c_str(), nullptr, nullptr));
  CHECK(!parseTokens((MINT + ":2:HACK:-1:1000.00").c_str(), nullptr, nullptr));

  // cap > max refused; cap == max, and a cap with no max, accepted
  CHECK(!parseTokens((MINT + ":2:HACK:1000.01:1000.00").c_str(), nullptr, nullptr));
  CHECK(parseTokens((MINT + ":2:HACK:1000.00:1000.00").c_str(), nullptr, nullptr));
  CHECK(parseTokens((MINT + ":2:HACK:5000:0").c_str(), nullptr, nullptr));
  CHECK(parseTokens((MINT + ":2:HACK:0:10").c_str(), table, &count));
  CHECK(count == 1 && table[0].cap == 0 && table[0].max == 1000);

  // wrong number of fields
  CHECK(!parseTokens((MINT + ":2:HACK:100.00").c_str(), nullptr, nullptr));
  CHECK(!parseTokens((MINT + ":2:HACK:100.00:1000.00:7").c_str(), nullptr, nullptr));

  // entry count: 1..VK_MAX_TOKENS
  const std::string four = three + "," + b58(V_SRC, 32) + ":2:FOUR:0:0";
  const std::string five = four + "," + b58(V_DST_OWNER, 32) + ":2:FIVE:0:0";
  CHECK(!parseTokens(four.c_str(), nullptr, nullptr));
  CHECK(!parseTokens(five.c_str(), nullptr, nullptr));
  CHECK(!parseTokens("", nullptr, nullptr));
  CHECK(!parseTokens(nullptr, nullptr, nullptr));
  CHECK(!parseTokens((ONE + ",").c_str(), nullptr, nullptr));        // empty last entry
  CHECK(!parseTokens(("," + ONE).c_str(), nullptr, nullptr));        // empty first entry
  CHECK(!parseTokens((ONE + ",," + ONE).c_str(), nullptr, nullptr));
  CHECK(!parseTokens((" " + ONE).c_str(), nullptr, nullptr));        // no blanks anywhere
  CHECK(!parseTokens((ONE + " ").c_str(), nullptr, nullptr));
}

static void test_key32_parser() {
  uint8_t out[32];
  memset(out, 0xEE, sizeof out);
  CHECK(parseKey32(B58_ISSUER, out));
  CHECK(memcmp(out, V_ISSUER_PUB, 32) == 0);
  CHECK(parseKey32(B58_ISSUER, nullptr));

  // 32 zero bytes are 32 '1' characters
  CHECK(parseKey32("11111111111111111111111111111111", out));
  const uint8_t zeros[32] = {0};
  CHECK(memcmp(out, zeros, 32) == 0);

  // wrong length: 31 and 33 bytes
  CHECK(!parseKey32(b58(V_ISSUER_PUB, 31).c_str(), nullptr));
  uint8_t longer[33];
  memcpy(longer, V_ISSUER_PUB, 32);
  longer[32] = 7;
  CHECK(!parseKey32(b58(longer, 33).c_str(), nullptr));

  // characters outside the alphabet; blanks; empty; null
  std::string bad = B58_ISSUER;
  bad[5] = '0';
  CHECK(!parseKey32(bad.c_str(), nullptr));
  bad[5] = 'O';
  CHECK(!parseKey32(bad.c_str(), nullptr));
  bad[5] = 'l';
  CHECK(!parseKey32(bad.c_str(), nullptr));
  CHECK(!parseKey32((std::string(B58_ISSUER) + " ").c_str(), nullptr));
  CHECK(!parseKey32("", nullptr));
  CHECK(!parseKey32(nullptr, nullptr));

  // a failed parse leaves `out` alone
  memset(out, 0xEE, sizeof out);
  CHECK(!parseKey32("garbage", out));
  CHECK(out[0] == 0xEE && out[31] == 0xEE);
}

static void test_u32_parser() {
  uint32_t value = 77;
  CHECK(parseU32("45", 10, 120, &value) && value == 45);
  CHECK(parseU32("10", 10, 120, &value) && value == 10);        // the range includes both ends
  CHECK(parseU32("120", 10, 120, &value) && value == 120);
  CHECK(parseU32("0", 0, 3600, &value) && value == 0);
  CHECK(parseU32("4294967295", 0, 0xFFFFFFFFu, &value) && value == 0xFFFFFFFFu);
  CHECK(parseU32("12", 10, 120, nullptr));

  value = 77;
  CHECK(!parseU32("9", 10, 120, &value));
  CHECK(!parseU32("121", 10, 120, &value));
  CHECK(!parseU32("4294967296", 0, 0xFFFFFFFFu, &value));       // one past the largest
  CHECK(!parseU32("99999999999", 0, 0xFFFFFFFFu, &value));
  CHECK(!parseU32("", 0, 100, &value));
  CHECK(!parseU32(nullptr, 0, 100, &value));
  CHECK(!parseU32("-1", 0, 100, &value));
  CHECK(!parseU32("+1", 0, 100, &value));
  CHECK(!parseU32(" 12", 0, 100, &value));
  CHECK(!parseU32("12 ", 0, 100, &value));
  CHECK(!parseU32("1.5", 0, 100, &value));
  CHECK(!parseU32("0x10", 0, 100, &value));
  CHECK(!parseU32("garbage", 0, 100, &value));
  CHECK(value == 77);                                           // untouched by every failure
}

static void test_str_parser() {
  CHECK(parseStr("https://api.devnet.solana.com", 8, 128));
  CHECK(parseStr("", 0, 32));
  CHECK(!parseStr("", 1, 32));
  CHECK(parseStr("12345678", 8, 128));                          // exactly the minimum
  CHECK(!parseStr("1234567", 8, 128));
  CHECK(!parseStr("x", 8, 128));
  CHECK(parseStr(std::string(128, 'a').c_str(), 8, 128));       // exactly the maximum
  CHECK(!parseStr(std::string(129, 'a').c_str(), 8, 128));
  CHECK(parseStr("My Hotspot ~!", 0, 32));                      // space and '~' are printable ASCII
  CHECK(!parseStr("tab\there", 0, 32));
  CHECK(!parseStr("line\n", 0, 32));
  CHECK(!parseStr("del\x7f", 0, 32));
  CHECK(!parseStr("caf\xc3\xa9", 0, 32));                       // UTF-8 beyond ASCII
  CHECK(!parseStr(nullptr, 0, 32));
}

static void test_validate() {
  CHECK(find("rpc_url") != nullptr && find("listener_url") != nullptr && find("display_name") != nullptr);
  CHECK(find("no_such_key") == nullptr && find(nullptr) == nullptr && find("") == nullptr);

  const ConfigKey *rpc = find("rpc_url");
  CHECK(rpc->type == Type::STR && rpc->flags == F_REQUIRED && rpc->min == 8 && rpc->max == 128 && rpc->def == nullptr);
  const ConfigKey *listener = find("listener_url");
  CHECK(listener->type == Type::STR && listener->flags == F_NONE && listener->min == 0 && listener->max == 128);
  CHECK(listener->def != nullptr && listener->def[0] == '\0');
  const ConfigKey *display = find("display_name");
  CHECK(display->type == Type::STR && display->flags == F_NONE && display->min == 0 && display->max == 32);
  CHECK(display->def != nullptr && display->def[0] == '\0');
  for (auto *key = ConfigKey::first(); key; key = key->next()) CHECK(strlen(key->name) <= 15);   // NVS key names

  CHECK(validate(*rpc, "http://127.0.0.1:8899"));
  CHECK(!validate(*rpc, "x"));
  CHECK(!validate(*rpc, nullptr));
  CHECK(validate(*find("approval_tmo_s"), "12"));
  CHECK(!validate(*find("approval_tmo_s"), "9"));
  CHECK(validate(*find("issuer_key"), B58_ISSUER));
  CHECK(!validate(*find("issuer_key"), "garbage"));
  CHECK(validate(*find("tokens"), ONE.c_str()));
  CHECK(!validate(*find("tokens"), "garbage"));
}

// ---- the store -----------------------------------------------------------------------------------
static void test_defaults() {
  fresh();
  // No begin() call: the accessors open the store themselves (the boot screen reads the theme
  // before vk::begin()).
  CHECK(!provisioned());
  CHECK(text("rpc_url") == "");                 // no default
  CHECK(text("listener_url") == "");
  CHECK(text("home_app") == "launcher");
  CHECK(text("approval_tmo_s") == "45");
  CHECK(u32("approval_tmo_s") == 45);
  CHECK(u32("req_ttl_s") == 60);
  CHECK(u32("rpc_url") == 0);                   // not a U32 key
  CHECK(text("no_such_key") == "" && u32("no_such_key") == 0);
  uint8_t key[32];
  CHECK(!key32("issuer_key", key));
  CHECK(!key32("no_such_key", key));
  vk_token_t table[VK_MAX_TOKENS];
  CHECK(tokens(table) == 0);
  begin();
  begin();                                      // idempotent
  CHECK(!provisioned());
}

static void test_round_trip() {
  fresh();
  begin();
  CHECK(set("rpc_url", "http://127.0.0.1:8899") == SetResult::OK);
  CHECK(text("rpc_url") == "http://127.0.0.1:8899");
  CHECK(set("req_ttl_s", "120") == SetResult::OK);
  CHECK(u32("req_ttl_s") == 120 && text("req_ttl_s") == "120");
  CHECK(set("req_ttl_s", "600") == SetResult::OK);      // a write clears the cache
  CHECK(u32("req_ttl_s") == 600);

  // unprovisioned: secure keys are written without a confirmation
  CHECK(set("approval_tmo_s", "12") == SetResult::OK);
  CHECK(u32("approval_tmo_s") == 12);
  CHECK(set("issuer_key", B58_ISSUER) == SetResult::OK);
  uint8_t key[32];
  CHECK(key32("issuer_key", key) && memcmp(key, V_ISSUER_PUB, 32) == 0);
  CHECK(!key32("rpc_url", key));                        // not a KEY32 key

  const std::string two = ONE + "," + ALT_MINT + ":0:PTS:0:500";
  CHECK(set("tokens", two.c_str()) == SetResult::OK);
  vk_token_t table[VK_MAX_TOKENS];
  CHECK(tokens(table) == 2);
  CHECK(memcmp(table[0].mint, V_MINT, 32) == 0 && strcmp(table[0].symbol, "HACK") == 0 && table[0].cap == 10000);
  CHECK(memcmp(table[1].mint, V_ALT_MINT, 32) == 0 && strcmp(table[1].symbol, "PTS") == 0 && table[1].max == 500);
  CHECK(tokens(nullptr) == 2);
  CHECK(set("tokens", ONE.c_str()) == SetResult::OK);   // the parsed table is cached; a write clears it
  CHECK(tokens(table) == 1);

  // an empty value is a value: it replaces a non-empty default, and survives
  CHECK(set("home_app", "") == SetResult::OK);
  CHECK(text("home_app") == "");
  CHECK(set("display_name", "Merch Booth 1") == SetResult::OK);
  CHECK(text("display_name") == "Merch Booth 1");

  // refusals change nothing
  CHECK(set("no_such_key", "1") == SetResult::UNKNOWN_KEY);
  CHECK(set(nullptr, "1") == SetResult::UNKNOWN_KEY);
  CHECK(set("rpc_url", "x") == SetResult::INVALID);
  CHECK(set("rpc_url", nullptr) == SetResult::INVALID);
  CHECK(set("req_ttl_s", "9") == SetResult::INVALID);
  CHECK(set("req_ttl_s", "garbage") == SetResult::INVALID);
  CHECK(set("tokens", "garbage") == SetResult::INVALID);
  CHECK(set("issuer_key", "garbage") == SetResult::INVALID);
  CHECK(text("rpc_url") == "http://127.0.0.1:8899" && u32("req_ttl_s") == 600 && tokens(table) == 1);

  // everything survives a reboot
  hostReboot();
  CHECK(text("rpc_url") == "http://127.0.0.1:8899");
  CHECK(u32("req_ttl_s") == 600);
  CHECK(u32("approval_tmo_s") == 12);
  CHECK(key32("issuer_key", key) && memcmp(key, V_ISSUER_PUB, 32) == 0);
  CHECK(tokens(table) == 1 && memcmp(table[0].mint, V_MINT, 32) == 0);
  CHECK(text("home_app") == "");
  CHECK(text("display_name") == "Merch Booth 1");
  CHECK(!provisioned());                                // no VKCOMMIT yet

  // the values are text in the NVS namespace "vkconf"
  Preferences raw;
  CHECK(raw.begin("vkconf", true));
  CHECK(raw.getString("rpc_url", "?") == "http://127.0.0.1:8899");
  CHECK(raw.getString("req_ttl_s", "?") == "600");
  CHECK(raw.getString("home_app", "?") == "");
  raw.end();
}

static void test_storage_failure() {
  fresh();
  CHECK(set("req_ttl_s", "120") == SetResult::OK);

  vk_host_nvs_fail_writes = 1;
  CHECK(set("req_ttl_s", "300") == SetResult::STORAGE);
  CHECK(vk_host_nvs_fail_writes == 0);
  CHECK(u32("req_ttl_s") == 120);                       // the old value is still there
  CHECK(set("req_ttl_s", "300") == SetResult::OK);      // and the next write works
  CHECK(u32("req_ttl_s") == 300);

  // a key never written
  vk_host_nvs_fail_writes = 1;
  CHECK(set("rpc_url", "http://127.0.0.1:8899") == SetResult::STORAGE);
  CHECK(text("rpc_url") == "");

  // an empty value: putString reports 0 bytes for it whether or not it was stored
  vk_host_nvs_fail_writes = 1;
  CHECK(set("home_app", "") == SetResult::STORAGE);
  CHECK(text("home_app") == "launcher");
  CHECK(set("home_app", "pay") == SetResult::OK);
  vk_host_nvs_fail_writes = 1;
  CHECK(set("home_app", "") == SetResult::STORAGE);
  CHECK(text("home_app") == "pay");
  CHECK(set("home_app", "") == SetResult::OK);
  CHECK(text("home_app") == "");

  // the serial command answers ERR nvs_full
  vk_host_nvs_fail_writes = 1;
  CHECK(last("VKSET", "req_ttl_s 400") == "ERR nvs_full");
  CHECK(u32("req_ttl_s") == 300);

  // a full NVS at VKCOMMIT
  fresh();
  CHECK(set("issuer_key", B58_ISSUER) == SetResult::OK);
  CHECK(set("tokens", ONE.c_str()) == SetResult::OK);
  CHECK(set("rpc_url", "http://127.0.0.1:8899") == SetResult::OK);
  vk_host_nvs_fail_writes = 1;
  String missing = "x";
  CHECK(!commit(missing));
  CHECK(missing == "");                                 // nothing is missing; the write failed
  CHECK(!provisioned());
  vk_host_nvs_fail_writes = 1;
  CHECK(last("VKCOMMIT", "") == "ERR nvs_full");
  CHECK(!provisioned());
  CHECK(last("VKCOMMIT", "") == "OK provisioned");
}

static void test_stored_garbage_is_ignored() {
  fresh();
  // A value that no longer validates (written by an older firmware, or a narrowed range) reads as
  // the default; a secure setting can never be taken from a value VKSET would have refused.
  Preferences raw;
  CHECK(raw.begin("vkconf", false));
  CHECK(raw.putString("approval_tmo_s", "5") == 1);
  CHECK(raw.putString("tokens", "garbage") == 7);
  CHECK(raw.putUInt("req_ttl_s", 99) == 4);             // not text at all
  raw.end();
  CHECK(u32("approval_tmo_s") == 45 && text("approval_tmo_s") == "45");
  CHECK(u32("req_ttl_s") == 60);
  vk_token_t table[VK_MAX_TOKENS];
  CHECK(tokens(table) == 0);
  String missing;
  CHECK(!commit(missing));                              // and a required key in that state counts as missing
}

static void test_commit() {
  fresh();
  String missing = "x";
  // Several keys are missing: the one reported is the first by name, so the reply is stable.
  CHECK(!commit(missing) && missing == "issuer_key");
  CHECK(!provisioned());
  CHECK(set("issuer_key", B58_ISSUER) == SetResult::OK);
  CHECK(!commit(missing) && missing == "rpc_url");
  CHECK(set("rpc_url", "http://127.0.0.1:8899") == SetResult::OK);
  CHECK(!commit(missing) && missing == "tokens");
  CHECK(last("VKCOMMIT", "") == "ERR missing tokens");
  CHECK(!provisioned());
  CHECK(hostRepaints == 0);
  CHECK(set("tokens", ONE.c_str()) == SetResult::OK);

  missing = "x";
  CHECK(commit(missing) && missing == "");
  CHECK(provisioned());
  CHECK(hostRepaints == 1);                             // the launcher's SETUP NEEDED row goes away
  CHECK(commit(missing));                               // again: still fine, nothing new to draw
  CHECK(hostRepaints == 1);

  hostReboot();
  CHECK(provisioned());                                 // survives a reboot
}

static void test_secure_change() {
  fresh();
  provision();

  // No approval engine: a secure change cannot be confirmed.
  CHECK(set("approval_tmo_s", "12") == SetResult::UNAVAILABLE);
  CHECK(u32("approval_tmo_s") == 45);
  CHECK(last("VKSET", "approval_tmo_s 12") == "ERR unavailable");
  // Validation comes first, and non-secure keys never need the engine.
  CHECK(set("approval_tmo_s", "5") == SetResult::INVALID);
  CHECK(set("req_ttl_s", "90") == SetResult::OK);
  CHECK(set("rpc_url", "https://api.devnet.solana.com") == SetResult::OK);
  CHECK(text("rpc_url") == "https://api.devnet.solana.com");

  // With the engine: PENDING, and the write happens on approval.
  engine();
  CHECK(set("approval_tmo_s", "12") == SetResult::PENDING);
  CHECK(cc_calls == 1 && cc_key == "approval_tmo_s" && cc_old == "45" && cc_new == "12");
  CHECK(u32("approval_tmo_s") == 45);                   // not yet
  cc_done(true);
  CHECK(u32("approval_tmo_s") == 12);
  cc_done(true);                                        // a stray second call does nothing
  CHECK(u32("approval_tmo_s") == 12);

  // Declined: unchanged.
  CHECK(set("approval_tmo_s", "14") == SetResult::PENDING);
  CHECK(cc_calls == 2 && cc_old == "12" && cc_new == "14");
  cc_done(false);
  CHECK(u32("approval_tmo_s") == 12);
  cc_done(true);                                        // the declined change is gone, not parked
  CHECK(u32("approval_tmo_s") == 12);

  // An invalid value never reaches the screen.
  CHECK(set("approval_tmo_s", "121") == SetResult::INVALID);
  CHECK(set("tokens", "garbage") == SetResult::INVALID);
  CHECK(cc_calls == 2);

  // The engine cannot raise it (an approval is already open): UNAVAILABLE, and the change that
  // is waiting on the screen is still the one that gets written.
  CHECK(set("approval_tmo_s", "20") == SetResult::PENDING);
  cc_accept = false;
  CHECK(set("approval_tmo_s", "30") == SetResult::UNAVAILABLE);
  CHECK(last("VKSET", (std::string("issuer_key ") + B58_DEVICE).c_str()) == "ERR unavailable");
  cc_accept = true;
  cc_done(true);
  CHECK(u32("approval_tmo_s") == 20);
  uint8_t key[32];
  CHECK(key32("issuer_key", key) && memcmp(key, V_ISSUER_PUB, 32) == 0);

  // KEY32 and TOKENS changes carry their full text; the screen shortens it.
  CHECK(set("issuer_key", B58_DEVICE) == SetResult::PENDING);
  CHECK(cc_key == "issuer_key" && cc_old == B58_ISSUER && cc_new == B58_DEVICE);
  cc_done(true);
  CHECK(key32("issuer_key", key) && memcmp(key, V_DEVICE_PUB, 32) == 0);

  // Approved, but NVS refuses the write: the old value stays.
  CHECK(set("approval_tmo_s", "33") == SetResult::PENDING);
  vk_host_nvs_fail_writes = 1;
  cc_done(true);
  CHECK(u32("approval_tmo_s") == 20);

  // The serial replies.
  CHECK(last("VKSET", "approval_tmo_s 12") == "OK pending");
  cc_done(true);
  CHECK(last("VKGET", "approval_tmo_s") == "OK 12");
  CHECK(last("VKSET", "req_ttl_s 75") == "OK");

  hostReboot();
  CHECK(provisioned() && u32("approval_tmo_s") == 12);
}

static void test_reset() {
  fresh();
  provision();
  CHECK(set("display_name", "Merch") == SetResult::OK);
  resets_seen = 0;

  // No engine: nothing happens.
  requestReset();
  CHECK(provisioned());
  CHECK(last("VKRESET", "") == "ERR unavailable");

  // The reset is a confirmation raised through confirmChange with the key name "(reset)".
  engine();
  requestReset();
  CHECK(cc_calls == 1 && cc_key == "(reset)" && cc_old == "" && cc_new == "");
  CHECK(provisioned() && text("display_name") == "Merch");     // nothing until it is approved
  cc_done(false);
  CHECK(provisioned() && text("display_name") == "Merch" && resets_seen == 0);

  hostRepaints = 0;
  CHECK(last("VKRESET", "") == "OK pending");
  CHECK(cc_calls == 2 && cc_key == "(reset)");
  cc_done(true);
  CHECK(!provisioned());
  CHECK(resets_seen == 1);                              // every VK_ON_RESET listener ran
  CHECK(hostRepaints == 1);                             // SETUP comes back
  CHECK(text("display_name") == "" && text("rpc_url") == "" && u32("approval_tmo_s") == 45);
  uint8_t key[32];
  vk_token_t table[VK_MAX_TOKENS];
  CHECK(!key32("issuer_key", key) && tokens(table) == 0);
  String missing;
  CHECK(!commit(missing) && missing == "issuer_key");

  hostReboot();
  CHECK(!provisioned() && text("rpc_url") == "");

  // The engine refuses (busy): nothing is pending afterwards.
  provision();
  cc_accept = false;
  CHECK(last("VKRESET", "") == "ERR unavailable");
  cc_accept = true;
  CHECK(provisioned());

  // A reset raised while a change is waiting replaces nothing unless the engine takes it.
  CHECK(set("approval_tmo_s", "12") == SetResult::PENDING);
  cc_accept = false;
  requestReset();
  cc_accept = true;
  cc_done(true);
  CHECK(provisioned() && u32("approval_tmo_s") == 12);

  // After a reset the badge is unprovisioned again: secure keys are written directly.
  requestReset();
  cc_done(true);
  CHECK(!provisioned());
  const int calls = cc_calls;
  CHECK(set("approval_tmo_s", "30") == SetResult::OK);
  CHECK(cc_calls == calls && u32("approval_tmo_s") == 30);
}

// ---- serial commands, info fields, status item ---------------------------------------------------
static void test_commands() {
  fresh();

  // VKKEYS: one "+ <name> <type> <flags> <help>" line per key, then "OK <count>".
  const std::vector<std::string> keys = run("VKKEYS", "");
  size_t registered = 0;
  for (auto *key = ConfigKey::first(); key; key = key->next()) ++registered;
  CHECK(registered == 8);                               // 3 from config.cpp, 5 from this file
  CHECK(keys.size() == registered + 1);
  CHECK(keys.back() == "OK " + std::to_string(registered));
  bool sawRpc = false, sawIssuer = false, sawTimeout = false, sawListener = false, sawTokens = false;
  for (size_t i = 0; i + 1 < keys.size(); ++i) {
    CHECK(keys[i].rfind("+ ", 0) == 0);
    if (keys[i].rfind("+ rpc_url STR required ", 0) == 0) sawRpc = true;
    if (keys[i] == "+ issuer_key KEY32 secure,required test issuer key") sawIssuer = true;
    if (keys[i] == "+ approval_tmo_s U32 secure test approval timeout") sawTimeout = true;
    if (keys[i].rfind("+ listener_url STR - ", 0) == 0) sawListener = true;
    if (keys[i] == "+ tokens TOKENS secure,required test token table") sawTokens = true;
  }
  CHECK(sawRpc && sawIssuer && sawTimeout && sawListener && sawTokens);

  // VKGET
  CHECK(last("VKGET", "approval_tmo_s") == "OK 45");
  CHECK(last("VKGET", "  approval_tmo_s  ") == "OK 45");
  CHECK(last("VKGET", "listener_url") == "OK ");        // an empty value
  CHECK(last("VKGET", "nope") == "ERR unknown_key");
  CHECK(last("VKGET", "") == "ERR unknown_key");

  // VKSET: the value is the rest of the line
  CHECK(run("VKSET", "rpc_url http://127.0.0.1:8899").size() == 1);
  CHECK(last("VKGET", "rpc_url") == "OK http://127.0.0.1:8899");
  CHECK(last("VKSET", "display_name Merch Booth 1") == "OK");
  CHECK(text("display_name") == "Merch Booth 1");
  CHECK(last("VKSET", "display_name") == "OK");         // no value: the empty string
  CHECK(text("display_name") == "");
  CHECK(last("VKSET", "rpc_url x") == "ERR invalid");   // T-CFG3 at WP10
  CHECK(last("VKGET", "rpc_url") == "OK http://127.0.0.1:8899");
  CHECK(last("VKSET", "tokens garbage") == "ERR invalid");      // T-CFG3 from WP13 on
  CHECK(last("VKSET", "nope 1") == "ERR unknown_key");
  CHECK(last("VKSET", "") == "ERR unknown_key");
  CHECK(last("VKSET", ("tokens " + ONE).c_str()) == "OK");
  CHECK(last("VKGET", "tokens") == "OK " + ONE);
  CHECK(last("VKSET", (std::string("issuer_key ") + B58_ISSUER).c_str()) == "OK");

  // VKCOMMIT
  CHECK(last("VKCOMMIT", "") == "OK provisioned");
  CHECK(provisioned());

  // VKRESET (the other replies are covered in test_reset)
  engine();
  CHECK(last("VKRESET", "") == "OK pending");
  cc_done(false);

  // VKAUTOSTART: sets upstream's autostart app; an empty id clears it
  CHECK(last("VKAUTOSTART", "home") == "OK");
  CHECK(hostAutostart == "home");
  CHECK(last("VKAUTOSTART", " pay ") == "OK");
  CHECK(hostAutostart == "pay");
  CHECK(last("VKAUTOSTART", "") == "OK");
  CHECK(hostAutostart == "");

  // VKWIFI <ssid>|<password>: the SSID is everything before the first '|'
  CHECK(last("VKWIFI", "My Hotspot|hunter2hunter2") == "OK joining");
  CHECK(hostWifiSsid == "My Hotspot" && hostWifiPassword == "hunter2hunter2");
  CHECK(last("VKWIFI", "net|pass|with|bars ") == "OK joining");
  CHECK(hostWifiSsid == "net" && hostWifiPassword == "pass|with|bars ");
  CHECK(last("VKWIFI", "open|") == "OK joining");        // an open network
  CHECK(hostWifiSsid == "open" && hostWifiPassword == "");
  CHECK(last("VKWIFI", "no separator") == "ERR usage");
  CHECK(last("VKWIFI", "|password") == "ERR usage");
  CHECK(last("VKWIFI", "") == "ERR usage");
  CHECK(hostWifiSsid == "open");                         // unchanged by the refusals

  // every command of this file is registered with an upper-case VK name and a help line
  const char *names[] = {"VKKEYS", "VKGET", "VKSET", "VKCOMMIT", "VKRESET", "VKWIFI", "VKAUTOSTART"};
  for (const char *name : names) {
    bool found = false;
    for (auto *command = vk::serial::SerialCommand::first(); command; command = command->next()) {
      if (strcmp(command->name, name) == 0) found = command->fn != nullptr && command->help != nullptr && command->help[0] != '\0';
    }
    CHECK(found);
  }
}

static void test_info_and_status() {
  fresh();
  String (*infoProvisioned)() = nullptr;
  String (*infoWifi)() = nullptr;
  for (auto *field = vk::serial::InfoField::first(); field; field = field->next()) {
    if (strcmp(field->name, "provisioned") == 0) infoProvisioned = field->fn;
    if (strcmp(field->name, "wifi") == 0) infoWifi = field->fn;
  }
  CHECK(infoProvisioned != nullptr && infoWifi != nullptr);

  if (infoProvisioned == nullptr || infoWifi == nullptr) return;

  // The status item `setup` is gone with the status-item registry: the launcher's SETUP NEEDED row
  // reads provisioned() itself.
  CHECK(infoProvisioned() == "0");
  CHECK(infoWifi() == "0" || infoWifi() == "1");
  provision();
  CHECK(infoProvisioned() == "1");
}

int main() {
  fixtures();
  test_tokens_parser();
  test_key32_parser();
  test_u32_parser();
  test_str_parser();
  test_validate();
  test_defaults();
  test_round_trip();
  test_storage_failure();
  test_stored_garbage_is_ignored();
  test_commit();
  test_secure_change();
  test_reset();
  test_commands();
  test_info_and_status();
  if (fails) {
    printf("%d config test(s) FAILED\n", fails);
    return 1;
  }
  printf("all config tests passed\n");
  return 0;
}
