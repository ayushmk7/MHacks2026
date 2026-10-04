// Settings page `store` (shell.md, "App store"): the state of upstream's store client and its two
// actions. The client stays compiled in, but DEFAULT_BROKER_URL is empty, so the page reads "off"
// until someone sets an address from the web page. Deleting this file removes the row only.

#include "../page.h"

#include "../../../net/broker_client.h"
#include "../../../net/wifi_mgr.h"

namespace {
using namespace vk::shell;
namespace receipt = vk::ui::receipt;
namespace th = vk::ui::theme;

List sList;

// Off when disabled in settings, and also when no address is set: there is nothing to talk to.
bool storeOff() { return !broker::enabled() || broker::url().length() == 0; }

void pageValue(char *out, size_t cap) { snprintf(out, cap, "%s", storeOff() ? "off" : broker::stateText()); }

void pageEnter() { sList = List(); }

void pageUpdate() {
  if (back()) return;  // CANCEL
  listMove(sList, 2, 2);
  if (buttons::pressed(BTN_A)) {
    if (sList.cursor == 0) {
      broker::setEnabled(!broker::enabled());
    } else {
      broker::forget();
    }
    repaint();
  }
}

void pageDraw() {
  frame("APP STORE", "SELECT choose");

  const bool off = storeOff();
  const broker::State state = broker::state();
  uint16_t stateColor = 0;
  if (off) {
    stateColor = onOffColor(false);
  } else if (state == broker::State::Error) {
    stateColor = th::color(th::STAMP_BAD);
  } else if (state == broker::State::Idle || state == broker::State::Offered ||
             state == broker::State::Installing) {
    stateColor = th::color(th::STAMP_OK);
  }
  receipt::row(X0, X1, 48, "STATE", off ? "off" : broker::stateText(), false, stateColor);

  const bool registered = broker::registered();
  receipt::row(X0, X1, 66, "REGISTERED", registered ? "yes" : "no", false, onOffColor(registered));

  const String address = broker::url();
  receipt::row(X0, X1, 84, "ADDRESS", address.length() ? address.c_str() : "none");

  const String error = broker::lastError();
  if (error.length()) text(10, 102, error.c_str(), th::STAMP_BAD);

  // The same reason the Wi-Fi page cannot take a passphrase: the badge has no keyboard.
  text(10, 118, "The address is set from the web page,", th::SUB);
  text(10, 130, "not from here: no keyboard.", th::SUB);
  if (wifi_mgr::connected()) {
    const String web = "http://" + wifi_mgr::ip().toString() + "/";
    text(10, 142, web.c_str());
  } else {
    text(10, 142, "Settings > Wi-Fi, then browse to the badge", th::SUB);
  }

  receipt::rule(158);

  const bool enabled = broker::enabled();
  const ListRow rows[] = {
      {"App store", onOff(enabled), onOffColor(enabled)},
      {"Forget registration", registered ? "re-register" : "", 0},
  };
  listDraw(sList, rows, 2, 168, 2);
}
}  // namespace

VK_SETTINGS_PAGE(store, "store", 60, "App store", pageValue, pageEnter, pageUpdate, pageDraw, 1000);
