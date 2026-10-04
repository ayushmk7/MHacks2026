// core/service.h
#pragma once
#include "registry.h"

namespace vk {
struct Service : Registered<Service> {
  const char *name;
  void (*begin)();     // may be nullptr; called once from vk::begin()
  void (*update)();    // may be nullptr; called every loop from vk::update()
  Service(const char *n, void (*b)(), void (*u)()) : name(n), begin(b), update(u) {}
};
}
#define VK_SERVICE(ident, begin_fn, update_fn) \
  static vk::Service vk_service_##ident(#ident, begin_fn, update_fn)
