// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_ATTESTATIONBATCH_H
#define BITCOIN_EVO_ATTESTATIONBATCH_H

#include <merkleblock.h>
#include <uint256.h>

#include <vector>

/** 5.4.4.2.1 (build-plan.md, F-244): the canonical-ordering half of batch
 *  construction -- deliberately separate from everything that needs the
 *  signing session itself (the scheduler, AsyncSignIfMember, a P2P request
 *  message, HandleNewRecoveredSig), none of which this file builds; see
 *  build-plan.md's own 5.4.4.2 row for why that half is still an open
 *  protocol question (N quorum members agreeing on identical batch
 *  membership with no leader), not a mechanical one. What IS mechanical,
 *  and built here: once a set of pending attestation requests is decided
 *  (by whatever 5.4.4.2.2 ends up being), every party holding that SAME
 *  set must independently compute the SAME Merkle root, or their BLS
 *  signature shares will not aggregate (every signer must sign the exact
 *  same message). Sorting by the leaf hashes themselves, not by arrival/
 *  insertion order, means agreement on ordering falls out of agreement on
 *  membership for free -- no separate ordering-agreement protocol needed.
 *  Reuses ComputeMerkleRoot (consensus/merkle.h) and CPartialMerkleTree
 *  (merkleblock.h, the existing Bitcoin SPV inclusion-proof primitive
 *  CAttestationPayload's own v2 shape already carries, evo/attestedtx.h)
 *  directly -- no new Merkle-tree code. */
uint256 ComputeCanonicalBatchRoot(std::vector <uint256> leaves);

/** The complementary half: given the SAME sorted leaf list
 *  ComputeCanonicalBatchRoot was (or would be) computed from, builds the
 *  CPartialMerkleTree proving `myLeaf` is included, for a caller to hand
 *  back to whichever party's own CAttestationPayload.batchProof needs to
 *  carry it. `sortedLeaves` must already be sorted the identical way
 *  ComputeCanonicalBatchRoot sorts internally -- this function does not
 *  re-sort, so a caller that skips the shared sort step would silently
 *  build a proof against the WRONG tree structure (confirmed, not assumed,
 *  by this file's own tests: a cheap way to prove the sort step is
 *  actually load-bearing rather than redundant). Returns a default-
 *  constructed (empty) tree if `myLeaf` is not present in `sortedLeaves`
 *  at all -- a caller's own bug to avoid, not something this function can
 *  recover from. */
CPartialMerkleTree BuildAttestationBatchProof(const std::vector <uint256> &sortedLeaves, const uint256 &myLeaf);

#endif //BITCOIN_EVO_ATTESTATIONBATCH_H
