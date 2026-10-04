// The one extra entry into the key path (signing.md, "Who owns what").
// Only src/vk/wallet/approval.cpp includes this file.
#pragma once

#include "signer.h"

namespace vk::wallet {

// Signs `bytes` for `domain` after the user pressed SELECT on the approval. Goes through signRaw.
Reason signForApproval(const SignDomain *domain, const uint8_t *bytes, size_t len, uint8_t sig[64]);

}  // namespace vk::wallet
