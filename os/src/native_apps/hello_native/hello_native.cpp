// src/native_apps/hello_native/hello_native.cpp
#include "../../vk/sdk/badge_sdk.hpp"

class HelloNative final : public badge::App {
 public:
  void on_draw() override {
    auto &c = display::canvas();
    c.fillScreen(theme::BG);
    display::textCentered("gm from C++", display::width() / 2, 100, theme::GREEN, 2);
    display::touch();
  }
  void on_button(uint8_t key, bool pressed) override {
    if (key == BTN_B && pressed) badge::exit();     // CANCEL
  }
};

BADGE_APP(HelloNative, "hello_native", "Hello (C++)", "1.0.0", "");
