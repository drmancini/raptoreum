// Copyright (c) 2017-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <index/base.h>
#include <shutdown.h>
#include <tinyformat.h>
#include <ui_interface.h>
#include <validation.h>
#include <warnings.h>

constexpr char DB_BEST_BLOCK = 'B';

constexpr int64_t
SYNC_LOG_INTERVAL = 30; // seconds
constexpr int64_t
SYNC_LOCATOR_WRITE_INTERVAL = 30; // seconds
// F-170 (4.1.1 follow-up): how often ThreadSync rechecks HaveBodies on a
// block it's blocked on. HaveBodies is a single in-memory status-bit check
// (no disk I/O), so this can be short without meaningfully spinning; the
// SYNC_LOG_INTERVAL-gated log line below still only prints every 30s.
constexpr int64_t
BODIES_RETRY_INTERVAL_MS = 1000; // milliseconds

template<typename... Args>
static void FatalError(const char *fmt, const Args &... args) {
    std::string strMessage = tfm::format(fmt, args...);
    SetMiscWarning(strMessage);
    LogPrintf("*** %s\n", strMessage);
    uiInterface.ThreadSafeMessageBox(
            "Error: A fatal internal error occurred, see debug.log for details",
            "", CClientUIInterface::MSG_ERROR);
    StartShutdown();
}

BaseIndex::DB::DB(const fs::path &path, size_t n_cache_size, bool f_memory, bool f_wipe, bool f_obfuscate) :
        CDBWrapper(path, n_cache_size, f_memory, f_wipe, f_obfuscate) {}

bool BaseIndex::DB::ReadBestBlock(CBlockLocator &locator) const {
    bool success = Read(DB_BEST_BLOCK, locator);
    if (!success) {
        locator.SetNull();
    }
    return success;
}

bool BaseIndex::DB::WriteBestBlock(const CBlockLocator &locator) {
    return Write(DB_BEST_BLOCK, locator);
}

BaseIndex::~BaseIndex() {
    Interrupt();
    Stop();
}

bool BaseIndex::Init() {
    CBlockLocator locator;
    if (!GetDB().ReadBestBlock(locator)) {
        locator.SetNull();
    }

    LOCK(cs_main);
    if (locator.IsNull()) {
        m_best_block_index = nullptr;
    } else {
        m_best_block_index = FindForkInGlobalIndex(::ChainActive(), locator);
    }
    m_synced = m_best_block_index.load() == ::ChainActive().Tip();
    return true;
}

static const CBlockIndex *NextSyncBlock(const CBlockIndex *pindex_prev) {
    AssertLockHeld(cs_main);

    if (!pindex_prev) {
        return ::ChainActive().Genesis();
    }

    const CBlockIndex *pindex = ::ChainActive().Next(pindex_prev);
    if (pindex) {
        return pindex;
    }

    return ::ChainActive().Next(::ChainActive().FindFork(pindex_prev));
}

void BaseIndex::ThreadSync() {
    const CBlockIndex *pindex = m_best_block_index.load();
    // F-177 (independent review of F-170's own retry loop, HIGH): `pindex`
    // is reassigned to the CURRENT SYNC TARGET a few lines below (`pindex =
    // pindex_next`), before that target has actually been written via
    // WriteBlock() later in the same iteration. F-170's retry-loop interrupt
    // exit wrote `pindex` at exactly that point -- the stuck target, not the
    // last block genuinely indexed -- so a clean shutdown while paused in
    // the retry loop persisted a locator that skipped the withheld block
    // entirely; a restart would then treat it as done (for BlockFilterIndex
    // specifically, this crashes on startup: its WriteBlock does a height-1
    // lookup that fails on a block that was never actually written).
    // `last_written_index` tracks what its name says -- only ever updated
    // right after WriteBlock() succeeds -- and is what every interrupt exit
    // in this function writes, instead of `pindex` directly.
    const CBlockIndex *last_written_index = pindex;
    if (!m_synced) {
        auto &consensus_params = Params().GetConsensus();

        int64_t last_log_time = 0;
        int64_t last_locator_write_time = 0;
        while (true) {
            if (m_interrupt) {
                WriteBestBlock(last_written_index);
                return;
            }

            {
                LOCK(cs_main);
                const CBlockIndex *pindex_next = NextSyncBlock(pindex);
                if (!pindex_next) {
                    WriteBestBlock(last_written_index);
                    m_best_block_index = pindex;
                    m_synced = true;
                    break;
                }
                pindex = pindex_next;
            }

            int64_t current_time = GetTime();
            if (last_log_time + SYNC_LOG_INTERVAL < current_time) {
                LogPrintf("Syncing %s with block chain from height %d\n",
                          GetName(), pindex->nHeight);
                last_log_time = current_time;
            }

            if (last_locator_write_time + SYNC_LOCATOR_WRITE_INTERVAL < current_time) {
                WriteBestBlock(pindex);
                last_locator_write_time = current_time;
            }

            // F-166 (4.1.1, F-160's own HIGH-severity finding among the
            // remaining named gaps): a windowed node's catch-up sync loop
            // can't proceed past a body-missing block -- stop syncing here
            // rather than escalating to FatalError, which aborts the WHOLE
            // NODE, not just this index. That distinction matters here
            // specifically: unlike a genuine disk-read failure below
            // (corruption, a real bug -- still fatal, kept as-is), "bodies
            // not held yet" is an ordinary, expected state for a windowed
            // node and must not take the node down. This is the shared fix
            // point for BOTH TxIndex and BlockFilterIndex (both derive from
            // BaseIndex and share this one call site) -- BaseIndex's own
            // live BlockConnected path (below) needs no equivalent guard,
            // confirmed by reading it: it's always handed an already-full
            // in-memory CBlock by its caller, never reads disk itself. Not
            // reachable today (no accept path produces a body-missing block
            // on the active chain this loop would ever reach, no
            // body-retention window exists yet to remove bodies from one
            // after the fact) -- future-proofing, matching F-160's own
            // classification.
            //
            // F-170 (independent review of F-166): F-166's own `return;`
            // here, and its log line's claim of "pausing... will resume
            // once available", did not match each other -- ThreadSync runs
            // on m_thread_sync, spawned exactly once (Start()), with no
            // restart mechanism anywhere; `return`ing here ends that thread
            // permanently, m_synced never becomes true, and every future
            // block is silently ignored by BlockConnected (which checks
            // m_synced) forever. Fixed to actually retry: block on THIS
            // pindex (not re-derived via NextSyncBlock, which would
            // advance past it -- pindex here IS the stuck target) until
            // either HaveBodies(pindex) becomes true or the thread is
            // interrupted (shutdown), matching CThreadInterrupt's own
            // sleep_for/return-on-interrupt convention used throughout this
            // codebase (net.cpp, llmq/*). Interrupt exit now also writes
            // the locator first, matching the outer loop's own top-of-loop
            // interrupt-exit convention two screens up, which F-166's
            // version omitted.
            // F-180 (independent review of F-170's own retry loop, LOW):
            // HaveBodies reads pindex->nStatus, a plain uint32_t (chain.h)
            // written under cs_main everywhere else (validation.cpp) --
            // reading it here with no lock at all is a genuine data race
            // under the C++ memory model, even though it's a single aligned
            // word unlikely to tear in practice. Wrapped in a brief
            // LOCK(cs_main) per check (once per BODIES_RETRY_INTERVAL_MS,
            // 1s) rather than switched to an atomic read: this thread holds
            // no other lock at this point in the loop (the earlier
            // LOCK(cs_main) scope above has already released), so there is
            // no lock-ordering cycle possible here, and cs_main is a
            // RecursiveMutex (validation.h) so even a caller that already
            // holds it elsewhere in the call stack would not deadlock --
            // the added contention is one global-lock acquisition per
            // second while paused, negligible next to validation's own use
            // of the same lock.
            while (true) {
                bool have_bodies;
                {
                    LOCK(cs_main);
                    have_bodies = HaveBodies(pindex);
                }
                if (have_bodies) {
                    break;
                }
                if (last_log_time + SYNC_LOG_INTERVAL < GetTime()) {
                    LogPrintf("%s: bodies not held for block %s (height %d), waiting for them to become available\n",
                              GetName(), pindex->GetBlockHash().ToString(), pindex->nHeight);
                    last_log_time = GetTime();
                }
                if (!m_interrupt.sleep_for(std::chrono::milliseconds(BODIES_RETRY_INTERVAL_MS))) {
                    // F-177: write the last block ACTUALLY indexed, not this
                    // loop's own stuck target -- see the comment at the top
                    // of this function.
                    WriteBestBlock(last_written_index);
                    return;
                }
            }
            CBlock block;
            if (!ReadBlockFromDisk(block, pindex, consensus_params)) {
                FatalError("%s: Failed to read block %s from disk",
                           __func__, pindex->GetBlockHash().ToString());
                return;
            }
            if (!WriteBlock(block, pindex)) {
                FatalError("%s: Failed to write block %s to index database",
                           __func__, pindex->GetBlockHash().ToString());
                return;
            }
            last_written_index = pindex;
        }
    }

    if (pindex) {
        LogPrintf("%s is enabled at height %d\n", GetName(), pindex->nHeight);
    } else {
        LogPrintf("%s is enabled\n", GetName());
    }
}

bool BaseIndex::WriteBestBlock(const CBlockIndex *block_index) {
    LOCK(cs_main);
    if (!GetDB().WriteBestBlock(::ChainActive().GetLocator(block_index))) {
        return error("%s: Failed to write locator to disk", __func__);
    }
    return true;
}

void BaseIndex::BlockConnected(const std::shared_ptr<const CBlock> &block, const CBlockIndex *pindex,
                               const std::vector <CTransactionRef> &txn_conflicted) {
    if (!m_synced) {
        return;
    }

    const CBlockIndex *best_block_index = m_best_block_index.load();
    if (!best_block_index) {
        if (pindex->nHeight != 0) {
            FatalError("%s: First block connected is not the genesis block (height=%d)",
                       __func__, pindex->nHeight);
            return;
        }
    } else {
        // Ensure block connects to an ancestor of the current best block. This should be the case
        // most of the time, but may not be immediately after the sync thread catches up and sets
        // m_synced. Consider the case where there is a reorg and the blocks on the stale branch are
        // in the ValidationInterface queue backlog even after the sync thread has caught up to the
        // new chain tip. In this unlikely event, log a warning and let the queue clear.
        if (best_block_index->GetAncestor(pindex->nHeight - 1) != pindex->pprev) {
            LogPrintf("%s: WARNING: Block %s does not connect to an ancestor of " /* Continued */
                      "known best chain (tip=%s); not updating index\n",
                      __func__, pindex->GetBlockHash().ToString(),
                      best_block_index->GetBlockHash().ToString());
            return;
        }
    }

    if (WriteBlock(*block, pindex)) {
        m_best_block_index = pindex;
    } else {
        FatalError("%s: Failed to write block %s to index",
                   __func__, pindex->GetBlockHash().ToString());
        return;
    }
}

void BaseIndex::ChainStateFlushed(const CBlockLocator &locator) {
    if (!m_synced) {
        return;
    }

    const uint256 &locator_tip_hash = locator.vHave.front();
    const CBlockIndex *locator_tip_index;
    {
        LOCK(cs_main);
        locator_tip_index = LookupBlockIndex(locator_tip_hash);
    }

    if (!locator_tip_index) {
        FatalError("%s: First block (hash=%s) in locator was not found",
                   __func__, locator_tip_hash.ToString());
        return;
    }

    // This checks that ChainStateFlushed callbacks are received after BlockConnected. The check may fail
    // immediately after the sync thread catches up and sets m_synced. Consider the case where
    // there is a reorg and the blocks on the stale branch are in the ValidationInterface queue
    // backlog even after the sync thread has caught up to the new chain tip. In this unlikely
    // event, log a warning and let the queue clear.
    const CBlockIndex *best_block_index = m_best_block_index.load();
    if (best_block_index->GetAncestor(locator_tip_index->nHeight) != locator_tip_index) {
        LogPrintf("%s: WARNING: Locator contains block (hash=%s) not on known best " /* Continued */
                  "chain (tip=%s); not writing index locator\n",
                  __func__, locator_tip_hash.ToString(),
                  best_block_index->GetBlockHash().ToString());
        return;
    }

    if (!GetDB().WriteBestBlock(locator)) {
        error("%s: Failed to write locator to disk", __func__);
    }
}

bool BaseIndex::BlockUntilSyncedToCurrentChain() {
    AssertLockNotHeld(cs_main);

    if (!m_synced) {
        return false;
    }

    {
        // Skip the queue-draining stuff if we know we're caught up with
        // ::ChainActive().Tip().
        LOCK(cs_main);
        const CBlockIndex *chain_tip = ::ChainActive().Tip();
        const CBlockIndex *best_block_index = m_best_block_index.load();
        if (best_block_index->GetAncestor(chain_tip->nHeight) == chain_tip) {
            return true;
        }
    }

    LogPrintf("%s: %s is catching up on block notifications\n", __func__, GetName());
    SyncWithValidationInterfaceQueue();
    return true;
}

void BaseIndex::Interrupt() {
    m_interrupt();
}

void BaseIndex::Start() {
    // Need to register this ValidationInterface before running Init(), so that
    // callbacks are not missed if Init sets m_synced to true.
    RegisterValidationInterface(this);
    if (!Init()) {
        FatalError("%s: %s failed to initialize", __func__, GetName());
        return;
    }

    m_thread_sync = std::thread(&TraceThread < std::function < void() >> , GetName(),
                                std::bind(&BaseIndex::ThreadSync, this));
}

void BaseIndex::Stop() {
    UnregisterValidationInterface(this);

    // F-170: ThreadSync can now block indefinitely inside its bodies-retry
    // wait (see the loop above) with nothing but m_interrupt to wake it --
    // under F-166's original code this join() always completed promptly on
    // its own, since that thread was guaranteed to terminate itself within
    // bounded time either way (m_synced reached, or an immediate `return;`
    // on a body-missing block). That guarantee no longer holds now that the
    // body-missing case retries instead of exiting, so Stop() must actually
    // signal the thread before joining it. F-181 (independent review of
    // F-170, LOW): the comment here previously claimed this matches
    // "upstream Bitcoin Core's own BaseIndex::Stop()" -- it doesn't;
    // upstream's Stop() has no Interrupt() call at all. The call itself is
    // this tree's own (harmless, idempotent) addition, not a port of
    // upstream behaviour -- this was simply missing here, latent and
    // untriggered until this fix made ThreadSync capable of blocking past
    // its own join() point.
    Interrupt();

    if (m_thread_sync.joinable()) {
        m_thread_sync.join();
    }
}
