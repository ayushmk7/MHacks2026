// src/vk/core/serial.cpp
// The serial command registry, VKHELP and VKINFO (platform/config.md, Serial commands).
// The config commands are registered from core/config.cpp, the dev commands from features/devtools/.
#include "serial.h"

#include <ctype.h>

#include "../vk_build.h"

namespace vk::serial {
namespace {

bool isBlank(char c) { return c == ' ' || c == '\t'; }

// Case-insensitive compare of the first `length` characters of `word` with the whole of `name`.
bool sameWord(const char *word, size_t length, const char *name) {
  for (size_t i = 0; i < length; ++i) {
    if (name[i] == '\0') return false;
    if (toupper((unsigned char)word[i]) != toupper((unsigned char)name[i])) return false;
  }
  return name[length] == '\0';
}

// VKHELP: one "+ <name> <help>" line per command, then "OK <count>".
void cmdHelp(const String &args, const Reply &reply) {
  (void)args;
  unsigned count = 0;
  for (auto *command = SerialCommand::first(); command; command = command->next()) {
    reply(String("+ ") + command->name + " " + (command->help ? command->help : ""));
    ++count;
  }
  reply(String("OK ") + String(count));
}

// VKINFO: "OK" followed by one name=value pair per registered info field, space-separated.
// The order is not guaranteed; readers parse by name.
void cmdInfo(const String &args, const Reply &reply) {
  (void)args;
  String line = "OK";
  for (auto *field = InfoField::first(); field; field = field->next()) {
    line += " ";
    line += field->name;
    line += "=";
    line += field->fn ? field->fn() : String("");
  }
  reply(line);
}

String infoProfile() { return String(VK_PROFILE_DEV ? "dev" : "release"); }
String infoApi() { return String((unsigned)VK_API_VERSION); }

VK_SERIAL_COMMAND(vkinfo, "VKINFO", cmdInfo, "firmware and wallet state, as name=value pairs");
VK_SERIAL_COMMAND(vkhelp, "VKHELP", cmdHelp, "list the VK serial commands");

// Registered in this order so that, among the fields of this file, VKINFO shows profile then api
// (a registry lists its newest entry first).
VK_INFO_FIELD(api, "api", infoApi);
VK_INFO_FIELD(profile, "profile", infoProfile);

}  // namespace

// The first word of the line, compared case-insensitively with every registered command.
// Returns false for anything else, so upstream's push protocol gets the line (hook H6).
bool handleLine(const String &line, const Reply &reply) {
  const char *text = line.c_str();
  const size_t total = line.length();

  size_t start = 0;
  while (start < total && isBlank(text[start])) ++start;
  size_t end = start;
  while (end < total && !isBlank(text[end])) ++end;
  if (end == start) return false;

  for (auto *command = SerialCommand::first(); command; command = command->next()) {
    if (!sameWord(text + start, end - start, command->name)) continue;
    // The arguments are the rest of the line after the blanks that follow the command word.
    // Nothing is trimmed from the end: a value may end in a space.
    size_t argsAt = end;
    while (argsAt < total && isBlank(text[argsAt])) ++argsAt;
    command->fn(String(text + argsAt), reply);
    return true;
  }
  return false;
}

}  // namespace vk::serial
