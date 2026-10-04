// C++ alias of the reason codes shared with the host-tested C code (signing.md, "Reason codes").
#pragma once

#include "pure/vk_reason.h"

namespace vk::wallet { using Reason = vk_reason_t; inline const char *reasonName(Reason r) { return vk_reason_name(r); } }
