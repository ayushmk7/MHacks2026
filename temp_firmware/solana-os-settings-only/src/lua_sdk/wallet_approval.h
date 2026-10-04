/*
  The wallet stand-in's approval screen (lib_wallet.cpp). While it is up the
  runtime still runs the app's on_update - so the app can keep calling
  wallet.poll() - but draws this screen instead of on_draw and sends the
  buttons here instead of on_button (P1-A §4.4).
*/
#pragma once

#include <Arduino.h>

namespace wallet_approval {

bool active();
void draw();
void button(uint8_t key, bool pressed);
// Drops any approval in progress. Called when the app stops.
void reset();

}  // namespace wallet_approval
