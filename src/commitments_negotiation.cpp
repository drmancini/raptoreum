// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <commitments_negotiation.h>

bool IsCommitmentFormatActive(const CBlockIndex* pindexPrev) {
    // TODO(4.6.1): see commitments_negotiation.h's own doc comment -- substitute
    // the real Updates().IsActive(EUpdate::<...>, pindexPrev) call here once
    // 4.6.1 registers the real bit. Deliberately unused until then: this must
    // not silently touch consensus/cs_main state, so it takes the pointer
    // (matching the eventual real signature) without reading it.
    (void) pindexPrev;
    return false;
}
