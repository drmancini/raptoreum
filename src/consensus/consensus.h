// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_CONSENSUS_H
#define BITCOIN_CONSENSUS_CONSENSUS_H

#include <stdlib.h>
#include <stdint.h>

/** The maximum allowed size for a serialized block, in bytes (network rule) */
static const unsigned int MAX_LEGACY_BLOCK_SIZE = 1000000;
static const unsigned int MAX_DIP0001_BLOCK_SIZE = 8000000;

inline unsigned int MaxBlockSize(bool fDIP0001Active = true) {
    return fDIP0001Active ? MAX_DIP0001_BLOCK_SIZE : MAX_LEGACY_BLOCK_SIZE;
}

/**
 * 1.2 (D-18): the deliberate accurate-sigop budget for a commitment block,
 * independent of block bytes. Derived from ordinary 2-in/2-out traffic at the
 * 520 tx/s sustained floor (X-1) and ~4 accurate sigops/tx (F-11, F-30):
 * 520 * 120 * 4 ~= 249,600, rounded up. At ~130 us/sigop (F-12) worst-case
 * validation is ~32.4 s, ~27% of the 120 s interval -- see transaction-
 * decoupling.md SS1A. Only meaningful when g_commitmentBudgetActive
 * (validation.h) is set; test-only until 4.6 has a real activation bit.
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
