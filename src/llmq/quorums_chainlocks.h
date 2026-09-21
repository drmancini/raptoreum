// Copyright (c) 2019-2021 The Dash Core developers
// Copyright (c) 2020-2023 The Raptoreum developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_QUORUMS_CHAINLOCKS_H
#define BITCOIN_LLMQ_QUORUMS_CHAINLOCKS_H

#include <bls/bls.h>
#include <hash.h>
#include <llmq/quorums_signing.h>
#include <net.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <saltedhasher.h>
#include <streams.h>
#include <sync.h>

#include <atomic>
#include <tuple>
#include <unordered_set>

class CConnman;

class CBlockIndex;

class CScheduler;

class CTxMemPool;

namespace llmq {

    extern const std::string CLSIG_REQUESTID_PREFIX;

    // 3.1 (DIP8 signing-attempts process, build-plan.md; F-136): a separate
    // request-id namespace for in-progress signing ATTEMPTS, distinct from
    // CLSIG_REQUESTID_PREFIX, which stays reserved for the one-shot
    // FINALIZATION round that produces the actual broadcastable
    // CChainLockSig. CChainLockSig's own wire format (nHeight/blockHash/sig)
    // has no room for an attempt number, and ProcessNewChainLock's
    // verification (quorums_chainlocks.cpp) recomputes a request id from
    // height alone -- so an attempt's own recovered signature could never be
    // verified by a peer if broadcast directly. DIP8's real two-round
    // structure (attempts converge internally, then one fixed-id round
    // produces the CLSIG) exists for exactly this reason, and is why this
    // fix needs two request-id prefixes, not one.
    extern const std::string CLSIG_ATTEMPT_REQUESTID_PREFIX;

    // How often (seconds) the attempt number advances. Deliberately NOT tied
    // to TrySignChainTip's own 5-second scheduler cadence: every node's
    // process starts retrying at a different wall-clock moment, so a purely
    // local "ticks since I started retrying" counter would desync nodes onto
    // different attempt numbers for the same real moment, and their
    // signature shares could never combine into a threshold signature.
    // Anchoring to GetAdjustedTime() (already used for this same kind of
    // cross-node timing elsewhere in quorums_chainlocks.cpp) means every
    // node computes the same attemptNum at roughly the same real time,
    // tolerant of a few seconds of skew -- 30s is comfortably above that and
    // above the 5s poll interval, so most scheduler ticks land inside the
    // same attempt slot rather than spinning up a fresh signing session
    // every 5 seconds forever. F-137 (Fable review of F-136) corrected an
    // overclaim here: GetAdjustedTime() is NOT a synchronized network clock
    // in the NTP sense -- it's each node's own local clock offset by the
    // median of its peers' claimed time (timedata.cpp, capped ±70 minutes),
    // so ordinary per-node skew of a few seconds is normal and shifts each
    // node's own slot boundary independently. That's exactly the skew this
    // margin is sized to tolerate, not something this design assumes away;
    // CLSIG_ATTEMPT_LOOKBACK (below) is the other half of that tolerance,
    // for when aggregation+gossip latency compounds with skew across a
    // boundary.
    static const int64_t CLSIG_ATTEMPT_INTERVAL = 30;

    // Pure and deterministic -- every quorum member must compute the
    // identical value for their shares to combine, so this has zero hidden
    // state and zero randomness (ShouldNegotiateCommitments's own pattern,
    // protocol.h: a free function exposed directly in the header so it is
    // unit-testable without any of the class's own live state).
    static inline int32_t GetChainLockAttemptNumber(int64_t nAdjustedTime) {
        return (int32_t) (nAdjustedTime / CLSIG_ATTEMPT_INTERVAL);
    }

    static inline uint256 GetChainLockAttemptRequestId(int32_t nHeight, int32_t nAttemptNum) {
        return ::SerializeHash(std::make_tuple(CLSIG_ATTEMPT_REQUESTID_PREFIX, nHeight, nAttemptNum));
    }

    // F-137 (Fable review of F-136): how many PAST attempt slots
    // DecideRecoveredSigOutcome still recognises as "what we're currently
    // waiting on". BLS threshold aggregation and gossip take real,
    // non-negligible time -- a node's own 5-second scheduler tick can roll
    // its local state into attempt N+1 in the seconds between an attempt-N
    // signature genuinely reaching threshold and that recovered signature
    // arriving back at this same node. Without a lookback, that node
    // discards its own attempt's success as "stale", and if enough nodes
    // race ahead the same way, too few are left still watching attempt N's
    // id to ever start finalization -- while the NEW attempt N+1 they moved
    // to never collects votes from the nodes that stayed behind either
    // (they're ignoring it, waiting on attempt N). Two slots (60s) is
    // comfortably above normal aggregation+gossip latency while staying
    // far short of the point where the msgHash itself would plausibly have
    // changed underneath us (guarded separately, below).
    static const int32_t CLSIG_ATTEMPT_LOOKBACK = 2;

    // What TrySignChainTip should do next, given the tip it sees and the
    // handler's own outstanding-signing state -- pure, no locks, no globals,
    // so the actual retry/one-shot-finalization decision is fully
    // unit-testable without a live smartnode, BLS quorum or signed recovered
    // signature (this file's own existing test precedent,
    // acceptancebit_tests.cpp's a_commitment_only_block_still_heals_after_
    // its_sibling_is_marked_conflicting comment, already declined that as
    // disproportionate scaffolding for testing CChainLocksHandler's side
    // effects).
    enum class ChainLockSignAction {
        kNone,
        kStartAttempt,
    };

    static inline ChainLockSignAction DecideChainLockSignAction(
            int32_t nTipHeight, int32_t nAttemptNum, int32_t nBestChainLockHeight,
            int32_t nLastSignedHeight, int32_t nLastSignedAttempt, bool fLastSignedIsFinalization) {
        if (nBestChainLockHeight >= nTipHeight) {
            // already have a CLSIG at least this good -- nothing to do
            return ChainLockSignAction::kNone;
        }
        if (nTipHeight == nLastSignedHeight) {
            if (fLastSignedIsFinalization) {
                // already finalizing this height -- never restart an attempt
                // out from under an outstanding finalization round, even if
                // the attempt slot has since rolled over
                return ChainLockSignAction::kNone;
            }
            if (nAttemptNum == nLastSignedAttempt) {
                // already have an outstanding attempt for this exact slot
                return ChainLockSignAction::kNone;
            }
        }
        return ChainLockSignAction::kStartAttempt;
    }

    // What HandleNewRecoveredSig should do with a just-recovered signature,
    // given the handler's own outstanding-signing state -- pure, for the
    // same testability reason as above.
    enum class RecoveredSigOutcome {
        kIgnore,
        kStartFinalization,
        kBuildChainLock,
    };

    static inline RecoveredSigOutcome DecideRecoveredSigOutcome(
            const uint256 &recoveredId, const uint256 &recoveredMsgHash,
            const uint256 &lastSignedRequestId, const uint256 &lastSignedMsgHash,
            int32_t nBestChainLockHeight, int32_t nLastSignedHeight, int32_t nLastSignedAttempt,
            bool fLastSignedIsFinalization) {
        if (recoveredMsgHash != lastSignedMsgHash) {
            // never what we're waiting on, regardless of id -- if the tip
            // (and so lastSignedMsgHash) has moved on, nothing about the id
            // lookback below can make an old block's signature relevant
            return RecoveredSigOutcome::kIgnore;
        }
        bool idMatches = (recoveredId == lastSignedRequestId);
        if (!idMatches && !fLastSignedIsFinalization) {
            // F-137: accept a recent PAST attempt slot's own id too -- see
            // CLSIG_ATTEMPT_LOOKBACK's own doc for why. Finalization's own
            // id is fixed (height-only, no attempt number) and unambiguous,
            // so this widening only makes sense while still attempting.
            for (int32_t back = 1; back <= CLSIG_ATTEMPT_LOOKBACK && !idMatches; back++) {
                idMatches = (recoveredId == GetChainLockAttemptRequestId(nLastSignedHeight, nLastSignedAttempt - back));
            }
        }
        if (!idMatches) {
            // not what we're currently waiting on -- stale or foreign
            return RecoveredSigOutcome::kIgnore;
        }
        if (nBestChainLockHeight >= nLastSignedHeight) {
            // already got the same or a better CLSIG some other way
            return RecoveredSigOutcome::kIgnore;
        }
        return fLastSignedIsFinalization ? RecoveredSigOutcome::kBuildChainLock
                                          : RecoveredSigOutcome::kStartFinalization;
    }

    class CChainLockSig {
    private:
        int32_t nHeight{-1};
        uint256 blockHash;
        CBLSSignature sig;

    public:
        CChainLockSig(int32_t nHeight, const uint256 &blockHash, const CBLSSignature &sig)
                : nHeight(nHeight), blockHash(blockHash), sig(sig) {}

        CChainLockSig() = default;

        [[nodiscard]] int32_t getHeight() const;

        [[nodiscard]] const uint256 &getBlockHash() const;

        [[nodiscard]] const CBLSSignature &getSig() const;

        [[nodiscard]] bool IsNull() const;

        [[nodiscard]] std::string ToString() const;

        SERIALIZE_METHODS(CChainLockSig, obj
        )
        {
            READWRITE(obj.nHeight, obj.blockHash, obj.sig);
        }
    };

    class CChainLocksHandler : public CRecoveredSigsListener {
        static const int64_t CLEANUP_INTERVAL = 1000 * 30;
        static const int64_t CLEANUP_SEEN_TIMEOUT = 24 * 60 * 60 * 1000;

        // how long to wait for islocks until we consider a block with non-islocked TXs to be safe to sign
        static const int64_t WAIT_FOR_ISLOCK_TIMEOUT = 10 * 60;

    private:
        CConnman &connman;
        CTxMemPool &mempool;
        std::unique_ptr <CScheduler> scheduler;
        std::unique_ptr <std::thread> scheduler_thread;
        mutable RecursiveMutex cs;
        bool tryLockChainTipScheduled
        GUARDED_BY(cs) {false};
        bool isEnabled
        GUARDED_BY(cs) {false};
        bool isEnforced
        GUARDED_BY(cs) {false};

        uint256 bestChainLockHash
        GUARDED_BY(cs);
        CChainLockSig bestChainLock
        GUARDED_BY(cs);

        CChainLockSig bestChainLockWithKnownBlock
        GUARDED_BY(cs);
        const CBlockIndex *bestChainLockBlockIndex
        GUARDED_BY(cs) {nullptr};
        const CBlockIndex *lastNotifyChainLockBlockIndex
        GUARDED_BY(cs) {nullptr};

        int32_t lastSignedHeight
        GUARDED_BY(cs) {-1};
        // 3.1: which attempt slot lastSignedHeight's outstanding ATTEMPT round
        // (if any) used -- DecideChainLockSignAction's own dedupe key.
        int32_t lastSignedAttempt
        GUARDED_BY(cs) {-1};
        // 3.1: whether lastSignedRequestId/lastSignedMsgHash currently name an
        // in-progress ATTEMPT round or the one-shot FINALIZATION round.
        bool lastSignedIsFinalization
        GUARDED_BY(cs) {false};
        uint256 lastSignedRequestId
        GUARDED_BY(cs);
        uint256 lastSignedMsgHash
        GUARDED_BY(cs);

        // We keep track of txids from recently received blocks so that we can check if all TXs got islocked
        using BlockTxs = std::unordered_map <uint256, std::shared_ptr<
                std::unordered_set < uint256, StaticSaltedHasher>>>;
        BlockTxs blockTxs
        GUARDED_BY(cs);
        std::unordered_map <uint256, int64_t> txFirstSeenTime
        GUARDED_BY(cs);

        std::map <uint256, int64_t> seenChainLocks
        GUARDED_BY(cs);

        int64_t lastCleanupTime
        GUARDED_BY(cs) {0};

    public:
        explicit CChainLocksHandler(CTxMemPool &_mempool, CConnman &_connman);

        ~CChainLocksHandler();

        void Start();

        void Stop();

        bool AlreadyHave(const CInv &inv) const;

        bool GetChainLockByHash(const uint256 &hash, CChainLockSig &ret) const;

        CChainLockSig GetBestChainLock() const;

        void ProcessMessage(CNode *pfrom, const std::string &strCommand, CDataStream &vRecv);

        void ProcessNewChainLock(NodeId from, const CChainLockSig &clsig, const uint256 &hash);

        void AcceptedBlockHeader(const CBlockIndex *pindexNew);

        void UpdatedBlockTip();

        void TransactionAddedToMempool(const CTransactionRef &tx, int64_t nAcceptTime);

        void BlockConnected(const std::shared_ptr<const CBlock> &pblock, const CBlockIndex *pindex,
                            const std::vector <CTransactionRef> &vtxConflicted);

        void BlockDisconnected(const std::shared_ptr<const CBlock> &pblock, const CBlockIndex *pindexDisconnected);

        void CheckActiveState();

        void TrySignChainTip();

        void EnforceBestChainLock();

        void HandleNewRecoveredSig(const CRecoveredSig &recoveredSig) override;

        bool HasChainLock(int nHeight, const uint256 &blockHash) const;

        bool HasConflictingChainLock(int nHeight, const uint256 &blockHash) const;

        bool IsTxSafeForMining(const uint256 &txid) const;

    private:
        // these require locks to be held already
        bool InternalHasChainLock(int nHeight, const uint256 &blockHash) const

        EXCLUSIVE_LOCKS_REQUIRED(cs);

        bool InternalHasConflictingChainLock(int nHeight, const uint256 &blockHash) const

        EXCLUSIVE_LOCKS_REQUIRED(cs);

        BlockTxs::mapped_type GetBlockTxs(const uint256 &blockHash);

        void Cleanup();
    };

    extern CChainLocksHandler *chainLocksHandler;

    bool AreChainLocksEnabled();

/*
template<typename Callable> void TraceCL(const std::string name, Callable func)
{
  std::string namestr = "rtm-" + name; util::ThreadRename(namestr.c_str());
  try { LogPrintf("%s thread start\n", name); func(); LogPrintf("%s thread stop\n", name); }
  catch (...) { PrintExceptionContinue(std::current_exception(), name.c_str()); throw; }
}
*/

} // namespace llmq

#endif // BITCOIN_LLMQ_QUORUMS_CHAINLOCKS_H
