// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <announcerring.h>

void CAnnouncerRing::Record(int height, const uint256 &hash, int currentTipHeight, int depth) {
    int cutoff = currentTipHeight - depth;

    for (auto it = m_heightByHash.begin(); it != m_heightByHash.end();) {
        if (it->second < cutoff) {
            it = m_heightByHash.erase(it);
        } else {
            ++it;
        }
    }

    if (height >= cutoff) {
        m_heightByHash[hash] = height;
    }
}

bool CAnnouncerRing::WasAnnounced(const uint256 &hash, int currentTipHeight, int depth) const {
    auto it = m_heightByHash.find(hash);
    if (it == m_heightByHash.end()) {
        return false;
    }
    return it->second >= currentTipHeight - depth;
}
