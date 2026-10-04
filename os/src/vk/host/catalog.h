// The apps the launcher lists, with their folders (app-host.md, "Manifest"; shell.md, "Launcher").
//
// Built from upstream's app list (app_store, which hook H11 extends with the native apps) and the
// launcher keys of each manifest: app.ini for a Lua app, the last BADGE_APP argument for a native
// one. Hidden apps are left out. Reading every app.ini costs a file read per app, so the list is
// built once and again only after app_store::refresh() has rescanned /apps (hook H25).
#pragma once

#include <Arduino.h>

namespace vk::host::catalog {

struct Entry {
  String id;
  String name;       // the manifest's name, or the id
  String category;   // "" = the launcher's top level
  bool countNotes;   // the cell shows the waiting-notification count (manifest key count=notes)
};

void invalidate();                 // hook H25: the next read rebuilds the list
uint32_t generation();             // changes each time the list is rebuilt
size_t count();                    // listed apps: Lua apps in upstream's order, then native apps by id
const Entry *at(size_t index);     // nullptr past the end

}  // namespace vk::host::catalog
