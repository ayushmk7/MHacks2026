// The BadgeOS shell framework (docs/os/ui/shell.md, "Framework"): the screen stack, the repaint
// rules, and the four functions upstream's src/ui/shell.h declares. This file replaces upstream's
// src/ui/shell.cpp; os.ino and the runtime keep calling shell::begin(), update(), onAppStopped()
// and showError() and never know the difference.
//
// The main loop calls shell::update() only while no app runs and no approval is open (hook H4),
// and flushes the canvas itself after every pass.
#include "../../ui/shell.h"

#include <string.h>

#include "../../apps/app_store.h"
#include "../../hal/leds.h"
#include "../../lua_sdk/lua_runtime.h"
#include "../../net/broker_client.h"
#include "../ui/repaint.h"
#include "page.h"

namespace vk::shell {

namespace {

constexpr int STACK_MAX = 6;                 // launcher -> settings -> a page -> a sub-screen -> a dialog
constexpr uint32_t CHROME_CHECK_MS = 500;    // how often the header text and the paper colour are compared
constexpr uint32_t PAUSE_GAP_MS = 300;       // a longer gap between the end of one pass and the next means the shell was paused

const Screen *sStack[STACK_MAX] = {&kLauncher};
int sDepth = 1;                              // the launcher is always at the bottom
bool sDirty = true;
uint32_t sLastDraw = 0;
uint32_t sLastChromeCheck = 0;
uint32_t sLastPass = 0;
char sHeaderDrawn[40] = "";                  // receipt::statusRight at the last draw
uint16_t sPaperDrawn = 0;                    // theme PAPER at the last draw

void enterTop() {
  const Screen *screen = sStack[sDepth - 1];
  if (screen->enter != nullptr) screen->enter();
  sDirty = true;
}

}  // namespace

const Screen *top() { return sStack[sDepth - 1]; }

void repaint() { sDirty = true; }

void push(const Screen *screen) {
  if (screen == nullptr) return;
  if (sDepth < STACK_MAX) {
    sStack[sDepth++] = screen;
  } else {
    sStack[STACK_MAX - 1] = screen;          // full: the new screen takes the top slot
  }
  enterTop();
}

void pop() {
  if (sDepth > 1) --sDepth;
  sDirty = true;
}

void home() {
  sDepth = 1;
  sDirty = true;
}

void replaceTop(const Screen *screen) {
  if (screen == nullptr) return;
  if (sDepth > 1) --sDepth;
  push(screen);
}

void showOver(const Screen *screen) {
  sDepth = 1;
  push(screen);
}

namespace {
bool sAppFromSettings = false;
}
void appFromSettings(bool yes) { sAppFromSettings = yes; }
bool appFromSettings() { return sAppFromSettings; }

}  // namespace vk::shell

// ---- upstream's interface (src/ui/shell.h) and screenName() --------------------------------------

namespace shell {

const char *screenName() { return runtime::running() ? "" : vk::shell::top()->name; }

void begin() {
  app_store::refresh();
  vk::shell::home();
}

void showError(const String &message) {
  vk::shell::appErrorSet(message);
  vk::shell::showOver(&vk::shell::kAppError);
  vk::shell::pulseLedBad(700);
}

void onAppStopped() {
  // No app_store::refresh() here (upstream's shell had one): the scan walks every app folder,
  // about 1.2 s with seventeen Lua apps installed, and it would freeze the badge each time an app
  // exits. The list cannot be stale: every path that installs or removes an app (serial and BLE
  // push, the web page, the store client, app_store::removeApp) rescans by itself.
  if (runtime::lastError().length()) {
    showError(runtime::lastError());
  } else {
    vk::shell::home();                       // the launcher keeps its cursor and clamps it to the new count
    if (vk::shell::appFromSettings()) vk::shell::push(&vk::shell::kSettings);   // Wallet, Inbox: back to Settings, same row
    leds::playIdle();
  }
}

void update() {
  using namespace vk::shell;
  const uint32_t now = millis();

  // The shell was not called for a while: an approval or an app had the screen, and the canvas
  // holds their drawing. Redraw in full on this first pass back.
  // The gap is measured from the END of the last pass: a page whose own draw is slow must not
  // look like a pause, or it would redraw on every pass.
  if (now - sLastPass >= PAUSE_GAP_MS) sDirty = true;

  // 1. The app-store offer is raised here, not by the store client: the main loop calls the shell
  // only when no app runs, which is the condition the prompt must respect. It is not raised over
  // `installing`, so the result of the last install is read before the next offer replaces it.
  if (broker::hasOffer() && top() != &kOffer && top() != &kInstalling) {
    push(&kOffer);
    pulseLed(900);
    // No input on this pass: its button edges were computed before the prompt existed, and a
    // SELECT meant for the previous screen must not count as consent to install code.
  } else {
    // 2. Input first, so a screen switch on this pass is drawn on this pass.
    const Screen *screen = top();
    if (screen->update != nullptr) {
      screen->update();
    } else {
      back();
    }
  }

  // 3. Timers. The screen on top may have changed in step 2.
  const Screen *screen = top();
  if (screen->refresh_ms != 0 && (now - sLastDraw) >= screen->refresh_ms) sDirty = true;
  if ((now - sLastChromeCheck) >= CHROME_CHECK_MS) {
    sLastChromeCheck = now;
    char header[sizeof sHeaderDrawn];
    vk::ui::receipt::statusRight(header, sizeof header);
    if (strcmp(header, sHeaderDrawn) != 0 || vk::ui::theme::color(vk::ui::theme::PAPER) != sPaperDrawn) sDirty = true;
  }

  // 4. A repaint asked for from outside: a closed approval, provisioning, a new balance, the inbox.
  if (vk::ui::consumeShellRepaint()) sDirty = true;

  // 5. Draw only when something changed: an idle shell costs no drawing and no display transfer.
  if (sDirty) {
    sDirty = false;
    if (screen->draw != nullptr) screen->draw();
    sLastDraw = millis();
    vk::ui::receipt::statusRight(sHeaderDrawn, sizeof sHeaderDrawn);
    sPaperDrawn = vk::ui::theme::color(vk::ui::theme::PAPER);
  }
  sLastPass = millis();
}

}  // namespace shell
