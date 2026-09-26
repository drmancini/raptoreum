// Copyright (c) 2017-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <index/txindex.h>
#include <script/standard.h>
#include <test/test_raptoreum.h>
#include <util/memory.h>
#include <util/system.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(txindex_tests)

// F7 (test review precedent, txvalidation_tests.cpp/blockbudget_tests.cpp,
// duplicated locally here per acceptancebit_tests.cpp's own PerfWithholdGuard
// since it isn't shared via a header): a thrown BOOST_REQUIRE between
// inserting into g_perf_withhold_hashes and erasing it would leave the flag
// set for every later test in the process.
struct PerfWithholdGuard {
    uint256 hash;

    explicit PerfWithholdGuard(const uint256 &hashIn) : hash(hashIn) {
        g_perf_withhold_hashes.insert(hash);
    }

    ~PerfWithholdGuard() { g_perf_withhold_hashes.erase(hash); }
};

// F-166 (4.1.1, F-160): BaseIndex::ThreadSync (index/base.cpp) is the shared
// sync loop for both TxIndex and BlockFilterIndex. Before the fix, a block
// on the active chain whose bodies aren't held (BLOCK_HAVE_BODIES clear --
// not reachable via any real accept path today, exercised here the same way
// F-161/F-163 did: accept a real block, then retroactively withhold it) made
// ReadBlockFromDisk fail and the loop called FatalError, which is node-wide
// (StartShutdown, not "stop this one index"). The fix makes the loop pause
// this one index gracefully instead. Exercised here via TxIndex; not
// separately duplicated for BlockFilterIndex since both derive from
// BaseIndex and share this one call site.
BOOST_FIXTURE_TEST_CASE(index_sync_pauses_instead_of_crashing_when_bodies_not_held, TestChain100Setup)
{
    CTransactionRef earlier_tx = m_coinbase_txns[0];

    CScript coinbase_script_pub_key = GetScriptForDestination(coinbaseKey.GetPubKey().GetID());
    std::vector<CMutableTransaction> no_txns;
    const CBlock &withheld_block = CreateAndProcessBlock(no_txns, coinbase_script_pub_key);
    uint256 withheld_hash = withheld_block.GetHash();
    uint256 withheld_coinbase_hash = withheld_block.vtx[0]->GetHash();

    CBlockIndex *pindex = LookupBlockIndex(withheld_hash);
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(::ChainActive().Contains(pindex));
    BOOST_REQUIRE(HaveBodies(pindex));

    PerfWithholdGuard guard(withheld_hash);
    pindex->nStatus &= ~BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(!HaveBodies(pindex));

    TxIndex txindex(1 << 20, true);
    txindex.Start();

    CTransactionRef tx_disk;
    uint256 block_hash;
    constexpr int64_t timeout_ms = 10 * 1000;
    int64_t time_start = GetTimeMillis();
    while (!txindex.FindTx(earlier_tx->GetHash(), block_hash, tx_disk)) {
        BOOST_REQUIRE(time_start + timeout_ms > GetTimeMillis());
        UninterruptibleSleep(std::chrono::milliseconds{100});
    }

    // Give the sync thread a further beat to reach (and pause at) the
    // withheld block.
    UninterruptibleSleep(std::chrono::milliseconds{200});

    // The withheld block's own coinbase must never have been written --
    // the loop paused before it, it didn't skip past it.
    BOOST_CHECK(!txindex.FindTx(withheld_coinbase_hash, block_hash, tx_disk));
    // And the index must never claim to be caught up while paused.
    BOOST_CHECK(!txindex.BlockUntilSyncedToCurrentChain());

    txindex.Stop();
}

// F-170 (independent review of F-166): F-166's own fix and its log line
// ("pausing sync (will resume once available)") claimed a pause-and-retry
// that didn't exist -- ThreadSync's `return;` on a body-missing block ended
// its own thread (m_thread_sync, spawned exactly once by Start(), never
// restarted) permanently, so m_synced could never become true and every
// later block was silently ignored by BlockConnected forever, regardless of
// whether the withheld block's body was ever supplied. This proves the
// FIXED behaviour directly: once the withheld block's body becomes
// available again (the guard lifted, the bit restored -- exactly what a
// future body-retention/fetch mechanism supplying a previously-missing body
// would look like from ThreadSync's own vantage point), the SAME index
// instance that was paused above resumes on its own, with no restart, and
// makes genuine further progress past the block it was stuck on.
BOOST_FIXTURE_TEST_CASE(index_sync_resumes_once_bodies_become_available, TestChain100Setup)
{
    CScript coinbase_script_pub_key = GetScriptForDestination(coinbaseKey.GetPubKey().GetID());
    std::vector<CMutableTransaction> no_txns;
    const CBlock &withheld_block = CreateAndProcessBlock(no_txns, coinbase_script_pub_key);
    uint256 withheld_hash = withheld_block.GetHash();
    uint256 withheld_coinbase_hash = withheld_block.vtx[0]->GetHash();

    // A further block past the withheld one -- proves the index catches all
    // the way up, not just past the withheld block alone.
    const CBlock &later_block = CreateAndProcessBlock(no_txns, coinbase_script_pub_key);
    uint256 later_coinbase_hash = later_block.vtx[0]->GetHash();

    CBlockIndex *pindex = LookupBlockIndex(withheld_hash);
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(HaveBodies(pindex));

    auto guard = MakeUnique<PerfWithholdGuard>(withheld_hash);
    pindex->nStatus &= ~BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(!HaveBodies(pindex));

    TxIndex txindex(1 << 20, true);
    txindex.Start();

    // Wait for the sync thread to reach (and pause at) the withheld block --
    // same technique as index_sync_pauses_instead_of_crashing_when_bodies_not_held.
    CTransactionRef tx_disk;
    uint256 block_hash;
    constexpr int64_t timeout_ms = 10 * 1000;
    int64_t time_start = GetTimeMillis();
    while (!txindex.FindTx(m_coinbase_txns[0]->GetHash(), block_hash, tx_disk)) {
        BOOST_REQUIRE(time_start + timeout_ms > GetTimeMillis());
        UninterruptibleSleep(std::chrono::milliseconds{100});
    }
    UninterruptibleSleep(std::chrono::milliseconds{200});
    BOOST_REQUIRE(!txindex.FindTx(withheld_coinbase_hash, block_hash, tx_disk));
    BOOST_REQUIRE(!txindex.BlockUntilSyncedToCurrentChain());

    // Now let the body become available again: lift the guard (so
    // ReadBlockFromDisk genuinely succeeds) and restore the status bit (so
    // HaveBodies agrees) -- both together, matching what a real
    // body-retention/fetch mechanism resolving the gap would produce.
    guard.reset();
    pindex->nStatus |= BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(HaveBodies(pindex));

    // The retry loop checks every BODIES_RETRY_INTERVAL_MS (1s); give it a
    // bounded window to notice and catch all the way up to the later block.
    time_start = GetTimeMillis();
    while (!txindex.BlockUntilSyncedToCurrentChain()) {
        BOOST_REQUIRE(time_start + timeout_ms > GetTimeMillis());
        UninterruptibleSleep(std::chrono::milliseconds{100});
    }

    // Genuine progress, not just the flag flipping: both the previously
    // withheld coinbase and the block after it are now indexed.
    BOOST_CHECK(txindex.FindTx(withheld_coinbase_hash, block_hash, tx_disk));
    BOOST_CHECK(txindex.FindTx(later_coinbase_hash, block_hash, tx_disk));

    txindex.Stop();
}

// F-177 (independent review of F-170's own retry loop, HIGH): the retry
// loop's interrupt-exit branch (`if (!m_interrupt.sleep_for(...)) {
// WriteBestBlock(pindex); return; }`) wrote the locator for `pindex` -- but
// at that point in the loop `pindex` is the STUCK sync TARGET (assigned via
// `pindex = pindex_next` earlier in this same iteration), not the last block
// actually indexed via WriteBlock (which only runs AFTER this retry loop
// exits successfully, later in the same iteration). This proves the on-disk
// locator, after an interrupt fired while paused in the retry loop, still
// points at the last block genuinely written -- not past the withheld one --
// by stopping a first TxIndex instance mid-retry, restoring the withheld
// block's body, then starting a SECOND TxIndex instance against the SAME
// on-disk database (f_memory=false, so it persists across instances within
// this test's own temp datadir) and confirming it still genuinely syncs the
// withheld block, rather than its Init() having already considered it done.
BOOST_FIXTURE_TEST_CASE(index_sync_writes_correct_locator_on_interrupt_during_retry, TestChain100Setup)
{
    CScript coinbase_script_pub_key = GetScriptForDestination(coinbaseKey.GetPubKey().GetID());
    std::vector<CMutableTransaction> no_txns;
    const CBlock &withheld_block = CreateAndProcessBlock(no_txns, coinbase_script_pub_key);
    uint256 withheld_hash = withheld_block.GetHash();
    uint256 withheld_coinbase_hash = withheld_block.vtx[0]->GetHash();

    CBlockIndex *pindex = LookupBlockIndex(withheld_hash);
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(::ChainActive().Tip() == pindex);
    BOOST_REQUIRE(HaveBodies(pindex));

    auto guard = MakeUnique<PerfWithholdGuard>(withheld_hash);
    pindex->nStatus &= ~BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(!HaveBodies(pindex));

    {
        // f_memory=false: persists to a real on-disk DB under this test's own
        // temp datadir, so a second instance below can reopen the same file.
        TxIndex txindex(1 << 20, /*f_memory=*/false);
        txindex.Start();

        CTransactionRef tx_disk;
        uint256 block_hash;
        constexpr int64_t timeout_ms = 10 * 1000;
        int64_t time_start = GetTimeMillis();
        while (!txindex.FindTx(m_coinbase_txns[0]->GetHash(), block_hash, tx_disk)) {
            BOOST_REQUIRE(time_start + timeout_ms > GetTimeMillis());
            UninterruptibleSleep(std::chrono::milliseconds{100});
        }
        // Give the sync thread a further beat to reach (and pause at) the
        // withheld block's own retry loop.
        UninterruptibleSleep(std::chrono::milliseconds{200});
        BOOST_REQUIRE(!txindex.FindTx(withheld_coinbase_hash, block_hash, tx_disk));

        // Stop() interrupts the thread while it's inside the bodies-retry
        // loop -- exactly the interrupt-exit branch under test.
        txindex.Stop();
    }

    // Restore the body so a genuine sync attempt against it can succeed --
    // isolating whether the SECOND instance's behaviour reflects a correct
    // locator (still attempts this block) or the buggy one (skips it, since
    // its own Init() would already consider it done).
    guard.reset();
    pindex->nStatus |= BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(HaveBodies(pindex));

    TxIndex txindex2(1 << 20, /*f_memory=*/false);
    txindex2.Start();

    constexpr int64_t timeout_ms2 = 10 * 1000;
    int64_t time_start2 = GetTimeMillis();
    while (!txindex2.BlockUntilSyncedToCurrentChain()) {
        BOOST_REQUIRE(time_start2 + timeout_ms2 > GetTimeMillis());
        UninterruptibleSleep(std::chrono::milliseconds{100});
    }

    // If the first instance's interrupt exit had written a locator pointing
    // PAST the withheld block (the bug), the second instance's Init() would
    // consider height 101 already done and never actually index its
    // coinbase. This confirms genuine sync happened, not a skip.
    uint256 block_hash2;
    CTransactionRef tx_disk2;
    BOOST_CHECK(txindex2.FindTx(withheld_coinbase_hash, block_hash2, tx_disk2));

    txindex2.Stop();
}

// F-171 (independent review of F-163): TxIndex::FindTx (index/txindex.cpp)
// reads blk*.dat directly via OpenBlockFile, bypassing ReadBlockFromDisk and
// therefore HaveBodies entirely -- undermining F-163's own GetTransaction
// guard, whose txindex fallback calls straight into this function. Same
// no-PerfWithholdGuard-needed reasoning as F-163/F-168: blk*.dat holds the
// real bytes unconditionally under Phase 1 (F-110), so clearing
// BLOCK_HAVE_BODIES alone is enough to distinguish "checks the bit" from
// "happened to fail because the bytes were genuinely gone".
BOOST_FIXTURE_TEST_CASE(findtx_respects_have_bodies_even_though_the_read_would_succeed, TestChain100Setup)
{
    TxIndex txindex(1 << 20, true);
    txindex.Start();

    constexpr int64_t timeout_ms = 10 * 1000;
    int64_t time_start = GetTimeMillis();
    while (!txindex.BlockUntilSyncedToCurrentChain()) {
        BOOST_REQUIRE(time_start + timeout_ms > GetTimeMillis());
        UninterruptibleSleep(std::chrono::milliseconds{100});
    }

    CTransactionRef earlier_tx = m_coinbase_txns[0];
    uint256 block_hash;
    CTransactionRef tx_disk;
    BOOST_REQUIRE(txindex.FindTx(earlier_tx->GetHash(), block_hash, tx_disk));
    BOOST_REQUIRE(tx_disk->GetHash() == earlier_tx->GetHash());

    CBlockIndex *pindex = LookupBlockIndex(block_hash);
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(HaveBodies(pindex));

    pindex->nStatus &= ~BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(!HaveBodies(pindex));

    uint256 block_hash2;
    CTransactionRef tx_disk2;
    BOOST_CHECK(!txindex.FindTx(earlier_tx->GetHash(), block_hash2, tx_disk2));

    // Restore before the fixture tears down.
    pindex->nStatus |= BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(HaveBodies(pindex));

    txindex.Stop();
}

BOOST_FIXTURE_TEST_CASE(txindex_initial_sync, TestChain100Setup
)
{
TxIndex txindex(1 << 20, true);

CTransactionRef tx_disk;
uint256 block_hash;

// Transaction should not be found in the index before it is started.
for (
const auto &txn
: m_coinbase_txns) {
BOOST_CHECK(!txindex.
FindTx(txn
->

GetHash(), block_hash, tx_disk

));
}

// BlockUntilSyncedToCurrentChain should return false before txindex is started.
BOOST_CHECK(!txindex.

BlockUntilSyncedToCurrentChain()

);

txindex.

Start();

// Allow tx index to catch up with the block index.
constexpr int64_t
timeout_ms = 10 * 1000;
int64_t time_start = GetTimeMillis();
while (!txindex.

BlockUntilSyncedToCurrentChain()

) {
BOOST_REQUIRE(time_start
+ timeout_ms >

GetTimeMillis()

);
UninterruptibleSleep(std::chrono::milliseconds{100}
);
}

// Check that txindex excludes genesis block transactions.
const CBlock &genesis_block = Params().GenesisBlock();
for (
const auto &txn
: genesis_block.vtx) {
BOOST_CHECK(!txindex.
FindTx(txn
->

GetHash(), block_hash, tx_disk

));
}

// Check that txindex has all txs that were in the chain before it started.
for (
const auto &txn
: m_coinbase_txns) {
if (!txindex.
FindTx(txn
->

GetHash(), block_hash, tx_disk

)) {
BOOST_ERROR("FindTx failed");
} else if (tx_disk->

GetHash()

!= txn->

GetHash()

) {
BOOST_ERROR("Read incorrect tx");
}
}

// Check that new transactions in new blocks make it into the index.
for (
int i = 0;
i < 10; i++) {
CScript coinbase_script_pub_key = GetScriptForDestination(coinbaseKey.GetPubKey().GetID());
std::vector <CMutableTransaction> no_txns;
const CBlock &block = CreateAndProcessBlock(no_txns, coinbase_script_pub_key);
const CTransaction &txn = *block.vtx[0];

BOOST_CHECK(txindex
.

BlockUntilSyncedToCurrentChain()

);
if (!txindex.
FindTx(txn
.

GetHash(), block_hash, tx_disk

)) {
BOOST_ERROR("FindTx failed");
} else if (tx_disk->

GetHash()

!= txn.

GetHash()

) {
BOOST_ERROR("Read incorrect tx");
}
}
}

BOOST_AUTO_TEST_SUITE_END()
