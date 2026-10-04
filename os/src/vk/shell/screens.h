// The BadgeOS shell's framework API (docs/os/ui/shell.md, "Framework"): the screen stack.
#pragma once

#include <Arduino.h>

namespace shell {
const char *screenName();          // name of the screen on top ("launcher", "wifi", ...); "" while an app runs
}

namespace vk::shell {

struct Screen {
  const char *name;                // what screenName() and VKSTATE report: [a-z_], unique
  void (*enter)();                 // called each time the screen is pushed; may be nullptr
  void (*update)();                // called every loop pass while the screen is on top: reads buttons
  void (*draw)();                  // full repaint of the canvas (the main loop flushes it)
  uint32_t refresh_ms;             // 0 = repaint only on request; otherwise also every refresh_ms
};

void push(const Screen *screen);   // show `screen` on top. The object must be static. Calls enter(), requests a repaint
void pop();                        // back one screen (not re-entered: it keeps its cursor); does nothing on the launcher
void home();                       // back to the launcher
void repaint();                    // redraw the top screen on this pass
const Screen *top();

// The screens the framework itself opens.
extern const Screen kLauncher;     // launcher.cpp
extern const Screen kSettings;     // settings_list.cpp
extern const Screen kAppDelete;    // dialogs.cpp
extern const Screen kAppError;     // dialogs.cpp
extern const Screen kOffer;        // dialogs.cpp
extern const Screen kInstalling;   // dialogs.cpp
void appDeleteOpen(const String &id, const String &name);   // remembers the app, then push(&kAppDelete)
void appErrorSet(const String &message);                    // the text kAppError shows

// ---- added for the launcher's folders (2026-10-04); nothing above changes ----
// What VKSTATE reports about the launcher (launcher.cpp): the open folder ("" at the top level),
// the cursor, and each cell's key: an app's id, or "folder:<category>" for a folder.
const char *launcherFolder();
int launcherCursor();
int launcherRowCount();
String launcherRowKey(int index);

}  // namespace vk::shell
