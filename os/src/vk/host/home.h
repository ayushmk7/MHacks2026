// "The badge is idle" (ui.md): no app is running, so the shell is showing. There is no home service:
// when no app runs, the shell (src/vk/shell/) is the launcher.
#pragma once

namespace vk::host { bool idle(); }
