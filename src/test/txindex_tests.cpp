// Copyright (c) 2017-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <index/txindex.h>
#include <script/standard.h>
#include <test/test_raptoreum.h>
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
