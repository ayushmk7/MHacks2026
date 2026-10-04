// src/vk/vk.cpp
// vk::begin() and vk::update() run the registered services; vk::modalActive() and
// vk::modalUpdate() hand the loop to the approval engine (architecture/overview.md, sections 5 and 7).
#include "vk.h"

#include "../badge_log.h"
#include "../hal/display.h"
#include "core/config.h"
#include "core/registry.h"
#include "core/serial.h"
#include "core/service.h"
#include "host/lua_registry.h"
#include "host/permissions.h"
#include "host/router.h"
#include "sdk/badge_sdk.hpp"
#include "shell/page.h"
#include "ui/leds.h"
#include "wallet/approval.h"
#include "wallet/signer.h"

namespace vk {
namespace {

// Through the base class by name: a registered struct may have a member that hides first() or
// next() (EspnowRoute has a field called `first`).
template <class T>
unsigned countOf() {
  unsigned count = 0;
  for (const T *item = Registered<T>::first(); item; item = item->Registered<T>::next()) ++count;
  return count;
}

}  // namespace

void begin() {
  config::begin();

  for (auto *service = Service::first(); service; service = service->next()) {
    if (service->begin) service->begin();
  }

  // One count per registry. A registry that is zero when its feature is in the build means the
  // linker dropped a registration (implementation-plan.md, review focus 5); T-BOOT1 reads this line.
  badge_log::tagf("vk",
                  "registries: services=%u commands=%u lua=%u domains=%u routes=%u "
                  "permissions=%u patterns=%u native=%u config=%u pages=%u",
                  countOf<Service>(), countOf<serial::SerialCommand>(), countOf<lua::LuaFunction>(),
                  countOf<wallet::SignDomain>(),
                  countOf<host::router::EspnowRoute>(), countOf<host::Permission>(),
                  countOf<ui::leds::LedPattern>(), countOf<badge::NativeApp>(),
                  countOf<config::ConfigKey>(), countOf<shell::SettingsPage>());
}

void update() {
  for (auto *service = Service::first(); service; service = service->next()) {
    if (service->update) service->update();
  }
}

namespace {
FlushStats sFlush = {0, 0};
}

void flush() {
  const uint32_t started = micros();
  if (!display::flush()) return;
  ++sFlush.transfers;
  sFlush.micros += micros() - started;
}

FlushStats flushStats() { return sFlush; }

bool modalActive() { return wallet::approval::active(); }

void modalUpdate() { wallet::approval::update(); }

}  // namespace vk
