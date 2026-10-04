// Settings page `about` (shell.md, "About"): what this badge runs, and the link to the project's
// repository as text. The link is the config key `repo_url`: no address is compiled in. The launcher
// shows it as a QR code. Deleting this file removes the page, the row and the key (and the launcher's code).

#include "../page.h"

#include "../../core/config.h"
#include "../../vk_build.h"
#include "../../wallet/signer.h"

namespace {
using namespace vk::shell;
namespace receipt = vk::ui::receipt;
namespace th = vk::ui::theme;

VK_CONFIG_KEY(repo_url, "repo_url", vk::config::Type::STR, "", vk::config::F_NONE, 0, 120,
              "link shown as a QR code on the launcher and as text on Settings > About (the project's repository)");

constexpr int URL_CAP = 121;       // the key's longest value and its terminator

// ---- layout (ui.md: stub 0..145 centred on 73, perforation at 146, body centred on 233) ----
constexpr int STUB_CX = 73, STUB_X0 = 10, STUB_X1 = 136;
constexpr int SPLIT_X = 146, BODY_CX = 233;
constexpr int NAME_Y = 36, NAME_RULE_Y = 56, ROWS_Y = 70;
constexpr int URL_Y = 104, URL_PITCH = 11, URL_LINES = 3, URL_COLS = 26;

char sShown[URL_CAP] = "";         // the link the last draw showed
uint32_t sCheckedAt = 0;

void readUrl(char *out, size_t cap) { strlcpy(out, vk::config::text("repo_url").c_str(), cap); }

// The row's value in the Settings list: the host of the link ("github.com"), or "not set".
void pageValue(char *out, size_t cap) {
  char url[URL_CAP];
  readUrl(url, sizeof url);
  const char *host = strstr(url, "://");
  host = host ? host + 3 : url;
  const size_t length = strcspn(host, "/?#");
  if (length == 0) {
    snprintf(out, cap, "not set");
  } else {
    snprintf(out, cap, "%.*s", (int)length, host);
  }
}

void pageUpdate() {
  if (back()) return;  // CANCEL
  // `VKSET repo_url ...` while the page is open: looked at twice a second, redrawn only on a change.
  if (millis() - sCheckedAt < 500) return;
  sCheckedAt = millis();
  char url[URL_CAP];
  readUrl(url, sizeof url);
  if (strcmp(url, sShown) != 0) repaint();
}

// The link as text. It has no spaces, so a line ends after the last '/' that fits,
// or at the edge when that would leave less than half a line. What does not fit ends in "..".
void drawUrl(const char *url) {
  const char *rest = url;
  for (int line = 0; line < URL_LINES && *rest; ++line) {
    int take = (int)strlen(rest);
    if (take > URL_COLS && line < URL_LINES - 1) {
      take = URL_COLS;
      for (int i = URL_COLS; i > URL_COLS / 2; --i) {
        if (rest[i - 1] == '/') { take = i; break; }
      }
    }
    char part[URL_CAP];
    snprintf(part, sizeof part, "%.*s", take, rest);
    const int cols = take > URL_COLS ? URL_COLS : take;      // the last line is cut by text()
    text(BODY_CX - (cols * 6 - 1) / 2, URL_Y + line * URL_PITCH, part, th::SUB, URL_COLS);
    rest += take;
  }
}

void pageDraw() {
  char status[40];
  receipt::page();
  receipt::statusRight(status, sizeof status);
  receipt::header("BADGEOS", status);
  receipt::perforation(SPLIT_X, 24, 212);

  // Left stub: the name, then what is running and whose badge it is.
  receipt::title("BadgeOS", NAME_Y, STUB_CX);
  receipt::rule(NAME_RULE_Y, STUB_X0, STUB_X1);

  char api[8];
  snprintf(api, sizeof api, "%d", (int)VK_API_VERSION);
  const char *location = vk::wallet::keyLocation();
  const bool secure = strcmp(location, "se050") == 0;
  const String address = vk::wallet::addressBase58();
  char brief[16] = "none";
  if (address.length() > 8) {
    snprintf(brief, sizeof brief, "%.4s..%s", address.c_str(), address.c_str() + address.length() - 4);
  }
  receipt::row(STUB_X0, STUB_X1, ROWS_Y, "VERSION", SOLANA_OS_VERSION);
  receipt::row(STUB_X0, STUB_X1, ROWS_Y + ROW_PITCH, "API", api);
  // As on the Identity page: the colour must not hide a key that is not in the secure chip.
  receipt::row(STUB_X0, STUB_X1, ROWS_Y + 2 * ROW_PITCH, "KEY", secure ? "secure chip" : location, false,
               th::color(secure ? th::STAMP_OK : th::STAMP_WARN));
  receipt::row(STUB_X0, STUB_X1, ROWS_Y + 3 * ROW_PITCH, "ADDRESS", brief);

  // Body: the link as text.
  readUrl(sShown, sizeof sShown);
  if (sShown[0] == '\0') {
    textCentered(BODY_CX, 104, "no link set");
    textCentered(BODY_CX, 122, "VKSET repo_url <url>", th::SUB);
  } else {
    drawUrl(sShown);
  }

  receipt::footer("", "CANCEL back");
}
}  // namespace

VK_SETTINGS_PAGE(about, "about", 140, "About", pageValue, nullptr, pageUpdate, pageDraw, 0);
