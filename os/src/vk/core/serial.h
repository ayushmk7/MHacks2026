// src/vk/core/serial.h
// USB serial commands (platform/config.md, Serial commands). Reached through hook H6 only, so the
// commands exist on the USB console and never on BLE or HTTP.
#pragma once

#include <Arduino.h>

#include <functional>

#include "registry.h"

namespace vk::serial {
using Reply = std::function<void(const String &)>;
struct SerialCommand : Registered<SerialCommand> {
  const char *name;                                    // upper case, starts with "VK"
  void (*fn)(const String &args, const Reply &reply);
  const char *help;
  SerialCommand(const char *n, void (*f)(const String &, const Reply &), const char *h) : name(n), fn(f), help(h) {}
};
#define VK_SERIAL_COMMAND(ident, name, fn, help) static vk::serial::SerialCommand vk_serial_##ident(name, fn, help)
bool handleLine(const String &line, const Reply &reply);     // true if it was one of ours

// One name=value pair in the VKINFO reply.
struct InfoField : Registered<InfoField> { const char *name; String (*fn)(); InfoField(const char *n, String (*f)()) : name(n), fn(f) {} };
#define VK_INFO_FIELD(ident, name, fn) static vk::serial::InfoField vk_info_##ident(name, fn)
}
