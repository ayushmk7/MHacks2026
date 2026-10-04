// src/native_apps/nativetest/nativetest.cpp
// A test fixture, not an app for people (apps.md, "Native test"): the native app the device tests
// launch, stop and draw over (t_native, t_shell, t_apr, t_notify, t_pages2). It fills the screen
// with two colours a test can count in a screenshot, and SELECT does nothing, so an approval test
// can press it freely. Compiled in the dev profile only, and hidden from the launcher.
// To write a native app, start from os/templates/native_app/ (scripts/new-app.sh --native).
#include "../../vk/vk_build.h"

#if VK_PROFILE_DEV

#include "../../vk/sdk/badge_sdk.hpp"

class NativeTest final : public badge::App {
 public:
  void on_draw() override {
    // The compile-time palette (src/ui/theme.h), not the active theme: t_native counts these two
    // colours in its screenshot.
    auto &c = display::canvas();
    c.fillScreen(theme::BG);
    display::textCentered("native test", display::width() / 2, 100, theme::GREEN, 2);
    display::touch();
  }
  void on_button(uint8_t key, bool pressed) override {
    if (key == BTN_B && pressed) badge::exit();     // CANCEL
  }
};

BADGE_APP(NativeTest, "nativetest", "Native test", "1.0.0", "", "hidden=1");

#endif  // VK_PROFILE_DEV
