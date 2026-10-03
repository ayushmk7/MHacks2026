// src/native_apps/tipjar/tipjar.cpp
#include <cstdio>
#include <cstring>
#include "../../sdk/badge_sdk.hpp"

class TipJar final : public badge::App {
  enum class State { Scan, Hello, Ready, Confirm } state_ = State::Scan;
  static constexpr uint64_t kTip = 100;            // raw units: 1.00 HACK at 2 decimals
  badge_peer_t target_{};
  badge_pay_peer_t who_{};
  uint8_t txid_[64]{};
  uint32_t lastPoll_ = 0;
  char msg_[64] = "SELECT to find a badge";

  void say(const char *text) { std::snprintf(msg_, sizeof msg_, "%s", text); }

 public:
  void on_start() override { badge_led_take(); }

  void on_button(badge_key_t key, bool pressed) override {
    if (!pressed) return;
    if (key == BADGE_KEY_B) { badge_system_exit(); return; }
    if (key != BADGE_KEY_A) return;

    if (state_ == State::Scan) {
      if (badge_espnow_peers(&target_, 1) == 0) { say("no badge nearby"); return; }   // strongest first
      badge_pay_hello(target_.mac);
      state_ = State::Hello;
      std::snprintf(msg_, sizeof msg_, "asking %s", target_.name);
    } else if (state_ == State::Ready) {
      uint8_t blockhash[32], message[256], signature[64], wire[1 + 64 + 256];
      char err[64];
      if (badge_rpc_blockhash(blockhash) != BADGE_OK) { say("rpc: no blockhash"); return; }
      const size_t n = badge_sol_transfer_message(who_.pubkey, kTip, blockhash, message, sizeof message);
      if (n == 0) { say("could not build"); return; }
      const badge_sign_hint_t hint = {who_.pubkey, who_.name, "100", nullptr};
      const badge_err_t rc = badge_identity_sign(message, n, &hint, signature);   // blocks on the approval screen
      if (rc != BADGE_OK) { state_ = State::Scan; std::snprintf(msg_, sizeof msg_, "not signed (%d)", (int)rc); return; }
      const size_t w = badge_sol_wire(message, n, signature, wire, sizeof wire);
      if (badge_rpc_send(wire, w, txid_, err, sizeof err) != BADGE_OK) {
        state_ = State::Scan; std::snprintf(msg_, sizeof msg_, "send: %s", err); return;
      }
      state_ = State::Confirm;
      say("sent, waiting");
    }
  }

  void on_update(float) override {
    if (state_ == State::Hello) {
      if (badge_pay_peer(target_.mac, &who_)) {
        state_ = State::Ready;
        std::snprintf(msg_, sizeof msg_, "SELECT tips %s", who_.name);
      }
    } else if (state_ == State::Confirm && badge_system_millis() - lastPoll_ > 1000) {
      lastPoll_ = badge_system_millis();
      badge_tx_status_t status;
      if (badge_rpc_status(txid_, &status) != BADGE_OK) return;
      if (status == BADGE_TX_CONFIRMED || status == BADGE_TX_FINALIZED) {
        badge_led_pulse(20, 241, 149, 900); state_ = State::Scan; say("tip confirmed");
      } else if (status == BADGE_TX_FAILED) {
        badge_led_pulse(255, 69, 69, 900); state_ = State::Scan; say("tip failed");
      }
    }
  }

  void on_draw() override {
    badge_gfx_clear(BADGE_BG);
    badge_gfx_text_center("Tip Jar", 160, 30, BADGE_SOLANA_GREEN, 3);
    badge_gfx_text_center(msg_, 160, 110, BADGE_WHITE, 1);
    badge_gfx_text_center("SELECT tip 1.00 HACK   CANCEL exit", 160, 226, BADGE_MUTED, 1);
  }
};

BADGE_APP(TipJar, "tipjar-native", "Tip Jar (C++)", "1.0.0", BADGE_CAP_SIGN | BADGE_CAP_NET | BADGE_CAP_RADIO);
