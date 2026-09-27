// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_RPC_MINING_H
#define BITCOIN_RPC_MINING_H

#include <amount.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>

#include <univalue.h>

#include <map>
#include <set>
#include <string>

class CTxMemPool;

static const int DEFAULT_GENERATE_THREADS = 1;

/** Generate blocks (mine) */
UniValue generateBlocks(const CTxMemPool &mempool, std::shared_ptr <CReserveScript> coinbaseScript, int nGenerate,
                        uint64_t nMaxTries, bool keepScript);

// 4.4.1 (F-191): commitment-mode getblocktemplate. `getblocktemplate` negotiates
// commitment-mode the same way it already negotiates BIP9 softfork rules --
// via its own `rules` request array, parsed into `setClientRules`
// (rpc/mining.cpp) -- this is that negotiated rule's name, not a versionbit
// deployment (Updates()/EUpdate never sees it).
static const std::string GBT_RULE_COMMITMENTS = "commitments";

/** Whether a getblocktemplate request's own client-declared rules negotiated
 *  commitment-mode (bare transaction identifiers instead of full transaction
 *  data in the "transactions" array). Trivial by design -- kept as its own
 *  function so the negotiation decision itself is directly unit-testable
 *  without needing a live RPC dispatch. */
bool WantsCommitmentModeTemplate(const std::set <std::string> &setClientRules);

/** Build one entry of getblocktemplate's own "transactions" array for a
 *  single non-coinbase transaction already selected into the template.
 *
 *  In commitment-mode (fCommitmentMode), the "data" field -- full transaction
 *  bytes, hex-encoded -- is omitted. Nothing else changes: "hash" is already
 *  the bare 32-byte identifier a miner needs to compute the merkle root --
 *  mining hardware never sees transaction bytes at all, only the 80-byte
 *  header (§9.2), and pool software computes the merkle root from the
 *  ordered hash list alone, exactly as it already does today. "data" was
 *  only ever needed later, to reconstruct a submittable full block; under
 *  commitment-mode that reconstruction uses bodies the submitting node
 *  already holds instead (submitblock's own commitment-mode path, 4.4.2 --
 *  not built by this function). "depends"/"fee"/"specialTxfee"/"sigops"
 *  stay in both modes: a pool still needs them to audit fees and to stay
 *  under the sigop budget regardless of serialization form. */
UniValue BuildGBTTransactionEntry(const CTransaction &tx, const std::map <uint256, int64_t> &setTxIndex,
                                  CAmount nFee, CAmount nSpecialTxFee, int64_t nSigOps, bool fCommitmentMode);

/** 4.4.2 (F-192): submitblock's commitment-mode orchestration -- resolve a
 *  CCommitmentBlock's committed transactions from THIS NODE'S OWN mempool
 *  (mempool.get, the same accessor GetTransaction's own mempool branch
 *  already uses, validation.cpp) and materialise the full CBlock via the
 *  pre-existing MaterialiseBlock (primitives/block.cpp, already wired into
 *  the real accept path at validation.cpp and independently tested since
 *  2.2.4/F-155) -- reused unchanged, not reimplemented.
 *
 *  Correct specifically because a commitment this node's own miner
 *  submitted names only transactions that were in its own mempool moments
 *  earlier when CreateNewBlock selected them; this is never a network-fetch
 *  path and must never be used to materialise a commitment block that
 *  arrived from a peer.
 *
 *  F-193 (fixed): an LLMQ quorum-commitment special transaction (miner.cpp's
 *  GetMineableCommitmentTx) is the one exception to the mempool-coverage
 *  assumption above -- CreateNewBlock inserts one directly from
 *  quorumBlockProcessor, bypassing mempool selection entirely, so it is
 *  never found by a mempool lookup. When the mempool lookup misses,
 *  FindMineableCommitmentTxByHash reconstructs each currently-mineable
 *  commitment for the next block exactly as CreateNewBlock does, and
 *  returns the one whose hash matches -- the same source, not a new one.
 *
 *  No new validation logic: pure lookup (mempool, then the quorum-commitment
 *  fallback) plus the pre-existing MaterialiseBlock call. Throws
 *  JSONRPCError (matching F-178's own established convention,
 *  rpc/smartnode.cpp) on a missing or a mismatched body rather than
 *  propagating a crash or an unclear generic error -- never returns a
 *  null/false sentinel on failure. Extracted as its own, header-declared
 *  function so this orchestration is directly unit-testable against a
 *  standalone CTxMemPool, without needing a live RPC dispatch context
 *  (matching F-191's own established convention for this file's
 *  dispatch-adjacent glue). */
CBlock MaterialiseSubmittedCommitmentBlock(const CCommitmentBlock &commitments, const CTxMemPool &mempool);

/** F-193: reconstruct each quorum-commitment transaction that CreateNewBlock could
 *  legally mine into the NEXT block (one per enabled LLMQ param set, at the current
 *  tip height + 1 -- exactly miner.cpp's own loop), and return the one whose hash
 *  matches txid. Returns nullptr if none matches, if DIP0003 isn't enabled, or if
 *  there is no chain tip. Requires cs_main (GetMineableCommitmentTx's own
 *  precondition); the caller is responsible for holding it. */
CTransactionRef FindMineableCommitmentTxByHash(const uint256 &txid);

#endif
