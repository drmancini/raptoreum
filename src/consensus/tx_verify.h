// Copyright (c) 2017-2023 The Bitcoin Core developers
// Copyright (c) 2020-2023 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_TX_VERIFY_H
#define BITCOIN_CONSENSUS_TX_VERIFY_H

#include <amount.h>

#include <stdint.h>
#include <vector>

class CBlockIndex;

class CCoinsViewCache;

class CTransaction;

class CValidationState;

/** Transaction validation functions */

namespace Consensus {
/**
 * Check whether all inputs of this transaction are valid (no double spends and amounts)
 * This does not modify the UTXO set. This does not check scripts and sigs.
 * @param[out] txfee Set to the transaction fee if successful.
 * Preconditions: tx.IsCoinBase() is false.
 */
    bool CheckTxInputs(const CTransaction &tx, CValidationState &state, const CCoinsViewCache &inputs, int nSpendHeight,
                       CAmount &txfee, CAmount &specialTxFee, bool fFeeVerify = false);
} // namespace Consensus

/** Auxiliary functions for transaction validation (ideally should not be exposed) */

/**
 * Count ECDSA signature operations the old-fashioned (pre-0.6) way
 * @return number of sigops this transaction's outputs will produce when spent
 * @see CTransaction::FetchInputs
 */
unsigned int GetLegacySigOpCount(const CTransaction &tx);

/**
 * Count ECDSA signature operations in pay-to-script-hash inputs.
 *
 * @param[in] mapInputs Map of previous transactions that have outputs we're spending
 * @return maximum number of sigops required to validate this transaction's inputs
 * @see CTransaction::FetchInputs
 */
unsigned int GetP2SHSigOpCount(const CTransaction &tx, const CCoinsViewCache &mapInputs);

/**
 * Count total signature operations for a transaction.
 * @param[in] tx     Transaction for which we are counting sigops
 * @param[in] inputs Map of previous transactions that have outputs we're spending
 * @param[out] flags Script verification flags
 * @return Total signature operation count for a tx
 */
unsigned int GetTransactionSigOpCount(const CTransaction &tx, const CCoinsViewCache &inputs, int flags);

/**
 * Accurate sigop count for the block resource budget (1.2, D-17, F-86, F-87).
 *
 * GetLegacySigOpCount + GetP2SHSigOpCount leave two gaps: neither examines a
 * spent scriptPubKey unless it's P2SH, and neither counts OP_CHECKDATASIG(VERIFY).
 * This counts, per transaction: every scriptSig and created-output script
 * accurately (as GetLegacySigOpCount does, but with fAccurate=true and
 * OP_CHECKDATASIG(VERIFY) included), plus an accurate count of every SPENT
 * scriptPubKey -- P2SH redeem script or the prevout script directly, whichever
 * applies -- for every prevout type, not just P2SH.
 *
 * A new path. Does NOT alter GetLegacySigOpCount, GetP2SHSigOpCount or
 * GetTransactionSigOpCount, which pre-fork validation still depends on and
 * must not change retroactively for history already on chain.
 *
 * @param[in] tx     Transaction for which we are counting sigops
 * @param[in] inputs Map of previous transactions that have outputs we're spending
 * @return Accurate signature operation count for a tx
 */
unsigned int GetAccurateSigOpCount(const CTransaction &tx, const CCoinsViewCache &inputs);

/**
 * Check if transaction is final and can be included in a block with the
 * specified height and time. Consensus critical.
 */
bool IsFinalTx(const CTransaction &tx, int nBlockHeight, int64_t nBlockTime);

/**
 * Calculates the block height and previous block's median time past at
 * which the transaction will be considered final in the context of BIP 68.
 * Also removes from the vector of input heights any entries which did not
 * correspond to sequence locked inputs as they do not affect the calculation.
 */
std::pair<int, int64_t>
CalculateSequenceLocks(const CTransaction &tx, int flags, std::vector<int> *prevHeights, const CBlockIndex &block);

bool EvaluateSequenceLocks(const CBlockIndex &block, std::pair<int, int64_t> lockPair);

/**
 * Check if transaction is final per BIP 68 sequence numbers and can be included in a block.
 * Consensus critical. Takes as input a list of heights at which tx's inputs (in order) confirmed.
 */
bool SequenceLocks(const CTransaction &tx, int flags, std::vector<int> *prevHeights, const CBlockIndex &block);

#endif // BITCOIN_CONSENSUS_TX_VERIFY_H
