// The approval screen's one drawing function (approval.md, "Screen").
// Implemented in approval_screen.cpp (WP12).
#pragma once

#include "../wallet/approval.h"

namespace vk::ui {

// `outcome` is non-null only in RESULT: it is what the result band and the footer's result word are drawn from.
// `footerBlink` is true while the footer is blinking after SELECT was pressed under rule DISABLED.
// The engine passes both; the screen keeps no state of its own.
void drawApproval(const vk::wallet::ApprovalRequest &, vk::wallet::approval::Phase, float holdProgress,
                  const vk::wallet::ApprovalOutcome *outcome, bool footerBlink);

}  // namespace vk::ui
