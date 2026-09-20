// Copyright (c) 2017-2023 The Bitcoin Core developers
// Copyright (c) 2020-2023 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_TX_VERIFY_H
#define BITCOIN_CONSENSUS_TX_VERIFY_H

#include <amount.h>

#include <stdint.h>
#include <vector>

class CBlock;

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
 * The view-independent half of GetAccurateSigOpCount: every scriptSig and
 * created-output script, counted accurately (fAccurate=true, OP_CHECKDATASIG
 * (VERIFY) included). No UTXO view needed, so this is what the two view-less
 * validation sites (CheckBlock, ContextualCheckBlock) must use under the
 * commitment budget -- NOT GetLegacySigOpCount.
 *
 * That matters because legacy is not a uniform undercount relative to the
 * accurate count (1.2 review, F-95, 2026-09-19): GetSigOpCount(false) charges
 * MAX_PUBKEYS_PER_MULTISIG (20) for every CHECKMULTISIG regardless of the real
 * key count, so a transaction that only CREATES a small bare multisig output
 * (say 1-of-3, standard, 3 real sigops) is charged 20 by legacy -- a massive
 * OVERcount in this direction, the opposite of F-13's spend-side undercount.
 * A miner filling to the accurate 250,000 budget could build a block whose
 * legacy count exceeds 250,000 at a much smaller true sigop count, and
 * CheckBlock would reject it -- stalling block production even though the
 * block was within budget by the measure the miner used to build it.
 *
 * @param[in] tx Transaction for which we are counting sigops
 * @return Accurate signature operation count for tx's own scriptSigs and
 *         created outputs, excluding the spend side
 */
unsigned int GetAccurateOwnSigOpCount(const CTransaction &tx);

/**
 * Accurate sigop count for the block resource budget (1.2, D-17, F-86, F-87).
 *
 * GetLegacySigOpCount + GetP2SHSigOpCount leave two gaps: neither examines a
 * spent scriptPubKey unless it's P2SH, and neither counts OP_CHECKDATASIG(VERIFY).
 * This counts, per transaction: every scriptSig and created-output script
 * accurately (GetAccurateOwnSigOpCount), plus an accurate count of every SPENT
 * scriptPubKey -- P2SH redeem script or the prevout script directly, whichever
 * applies -- for every prevout type, not just P2SH.
 *
 * A new path. Does NOT alter GetLegacySigOpCount, GetP2SHSigOpCount or
 * GetTransactionSigOpCount, which pre-fork validation still depends on and
 * must not change retroactively for history already on chain.
 *
 * Precondition (B3, F-120, Fable review, 2026-09-19): every txin.prevout must already
 * resolve in `inputs`. AccessCoin's missing-coin sentinel is a default (empty)
 * CTxOut, whose script contributes zero sigops rather than raising an error --
 * an unresolvable input silently UNDERcounts instead of failing loudly. Both
 * call sites (AcceptToMemoryPool, ConnectBlock) run this only after their own
 * Consensus::CheckTxInputs already required every input to resolve, so the
 * precondition holds there; a new call site must establish it too.
 *
 * @param[in] tx     Transaction for which we are counting sigops
 * @param[in] inputs Map of previous transactions that have outputs we're spending
 * @return Accurate signature operation count for a tx
 */
unsigned int GetAccurateSigOpCount(const CTransaction &tx, const CCoinsViewCache &inputs);

/**
 * 1.2 (body-byte / input-count cap, Mike, 2026-09-19): aggregate real
 * (non-coinbase) input count across a block. The coinbase's own single dummy
 * input touches no UTXO and costs none of the per-input validation work this
 * bounds -- F-91/F-93/F-94/F-97's mechanism is about real spends, so it is
 * excluded. No UTXO view needed: input count is directly available from
 * tx.vin.size(), so this is fully accurate at the view-less validation sites
 * (CheckBlock, ContextualCheckBlock), unlike the sigop budget's spend-side
 * term.
 *
 * @param[in] block Block for which we are counting aggregate inputs
 * @return Aggregate non-coinbase input count across the block
 */
unsigned int GetBlockInputCount(const CBlock &block);

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
