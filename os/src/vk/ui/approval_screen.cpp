// The approval screen: one fixed layout (approval.md, "Screen"), drawn with the receipt kit.
// A theme changes paper and ink only. The screen keeps no state: everything comes from the arguments.
#include "approval_screen.h"

#include <stdio.h>
#include <string.h>

#include "../../hal/display.h"
#include "../vk_build.h"
#include "../wallet/signer.h"
#include "receipt.h"
#include "theme.h"

namespace vk::ui {

namespace {

using vk::wallet::ApprovalOutcome;
using vk::wallet::ApprovalRequest;
using vk::wallet::SelectRule;
using vk::wallet::Severity;
using vk::wallet::approval::Phase;

constexpr uint16_t rgb565(uint32_t rgb) {
  return (uint16_t)((((rgb >> 16) & 0xF8) << 8) | (((rgb >> 8) & 0xFC) << 3) | ((rgb & 0xFF) >> 3));
}

// The severity colours are constants here and are NOT theme tokens: a theme that could recolour
// them could make a blocked payment look approved. The text on them is always black.
constexpr uint16_t SEVERITY_GREEN = rgb565(0x1FBF75);
constexpr uint16_t SEVERITY_AMBER = rgb565(0xFFB020);
constexpr uint16_t SEVERITY_RED = rgb565(0xFF4545);
constexpr uint16_t BAND_TEXT = 0x0000;

// ---- the layout table of approval.md, in pixels -------------------------------------------------
constexpr int SCREEN_W = 320, SCREEN_H = 240;
constexpr int MARGIN = 10;
constexpr int BAND_Y = 20, BAND_H = 27;                 // y 20..46, full width
constexpr int BAND_BASELINE = 38;                        // 11 px capitals centred in the band: rows 28..38
constexpr int BAND_SMALL_Y = 30;                         // the Font0 fallback: rows 30..36
constexpr int BAND_MAX_W = 300;
constexpr int PERF_X = 146, PERF_Y0 = 52, PERF_Y1 = 198;
constexpr int STUB_CX = 73;                              // left stub x 0..145
constexpr int STUB_MAX_W = 136;
constexpr int AMOUNT_Y = 58;
constexpr int SUB_Y = 134, SUB_MAX_CHARS = 23;
constexpr int TEXT_MID_Y = 92;                           // wrapped `big`: centred in rows 58..126
constexpr int TEXT_CAP_H = 12, TEXT_PITCH = 17, TEXT_MAX_LINES = 4;   // FreeSerifBold9pt7b
constexpr int BODY_X0 = 156, BODY_X1 = 310;              // body x 147..319
constexpr int ROW_Y0 = 58, ROW_PITCH = 18, MAX_ROWS = 4; // rows at y = 58, 76, 94, 112
constexpr int RULE_BELOW_ROW = 19;                       // a row ends at y+12; the rule is 6 px under it
constexpr int FOOTER_RULE_Y = 216, FOOTER_TEXT_Y = 224;

// Copies `in`, upper-cased when asked, with every byte outside printable ASCII as '?'. The engine
// already restricts the request's strings; this keeps the screen safe on its own.
void clean(char *out, size_t cap, const char *in, bool upper) {
  size_t n = 0;
  for (; in[n] != '\0' && n + 1 < cap; ++n) {
    char ch = in[n];
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    if (upper && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    out[n] = ch;
  }
  out[n] = '\0';
}

void restoreFont(LGFX_Sprite &c) {
  c.setFont(&fonts::Font0);
  c.setTextSize(1);
  c.setTextDatum(textdatum_t::top_left);
}

// ---- what the band says ----------------------------------------------------------------------------
// The coloured band alone carries the verdict: no stamp is drawn, in any phase.
struct Verdict {
  const char *band;        // nullptr: the request's headline
  uint16_t bandColor;
};

bool isConfirmation(const ApprovalRequest &r) { return strcmp(r.domain, "confirm") == 0; }

Verdict verdictOf(const ApprovalRequest &r, const ApprovalOutcome *outcome) {
  if (outcome != nullptr) {
    // RESULT: the band's text is the result word (the footer repeats it).
    if (outcome->approved) return {outcome->sig != nullptr ? "SIGNED" : "APPROVED", SEVERITY_GREEN};
    switch (outcome->reason) {
      case VK_CANCELLED:   return {"CANCELLED", theme::color(theme::FAINT)};
      case VK_TIMEOUT:     return {"TIMED OUT", theme::color(theme::FAINT)};
      case VK_SIGN_FAILED: return {"SIGN FAILED", SEVERITY_RED};
      default:             return {"BLOCKED", SEVERITY_RED};
    }
  }
  switch (r.severity) {
    case Severity::RED:   return {nullptr, SEVERITY_RED};
    case Severity::GREEN: return {nullptr, SEVERITY_GREEN};
    default:              return {nullptr, SEVERITY_AMBER};
  }
}

// ---- regions ----------------------------------------------------------------------------------------

void drawHeader(const ApprovalRequest &r) {
  char title[sizeof r.title], app[sizeof r.app_id], left[80], right[32];
  clean(title, sizeof title, r.title, true);
  clean(app, sizeof app, r.app_id, false);
  // "asked by" is left out for a confirmation raised by firmware.
  if (isConfirmation(r) || app[0] == '\0') snprintf(left, sizeof left, "%s", title);
  else snprintf(left, sizeof left, "%s \xC2\xB7 asked by %s", title, app);

  const char *where = vk::wallet::keyLocation();
  const char *key = strcmp(where, "se050") == 0 ? "KEY SE" : strcmp(where, "software") == 0 ? "KEY SW" : "NO KEY";
#if VK_PROFILE_DEV
  snprintf(right, sizeof right, "DEV BUILD \xC2\xB7 %s", key);   // always visible in a dev build
#else
  snprintf(right, sizeof right, "%s", key);
#endif
  receipt::headerText(left, right);
}

void drawBand(LGFX_Sprite &c, const char *text, uint16_t color) {
  c.fillRect(0, BAND_Y, SCREEN_W, BAND_H, color);
  char t[40];
  clean(t, sizeof t, text, false);
  c.setFont(&fonts::FreeMonoBold9pt7b);
  c.setTextSize(1);
  c.setTextColor(BAND_TEXT);
  if ((int)c.textWidth(t) <= BAND_MAX_W) {
    c.setTextDatum(textdatum_t::baseline_center);
    c.drawString(t, SCREEN_W / 2, BAND_BASELINE);
    restoreFont(c);
  } else {
    restoreFont(c);
    display::textCentered(t, SCREEN_W / 2, BAND_SMALL_Y, BAND_TEXT);
  }
}

// A payment's `big` is "<number> <unit>": split at the last space. Anything else (an app's name, a
// setting's key) is text, even when it contains a space.
bool splitAmount(const char *big, char *number, size_t numberCap, char *unit, size_t unitCap) {
  const char *space = strrchr(big, ' ');
  if (space == nullptr || space == big || space[1] == '\0') return false;
  bool digit = false;
  for (const char *p = big; p < space; ++p) {
    if (*p >= '0' && *p <= '9') digit = true;
    else if (*p != '.' && *p != ',') return false;
  }
  if (!digit) return false;
  const size_t n = (size_t)(space - big);
  if (n + 1 > numberCap) return false;
  memcpy(number, big, n);
  number[n] = '\0';
  strlcpy(unit, space + 1, unitCap);
  return true;
}

// `big` as wrapped text in FreeSerifBold9pt7b, centred in the stub.
void drawWrapped(LGFX_Sprite &c, const char *text) {
  char lines[TEXT_MAX_LINES][40];
  int count = 0;
  c.setFont(&fonts::FreeSerifBold9pt7b);
  c.setTextSize(1);
  const char *p = text;
  while (count < TEXT_MAX_LINES) {
    while (*p == ' ') ++p;
    if (*p == '\0') break;
    // The longest start of the rest that fits; broken at its last space when there is one.
    char tmp[40];
    size_t len = 0;
    int lastSpace = -1;
    while (p[len] != '\0' && len + 1 < sizeof tmp) {
      memcpy(tmp, p, len + 1);
      tmp[len + 1] = '\0';
      if ((int)c.textWidth(tmp) > STUB_MAX_W) break;
      if (p[len] == ' ') lastSpace = (int)len;
      ++len;
    }
    if (p[len] != '\0' && p[len] != ' ' && lastSpace > 0) len = (size_t)lastSpace;
    if (len == 0) len = 1;                             // never stand still
    memcpy(lines[count], p, len);
    lines[count][len] = '\0';
    ++count;
    p += len;
  }
  const int height = (count - 1) * TEXT_PITCH + TEXT_CAP_H;
  const int top = TEXT_MID_Y - height / 2;
  c.setTextColor(theme::color(theme::INK));
  c.setTextDatum(textdatum_t::baseline_center);
  for (int i = 0; i < count; ++i) c.drawString(lines[i], STUB_CX, top + TEXT_CAP_H - 1 + i * TEXT_PITCH);
  restoreFont(c);
}

void drawStub(LGFX_Sprite &c, const ApprovalRequest &r) {
  char big[sizeof r.big], number[sizeof r.big], unit[sizeof r.big];
  clean(big, sizeof big, r.big, false);
  if (splitAmount(big, number, sizeof number, unit, sizeof unit)) {
    receipt::amount(STUB_CX, AMOUNT_Y, "AMOUNT", number, unit);
  } else if (big[0] != '\0') {
    drawWrapped(c, big);
  }

  char sub[sizeof r.sub];
  clean(sub, sizeof sub, r.sub, true);
  if (strlen(sub) > (size_t)SUB_MAX_CHARS) {
    sub[SUB_MAX_CHARS - 2] = '.';
    sub[SUB_MAX_CHARS - 1] = '.';
    sub[SUB_MAX_CHARS] = '\0';
  }
  if (sub[0] != '\0') display::textCentered(sub, STUB_CX, SUB_Y, theme::color(theme::INK));
}

void drawBody(const ApprovalRequest &r) {
  const int count = r.line_count < MAX_ROWS ? r.line_count : MAX_ROWS;
  for (int i = 0; i < count; ++i) {
    char label[sizeof r.lines[i].label], value[sizeof r.lines[i].value];
    clean(label, sizeof label, r.lines[i].label, true);
    clean(value, sizeof value, r.lines[i].value, false);
    receipt::row(BODY_X0, BODY_X1, ROW_Y0 + i * ROW_PITCH, label, value);
  }
  if (count > 0) receipt::rule(ROW_Y0 + (count - 1) * ROW_PITCH + RULE_BELOW_ROW, BODY_X0, BODY_X1);
}

// `result` is the result word in RESULT (the same word as the band), otherwise nullptr.
void drawFooter(LGFX_Sprite &c, const ApprovalRequest &r, Phase phase, const char *result, bool blink) {
  const char *left, *right;
  if (result != nullptr) {
    left = result;                                       // the keys do nothing now: no key hints
    right = "";
  } else if (phase == Phase::SIGNING) {
    left = "Signing...";
    right = "";
  } else if (r.dev_override) {
    left = "DEV: hold SELECT to sign anyway";
    right = "CANCEL close";
  } else if (r.severity == Severity::RED || r.select == SelectRule::DISABLED) {
    left = "Blocked";
    right = "CANCEL close";
  } else if (r.select == SelectRule::HOLD) {
    left = "Hold SELECT";
    right = "CANCEL reject";
  } else {
    left = "SELECT approve";
    right = "CANCEL reject";
  }
  receipt::footer(left, right);
  if (blink) {
    // One frame of the footer inverted: ink ground, paper text.
    const uint16_t paper = theme::color(theme::PAPER);
    c.fillRect(0, FOOTER_RULE_Y + 1, SCREEN_W, SCREEN_H - FOOTER_RULE_Y - 1, theme::color(theme::INK));
    display::text(left, MARGIN, FOOTER_TEXT_Y, paper);
    display::textRight(right, SCREEN_W - MARGIN, FOOTER_TEXT_Y, paper);
  }
}

}  // namespace

void drawApproval(const vk::wallet::ApprovalRequest &request, vk::wallet::approval::Phase phase, float holdProgress,
                  const vk::wallet::ApprovalOutcome *outcome, bool footerBlink) {
  LGFX_Sprite &c = display::canvas();
  const Verdict verdict = verdictOf(request, outcome);

  receipt::page();
  drawHeader(request);
  drawBand(c, verdict.band != nullptr ? verdict.band : request.headline, verdict.bandColor);
  receipt::perforation(PERF_X, PERF_Y0, PERF_Y1);
  drawStub(c, request);
  drawBody(request);
  if (phase == Phase::HOLDING) receipt::holdBar(holdProgress);
  drawFooter(c, request, phase, outcome != nullptr ? verdict.band : nullptr, footerBlink);
  display::touch();
}

}  // namespace vk::ui
