// src/vk/core/config.cpp
// The config store (platform/config.md): NVS namespace "vkconf" through Preferences, the text
// parsers, provisioning, the config serial commands and the info fields `provisioned` and `wifi`.
//
// Host-test seam (test/host/test_config.cpp): with VK_HOST_TEST this file compiles against the
// shim's Arduino.h and Preferences.h alone. Every upstream call (log, display, Wi-Fi, settings)
// and the shell repaint (defined in ui/repaint.cpp, which the suite does not link) is behind
// #ifndef VK_HOST_TEST.
#include "config.h"

#include <Preferences.h>
#include <string.h>

#include "../ui/repaint.h"
#include "serial.h"

#ifndef VK_HOST_TEST
#include "../../badge_log.h"
#include "../../hal/display.h"
#include "../../net/wifi_mgr.h"
#include "../../settings.h"
#include "../../ui/theme.h"      // upstream's palette, ::theme::
#define VK_CONFIG_LOG(...) ::badge_log::tagf("vk", __VA_ARGS__)
#else
#define VK_CONFIG_LOG(...) do { } while (0)
#endif

namespace vk::config {

bool (*confirmChange)(const char *key, const char *oldText, const char *newText, void (*done)(bool approved)) = nullptr;

#ifdef VK_HOST_TEST
String hostWifiSsid, hostWifiPassword, hostAutostart;
unsigned hostRepaints = 0;
#endif

// ---------------------------------------------------------------------------------------------
// Keys owned by core (platform/config.md, Keys)
// ---------------------------------------------------------------------------------------------
VK_CONFIG_KEY(rpc_url, "rpc_url", Type::STR, nullptr, F_REQUIRED, 8, 128, "Solana JSON-RPC endpoint");
VK_CONFIG_KEY(listener_url, "listener_url", Type::STR, "", F_NONE, 0, 128, "base URL of the backend's badge listener");
VK_CONFIG_KEY(display_name, "display_name", Type::STR, "", F_NONE, 0, 32,
              "name this badge claims in requests and contact cards; empty = the device name");

// ---------------------------------------------------------------------------------------------
// Text parsers. Pure: no NVS, no state.
// ---------------------------------------------------------------------------------------------
namespace {

// Longest value the store keeps. A VKSET line is under 250 characters, and a stored value is read
// back through a buffer of this size plus one.
constexpr size_t VALUE_MAX = 255;

// Longest token-table entry: mint (44) : decimals (1) : symbol (4) : cap (21) : max (21).
constexpr size_t TOKEN_ENTRY_MAX = 44 + 1 + 1 + 1 + 4 + 1 + 21 + 1 + 21;

// One entry of the token table, `mint:decimals:symbol:cap:max`, given as `length` characters.
bool parseTokenEntry(const char *entry, size_t length, vk_token_t &out) {
  if (length == 0 || length > TOKEN_ENTRY_MAX) return false;
  char buffer[TOKEN_ENTRY_MAX + 1];
  memcpy(buffer, entry, length);
  buffer[length] = '\0';

  // Exactly five fields, split in place.
  char *field[5];
  size_t fields = 0;
  field[fields++] = buffer;
  for (char *p = buffer; *p != '\0'; ++p) {
    if (*p != ':') continue;
    if (fields == 5) return false;
    *p = '\0';
    field[fields++] = p + 1;
  }
  if (fields != 5) return false;

  vk_token_t token;
  memset(&token, 0, sizeof token);

  if (!parseKey32(field[0], token.mint)) return false;

  // decimals: one digit, 0-9
  if (field[1][0] < '0' || field[1][0] > '9' || field[1][1] != '\0') return false;
  token.decimals = (uint8_t)(field[1][0] - '0');

  // symbol: 1-4 characters of [A-Z0-9] (it must fit a REQ frame's 4-byte currency field)
  const size_t symbolLength = strlen(field[2]);
  if (symbolLength < 1 || symbolLength > 4) return false;
  for (size_t i = 0; i < symbolLength; ++i) {
    const char c = field[2][i];
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
  }
  memcpy(token.symbol, field[2], symbolLength);      // the memset above left the terminator

  // cap and max: display units; 0 = none
  if (sol_parse_amount(field[3], token.decimals, &token.cap) != 0) return false;
  if (sol_parse_amount(field[4], token.decimals, &token.max) != 0) return false;
  if (token.max != 0 && token.cap > token.max) return false;

  out = token;
  return true;
}

}  // namespace

bool parseStr(const char *text, uint32_t minLen, uint32_t maxLen) {
  if (text == nullptr) return false;
  size_t length = 0;
  for (; text[length] != '\0'; ++length) {
    const unsigned char c = (unsigned char)text[length];
    if (c < 0x20 || c > 0x7E) return false;          // printable ASCII only
  }
  return length >= minLen && length <= maxLen;
}

bool parseU32(const char *text, uint32_t lowest, uint32_t highest, uint32_t *out) {
  if (text == nullptr || text[0] == '\0') return false;
  uint64_t value = 0;
  for (size_t i = 0; text[i] != '\0'; ++i) {
    if (i >= 10) return false;                       // 4294967295 has 10 digits
    if (text[i] < '0' || text[i] > '9') return false;
    value = value * 10 + (uint64_t)(text[i] - '0');
  }
  if (value > 0xFFFFFFFFull || value < lowest || value > highest) return false;
  if (out != nullptr) *out = (uint32_t)value;
  return true;
}

bool parseKey32(const char *text, uint8_t out[32]) {
  if (text == nullptr) return false;
  uint8_t bytes[32];
  if (sol_b58_decode(text, bytes, sizeof bytes) != 0) return false;    // refuses a bad character and any other length
  if (out != nullptr) memcpy(out, bytes, sizeof bytes);
  return true;
}

bool parseTokens(const char *text, vk_token_t out[VK_MAX_TOKENS], size_t *count) {
  if (text == nullptr) return false;
  vk_token_t table[VK_MAX_TOKENS];
  size_t entries = 0;
  const char *entry = text;
  for (;;) {
    const char *comma = strchr(entry, ',');
    const size_t length = comma ? (size_t)(comma - entry) : strlen(entry);
    if (entries >= VK_MAX_TOKENS) return false;
    if (!parseTokenEntry(entry, length, table[entries])) return false;   // an empty entry fails here
    ++entries;
    if (comma == nullptr) break;
    entry = comma + 1;
  }
  if (out != nullptr) memcpy(out, table, entries * sizeof(vk_token_t));
  if (count != nullptr) *count = entries;
  return true;
}

bool validate(const ConfigKey &key, const char *text) {
  if (text == nullptr || strlen(text) > VALUE_MAX) return false;
  switch (key.type) {
    case Type::STR:    return parseStr(text, key.min, key.max);
    case Type::U32:    return parseU32(text, key.min, key.max, nullptr);
    case Type::KEY32:  return parseKey32(text, nullptr);
    case Type::TOKENS: return parseTokens(text, nullptr, nullptr);
  }
  return false;
}

const ConfigKey *find(const char *name) {
  if (name == nullptr) return nullptr;
  for (auto *key = ConfigKey::first(); key; key = key->next()) {
    if (strcmp(key->name, name) == 0) return key;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------------------------
// The store
// ---------------------------------------------------------------------------------------------
namespace {

constexpr char NVS_NAMESPACE[] = "vkconf";
// The provisioned flag lives in the same namespace, so VKRESET erases it with everything else.
// It is not a config key: VKSET reaches only registered keys, and none is named like this.
constexpr char PROVISIONED_KEY[] = "_provisioned";
// The config key that tokens() reads.
constexpr char TOKENS_KEY[] = "tokens";
// The key name passed to confirmChange for a reset. core/ never includes a wallet header, so the
// reset confirmation cannot be raised with vk::wallet::approval::confirm from here. Both
// confirmations go through confirmChange: a secure change passes the key's name with its old and
// new text; VKRESET passes this name with two empty strings, and the approval engine then shows
// the headline ERASE WALLET CONFIG instead of SECURITY SETTING (platform/config.md, Provisioning).
constexpr char RESET_KEY[] = "(reset)";

// One cached value: the effective text (stored, else the default) and, for a U32 key, its number.
struct Slot {
  const ConfigKey *key = nullptr;
  bool has = false;            // false: nothing stored (or the stored text is not valid) and no default
  String text;
  bool numberOk = false;
  uint32_t number = 0;
};
// The keys of platform/config.md number 18. Keys beyond the cache still work; they are read from
// NVS on every call.
constexpr size_t CACHE_SLOTS = 32;

// CamelCase on purpose: the Arduino core defines CHANGE (an interrupt mode) as a macro.
enum class Pending : uint8_t { Nothing, SetKey, EraseAll };

struct State {
  Preferences prefs;
  bool begun = false;
  bool open = false;           // the namespace is open; false after a failed open (defaults only, writes fail)
  bool provisioned = false;
  Slot slots[CACHE_SLOTS];
  size_t slotCount = 0;
  bool tokensCached = false;
  size_t tokenCount = 0;
  vk_token_t tokenTable[VK_MAX_TOKENS] = {};
  // The one confirmation that is on the screen (the approval engine shows one at a time).
  Pending pending = Pending::Nothing;
  const ConfigKey *pendingKey = nullptr;
  String pendingValue;
};

// Function-local, so that it exists whenever an accessor is first called (the boot screen reads
// the theme before vk::begin()).
State &state() {
  static State s;
  return s;
}

void clearCache() {
  State &s = state();
  for (size_t i = 0; i < s.slotCount; ++i) s.slots[i] = Slot();
  s.slotCount = 0;
  s.tokensCached = false;
  s.tokenCount = 0;
}

// The launcher's SETUP NEEDED row appears or disappears: ask the shell to redraw.
void provisioningChanged() {
#ifndef VK_HOST_TEST
  vk::ui::requestShellRepaint();
#else
  ++hostRepaints;
#endif
}

// Reads one key from NVS into `slot`. A stored text that does not validate (written by an older
// firmware, or NVS damage) is ignored, so no setting is ever taken from a value VKSET would refuse.
void fill(const ConfigKey *key, Slot &slot) {
  State &s = state();
  slot = Slot();
  slot.key = key;
  if (s.open) {
    char buffer[VALUE_MAX + 1];
    // Returns the length including the terminator: 0 means "no such string", 1 an empty one.
    if (s.prefs.getString(key->name, buffer, sizeof buffer) > 0) {
      if (validate(*key, buffer)) {
        slot.text = buffer;
        slot.has = true;
      } else {
        VK_CONFIG_LOG("config: the stored value of %s is not valid; using the default", key->name);
      }
    }
  }
  if (!slot.has && key->def != nullptr) {
    slot.text = key->def;
    slot.has = true;
  }
  if (slot.has && key->type == Type::U32) {
    slot.numberOk = parseU32(slot.text.c_str(), 0, 0xFFFFFFFFu, &slot.number);
  }
}

// The cached value of a key, read on first use. `scratch` is used when the cache is full.
// begin() must have run.
const Slot &lookup(const ConfigKey *key, Slot &scratch) {
  State &s = state();
  for (size_t i = 0; i < s.slotCount; ++i) {
    if (s.slots[i].key == key) return s.slots[i];
  }
  Slot &slot = s.slotCount < CACHE_SLOTS ? s.slots[s.slotCount++] : scratch;
  fill(key, slot);
  return slot;
}

// Writes one value and checks the result (the NVS partition is small and shared, so it can fill
// up). Clears the cache either way.
bool writeValue(const ConfigKey *key, const char *value) {
  State &s = state();
  clearCache();
  if (!s.open) return false;
  const size_t length = strlen(value);
  const size_t written = s.prefs.putString(key->name, value);
  if (length > 0) return written == length;
  // putString reports the bytes stored, which is 0 for an empty string whether or not the write
  // worked. Read it back: 1 is an empty string (its terminator); 0 is a missing key or a longer
  // value still in place.
  char probe[2];
  return s.prefs.getString(key->name, probe, sizeof probe) == 1;
}

// VKRESET, approved: erase the namespace (values and the provisioned flag) and tell every reset
// listener. The device key, the history and the contacts are not touched.
void eraseAll() {
  State &s = state();
  if (!s.open || !s.prefs.clear()) {
    VK_CONFIG_LOG("config: reset failed: NVS refused the erase");
    return;
  }
  clearCache();
  s.provisioned = false;
  for (auto *listener = ResetListener::first(); listener; listener = listener->next()) {
    if (listener->fn) listener->fn();
  }
  provisioningChanged();
  VK_CONFIG_LOG("config: wallet config erased");
}

// The approval engine's answer to the confirmation raised by set() or requestReset().
void onConfirmDone(bool approved) {
  State &s = state();
  const Pending kind = s.pending;
  const ConfigKey *key = s.pendingKey;
  const String value = s.pendingValue;
  s.pending = Pending::Nothing;
  s.pendingKey = nullptr;
  s.pendingValue = String();
  if (kind == Pending::Nothing || !approved) return;

  if (kind == Pending::EraseAll) {
    eraseAll();
    return;
  }
  if (writeValue(key, value.c_str())) {
    VK_CONFIG_LOG("config: %s changed", key->name);
  } else {
    VK_CONFIG_LOG("config: %s approved but NVS refused the write; the old value stays", key->name);
  }
}

// Raises a confirmation through confirmChange. False when there is no approval engine or it
// cannot show the screen now (an approval is already open); the confirmation that is already
// waiting, if any, is then left as it was.
bool raiseConfirmation(Pending kind, const ConfigKey *key, const char *oldText, const char *newText) {
  if (confirmChange == nullptr) return false;
  State &s = state();
  const Pending previousKind = s.pending;
  const ConfigKey *previousKey = s.pendingKey;
  const String previousValue = s.pendingValue;
  // Recorded before the call: the engine is free to answer from inside it.
  s.pending = kind;
  s.pendingKey = key;
  s.pendingValue = newText;
  if (confirmChange(kind == Pending::EraseAll ? RESET_KEY : key->name, oldText, newText, onConfirmDone)) return true;
  s.pending = previousKind;
  s.pendingKey = previousKey;
  s.pendingValue = previousValue;
  return false;
}

bool raiseReset() {
  begin();
  return raiseConfirmation(Pending::EraseAll, nullptr, "", "");
}

}  // namespace

void begin() {
  State &s = state();
  if (s.begun) return;
  s.begun = true;
  s.open = s.prefs.begin(NVS_NAMESPACE, false);
  if (!s.open) {
    VK_CONFIG_LOG("config: cannot open the NVS namespace %s; defaults only", NVS_NAMESPACE);
    return;
  }
  s.provisioned = s.prefs.getUChar(PROVISIONED_KEY, 0) == 1;
}

bool provisioned() {
  begin();
  return state().provisioned;
}

String text(const char *name) {
  begin();
  const ConfigKey *key = find(name);
  if (key == nullptr) return String();
  Slot scratch;
  const Slot &slot = lookup(key, scratch);
  return slot.has ? slot.text : String();
}

uint32_t u32(const char *name) {
  begin();
  const ConfigKey *key = find(name);
  if (key == nullptr || key->type != Type::U32) return 0;
  Slot scratch;
  const Slot &slot = lookup(key, scratch);
  return slot.numberOk ? slot.number : 0;
}

bool key32(const char *name, uint8_t out[32]) {
  begin();
  const ConfigKey *key = find(name);
  if (key == nullptr || key->type != Type::KEY32) return false;
  Slot scratch;
  const Slot &slot = lookup(key, scratch);
  return slot.has && parseKey32(slot.text.c_str(), out);
}

size_t tokens(vk_token_t out[VK_MAX_TOKENS]) {
  begin();
  State &s = state();
  if (!s.tokensCached) {
    s.tokenCount = 0;
    const ConfigKey *key = find(TOKENS_KEY);
    if (key != nullptr && key->type == Type::TOKENS) {
      Slot scratch;
      const Slot &slot = lookup(key, scratch);
      if (slot.has && !parseTokens(slot.text.c_str(), s.tokenTable, &s.tokenCount)) s.tokenCount = 0;
    }
    s.tokensCached = true;
  }
  if (out != nullptr && s.tokenCount > 0) memcpy(out, s.tokenTable, s.tokenCount * sizeof(vk_token_t));
  return s.tokenCount;
}

SetResult set(const char *name, const char *value) {
  const ConfigKey *key = find(name);
  if (key == nullptr) return SetResult::UNKNOWN_KEY;
  if (!validate(*key, value)) return SetResult::INVALID;
  begin();
  if (state().provisioned && (key->flags & F_SECURE)) {
    // A secure key on a provisioned badge: the write happens in onConfirmDone, after a hold-SELECT
    // on the badge's own screen. UNAVAILABLE when there is no approval engine, and also when the
    // engine cannot show the screen now (the enum has no separate value for that).
    const String old = text(name);
    return raiseConfirmation(Pending::SetKey, key, old.c_str(), value) ? SetResult::PENDING : SetResult::UNAVAILABLE;
  }
  return writeValue(key, value) ? SetResult::OK : SetResult::STORAGE;
}

// False with `missing` set: a required key has no valid value (the first by name, so the answer
// does not depend on registration order). False with `missing` empty: NVS refused the write.
bool commit(String &missing) {
  begin();
  missing = "";
  const ConfigKey *firstMissing = nullptr;
  for (auto *key = ConfigKey::first(); key; key = key->next()) {
    if (!(key->flags & F_REQUIRED)) continue;
    Slot scratch;
    if (lookup(key, scratch).has) continue;
    if (firstMissing == nullptr || strcmp(key->name, firstMissing->name) < 0) firstMissing = key;
  }
  if (firstMissing != nullptr) {
    missing = firstMissing->name;
    return false;
  }
  State &s = state();
  if (s.provisioned) return true;
  if (!s.open || s.prefs.putUChar(PROVISIONED_KEY, 1) != 1) {
    VK_CONFIG_LOG("config: commit failed: NVS refused the write");
    return false;
  }
  s.provisioned = true;
  provisioningChanged();
  VK_CONFIG_LOG("config: provisioned");
  return true;
}

void requestReset() { (void)raiseReset(); }

#ifdef VK_HOST_TEST
void hostReboot() {
  State &s = state();
  s.prefs.end();
  clearCache();
  s.begun = false;
  s.open = false;
  s.provisioned = false;
  s.pending = Pending::Nothing;
  s.pendingKey = nullptr;
  s.pendingValue = String();
}
#endif

// ---------------------------------------------------------------------------------------------
// Serial commands (platform/config.md, Serial commands), info fields, status item
// ---------------------------------------------------------------------------------------------
namespace {

const char *typeName(Type type) {
  switch (type) {
    case Type::STR:    return "STR";
    case Type::U32:    return "U32";
    case Type::KEY32:  return "KEY32";
    case Type::TOKENS: return "TOKENS";
  }
  return "?";
}

// One word, so the help text that follows it on a VKKEYS line can contain spaces.
const char *flagsName(uint8_t flags) {
  const bool secure = (flags & F_SECURE) != 0, required = (flags & F_REQUIRED) != 0;
  if (secure && required) return "secure,required";
  if (secure) return "secure";
  if (required) return "required";
  return "-";
}

// VKKEYS: one "+ <name> <type> <flags> <help>" line per config key, then "OK <count>".
void cmdKeys(const String &args, const serial::Reply &reply) {
  (void)args;
  unsigned count = 0;
  for (auto *key = ConfigKey::first(); key; key = key->next()) {
    String line = "+ ";
    line += key->name;
    line += " ";
    line += typeName(key->type);
    line += " ";
    line += flagsName(key->flags);
    line += " ";
    line += key->help ? key->help : "";
    reply(line);
    ++count;
  }
  reply(String("OK ") + String(count));
}

// VKGET <key>: "OK <value>" or "ERR unknown_key". All values are public.
void cmdGet(const String &args, const serial::Reply &reply) {
  String name = args;
  name.trim();
  if (find(name.c_str()) == nullptr) {
    reply(String("ERR unknown_key"));
    return;
  }
  reply(String("OK ") + text(name.c_str()));
}

// VKSET <key> <value>: the key is the first word, the value is the rest of the line after the
// blanks that follow the key. Nothing is trimmed from the end.
void cmdSet(const String &args, const serial::Reply &reply) {
  const char *line = args.c_str();
  size_t keyLength = 0;
  while (line[keyLength] != '\0' && line[keyLength] != ' ' && line[keyLength] != '\t') ++keyLength;
  const String name = args.substring(0, (unsigned int)keyLength);
  const char *value = line + keyLength;
  while (*value == ' ' || *value == '\t') ++value;

  const char *answer = "ERR invalid";
  switch (set(name.c_str(), value)) {
    case SetResult::OK:          answer = "OK"; break;
    case SetResult::PENDING:     answer = "OK pending"; break;
    case SetResult::UNKNOWN_KEY: answer = "ERR unknown_key"; break;
    case SetResult::INVALID:     answer = "ERR invalid"; break;
    case SetResult::UNAVAILABLE: answer = "ERR unavailable"; break;
    case SetResult::STORAGE:     answer = "ERR nvs_full"; break;
  }
  reply(String(answer));
}

// VKCOMMIT: "OK provisioned" or "ERR missing <key>" ("ERR nvs_full" if the flag cannot be written).
void cmdCommit(const String &args, const serial::Reply &reply) {
  (void)args;
  String missing;
  if (commit(missing)) {
    reply(String("OK provisioned"));
  } else if (missing.length() > 0) {
    reply(String("ERR missing ") + missing);
  } else {
    reply(String("ERR nvs_full"));
  }
}

// VKRESET: "OK pending" once the confirmation is on the badge's screen. "ERR unavailable" when it
// cannot be raised (no approval engine, or an approval is already open).
void cmdReset(const String &args, const serial::Reply &reply) {
  (void)args;
  reply(String(raiseReset() ? "OK pending" : "ERR unavailable"));
}

// VKAUTOSTART <id>: sets upstream's autostart app; an empty id clears it.
void cmdAutostart(const String &args, const serial::Reply &reply) {
  String id = args;
  id.trim();
#ifndef VK_HOST_TEST
  ::settings::setAutostartApp(id);
#else
  hostAutostart = id;
#endif
  reply(String("OK"));
}

// VKWIFI <ssid>|<password>: the SSID is everything before the first '|' (it may contain spaces),
// the password everything after. Saves the network and joins it, as upstream's JOINWIFI does.
void cmdWifi(const String &args, const serial::Reply &reply) {
  const int bar = args.indexOf('|');
  if (bar <= 0) {                                    // no separator, or an empty SSID
    reply(String("ERR usage"));
    return;
  }
  const String ssid = args.substring(0, (unsigned int)bar);
  const String password = args.substring((unsigned int)bar + 1);
  reply(String("OK joining"));                       // before associating, like JOINWIFI
#ifndef VK_HOST_TEST
  ::wifi_mgr::connect(ssid, password, true);
#else
  hostWifiSsid = ssid;
  hostWifiPassword = password;
#endif
}

VK_SERIAL_COMMAND(vkkeys, "VKKEYS", cmdKeys, "list the config keys: name type flags help");
VK_SERIAL_COMMAND(vkget, "VKGET", cmdGet, "<key>: read a config value");
VK_SERIAL_COMMAND(vkset, "VKSET", cmdSet, "<key> <value>: set a config value (a secure key is confirmed on the badge)");
VK_SERIAL_COMMAND(vkcommit, "VKCOMMIT", cmdCommit, "finish provisioning once every required key is set");
VK_SERIAL_COMMAND(vkreset, "VKRESET", cmdReset, "erase the wallet config (confirmed on the badge)");
VK_SERIAL_COMMAND(vkwifi, "VKWIFI", cmdWifi, "<ssid>|<password>: save a Wi-Fi network and join it");
VK_SERIAL_COMMAND(vkautostart, "VKAUTOSTART", cmdAutostart, "<id>: app started at boot; empty clears it");

String infoProvisioned() { return String(provisioned() ? "1" : "0"); }

// 1 when the badge is joined to a network. connected() alone is also true in hotspot mode.
String infoWifi() {
#ifndef VK_HOST_TEST
  const bool joined = ::wifi_mgr::mode() == ::wifi_mgr::Mode::Station && ::wifi_mgr::connected();
  return String(joined ? "1" : "0");
#else
  return String("0");
#endif
}

VK_INFO_FIELD(provisioned, "provisioned", infoProvisioned);
VK_INFO_FIELD(wifi, "wifi", infoWifi);

}  // namespace

}  // namespace vk::config
