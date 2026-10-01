// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/attestationbatch.h>

#include <consensus/merkle.h>

#include <algorithm>

uint256 ComputeCanonicalBatchRoot(std::vector <uint256> leaves) {
    std::sort(leaves.begin(), leaves.end());
    return ComputeMerkleRoot(leaves);
}

CPartialMerkleTree BuildAttestationBatchProof(const std::vector <uint256> &sortedLeaves, const uint256 &myLeaf) {
    auto it = std::find(sortedLeaves.begin(), sortedLeaves.end(), myLeaf);
    if (it == sortedLeaves.end()) {
        return CPartialMerkleTree();
    }
    std::vector<bool> match(sortedLeaves.size(), false);
    match[std::distance(sortedLeaves.begin(), it)] = true;
    return CPartialMerkleTree(sortedLeaves, match);
}
