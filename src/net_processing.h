// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NET_PROCESSING_H
#define BITCOIN_NET_PROCESSING_H

#include <consensus/params.h>
#include <coveragetelemetry.h>
#include <net.h>
#include <sync.h>
#include <validationinterface.h>

#include <vector>

class CTxMemPool;

class ChainstateManager;

extern RecursiveMutex cs_main;

/** Default for -maxorphantxsize, maximum size in megabytes the orphan map can grow before entries are removed */
static const unsigned int DEFAULT_MAX_ORPHAN_TRANSACTIONS_SIZE = 10; // this allows around 100 TXs of max size (and many more of normal size)
/** Default number of orphan+recently-replaced txn to keep around for block reconstruction */
static const unsigned int DEFAULT_BLOCK_RECONSTRUCTION_EXTRA_TXN = 100;
/** Default for BIP61 (sending reject messages) */
static constexpr bool DEFAULT_ENABLE_BIP61 = true;

class PeerLogicValidation final : public CValidationInterface, public NetEventsInterface {
private:
    CConnman *const connman;
    BanMan *const m_banman;
    ChainstateManager &m_chainman;
    CTxMemPool &m_mempool;

    bool SendRejectsAndCheckIfBanned(CNode *pnode, bool enable_bip61)

    EXCLUSIVE_LOCKS_REQUIRED(cs_main);
public:
    PeerLogicValidation(CConnman *connmanIn, BanMan *banman, CScheduler &scheduler, ChainstateManager &chainman,
                        CTxMemPool &pool, bool enable_bip61);

    /**
     * Overridden from CValidationInterface.
     */
    void BlockConnected(const std::shared_ptr<const CBlock> &pblock, const CBlockIndex *pindexConnected,
                        const std::vector <CTransactionRef> &vtxConflicted) override;

    /**
     * Overridden from CValidationInterface.
     */
    void UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *pindexFork, bool fInitialDownload) override;

    /**
     * Overridden from CValidationInterface.
     */
    void BlockChecked(const CBlock &block, const CValidationState &state) override;

    /**
     * Overridden from CValidationInterface.
     */
    void NewPoWValidBlock(const CBlockIndex *pindex, const std::shared_ptr<const CBlock> &pblock) override;

    /** Initialize a peer by adding it to mapNodeState and pushing a message requesting its version */
    void InitializeNode(CNode *pnode) override;

    /** Handle removal of a peer by updating various state and removing it from mapNodeState */
    void FinalizeNode(NodeId nodeid, bool &fUpdateConnectionTime) override;

    /**
    * Process protocol messages received from a given node
    *
    * @param[in]   pfrom           The node which we have received messages from.
    * @param[in]   interrupt       Interrupt condition for processing threads
    */
    bool ProcessMessages(CNode *pfrom, std::atomic<bool> &interrupt) override;

    /**
    * Send queued protocol messages to be sent to a give node.
    *
    * @param[in]   pto             The node which we are sending messages to.
    * @return                      True if there is more work to be done
    */
    bool SendMessages(CNode *pto) override

    EXCLUSIVE_LOCKS_REQUIRED(pto
    ->cs_sendProcessing);

    /** Consider evicting an outbound peer based on the amount of time they've been behind our tip */
    void ConsiderEviction(CNode *pto, int64_t time_in_seconds)

    EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Evict extra outbound peers. If we think our tip may be stale, connect to an extra outbound */
    void CheckForStaleTipAndEvictPeers(const Consensus::Params &consensusParams);

    /** If we have extra outbound peers, try to disconnect the one with the oldest block announcement */
    void EvictExtraOutboundPeers(int64_t time_in_seconds)

    EXCLUSIVE_LOCKS_REQUIRED(cs_main);

private:
    int64_t m_stale_tip_check_time; //! Next time to check for stale tip

    /** Enable BIP61 (sending reject messages) */
    const bool m_enable_bip61;
};

struct CNodeStateStats {
    int nMisbehavior = 0;
    int nSyncHeight = -1;
    int nCommonHeight = -1;
    std::vector<int> vHeightInFlight;
    //! 4.2 (F-206, build-plan.md's 4.2 row): per-peer body-range miss-rate
    //! telemetry (coveragetelemetry.h) -- every validated GETBODYRANGE round
    //! trip with this peer, hit or miss, since the connection was
    //! established. Reset on reconnect, matching nMisbehavior's own
    //! per-connection scope (CNodeState is torn down on disconnect); not
    //! persisted across restarts, matching this file's own established
    //! convention for similar transient per-peer body-range state
    //! (nBodyRangePeerInFlight and friends, net_processing.cpp).
    uint64_t nBodyRangeHits = 0;
    uint64_t nBodyRangeMisses = 0;
};

/** Get statistics from node state */
bool GetNodeStateStats(NodeId nodeid, CNodeStateStats &stats);

/** 4.2 (F-206, build-plan.md's 4.2 row); F-212 (this rework's own data-model
 *  types): one coverage-range bucket (coveragetelemetry.h's
 *  CoverageRangeIndex/COVERAGE_RANGE_SIZE/SummarizeCoverageRange) as
 *  reported by GetBodyRangeCoverageStats below -- the height span it covers
 *  plus a ROLLED-UP summary of every height in that span the cross-peer
 *  ledger has an opinion about. `stats` is computed on demand by scanning
 *  the underlying per-height ledger (coveragetelemetry.h's own doc comment
 *  on why this is now a rollup rather than the ledger's own storage
 *  granularity), not maintained incrementally the way the pre-F-212
 *  RangeCoverageStats this struct's `stats` field used to hold was. */
struct BodyRangeCoverageEntry {
    int nRangeStartHeight;
    int nRangeEndHeightInclusive;
    CoverageRangeSummary stats;
};

/** 4.2 (F-206); F-212: snapshot of every coverage bucket this node has at
 *  least one observed height in, from ANY peer, since startup (or the last
 *  restart folded persisted history back in -- see
 *  LoadCoverageTelemetrySnapshot below) -- the cross-peer, standing view of
 *  which spans of chain history this node has evidence for at all. See
 *  coveragetelemetry.h for CoverageRangeSummary's own fields and
 *  docs/transaction-decoupling.md SS14.9 for what the shape of these numbers
 *  across repeated polls is meant to reveal (this function does not
 *  interpret that shape itself -- SS14.5: "a query and a counter, not a
 *  challenge economy"). Buckets with zero observed heights are never
 *  present. Ordering is unspecified. */
void GetBodyRangeCoverageStats(std::vector<BodyRangeCoverageEntry> &vStatsOut);

/** F-212 (gap 1's own shape-detail escape hatch): one height's own raw,
 *  deduped coverage record, as reported by GetBodyRangeCoverageHeights
 *  below. A bucket rollup (BodyRangeCoverageEntry/GetBodyRangeCoverageStats
 *  above) cannot by itself tell a 300-block contiguous erasure apart from
 *  300 scattered corrupt blocks (SS14.9's own example) -- both would roll up
 *  to the same nMissOnly count. This is the per-height detail a caller needs
 *  to actually see that shape; it is deliberately NOT this phase's job to
 *  compute the shape FOR the caller (SS14.5: "a query and a counter"). */
struct BodyRangeCoverageHeightEntry {
    int nHeight;
    HeightCoverageStatus status;
    int64_t nFirstObservedTime;
    int64_t nLastObservedTime;
    int64_t nLastChangeTime;
};

/** F-212: every OBSERVED height (coveragetelemetry.h's CoverageHeightMap is
 *  sparse -- a height with no entry has never been observed) in
 *  `[nStartHeight, nEndHeightInclusive]`, in ascending height order. Bounded
 *  by the caller's own range so a query against a long-synced chain cannot
 *  accidentally dump millions of records at once -- matching this file's
 *  own getbodyrangecoverage RPC convention of never doing unbounded work on
 *  a plain data-read call. */
void GetBodyRangeCoverageHeights(int nStartHeight, int nEndHeightInclusive,
                                  std::vector<BodyRangeCoverageHeightEntry> &vHeightsOut);

/** F-212 (gap 3): copies the live, cs_main-guarded cross-peer coverage
 *  ledger out into `out` -- the read side init.cpp's own shutdown-time and
 *  periodic CFlatDB<CCoverageTelemetryCache>::Dump calls use. A plain
 *  snapshot copy, not a reference -- the live ledger stays guarded by
 *  cs_main and must never be handed out unlocked (this file's own
 *  net_processing.cpp doc comment on mapBodyRangeCoverage). */
void GetCoverageTelemetrySnapshot(CoverageHeightMap &out);

/** F-212 (gap 3): folds `snapshot` (freshly loaded from coveragetelemetry.dat
 *  by init.cpp's own CFlatDB<CCoverageTelemetryCache>::Load call, once, at
 *  startup, before networking begins) into the live, cs_main-guarded
 *  cross-peer coverage ledger, via coveragetelemetry.h's own
 *  MergeCoverageTelemetrySnapshot (the same monotonic-upgrade rule live
 *  observations use). Intended to run exactly once, early in startup, while
 *  the live ledger is still empty -- but safe to call at any time, since the
 *  merge itself makes no assumption about the live ledger's own state. */
void LoadCoverageTelemetrySnapshot(const CoverageHeightMap &snapshot);

bool IsBanned(NodeId nodeid)

EXCLUSIVE_LOCKS_REQUIRED(cs_main);

// Upstream moved this into net_processing.cpp (13417), however since we use Misbehaving in a number of raptoreum specific
// files such as mnauth.cpp and governance.cpp it makes sense to keep it in the header
/** Increase a node's misbehavior score. */
void Misbehaving(NodeId nodeid, int howmuch, const std::string &message = "")

EXCLUSIVE_LOCKS_REQUIRED(cs_main);

void EraseObjectRequest(NodeId nodeId, const CInv &inv)

EXCLUSIVE_LOCKS_REQUIRED(cs_main);

void RequestObject(NodeId nodeId, const CInv &inv, std::chrono::microseconds current_time, bool fForce = false)

EXCLUSIVE_LOCKS_REQUIRED(cs_main);

size_t GetRequestedObjectCount(NodeId nodeId)

EXCLUSIVE_LOCKS_REQUIRED(cs_main);

/** Relay transaction to every node */
void RelayTransaction(const uint256 &, const CConnman &connman);


/** Test-only relay trickle overrides; see net_processing.cpp. 0 = shipped behaviour. */
extern unsigned int g_perf_inv_max;
extern unsigned int g_perf_inv_interval;
extern bool g_perf_inv_nosort;

/** F-207 (independent adversarial review of F-206): test-only. Seeds a single
 *  mapBodyRangeInFlight entry directly, bypassing SendMessages' own real
 *  fetch-selection machinery, so a test can drive the real BODYRANGE
 *  response handler end-to-end via PeerLogicValidation::ProcessMessages. See
 *  net_processing.cpp's own doc comment at the definition for why this
 *  exists instead of a predicate-level test. */
void SeedBodyRangeInFlightForTest(const uint256 &hashBlock, NodeId peer, uint32_t nStartIndex, uint32_t nCount);

#endif // BITCOIN_NET_PROCESSING_H
