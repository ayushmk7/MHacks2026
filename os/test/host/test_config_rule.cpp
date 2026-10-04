// LINK: src/vk/core/config.cpp src/vk/core/utc_offset.c
// test_config_rule - a config key's own rule beyond its type and range (VK_CONFIG_RULE in
// src/vk/core/config.h; docs/os/platform/config.md, "Key rules"). The firmware registers one, for
// `utc_offset` (core/clock.cpp); this suite registers the same key and rule, then checks that every
// way into the store applies it: validate(), set() and VKSET, and a stored text that breaks the
// rule (written by an older firmware) is ignored like any invalid stored value.
#include <Arduino.h>
#include <Preferences.h>

#include <string.h>

#include <string>

#include "../../src/vk/core/config.h"
#include "../../src/vk/core/serial.h"
#include "../../src/vk/core/utc_offset.h"

using namespace vk::config;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

VK_CONFIG_KEY(utc_offset, "utc_offset", Type::STR, "", F_NONE, 0, 6, "test UTC offset");
VK_CONFIG_RULE(utc_offset, "utc_offset", vk_utc_offset_valid);
VK_CONFIG_KEY(plain, "plain_s", Type::STR, "", F_NONE, 0, 6, "a key with no rule");

static void fresh() {
  vk_host_nvs_reset();
  hostReboot();
  confirmChange = nullptr;
}

// "VKSET <args>" through the registered command, as test_config does it.
static std::string vkset(const char *args) {
  std::string last = "(no reply)";
  for (auto *command = vk::serial::SerialCommand::first(); command; command = command->next()) {
    if (strcmp(command->name, "VKSET") != 0) continue;
    command->fn(String(args), [&last](const String &line) { last = line.c_str(); });
  }
  return last;
}

int main() {
  fresh();
  const ConfigKey *key = find("utc_offset");
  CHECK(key != nullptr);
  CHECK(ruleFor("utc_offset") != nullptr);
  CHECK(ruleFor("plain_s") == nullptr);
  CHECK(ruleFor(nullptr) == nullptr);

  // validate(): type and range first, then the rule.
  CHECK(validate(*key, "+05:30"));
  CHECK(validate(*key, ""));
  CHECK(!validate(*key, "+05:31"));       // the right length, refused by the rule
  CHECK(!validate(*key, "+05:300"));      // too long for the range
  CHECK(validate(*find("plain_s"), "+05:31"));   // no rule: length only

  // set() and VKSET.
  CHECK(set("utc_offset", "-04:00") == SetResult::OK);
  CHECK(text("utc_offset") == "-04:00");
  CHECK(set("utc_offset", "-4:00") == SetResult::INVALID);
  CHECK(text("utc_offset") == "-04:00");
  CHECK(vkset("utc_offset +99:00") == "ERR invalid");
  CHECK(vkset("utc_offset +01:00") == "OK");
  CHECK(text("utc_offset") == "+01:00");
  CHECK(vkset("utc_offset") == "OK");    // empty: UTC
  CHECK(text("utc_offset") == "");

  // A stored text that breaks the rule reads as the default.
  {
    Preferences raw;
    CHECK(raw.begin("vkconf", false));
    CHECK(raw.putString("utc_offset", "+05:31") == 6);
    raw.end();
  }
  hostReboot();
  CHECK(text("utc_offset") == "");

  if (fails) {
    printf("%d failure(s)\n", fails);
    return 1;
  }
  printf("all config_rule tests passed\n");
  return 0;
}
