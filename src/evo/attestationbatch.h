// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_ATTESTATIONBATCH_H
#define BITCOIN_EVO_ATTESTATIONBATCH_H

#include <serialize.h>
#include <uint256.h>

#include <cstdint>
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
 *  Reuses ComputeMerkleRoot (consensus/merkle.h) directly -- no new root-
 *  computation code. */
uint256 ComputeCanonicalBatchRoot(std::vector <uint256> leaves);

/** 5.4.4.1-OPEN item (b) (build-plan.md, F-247), the owner's own choice
 *  (AskUserQuestion, 2026-10-01) over two alternatives: canonicalizing
 *  CPartialMerkleTree's own shared vBits encoding (touches a pre-existing
 *  class used for real SPV merkleblock wire messages elsewhere, a far
 *  wider blast radius than this one feature needs), or committing a
 *  canonical encoding into what the quorum signs instead (does not, on
 *  its own, give CheckAttestedTx -- which never holds the full leaf list,
 *  only one leaf and a proof -- anything to reconstruct that canonical
 *  form FROM, so it would need pairing with another change anyway).
 *
 *  A proof that ONE specific leaf belongs to a batch, as a plain ordered
 *  list of sibling hashes from the leaf's own level up to the root, plus
 *  the leaf's own original index -- nothing else. Deliberately NOT
 *  CPartialMerkleTree (merkleblock.h, the general multi-leaf SPV
 *  inclusion-proof primitive 5.4.4.1/F-243 originally reused for this):
 *  that class's own flag-byte/vBits encoding has no canonical form for a
 *  decoded result (confirmed, not assumed, F-244 -- a flipped, unused
 *  padding bit decodes identically, a real txid-malleability vector for a
 *  consensus-relevant payload). A bare sibling-hash list has no padding,
 *  flag bits, or any other encoding choice left to be malleable in at all
 *  -- canonical by construction, not by an added check bolted onto a
 *  format that still has room to differ underneath it. */
class CAttestationBatchProof {
public:
    std::vector <uint256> siblingHashes; // leaf's own level first, the level just below the root last
    uint32_t leafIndex{0}; // this leaf's own index into the sorted leaf list BuildAttestationBatchProof was given

    SERIALIZE_METHODS(CAttestationBatchProof, obj)
    {
        READWRITE(obj.siblingHashes, obj.leafIndex);
    }
};

/** The complementary half: given the SAME sorted leaf list
 *  ComputeCanonicalBatchRoot was (or would be) computed from, builds a
 *  CAttestationBatchProof proving `myLeaf` is included, for a caller to
 *  hand back to whichever party's own CAttestationPayload.batchProof
 *  needs to carry it. `sortedLeaves` must already be sorted the identical
 *  way ComputeCanonicalBatchRoot sorts internally -- this function does
 *  not re-sort, so a caller that skips the shared sort step would
 *  silently build a proof against the WRONG tree structure (confirmed,
 *  not assumed, by this file's own tests: a cheap way to prove the sort
 *  step is actually load-bearing rather than redundant). Mirrors
 *  ComputeMerkleRoot's own pairing/odd-duplication rule (consensus/merkle.cpp)
 *  exactly at every level -- confirmed by reading it line by line before
 *  writing this, not assumed compatible; a self-paired node's own
 *  "sibling" at an odd level is a copy of itself, the exact value
 *  ComputeMerkleRoot would have hashed it against. Returns a default-
 *  constructed (empty siblingHashes, leafIndex 0) proof if `myLeaf` is not
 *  present in `sortedLeaves` at all -- a caller's own bug to avoid, not
 *  something this function can recover from; such a proof simply fails
 *  ExtractAttestationBatchRoot's own downstream verification against any
 *  real root, the same as any other malformed proof does, not a special
 *  case this function itself needs to flag. */
CAttestationBatchProof BuildAttestationBatchProof(const std::vector <uint256> &sortedLeaves, const uint256 &myLeaf);

/** Recomputes the batch root `proof` claims `leaf` belongs to, by
 *  re-hashing up from `leaf` through each of `proof`'s own sibling
 *  hashes, following ComputeMerkleRoot's own pairing rule at every level
 *  (leafIndex's own parity at each level, halved going up, decides left/
 *  right concatenation order -- even is left, matching the pairing
 *  SHA256D64/ComputeMerkleRoot itself performs on positions (2i, 2i+1)).
 *  There is no "matching" step the way CPartialMerkleTree::ExtractMatches
 *  has, and therefore no "zero matches"/"more than one match" failure
 *  mode to guard against at all: `leaf` is hashed up directly, never
 *  looked up in a list, so the result is always a single, deterministic
 *  candidate root -- a malformed or adversarial proof simply produces the
 *  WRONG root, which fails verification the same ordinary way any wrong
 *  root already does (CheckAttestedTx's own VerifyRecoveredSig call), not
 *  a distinct error path this function itself needs to detect. */
uint256 ExtractAttestationBatchRoot(const uint256 &leaf, const CAttestationBatchProof &proof);

#endif //BITCOIN_EVO_ATTESTATIONBATCH_H
