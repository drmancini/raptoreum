// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/attestationbatch.h>

#include <consensus/merkle.h>
#include <hash.h>

#include <algorithm>

uint256 ComputeCanonicalBatchRoot(std::vector <uint256> leaves) {
    std::sort(leaves.begin(), leaves.end());
    return ComputeMerkleRoot(leaves);
}

CAttestationBatchProof BuildAttestationBatchProof(const std::vector <uint256> &sortedLeaves, const uint256 &myLeaf) {
    auto it = std::find(sortedLeaves.begin(), sortedLeaves.end(), myLeaf);
    if (it == sortedLeaves.end()) {
        return CAttestationBatchProof();
    }

    CAttestationBatchProof proof;
    proof.leafIndex = static_cast<uint32_t>(std::distance(sortedLeaves.begin(), it));

    std::vector <uint256> level = sortedLeaves;
    uint32_t idx = proof.leafIndex;
    while (level.size() > 1) {
        // ComputeMerkleRoot's own odd-duplication rule (consensus/merkle.cpp),
        // mirrored exactly: duplicate the last element before pairing, so a
        // self-paired node's own "sibling" is a copy of itself -- the exact
        // value ComputeMerkleRoot would have hashed it against.
        if (level.size() & 1) {
            level.push_back(level.back());
        }
        uint32_t siblingIdx = (idx % 2 == 0) ? idx + 1 : idx - 1;
        proof.siblingHashes.push_back(level[siblingIdx]);

        std::vector <uint256> next(level.size() / 2);
        for (size_t i = 0; i < next.size(); i++) {
            next[i] = Hash(level[2 * i].begin(), level[2 * i].end(), level[2 * i + 1].begin(),
                           level[2 * i + 1].end());
        }
        level = std::move(next);
        idx /= 2;
    }
    return proof;
}

uint256 ExtractAttestationBatchRoot(const uint256 &leaf, const CAttestationBatchProof &proof) {
    uint256 current = leaf;
    uint32_t idx = proof.leafIndex;
    for (const uint256 &sibling: proof.siblingHashes) {
        if (idx % 2 == 0) {
            current = Hash(current.begin(), current.end(), sibling.begin(), sibling.end());
        } else {
            current = Hash(sibling.begin(), sibling.end(), current.begin(), current.end());
        }
        idx /= 2;
    }
    return current;
}
