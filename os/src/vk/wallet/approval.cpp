// Approval engine, WP01 stub: never active, so vk::modalActive() is false and hooks H4 and H19
// leave upstream's loop unchanged. The state machine arrives in WP12.
#include "approval.h"

namespace vk::wallet::approval {

bool open(const ApprovalRequest &request, const SignDomain *domain, const uint8_t *bytes, size_t len, const char *app_id) {
  (void)request; (void)domain; (void)bytes; (void)len; (void)app_id;
  return false;
}

Poll takeResult(uint8_t sig[64], Reason &reason) {
  (void)sig;
  reason = VK_IDLE;
  return Poll::IDLE;
}

Poll peekResult() { return Poll::IDLE; }

bool confirm(const ApprovalRequest &request, ConfirmDone done, void *arg) {
  (void)request; (void)done; (void)arg;
  return false;
}

bool active() { return false; }
void update() {}
Phase phase() { return Phase::IDLE; }
const ApprovalRequest *current() { return nullptr; }
void appStopping(const char *app_id) { (void)app_id; }

}  // namespace vk::wallet::approval
