// vk::host::catalog: the apps the launcher lists (catalog.h), and badge.system.launcher_apps().
#include "catalog.h"

#include "../../apps/app_store.h"
#include "lua_registry.h"
#include "manifest.h"
#include "native.h"

namespace vk::host::catalog {

namespace {

constexpr size_t MAX_ENTRIES = 48;   // upstream's 32 Lua apps and the native ones

Entry sEntries[MAX_ENTRIES];
size_t sCount = 0;
bool sDirty = true;
size_t sBuiltFrom = (size_t)-1;      // app_store::count() at the last build
uint32_t sGeneration = 0;

// The launcher keys of a native app are its BADGE_APP string, keys separated by ';'.
void nativeKeys(const String &id, manifest::Launcher &out) {
  String text = native::launcherKeys(id);
  text.replace(';', '\n');
  manifest::parseLauncher(text, out);
}

void rebuild() {
  sCount = 0;
  const size_t total = app_store::count();
  const size_t firstNative = total - native::count();   // hook H11: the native apps come last
  app_store::Info info;
  for (size_t i = 0; i < total && sCount < MAX_ENTRIES; ++i) {
    if (!app_store::at(i, info)) continue;
    manifest::Launcher keys;
    if (native::exists(info.id)) {
      // A pushed Lua folder with a native app's id never runs (native-apps.md): the id is
      // listed once, where the native apps are.
      if (i < firstNative) continue;
      nativeKeys(info.id, keys);
    } else {
      manifest::loadLauncher(info.id, keys);
    }
    if (keys.hidden) continue;
    Entry &entry = sEntries[sCount++];
    entry.id = info.id;
    entry.name = info.name.length() ? info.name : info.id;
    entry.category = keys.category;
    entry.countNotes = keys.countNotes;
  }
  sBuiltFrom = total;
  sDirty = false;
  ++sGeneration;
}

void ensure() {
  // The count check covers a path that changes the list without a rescan (there is none upstream
  // today; it costs nothing).
  if (sDirty || app_store::count() != sBuiltFrom) rebuild();
}

// badge.system.launcher_apps() -> array of {id, name, category}: every app the launcher lists,
// in its order, folders flattened ("" is the top level). Hidden apps are not in it.
int l_launcher_apps(lua_State *L) {
  ensure();
  lua_createtable(L, (int)sCount, 0);
  for (size_t i = 0; i < sCount; ++i) {
    lua_createtable(L, 0, 3);
    lua_pushstring(L, sEntries[i].id.c_str());
    lua_setfield(L, -2, "id");
    lua_pushstring(L, sEntries[i].name.c_str());
    lua_setfield(L, -2, "name");
    lua_pushstring(L, sEntries[i].category.c_str());
    lua_setfield(L, -2, "category");
    lua_rawseti(L, -2, (lua_Integer)(i + 1));
  }
  return 1;
}

VK_LUA_FUNCTION(launcher_apps, "system", "launcher_apps", nullptr, l_launcher_apps);

}  // namespace

void invalidate() { sDirty = true; }

uint32_t generation() {
  ensure();
  return sGeneration;
}

size_t count() {
  ensure();
  return sCount;
}

const Entry *at(size_t index) {
  ensure();
  return index < sCount ? &sEntries[index] : nullptr;
}

}  // namespace vk::host::catalog
