// Settings page `identity` and its sub-screen `identity_new` (shell.md, "Identity", "New identity").
//
// The one shell file that includes identity.h: it shows the badge ID, where the key lives and the
// public key, and it calls identity::regenerate(). It never signs.
//
// The identity is the wallet key, so a new identity is never made on a plain press: SELECT on
// `identity_new` raises the hold-SELECT firmware confirmation, and only its approval regenerates.

#include "../page.h"

#include "../../../badge_log.h"
#include "../../../identity/identity.h"
#include "../../../net/broker_client.h"
#include "../../ui/repaint.h"
#include "../../wallet/approval.h"

namespace {
using namespace vk::shell;
namespace receipt = vk::ui::receipt;
namespace th = vk::ui::theme;

constexpr int KEY_PER_LINE = 32;   // base58 is 43 or 44 characters: two lines

// ---------------------------------------------------------------------------------------------
// identity_new
// ---------------------------------------------------------------------------------------------

void newUpdate();
void newDraw();

const Screen kIdentityNew = {"identity_new", nullptr, newUpdate, newDraw, 0};

// Called once from the main loop when the confirmation closes. Rejected or timed out: nothing
// happens and the warning screen is still on top (the approval engine requests the repaint).
void onNewIdentityConfirmed(bool approved, void *) {
  if (!approved) return;
  // Blocks for a second or two on both key paths.
  const bool ok = identity::regenerate();
  // The stored token was issued to the old public key: the next broker tick registers again under
  // the new badge ID instead of collecting refusals.
  broker::forget();
  badge_log::tagf("ui", "identity regenerated: %s", ok ? "ok" : "failed");
  if (ok) {
    pulseLed(700);
  } else {
    pulseLedBad(700);
  }
  if (top() == &kIdentityNew) pop();   // back to `identity`, which now shows the new ID
  vk::ui::requestShellRepaint();
}

void newUpdate() {
  if (back()) return;  // CANCEL: back to `identity`, nothing done
  if (!buttons::pressed(BTN_A)) return;

  using namespace vk::wallet;
  ApprovalRequest request{};
  strlcpy(request.title, "New identity", sizeof request.title);
  strlcpy(request.headline, "ERASE BADGE KEY", sizeof request.headline);
  strlcpy(request.big, identity::badgeId().c_str(), sizeof request.big);
  strlcpy(request.lines[0].label, "Erases", sizeof request.lines[0].label);
  strlcpy(request.lines[0].value, "the badge key", sizeof request.lines[0].value);
  strlcpy(request.lines[1].label, "Keeps", sizeof request.lines[1].label);
  strlcpy(request.lines[1].value, "apps, history, contacts", sizeof request.lines[1].value);
  request.line_count = 2;
  request.severity = Severity::AMBER;
  request.select = SelectRule::HOLD;
  // False when another approval is open: nothing happens.
  approval::confirm(request, onNewIdentityConfirmed, nullptr);
}

void newDraw() {
  frame("NEW IDENTITY", "SELECT continue");
  text(10, 48, "This throws the keypair away and");
  text(10, 62, "makes a new one. It cannot be undone.");
  text(10, 84, "The badge ID and wallet address change, so:", th::SUB);
  text(10, 100, "- funds stay with the old address: move them first", th::SUB);
  text(10, 114, "- the registry record and contacts name the old key", th::SUB, 52);
  text(10, 128, "- a configured app store loses this badge", th::SUB);
  text(10, 142, "- installed apps are left alone", th::SUB);

  const String id = identity::badgeId();
  const String change = id.length() ? id + "  ->  ?" : String("no identity yet");
  text(10, 166, change.c_str(), th::STAMP_WARN);
}

// ---------------------------------------------------------------------------------------------
// identity
// ---------------------------------------------------------------------------------------------

void pageValue(char *out, size_t cap) {
  const String id = identity::ready() ? identity::badgeId() : String();
  snprintf(out, cap, "%s", id.length() ? id.c_str() : "not ready");
}

void pageUpdate() {
  if (back()) return;  // CANCEL
  if (buttons::pressed(BTN_A)) push(&kIdentityNew);
}

void pageDraw() {
  frame("IDENTITY", "SELECT new identity");

  // The badge ID is the one string a stranger reads off this screen, so it is printed large.
  const String id = identity::ready() ? identity::badgeId() : String("--------");
  receipt::amount(160, 44, "BADGE ID", id.c_str(), "");

  // The two key paths promise different things; the colour must not hide a dead SE050.
  const bool secure = identity::source() == identity::Source::SecureElement;
  receipt::row(X0, X1, 104, "KEY LIVES IN", identity::sourceName(), false,
               th::color(secure ? th::STAMP_OK : th::STAMP_WARN));

  const String status = identity::status();
  text(10, 120, status.c_str(), th::SUB);

  text(10, 136, "PUBLIC KEY", th::SUB);
  // Wrapped by hand: a half-shown key is useless for checking against a wallet.
  const String key = identity::publicKeyBase58();
  int y = 148;
  for (int offset = 0; offset < (int)key.length() && y <= 160; offset += KEY_PER_LINE, y += 12) {
    const String part = key.substring(offset, offset + KEY_PER_LINE);
    text(10, y, part.c_str());
  }

  receipt::row(X0, X1, 190, "New identity", "changes the badge ID", true);
}
}  // namespace

VK_SETTINGS_PAGE(identity, "identity", 70, "Identity", pageValue, nullptr, pageUpdate, pageDraw, 0);
