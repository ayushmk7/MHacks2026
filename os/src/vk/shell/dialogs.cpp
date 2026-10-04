// Screens `app_delete`, `app_error`, `offer` and `installing` (docs/os/ui/shell.md, "Delete
// confirmation", "App error", "App-store offer", "Installing"). Each keeps the behaviour of the
// upstream screen it replaces: every call upstream made is made here.
#include <stdio.h>

#include "../../apps/app_store.h"
#include "../../badge_log.h"
#include "../../lua_sdk/lua_runtime.h"
#include "../../net/broker_client.h"
#include "../../ui/shell.h"
#include "page.h"

namespace vk::shell {

namespace {

using namespace vk::ui;

// =====================================================================================================
// Delete confirmation
// =====================================================================================================

// The app is remembered by id, not by launcher index: a push can install or remove an app while
// the prompt is up, and an index would then point at a different app than the one on screen.
String sDeleteId;
String sDeleteName;

// The app-error screen offers "SELECT retry" only for an app that stopped with an error. It is also
// where a failed delete lands, and there runtime::lastApp() is whatever app ran last, not the one
// that could not be deleted. appErrorSet() makes it true; the delete path clears it after showError().
bool sErrorRetryable = true;

void deleteDraw() {
  frame("DELETE APP", "SELECT delete", "CANCEL keep");
  receipt::row(X0, X1, 48, "APP", sDeleteName.c_str());
  app_store::Info info;
  if (app_store::byId(sDeleteId, info)) {
    char size[16];
    snprintf(size, sizeof size, "%u KB", (unsigned)((info.sizeBytes + 1023) / 1024));
    receipt::row(X0, X1, 66, "ID", sDeleteId.c_str());
    receipt::row(X0, X1, 84, "VERSION", info.version.length() ? info.version.c_str() : "-");
    receipt::row(X0, X1, 102, "SIZE", size);
  }
  text(10, 126, "Its files and saved data go with it.", theme::SUB);
  text(10, 140, "This cannot be undone.", theme::STAMP_WARN);
}

void deleteUpdate() {
  if (buttons::pressed(BTN_A)) {
    const String id = sDeleteId;
    const String name = sDeleteName;
    sDeleteId = "";
    sDeleteName = "";
    // The app can have gone away underneath the prompt (a push can remove it): already gone counts
    // as removed, not as an error the wearer has to make sense of.
    const bool removed = !app_store::exists(id) || app_store::removeApp(id);
    app_store::refresh();
    if (removed) {
      badge_log::tagf("os", "deleted app '%s'", id.c_str());
      pulseLed(500);                         // done as asked: the theme's pulse, not the red of a failure
      home();                                // the launcher clamps its cursor to the shorter list
    } else {
      badge_log::tagf("os", "could not delete app '%s'", id.c_str());
      ::shell::showError("Could not delete " + name);
      sErrorRetryable = false;               // there is no app to retry: runtime::lastApp() is unrelated
    }
    return;
  }
  if (buttons::pressed(BTN_B)) {
    sDeleteId = "";
    sDeleteName = "";
    pop();
  }
}

// =====================================================================================================
// App error
// =====================================================================================================

constexpr int ERROR_COLS = 50;
constexpr int ERROR_LINES = 13;

String sError;

void errorDraw() {
  frame("APP STOPPED", sErrorRetryable ? "SELECT retry" : "", "CANCEL launcher");
  // Wrapped by hand: a traceback is one long line, and running off the edge would hide the useful part.
  int offset = 0;
  for (int line = 0; line < ERROR_LINES && offset < (int)sError.length(); ++line) {
    String chunk = sError.substring(offset, offset + ERROR_COLS);
    const int newline = chunk.indexOf('\n');
    if (newline >= 0) {
      chunk = chunk.substring(0, newline);
      offset += newline + 1;
    } else {
      offset += ERROR_COLS;
    }
    text(10, 48 + 12 * line, chunk.c_str(), line == 0 ? theme::STAMP_BAD : theme::INK, ERROR_COLS);
  }
}

void errorUpdate() {
  if (buttons::pressed(BTN_A)) {
    if (!sErrorRetryable) return;
    // The current app is already empty (it was torn down): the retry target is the last app.
    const String retry = runtime::lastApp();
    if (retry.length() && app_store::exists(retry)) {
      runtime::clearError();
      runtime::requestLaunch(retry);
    }
    return;
  }
  if (buttons::pressed(BTN_B)) {
    runtime::clearError();
    home();
  }
}

// =====================================================================================================
// App-store offer
//
// The one screen that appears without being asked for, and the only thing between "anyone who knows
// a badge ID" and code on this badge's flash. The framework raises it (shell.cpp, step 1) and skips
// input on that pass, so a SELECT meant for the previous screen is never read as consent.
// =====================================================================================================

constexpr int OFFER_VISIBLE = 4;
constexpr int OFFER_Y = 88;
constexpr int OFFER_PITCH = 31;              // a row and its subline

List sOfferList;

void offerEnter() { sOfferList = List(); }

void offerUpdate() {
  // The offer can go away underneath the prompt (it expires, or the broker withdraws it), so the
  // screen must be able to leave on its own.
  if (!broker::hasOffer()) {
    if (broker::installing()) {
      replaceTop(&kInstalling);              // its enter() remembers millis()
    } else {
      home();
    }
    return;
  }
  listMove(sOfferList, broker::offer().count, OFFER_VISIBLE);
  if (buttons::pressed(BTN_A)) {
    broker::accept();
    replaceTop(&kInstalling);
    return;
  }
  if (buttons::pressed(BTN_B)) {
    broker::decline();
    home();
  }
}

void offerDraw() {
  const broker::Offer &offer = broker::offer();
  const int count = offer.count;
  frame("APP OFFER", "SELECT install", "CANCEL decline");

  char line[96];
  snprintf(line, sizeof line, "%u script%s", (unsigned)count, count == 1 ? "" : "s");
  receipt::row(X0, X1, 48, offer.repo.length() ? offer.repo.c_str() : "unknown source", line);
  snprintf(line, sizeof line, "%s%s%s", offer.ref.length() ? offer.ref.c_str() : "-",
           offer.sender.length() ? " via " : "", offer.sender.length() ? offer.sender.c_str() : "");
  receipt::subline(X0, X1, 61, line);
  receipt::rule(78);

  int cursor = sOfferList.cursor;
  if (cursor >= count) cursor = count - 1;
  if (cursor < 0) cursor = 0;
  int scroll = sOfferList.scroll;
  if (cursor < scroll) scroll = cursor;
  if (cursor >= scroll + OFFER_VISIBLE) scroll = cursor - OFFER_VISIBLE + 1;

  for (int i = 0; i < OFFER_VISIBLE && scroll + i < count; ++i) {
    const int index = scroll + i;
    const broker::ScriptInfo &script = offer.scripts[index];
    const int y = OFFER_Y + i * OFFER_PITCH;
    const bool selected = index == cursor;
    char size[16];
    if (script.bytes >= 1024) {
      snprintf(size, sizeof size, "%u KB", (unsigned)(script.bytes / 1024));
    } else {
      snprintf(size, sizeof size, "%u B", (unsigned)script.bytes);
    }
    receipt::row(X0, X1, y, script.name.c_str(), size, selected);
    // The description is the author's own first comment line: the closest thing to an answer to
    // "what does this do" before it runs.
    const String &detail = script.description.length() ? script.description : script.file;
    receipt::subline(X0, X1, y + 13, detail.c_str(), selected);
  }

  if (count > OFFER_VISIBLE) {
    snprintf(line, sizeof line, "%d/%d", cursor + 1, count);
    textRight(X1, TITLE_Y + 2, line, theme::FAINT);
  }
}

// =====================================================================================================
// Installing
// =====================================================================================================

// A stalled install (the network dropped mid-download, the broker never returns a result) must not
// trap the badge: the force-quit hold only rescues apps, not shell screens. After this long on the
// screen, CANCEL leaves.
constexpr uint32_t INSTALL_ESCAPE_MS = 20000;

uint32_t sInstallStart = 0;

void installingEnter() { sInstallStart = millis(); }

uint32_t installElapsed() { return millis() - sInstallStart; }

bool installStuck() { return installElapsed() >= INSTALL_ESCAPE_MS; }

void installingUpdate() {
  if (broker::lastResult().length() == 0) {
    // Still working. The broker keeps its state; a result landing later is harmless.
    if (buttons::pressed(BTN_B) && installStuck()) home();
    return;
  }
  if (buttons::pressed(BTN_A) || buttons::pressed(BTN_B)) {
    broker::clearResult();
    home();                                  // the install already refreshed the app list
  }
}

void installingDraw() {
  const String result = broker::lastResult();
  if (result.length() > 0) {
    frame(broker::failedCount() == 0 ? "INSTALLED" : "FINISHED WITH ERRORS", "SELECT or CANCEL: launcher", "");
    textCentered(160, 70, result.c_str());
    textCentered(160, 90, "The launcher has them now.", theme::SUB);
    return;
  }

  // Until CANCEL works, the footer counts down to it: a screen that ignores CANCEL with no word
  // about it looks hung.
  const uint32_t elapsed = installElapsed();
  const bool stuck = elapsed >= INSTALL_ESCAPE_MS;
  char wait[24] = "";
  if (!stuck) snprintf(wait, sizeof wait, "CANCEL in %us", (unsigned)((INSTALL_ESCAPE_MS - elapsed + 999) / 1000));
  frame("INSTALLING", stuck ? "stuck? CANCEL to give up" : "verifying sha256 before each write", wait);
  const unsigned percent = broker::installProgress();
  char value[16];
  snprintf(value, sizeof value, "%u%%", percent);
  receipt::amount(160, 48, "", value, "");
  bar(41, 112, 30, percent, 100);
  snprintf(value, sizeof value, "%u", (unsigned)broker::installedCount());
  receipt::row(X0, X1, 136, "OK", value);
  snprintf(value, sizeof value, "%u", (unsigned)broker::failedCount());
  receipt::row(X0, X1, 154, "FAILED", value);
}

}  // namespace

const Screen kAppDelete = {"app_delete", nullptr, deleteUpdate, deleteDraw, 0};
const Screen kAppError = {"app_error", nullptr, errorUpdate, errorDraw, 0};
const Screen kOffer = {"offer", offerEnter, offerUpdate, offerDraw, 0};
const Screen kInstalling = {"installing", installingEnter, installingUpdate, installingDraw, 250};

void appDeleteOpen(const String &id, const String &name) {
  sDeleteId = id;
  sDeleteName = name;
  push(&kAppDelete);
}

void appErrorSet(const String &message) {
  sError = message;
  sErrorRetryable = true;
  sError.replace("\t", "  ");   // a Lua traceback indents with tabs, which the kit would draw as "?"
}

}  // namespace vk::shell
