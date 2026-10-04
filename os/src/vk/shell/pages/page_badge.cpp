// Settings page `badge` (docs/os/ui/shell.md, "Badge"): what a person sets on the badge itself, with
// no laptop. The name other badges see (config key display_name, on the keyboard), the time zone the
// header shows (config key utc_offset: display only), where the clock stands, how often the balance
// is fetched (balance_poll_s), and the app started at boot (upstream's autostart setting, chosen from
// the installed apps).
//
// The clock is never set by hand here: the wallet checks trust only the network's time (SNTP) and a
// verified record's floor (wallet/checks.md, "Clock"); the offset moves what is printed, nothing else.
//
// It also registers the choices of two keys (edit.h): utc_offset (every quarter hour) and pay_app
// (the installed apps), so Settings > Advanced offers a picker for them. Deleting this file removes
// the page and those two pickers.

#include "../page.h"

#include "../../../apps/app_store.h"
#include "../../../badge_log.h"
#include "../../../net/wifi_mgr.h"
#include "../../../settings.h"
#include "../../core/clock.h"
#include "../../core/config.h"
#include "../../core/utc_offset.h"
#include "../edit.h"

namespace {
using namespace vk::shell;
namespace receipt = vk::ui::receipt;
namespace tk = vk::ui::theme;
namespace cfg = vk::config;
namespace edit = vk::shell::edit;

constexpr int ROWS = 5;
enum Row { ROW_NAME, ROW_ZONE, ROW_CLOCK, ROW_POLL, ROW_BOOT };

constexpr int RULE_Y = 140;
constexpr int TIME_Y = 150;
constexpr int HELP_Y = 170;          // two lines, 12 px apart
constexpr int NOTE_Y = 196;
constexpr uint32_t CHECK_MS = 500;   // how often the page looks for a change it did not make

List sList;
char sDrawn[200] = "";               // what the last draw showed (see signature())
uint32_t sCheckedAt = 0;

bool joined() { return wifi_mgr::mode() == wifi_mgr::Mode::Station && wifi_mgr::connected(); }

int32_t offsetMinutes() {
  int32_t minutes = 0;
  if (!vk_utc_offset_parse(cfg::text("utc_offset").c_str(), &minutes)) minutes = 0;
  return minutes;
}

// ---- the app lists --------------------------------------------------------------------------------

bool appAt(size_t index, char *value, size_t valueCap, char *label, size_t labelCap) {
  app_store::Info info;
  if (!app_store::at(index, info)) return false;
  strlcpy(value, info.id.c_str(), valueCap);
  strlcpy(label, info.name.length() ? info.name.c_str() : info.id.c_str(), labelCap);
  return true;
}

size_t appCount() { return app_store::count(); }

// Start at boot: "none" (the launcher), then every installed app.
bool bootAt(size_t index, char *value, size_t valueCap, char *label, size_t labelCap) {
  if (index == 0) {
    strlcpy(value, "", valueCap);
    strlcpy(label, "none: the launcher", labelCap);
    return true;
  }
  return appAt(index - 1, value, valueCap, label, labelCap);
}

void bootPicked(const char *value) {
  settings::setAutostartApp(String(value));
  badge_log::tagf("os", "autostart app set on the badge: '%s'", value);
  edit::noteSay("saved");
}

void bootText(char *out, size_t cap) {
  const String id = settings::autostartApp();
  if (id.length() == 0) {
    snprintf(out, cap, "none");
    return;
  }
  app_store::Info info;
  if (app_store::byId(id, info)) {
    snprintf(out, cap, "%s", info.name.length() ? info.name.c_str() : id.c_str());
  } else {
    snprintf(out, cap, "%s (not installed)", id.c_str());
  }
}

// ---- the offsets ----------------------------------------------------------------------------------

size_t offsetCount() { return vk_utc_offset_count(); }

// UTC is stored as "" (the key's default), every other offset as "+HH:MM".
bool offsetAt(size_t index, char *value, size_t valueCap, char *label, size_t labelCap) {
  if (index >= vk_utc_offset_count()) return false;
  const int32_t minutes = vk_utc_offset_at(index);
  if (minutes == 0) {
    strlcpy(value, "", valueCap);
  } else {
    vk_utc_offset_format(minutes, value, valueCap);
  }
  vk_utc_offset_label(minutes, label, labelCap);
  return true;
}

void offsetStep(int direction) {
  const int32_t now = offsetMinutes();
  const int32_t next = vk_utc_offset_step(now, direction);
  if (next == now) return;   // at a limit
  char text[VK_UTC_OFFSET_TEXT_CAP] = "";
  if (next != 0) vk_utc_offset_format(next, text, sizeof text);
  const cfg::SetResult result = cfg::set("utc_offset", text);   // not a secure key: written at once
  if (result != cfg::SetResult::OK) edit::noteResult("utc_offset", result);
  repaint();
}

VK_KEY_CHOICES(utc_offset, "utc_offset", offsetCount, offsetAt);
VK_KEY_CHOICES(pay_app, "pay_app", appCount, appAt);

// ---- texts --------------------------------------------------------------------------------------

void pollText(uint32_t seconds, char *out, size_t cap) {
  if (seconds == 0) {
    snprintf(out, cap, "off");
  } else if (seconds % 3600 == 0) {
    snprintf(out, cap, "every %u h", (unsigned)(seconds / 3600));
  } else if (seconds % 60 == 0) {
    snprintf(out, cap, "every %u min", (unsigned)(seconds / 60));
  } else {
    snprintf(out, cap, "every %u s", (unsigned)seconds);
  }
}

const char *clockText(uint16_t *color) {
  switch (vk::clock::source()) {
    case vk::clock::Source::SNTP:
      *color = tk::color(tk::STAMP_OK);
      return "synced";
    case vk::clock::Source::FLOOR:
      *color = tk::color(tk::STAMP_WARN);
      return "unsynced";
    case vk::clock::Source::NONE:
      break;
  }
  *color = tk::color(tk::STAMP_WARN);
  return "not set";
}

// Everything the page shows that can change without a key press here, as one string.
void signature(char *out, size_t cap) {
  char boot[40];
  bootText(boot, sizeof boot);
  const bool noted = edit::note() != nullptr;
  const uint32_t minute = vk::clock::ok() ? vk::clock::now() / 60 : 0;
  snprintf(out, cap, "%s|%s|%d|%d|%lu|%lu|%s|%d", cfg::text("display_name").c_str(), cfg::text("utc_offset").c_str(),
           (int)vk::clock::source(), joined() ? 1 : 0, (unsigned long)cfg::u32("balance_poll_s"),
           (unsigned long)minute, boot, noted ? 1 : 0);
}

// ---- the page -------------------------------------------------------------------------------------

void pageValue(char *out, size_t cap) {
  const String name = cfg::text("display_name");
  snprintf(out, cap, "%s", name.length() ? name.c_str() : "name, time");
}

void pageEnter() {
  sList = List();
  sDrawn[0] = '\0';
  sCheckedAt = millis();
}

void pageUpdate() {
  if (back()) return;  // CANCEL
  listMove(sList, ROWS, ROWS);
  const bool select = buttons::pressed(BTN_A);
  int direction = 0;
  switch (sList.cursor) {
    case ROW_NAME:
      if (select) edit::type("display_name", "NAME");
      break;
    case ROW_ZONE:
      if (buttons::repeated(BTN_LEFT)) direction = -1;
      if (buttons::repeated(BTN_RIGHT)) direction = 1;
      if (direction != 0) offsetStep(direction);
      if (select) edit::choose("utc_offset", "TIME ZONE");
      break;
    case ROW_CLOCK:
      break;   // nothing to set: the time comes from the network (Settings > Wi-Fi, or Setup)
    case ROW_POLL:
      if (buttons::pressed(BTN_LEFT)) direction = -1;
      if (buttons::pressed(BTN_RIGHT)) direction = 1;
      if (direction != 0) edit::step("balance_poll_s", direction);
      if (select) edit::type("balance_poll_s", "BALANCE CHECK, S");
      break;
    case ROW_BOOT:
      if (select) {
        const String current = settings::autostartApp();
        edit::pick("START AT BOOT", app_store::count() + 1, bootAt, current.c_str(), bootPicked);
      }
      break;
    default:
      break;
  }
  // A change made elsewhere (VKSET, the network time, the minute, a note running out): redraw only then.
  if (millis() - sCheckedAt < CHECK_MS) return;
  sCheckedAt = millis();
  char now[sizeof sDrawn];
  signature(now, sizeof now);
  if (strcmp(now, sDrawn) != 0) repaint();
}

void pageDraw() {
  signature(sDrawn, sizeof sDrawn);

  static const char *const FOOT[ROWS] = {"SELECT type", "LEFT/RIGHT change  SELECT list", "",
                                         "LEFT/RIGHT change  SELECT type", "SELECT choose"};
  const bool online = joined();
  const char *foot = FOOT[sList.cursor >= 0 && sList.cursor < ROWS ? sList.cursor : 0];
  frame("BADGE", foot);

  const String name = cfg::text("display_name");
  char zone[VK_UTC_OFFSET_LABEL_CAP];
  vk_utc_offset_label(offsetMinutes(), zone, sizeof zone);
  uint16_t clockColor = 0;
  const char *clock = clockText(&clockColor);
  char poll[20];
  pollText(cfg::u32("balance_poll_s"), poll, sizeof poll);
  char boot[40];
  bootText(boot, sizeof boot);

  const ListRow rows[ROWS] = {
      {"Name", name.length() ? name.c_str() : "device name", name.length() ? (uint16_t)0 : tk::color(tk::SUB)},
      {"Time zone", zone, 0},
      {"Clock", clock, clockColor},
      {"Balance check", poll, 0},
      {"Start at boot", boot, 0},
  };
  listDraw(sList, rows, ROWS, LIST_Y, ROWS);
  receipt::rule(RULE_Y);

  // The local time, and the UTC it comes from: the person can see the offset is right.
  char local[40] = "no time yet";
  if (vk::clock::ok()) {
    char here[6], utc[6];
    vk_utc_offset_clock(vk::clock::now(), offsetMinutes(), here, sizeof here);
    vk_utc_offset_clock(vk::clock::now(), 0, utc, sizeof utc);
    snprintf(local, sizeof local, "%s  (UTC %s)", here, utc);
  }
  receipt::row(X0, X1, TIME_Y, "LOCAL TIME", local);

  // What the row under the cursor does.
  char line1[56] = "", line2[56] = "";
  switch (sList.cursor) {
    case ROW_NAME:
      snprintf(line1, sizeof line1, "Other badges see it in requests and contacts.");
      snprintf(line2, sizeof line2, "Empty: the device name, %s.", settings::deviceName().c_str());
      break;
    case ROW_ZONE:
      snprintf(line1, sizeof line1, "Moves the time this badge shows. Payments");
      snprintf(line2, sizeof line2, "always check the network's own (UTC) time.");
      break;
    case ROW_CLOCK:
      if (vk::clock::source() == vk::clock::Source::SNTP) {
        snprintf(line1, sizeof line1, "Set by the network (SNTP).");
        snprintf(line2, sizeof line2, "Server: %s", cfg::text("ntp_server").c_str());
      } else {
        strlcpy(line1, online ? "Waiting for the network time." : "From the network: join it in Settings > Wi-Fi.",
                sizeof line1);
        strlcpy(line2, "It is never typed in: payments check it.", sizeof line2);
      }
      break;
    case ROW_POLL:
      snprintf(line1, sizeof line1, "How often the balance is fetched. Longer");
      snprintf(line2, sizeof line2, "saves battery; off never fetches it.");
      break;
    case ROW_BOOT:
      snprintf(line1, sizeof line1, "The app that opens when the badge starts.");
      snprintf(line2, sizeof line2, "CANCEL in it returns to the launcher.");
      break;
    default:
      break;
  }
  text(X0, HELP_Y, line1, tk::SUB);
  text(X0, HELP_Y + 12, line2, tk::SUB);

  bool bad = false;
  const char *note = edit::note(&bad);
  if (note != nullptr) text(X0, NOTE_Y, note, bad ? tk::STAMP_WARN : tk::STAMP_OK);
}
}  // namespace

VK_SETTINGS_PAGE(badge, "badge", 142, "Badge", pageValue, pageEnter, pageUpdate, pageDraw, 0);
