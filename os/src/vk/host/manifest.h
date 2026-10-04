// The app.ini keys Badge OS adds (app-host.md, "Manifest").
#pragma once

#include <Arduino.h>

namespace vk::host::manifest {
struct Extra { String permissions; uint32_t min_api = 1; };
bool load(const String &appId, Extra &out);     // reads /apps/<id>/app.ini via app_store::readFile

// ---- Added with manifest.cpp (WP30). Nothing above this line was changed. ----

// load() returns false only for an app.ini that is there and has a bad `min_api` (see parse()).
// An app with no app.ini (upstream allows a folder holding only main.lua), an id with no folder and
// a native id all give the defaults, an empty permission list and min_api 1, and true.

// The parser behind load(). Pure: no file access, host-tested in test/host/test_manifest.cpp.
// Reads `permissions` and `min_api` from the text of an app.ini the way upstream reads its own keys:
// one key=value per line, spaces around the key and the value dropped, the key in any case, blank
// lines and lines starting with '#' or ';' skipped, every other key ignored, the last line of a
// repeated key wins.
//   permissions  stored without spaces and without empty items ("sign, net,," gives "sign,net");
//                the names are not checked here (preLaunch() knows the registered ones).
//   min_api      decimal digits only; a value above 4294967295 is stored as 4294967295.
// Returns false when a `min_api` line holds anything else (empty, a sign, letters); `out.min_api` is
// then 4294967295, so a caller that ignores the result still refuses the app.
bool parse(const String &iniText, Extra &out);

// ---- Added for the launcher's folders (2026-10-04). Nothing above this line was changed. ----

// The launcher keys of a manifest (app-host.md, "Manifest"). In app.ini for a Lua app; in the
// optional last argument of BADGE_APP for a native app, with ';' between the keys
// ("category=tests;hidden=1").
//   category  the launcher folder the app is listed in: [a-z0-9_-], at most 16 characters, stored
//             in lower case. Empty (the default, or a value with any other character): the app is
//             listed at the top level.
//   hidden    1 or "true": not listed on the launcher (or by badge.system.launcher_apps());
//             it still starts over serial and with badge.system.launch(). Default 0.
//   count     a number the launcher shows on the app's cell. One value is known: `notes`, the
//             waiting notifications (the Inbox declares it). Anything else: no number.
// `profile` (dev: not installed by push-apps.sh release) and `include` (another app's Lua files
// are pushed with this one) are read by scripts/push-apps.sh only; the firmware ignores them.
struct Launcher { String category; bool hidden = false; bool countNotes = false; };
bool parseLauncher(const String &iniText, Launcher &out);   // pure; never fails: a bad value is the default
bool loadLauncher(const String &appId, Launcher &out);      // /apps/<id>/app.ini; false when there is none
}
