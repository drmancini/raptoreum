// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COMMITMENTS_NEGOTIATION_H
#define BITCOIN_COMMITMENTS_NEGOTIATION_H

class CBlockIndex;

/**
 * 4.6.2 (build-plan.md's 4.6 row, docs/findings.md's F-205 thread 2): the
 * real, height-gated trigger for commitment-form P2P negotiation
 * (NODE_COMMITMENTS/SENDCOMMITMENTS/CanReceiveCommitments, protocol.h) --
 * a PLACEHOLDER pending 4.6.1's own real EUpdate bit.
 *
 * TODO(4.6.1): 4.6.1 bundles format, the block-size/input-count budget
 * (currently g_commitmentBudgetActive, validation.h/.cpp) and the
 * message-length raise behind ONE real, height-gated EUpdate bit
 * (docs/findings.md's F-205, thread 1). Once that bit is registered, this
 * function's body must become something shaped like:
 *
 *     return pindexPrev != nullptr &&
 *            Updates().IsActive(EUpdate::<THE REAL BIT 4.6.1 REGISTERS>, pindexPrev);
 *
 * matching this codebase's own established pattern for exactly this "is a
 * height-gated behaviour live right now" question at a call site --
 * `Updates().IsActive(EUpdate::DEPLOYMENT_V17, pindex)` (validation.cpp:2250)
 * and `Updates().IsActive(EUpdate::ROUND_VOTING, pQuorumBaseBlockIndex)`
 * (llmq/quorums_dkgsessionmgr.cpp:372) are the two live precedents this was
 * modelled on, not a new interface invented for this feature. `Updates()`
 * itself is the free function declared in chainparams.h.
 *
 * This is deliberately NOT implemented against a guessed `EUpdate` enum
 * value: `EUpdate` is a single shared enum, and 4.6.1 (a sibling task, not
 * yet built in this worktree) owns picking its real bit number and name --
 * inventing one here risks colliding with whatever 4.6.1 actually adds.
 * Until 4.6.1 lands there is no bit to query, so this always returns
 * false -- false is the safe default for anything consensus-adjacent whose
 * activation mechanism hasn't shipped yet, matching
 * `g_commitmentBudgetActive`'s own `std::atomic<bool>{false}` default
 * (validation.cpp:128).
 */
bool IsCommitmentFormatActive(const CBlockIndex* pindexPrev);

/**
 * Pure boolean composition, deliberately kept separate from
 * IsCommitmentFormatActive's own placeholder body (and inline here, matching
 * protocol.h's own CanReceiveCommitments/ShouldNegotiateCommitments
 * convention) so it stays mutation-testable in full today: while
 * IsCommitmentFormatActive is hardcoded false (no real bit exists yet), no
 * test observing ShouldNegotiateCommitmentsNow end-to-end can distinguish
 * "fManualOverride || IsCommitmentFormatActive(...)" from "fManualOverride"
 * alone, since the second operand is always false either way. This
 * combinator has no such blind spot -- its own unit tests exercise all four
 * (bool, bool) combinations directly.
 */
static inline bool CombineNegotiationTrigger(bool fRealActivation, bool fManualOverride) {
    return fManualOverride || fRealActivation;
}

/**
 * Whether commitment-form P2P negotiation should be attempted right now, at
 * one of the call sites this gates: NODE_COMMITMENTS advertisement at
 * startup (init.cpp), and the GETBODYRANGE fetch-issuing gate
 * (net_processing.cpp). `fManualOverride` is this project's own existing
 * `-commitmentblocks`/`-fetchbodyrange` debug-only flag (DEBUG_TEST,
 * default off) at that call site -- kept as a live, always-available
 * override so existing test coverage (feature_body_refetch.py,
 * bodyrange_tests.cpp/bodystore_tests.cpp and friends) keeps exercising
 * this machinery unchanged while there is no real bit to activate against.
 * Production behaviour is driven by IsCommitmentFormatActive once 4.6.1
 * lands; until then this is equivalent to the flag alone.
 */
static inline bool ShouldNegotiateCommitmentsNow(const CBlockIndex* pindexPrev, bool fManualOverride) {
    return CombineNegotiationTrigger(IsCommitmentFormatActive(pindexPrev), fManualOverride);
}

#endif // BITCOIN_COMMITMENTS_NEGOTIATION_H
