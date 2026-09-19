// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_CONSENSUS_H
#define BITCOIN_CONSENSUS_CONSENSUS_H

#include <stdlib.h>
#include <stdint.h>
#include <limits>

/** The maximum allowed size for a serialized block, in bytes (network rule) */
static const unsigned int MAX_LEGACY_BLOCK_SIZE = 1000000;
static const unsigned int MAX_DIP0001_BLOCK_SIZE = 8000000;

/**
 * 1.2 (body-byte / input-count cap, Mike, 2026-09-19): two limits, not one,
 * because a byte cap alone can't distinguish realistic traffic from an
 * attacker packing minimal-size inputs. Sized for a ~1,500 tx/s theoretical
 * ceiling of REALISTIC multi-input/output traffic (not the 2-in/2-out toy
 * corpus used elsewhere): a planning shape of 3 inputs + 3 outputs (~556 B,
 * mirroring K-14's own "a choice, not a measurement" convention) needs
 * 1,500 * 120 * 556 =~ 100 MB of body bytes and 1,500 * 120 * 3 = 540,000
 * inputs.
 *
 * COMMITMENT_BUDGET_BODY_BYTES (~110 MB) is generous: sized for storage and
 * to comfortably fit that traffic, not for time -- an attacker who packs the
 * same bytes into ~41-byte minimal inputs (the blanked-preimage minimum,
 * F-91/F-93) gets ~4.5x the input count realistic traffic ever achieves per
 * byte, which is exactly why a byte cap alone cannot bound worst-case time.
 *
 * COMMITMENT_BUDGET_MAX_INPUTS (700,000) is the one that actually bounds
 * time, independent of how generous the byte cap is: ~30% headroom over the
 * 540,000 legitimate need, and at F-97's ~31 us/input worst-case rate,
 * 700,000 * 31 us =~ 21.7 s, ~18% of the 120 s interval -- regardless of the
 * byte cap's size, because input count is what actually drives the cost
 * (F-93, F-94, F-97), not bytes.
 */
static const unsigned int COMMITMENT_BUDGET_BODY_BYTES = 110000000;
static const unsigned int COMMITMENT_BUDGET_MAX_INPUTS = 700000;

/**
 * The maximum allowed size for a serialized block, in bytes (network rule).
 * fCommitmentBudgetActive (default false, so every existing call site is
 * unchanged) swaps the byte-indexed legacy cap for the deliberate body-byte
 * budget above. Before 1.3's identifier/body split exists, a materialised
 * commitment block's own serialized size already IS the body bytes it
 * commits to, so this is a faithful stand-in for "committed body bytes <=
 * budget" until that split is real.
 */
inline unsigned int MaxBlockSize(bool fDIP0001Active = true, bool fCommitmentBudgetActive = false) {
    return fCommitmentBudgetActive ? COMMITMENT_BUDGET_BODY_BYTES
                                   : (fDIP0001Active ? MAX_DIP0001_BLOCK_SIZE : MAX_LEGACY_BLOCK_SIZE);
}

/**
 * The maximum allowed aggregate (non-coinbase) input count across a block.
 * No pre-1.2 concept exists for this -- it returns effectively unlimited
 * when the budget is off, since nothing bounded aggregate input count
 * before this. See GetBlockInputCount (consensus/tx_verify.h) for what it
 * counts and why the coinbase's own dummy input is excluded.
 */
inline unsigned int MaxBlockInputs(bool fCommitmentBudgetActive = false) {
    return fCommitmentBudgetActive ? COMMITMENT_BUDGET_MAX_INPUTS : std::numeric_limits<unsigned int>::max();
}

/**
 * 1.2 (D-18): the deliberate accurate-sigop budget for a commitment block,
 * independent of block bytes. Derived from ordinary 2-in/2-out traffic at the
 * 520 tx/s sustained floor (X-1) and ~4 accurate sigops/tx (F-11, F-30):
 * 520 * 120 * 4 ~= 249,600, rounded up. Worst-case validation time was
 * mis-derived twice (D-18): ~130 us/sigop (F-12) gave a wrong "~27%, tighter
 * than today"; the honest, directly-measured worst case at K-3's real per-tx
 * ceiling is ~320 us/sigop (F-97), ~80 s, ~67% of the 120 s interval -- see
 * transaction-decoupling.md SS1A and findings.md D-18. Only meaningful when
 * g_commitmentBudgetActive (validation.h) is set; test-only until 4.6 has a
 * real activation bit.
 */
static const unsigned int COMMITMENT_BUDGET_SIGOPS = 250000;

/**
 * The maximum allowed number of signature check operations in a block
 * (network rule). fCommitmentBudgetActive (default false, so every existing
 * call site is unchanged) swaps the byte-indexed legacy cap for the
 * deliberate committed-count budget above (1.2, F-88's gating mechanism,
 * mirroring the fDIP0001Active pattern already used here).
 */
inline unsigned int MaxBlockSigOps(bool fDIP0001Active = true, bool fCommitmentBudgetActive = false) {
    return fCommitmentBudgetActive ? COMMITMENT_BUDGET_SIGOPS : MaxBlockSize(fDIP0001Active) / 50;
}

/** The maximum allowed size of version 3 extra payload */
static const unsigned int MAX_TX_EXTRA_PAYLOAD = 10000;
/** Coinbase transaction outputs can only be spent after this number of new blocks (network rule) */
static const int COINBASE_MATURITY = 100;

/** Flags for nSequence and nLockTime locks */
/** Interpret sequence numbers as relative lock-time constraints. */
static constexpr unsigned int LOCKTIME_VERIFY_SEQUENCE = (1 << 0);
/** Use GetMedianTimePast() instead of nTime for end point timestamp. */
static constexpr unsigned int LOCKTIME_MEDIAN_TIME_PAST = (1 << 1);

#endif // BITCOIN_CONSENSUS_CONSENSUS_H
