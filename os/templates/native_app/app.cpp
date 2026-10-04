// src/native_apps/__ID__/__ID__.cpp
// __NAME__: a native app made from os/templates/native_app by scripts/new-app.sh.
//
// A two-row list in the Receipt look: "Count" goes up on SELECT, "Reset" puts it back to zero.
// Replace the rows and the key handling with your own app; keep the shape. This one file is the
// whole app: the BADGE_APP line at the bottom registers it, so the build compiles it and the
// launcher lists it with no other edit. Deleting this folder and reflashing removes it.
//
// Rules for native code (docs/os/platform/native-apps.md): never block (on_update and on_draw
// together under 20 ms); draw only into display::canvas(), with the receipt kit and theme colours;
// CANCEL always leads out; sign only through vk::wallet::begin; release what you take in on_stop.
#include <stdio.h>

#include "../../vk/sdk/badge_sdk.hpp"
#include "../../vk/ui/receipt.h"

namespace {

namespace receipt = vk::ui::receipt;      // every colour comes from the active theme through it

// The list layout every BadgeOS list screen uses (docs/os/ui/ui.md, "Screens").
constexpr int X0 = 10, X1 = 310;          // the page margins
constexpr int TITLE_Y = 26;
constexpr int FIRST_ROW_Y = 46;           // a row at y owns y-5 .. y+12
constexpr int ROW_PITCH = 18;
constexpr int ROWS = 2;
constexpr float PAUSE_S = 0.25f;          // a frame this late follows a pause (an approval drew over us)

class __CLASS__ final : public badge::App {
 public:
  void on_start() override { dirty_ = true; }

  // A native app is told nothing when an approval closes over it: a late frame is how it knows
  // the canvas holds another picture and must be drawn again.
  void on_update(float dt) override {
    if (dt > PAUSE_S) dirty_ = true;
  }

  // Draw only when something changed: a full frame costs a draw and a 34 ms transfer to the panel.
  void on_draw() override {
    if (!dirty_) return;
    dirty_ = false;

    char right[32];
    receipt::statusRight(right, sizeof right);
    receipt::page();
    receipt::header("BADGEOS", right);
    receipt::title("__TITLE__", TITLE_Y);

    char value[16];
    snprintf(value, sizeof value, "%u", (unsigned)count_);
    receipt::row(X0, X1, FIRST_ROW_Y, "COUNT", value, selected_ == 0);
    receipt::row(X0, X1, FIRST_ROW_Y + ROW_PITCH, "RESET", "", selected_ == 1);

    receipt::footer("SELECT add", "CANCEL exit");
  }

  void on_button(uint8_t key, bool pressed) override {
    if (!pressed) return;
    dirty_ = true;
    switch (key) {
      case BTN_UP:
        if (selected_ > 0) --selected_;
        break;
      case BTN_DOWN:
        if (selected_ + 1 < ROWS) ++selected_;
        break;
      case BTN_A:                          // SELECT
        if (selected_ == 0) ++count_;
        else count_ = 0;
        break;
      case BTN_B:                          // CANCEL: back to the launcher
        badge::exit();
        break;
      default:
        break;
    }
  }

 private:
  uint32_t count_ = 0;
  int selected_ = 0;
  bool dirty_ = true;
};

}  // namespace

// id, the name the launcher shows, version, permissions (comma-separated, as in app.ini: "" for
// none), and the optional launcher keys of app.ini with ';' between them ("category=games",
// "hidden=1"). A dev-only app wraps this whole file in #if VK_PROFILE_DEV (include vk/vk_build.h).
BADGE_APP(__CLASS__, "__ID__", "__NAME__", "1.0.0", "", "__LAUNCHER__");
