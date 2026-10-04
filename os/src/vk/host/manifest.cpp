// src/vk/host/manifest.cpp
// The app.ini keys Badge OS adds (app-host.md, "Manifest"): `permissions` and `min_api`, read at
// launch, and the launcher's `category` and `hidden`.
// Upstream's own parser and its Info struct are not touched: this one reads the same file again.
//
// Host-test seam (test/host/test_manifest.cpp): parse() needs only the String class; load(), which
// reads the file through upstream's app store, is compiled for the firmware only.
#include "manifest.h"

#ifndef VK_HOST_TEST
#include "../../apps/app_store.h"
#include "../../config.h"   // APP_MANIFEST
#endif

namespace vk::host::manifest {

namespace {

constexpr uint32_t MIN_API_DEFAULT = 1;
constexpr uint32_t MIN_API_BAD = 0xFFFFFFFFu;

// "sign, net,," -> "sign,net": every item trimmed, empty items dropped, order kept.
String cleanList(const String &value) {
  String out;
  const unsigned int length = value.length();
  unsigned int at = 0;
  while (at < length) {
    int end = value.indexOf(',', at);
    if (end < 0) end = (int)length;
    String item = value.substring(at, (unsigned int)end);
    item.trim();
    if (item.length()) {
      if (out.length()) out += ',';
      out += item;
    }
    at = (unsigned int)end + 1;
  }
  return out;
}

// Decimal digits only. A value that does not fit is stored as the largest one: such an app needs a
// newer API than any firmware has.
bool parseMinApi(const String &value, uint32_t &out) {
  if (value.length() == 0) return false;
  uint64_t number = 0;
  for (unsigned int i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c < '0' || c > '9') return false;
    number = number * 10 + (uint64_t)(c - '0');
    if (number > 0xFFFFFFFFull) number = 0xFFFFFFFFull;
  }
  out = (uint32_t)number;
  return true;
}

constexpr unsigned int CATEGORY_MAX = 16;

// Calls fn(key, value) for every `key=value` line of an app.ini text, the way upstream reads its own
// keys: spaces around both dropped, the key in lower case, blank lines and comments skipped.
template <typename Fn>
void eachKey(const String &iniText, Fn fn) {
  const unsigned int length = iniText.length();
  unsigned int at = 0;
  while (at < length) {
    int end = iniText.indexOf('\n', at);
    if (end < 0) end = (int)length;
    String line = iniText.substring(at, (unsigned int)end);
    at = (unsigned int)end + 1;

    line.trim();                         // also drops the '\r' of a CRLF file
    if (line.length() == 0 || line[0] == '#' || line[0] == ';') continue;
    const int separator = line.indexOf('=');
    if (separator <= 0) continue;

    String key = line.substring(0, (unsigned int)separator);
    String value = line.substring((unsigned int)separator + 1);
    key.trim();
    key.toLowerCase();
    value.trim();
    fn(key, value);
  }
}

// A folder name: [a-z0-9_-], 1 to CATEGORY_MAX characters, any case in the file. Else "".
String cleanCategory(const String &value) {
  if (value.length() == 0 || value.length() > CATEGORY_MAX) return "";
  String out = value;
  out.toLowerCase();
  for (unsigned int i = 0; i < out.length(); ++i) {
    const char c = out[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return "";
  }
  return out;
}

}  // namespace

bool parse(const String &iniText, Extra &out) {
  out.permissions = "";
  out.min_api = MIN_API_DEFAULT;
  bool ok = true;
  eachKey(iniText, [&](const String &key, const String &value) {
    if (key == "permissions") {
      out.permissions = cleanList(value);
    } else if (key == "min_api") {
      ok = parseMinApi(value, out.min_api);
      if (!ok) out.min_api = MIN_API_BAD;
    }
  });
  return ok;
}

bool parseLauncher(const String &iniText, Launcher &out) {
  out.category = "";
  out.hidden = false;
  out.countNotes = false;
  eachKey(iniText, [&](const String &key, const String &value) {
    if (key == "category") {
      out.category = cleanCategory(value);
    } else if (key == "count") {
      out.countNotes = value == "notes";
    } else if (key == "hidden") {
      String flag = value;
      flag.toLowerCase();
      out.hidden = flag == "1" || flag == "true";
    }
  });
  return true;
}

#ifndef VK_HOST_TEST
bool load(const String &appId, Extra &out) {
  out.permissions = "";
  out.min_api = MIN_API_DEFAULT;
  String text;
  if (!app_store::readFile(appId, APP_MANIFEST, text)) return true;   // no app.ini: the defaults
  return parse(text, out);
}

bool loadLauncher(const String &appId, Launcher &out) {
  String text;
  if (!app_store::readFile(appId, APP_MANIFEST, text)) {
    out = Launcher();
    return false;
  }
  return parseLauncher(text, out);
}
#endif

}  // namespace vk::host::manifest
