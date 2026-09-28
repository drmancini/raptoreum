// Copyright (c) 2011-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <blockencodings.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <chainparams.h>
#include <pow.h>
#include <random.h>

#include <test/test_raptoreum.h>

#include <boost/test/unit_test.hpp>

std::vector <std::pair<uint256, CTransactionRef>> extra_txn;

BOOST_FIXTURE_TEST_SUITE(blockencodings_tests, RegTestingSetup
)

static CBlock BuildBlockTestCase() {
    CBlock block;
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].scriptSig.resize(10);
    tx.vout.resize(1);
    tx.vout[0].nValue = 42;

    block.vtx.resize(3);
    block.vtx[0] = MakeTransactionRef(tx);
    block.nVersion = 42;
    block.hashPrevBlock = InsecureRand256();
    block.nBits = 0x207fffff;

    tx.vin[0].prevout.hash = InsecureRand256();
    tx.vin[0].prevout.n = 0;
    block.vtx[1] = MakeTransactionRef(tx);

    tx.vin.resize(10);
    for (size_t i = 0; i < tx.vin.size(); i++) {
        tx.vin[i].prevout.hash = InsecureRand256();
        tx.vin[i].prevout.n = 0;
    }
    block.vtx[2] = MakeTransactionRef(tx);

    bool mutated;
    block.hashMerkleRoot = BlockMerkleRoot(block, &mutated);
    assert(!mutated);
    while (!CheckProofOfWork(block.GetPOWHash(), block.nBits, Params().GetConsensus())) ++block.nNonce;
    return block;
}

// Number of shared use_counts we expect for a tx we haven't touched
// (block + mempool + our copy from the GetSharedTx call)
constexpr long SHARED_TX_OFFSET{3};

BOOST_AUTO_TEST_CASE(SimpleRoundTripTest)
        {
                CTxMemPool pool;
        TestMemPoolEntryHelper entry;
        CBlock block(BuildBlockTestCase());

        LOCK2(cs_main, pool.cs);
        pool.addUnchecked(entry.FromTx(block.vtx[2]));
        BOOST_CHECK_EQUAL(pool.mapTx.find(block.vtx[2]->GetHash())->GetSharedTx().use_count(), SHARED_TX_OFFSET + 0);

        // Do a simple ShortTxIDs RT
        {
            CBlockHeaderAndShortTxIDs shortIDs(block);

            CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
            stream << shortIDs;

            CBlockHeaderAndShortTxIDs shortIDs2;
            stream >> shortIDs2;

            PartiallyDownloadedBlock partialBlock(&pool);
            BOOST_CHECK(partialBlock.InitData(shortIDs2, extra_txn) == READ_STATUS_OK);
            BOOST_CHECK(partialBlock.IsTxAvailable(0));
            BOOST_CHECK(!partialBlock.IsTxAvailable(1));
            BOOST_CHECK(partialBlock.IsTxAvailable(2));

            BOOST_CHECK_EQUAL(pool.mapTx.find(block.vtx[2]->GetHash())->GetSharedTx().use_count(),
                              SHARED_TX_OFFSET + 1);

            size_t poolSize = pool.size();
            pool.removeRecursive(*block.vtx[2], MemPoolRemovalReason::MANUAL);
            BOOST_CHECK_EQUAL(pool.size(), poolSize - 1);

            CBlock block2;
            {
                PartiallyDownloadedBlock tmp = partialBlock;
                BOOST_CHECK(partialBlock.FillBlock(block2, {}) == READ_STATUS_INVALID); // No transactions
                partialBlock = tmp;
            }

            // Wrong transaction
            {
                PartiallyDownloadedBlock tmp = partialBlock;
                partialBlock.FillBlock(block2,
                                       {block.vtx[2]}); // Current implementation doesn't check txn here, but don't require that
                partialBlock = tmp;
            }
            bool mutated;
            BOOST_CHECK(block.hashMerkleRoot != BlockMerkleRoot(block2, &mutated));

            CBlock block3;
            BOOST_CHECK(partialBlock.FillBlock(block3, {block.vtx[1]}) == READ_STATUS_OK);
            BOOST_CHECK_EQUAL(block.GetHash().ToString(), block3.GetHash().ToString());
            BOOST_CHECK_EQUAL(block.hashMerkleRoot.ToString(), BlockMerkleRoot(block3, &mutated).ToString());
            BOOST_CHECK(!mutated);
        }
        }

class TestHeaderAndShortIDs {
    // Utility to encode custom CBlockHeaderAndShortTxIDs
public:
    CBlockHeader header;
    uint64_t nonce;
    std::vector <uint64_t> shorttxids;
    std::vector <PrefilledTransaction> prefilledtxn;

    explicit TestHeaderAndShortIDs(const CBlockHeaderAndShortTxIDs &orig) {
        CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
        stream << orig;
        stream >> *this;
    }

    explicit TestHeaderAndShortIDs(const CBlock &block) :
            TestHeaderAndShortIDs(CBlockHeaderAndShortTxIDs(block)) {}

    uint64_t GetShortID(const uint256 &txhash) const {
        CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
        stream << *this;
        CBlockHeaderAndShortTxIDs base;
        stream >> base;
        return base.GetShortID(txhash);
    }

    SERIALIZE_METHODS(TestHeaderAndShortIDs, obj
    ) { READWRITE(obj.header, obj.nonce, Using < VectorFormatter < CustomUintFormatter <
                                         CBlockHeaderAndShortTxIDs::SHORTTXIDS_LENGTH>>>(obj.shorttxids), obj.prefilledtxn);
    }
};

BOOST_AUTO_TEST_CASE(NonCoinbasePreforwardRTTest)
        {
                CTxMemPool pool;
        TestMemPoolEntryHelper entry;
        CBlock block(BuildBlockTestCase());

        LOCK2(cs_main, pool.cs);
        pool.addUnchecked(entry.FromTx(block.vtx[2]));
        BOOST_CHECK_EQUAL(pool.mapTx.find(block.vtx[2]->GetHash())->GetSharedTx().use_count(), SHARED_TX_OFFSET + 0);

        uint256 txhash;

        // Test with pre-forwarding tx 1, but not coinbase
        {
            TestHeaderAndShortIDs shortIDs(block);
            shortIDs.prefilledtxn.resize(1);
            shortIDs.prefilledtxn[0] = {1, block.vtx[1]};
            shortIDs.shorttxids.resize(2);
            shortIDs.shorttxids[0] = shortIDs.GetShortID(block.vtx[0]->GetHash());
            shortIDs.shorttxids[1] = shortIDs.GetShortID(block.vtx[2]->GetHash());

            CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
            stream << shortIDs;

            CBlockHeaderAndShortTxIDs shortIDs2;
            stream >> shortIDs2;

            PartiallyDownloadedBlock partialBlock(&pool);
            BOOST_CHECK(partialBlock.InitData(shortIDs2, extra_txn) == READ_STATUS_OK);
            BOOST_CHECK(!partialBlock.IsTxAvailable(0));
            BOOST_CHECK(partialBlock.IsTxAvailable(1));
            BOOST_CHECK(partialBlock.IsTxAvailable(2));

            BOOST_CHECK_EQUAL(pool.mapTx.find(block.vtx[2]->GetHash())->GetSharedTx().use_count(),
                              SHARED_TX_OFFSET + 1);

            CBlock block2;
            {
                PartiallyDownloadedBlock tmp = partialBlock;
                BOOST_CHECK(partialBlock.FillBlock(block2, {}) == READ_STATUS_INVALID); // No transactions
                partialBlock = tmp;
            }

            // Wrong transaction
            {
                PartiallyDownloadedBlock tmp = partialBlock;
                partialBlock.FillBlock(block2,
                                       {block.vtx[1]}); // Current implementation doesn't check txn here, but don't require that
                partialBlock = tmp;
            }
            BOOST_CHECK_EQUAL(pool.mapTx.find(block.vtx[2]->GetHash())->GetSharedTx().use_count(),
                              SHARED_TX_OFFSET + 2); // +2 because of partialBlock and block2
            bool mutated;
            BOOST_CHECK(block.hashMerkleRoot != BlockMerkleRoot(block2, &mutated));

            CBlock block3;
            PartiallyDownloadedBlock partialBlockCopy = partialBlock;
            BOOST_CHECK(partialBlock.FillBlock(block3, {block.vtx[0]}) == READ_STATUS_OK);
            BOOST_CHECK_EQUAL(block.GetHash().ToString(), block3.GetHash().ToString());
            BOOST_CHECK_EQUAL(block.hashMerkleRoot.ToString(), BlockMerkleRoot(block3, &mutated).ToString());
            BOOST_CHECK(!mutated);

            BOOST_CHECK_EQUAL(pool.mapTx.find(block.vtx[2]->GetHash())->GetSharedTx().use_count(),
                              SHARED_TX_OFFSET + 3); // +2 because of partialBlock and block2 and block3

            txhash = block.vtx[2]->GetHash();
            block.vtx.clear();
            block2.vtx.clear();
            block3.vtx.clear();
            BOOST_CHECK_EQUAL(pool.mapTx.find(txhash)->GetSharedTx().use_count(),
                              SHARED_TX_OFFSET + 1 - 1); // + 1 because of partialBlock; -1 because of block.
        }
        BOOST_CHECK_EQUAL(pool.mapTx.find(txhash)->GetSharedTx().use_count(), SHARED_TX_OFFSET - 1); // -1 because of block
        }

BOOST_AUTO_TEST_CASE(SufficientPreforwardRTTest)
        {
                CTxMemPool pool;
        TestMemPoolEntryHelper entry;
        CBlock block(BuildBlockTestCase());

        LOCK2(cs_main, pool.cs);
        pool.addUnchecked(entry.FromTx(block.vtx[1]));
        BOOST_CHECK_EQUAL(pool.mapTx.find(block.vtx[1]->GetHash())->GetSharedTx().use_count(), SHARED_TX_OFFSET + 0);

        uint256 txhash;

        // Test with pre-forwarding coinbase + tx 2 with tx 1 in mempool
        {
            TestHeaderAndShortIDs shortIDs(block);
            shortIDs.prefilledtxn.resize(2);
            shortIDs.prefilledtxn[0] = {0, block.vtx[0]};
            shortIDs.prefilledtxn[1] = {1, block.vtx[2]}; // id == 1 as it is 1 after index 1
            shortIDs.shorttxids.resize(1);
            shortIDs.shorttxids[0] = shortIDs.GetShortID(block.vtx[1]->GetHash());

            CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
            stream << shortIDs;

            CBlockHeaderAndShortTxIDs shortIDs2;
            stream >> shortIDs2;

            PartiallyDownloadedBlock partialBlock(&pool);
            BOOST_CHECK(partialBlock.InitData(shortIDs2, extra_txn) == READ_STATUS_OK);
            BOOST_CHECK(partialBlock.IsTxAvailable(0));
            BOOST_CHECK(partialBlock.IsTxAvailable(1));
            BOOST_CHECK(partialBlock.IsTxAvailable(2));

            BOOST_CHECK_EQUAL(pool.mapTx.find(block.vtx[1]->GetHash())->GetSharedTx().use_count(),
                              SHARED_TX_OFFSET + 1);

            CBlock block2;
            PartiallyDownloadedBlock partialBlockCopy = partialBlock;
            BOOST_CHECK(partialBlock.FillBlock(block2, {}) == READ_STATUS_OK);
            BOOST_CHECK_EQUAL(block.GetHash().ToString(), block2.GetHash().ToString());
            bool mutated;
            BOOST_CHECK_EQUAL(block.hashMerkleRoot.ToString(), BlockMerkleRoot(block2, &mutated).ToString());
            BOOST_CHECK(!mutated);

            txhash = block.vtx[1]->GetHash();
            block.vtx.clear();
            block2.vtx.clear();
            BOOST_CHECK_EQUAL(pool.mapTx.find(txhash)->GetSharedTx().use_count(),
                              SHARED_TX_OFFSET + 1 - 1); // + 1 because of partialBlock; -1 because of block
        }
        BOOST_CHECK_EQUAL(pool.mapTx.find(txhash)->GetSharedTx().use_count(), SHARED_TX_OFFSET - 1); // -1 because of block
        }

BOOST_AUTO_TEST_CASE(EmptyBlockRoundTripTest)
        {
                CTxMemPool pool;
        CMutableTransaction coinbase;
        coinbase.vin.resize(1);
        coinbase.vin[0].scriptSig.resize(10);
        coinbase.vout.resize(1);
        coinbase.vout[0].nValue = 42;

        CBlock block;
        block.vtx.resize(1);
        block.vtx[0] = MakeTransactionRef(std::move(coinbase));
        block.nVersion = 42;
        block.hashPrevBlock = InsecureRand256();
        block.nBits = 0x207fffff;

        bool mutated;
        block.hashMerkleRoot = BlockMerkleRoot(block, &mutated);
        assert(!mutated);
        while (!CheckProofOfWork(block.GetPOWHash(), block.nBits, Params().GetConsensus())) ++block.nNonce;

        // Test simple header round-trip with only coinbase
        {
            CBlockHeaderAndShortTxIDs shortIDs(block);

            CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
            stream << shortIDs;

            CBlockHeaderAndShortTxIDs shortIDs2;
            stream >> shortIDs2;

            PartiallyDownloadedBlock partialBlock(&pool);
            BOOST_CHECK(partialBlock.InitData(shortIDs2, extra_txn) == READ_STATUS_OK);
            BOOST_CHECK(partialBlock.IsTxAvailable(0));

            CBlock block2;
            std::vector <CTransactionRef> vtx_missing;
            BOOST_CHECK(partialBlock.FillBlock(block2, vtx_missing) == READ_STATUS_OK);
            BOOST_CHECK_EQUAL(block.GetHash().ToString(), block2.GetHash().ToString());
            BOOST_CHECK_EQUAL(block.hashMerkleRoot.ToString(), BlockMerkleRoot(block2, &mutated).ToString());
            BOOST_CHECK(!mutated);
        }
        }

BOOST_AUTO_TEST_CASE(TransactionsRequestSerializationTest) {
        BlockTransactionsRequest req1;
        req1.blockhash = InsecureRand256();
        req1.indexes.resize(4);
        req1.indexes[0] = 0;
        req1.indexes[1] = 1;
        req1.indexes[2] = 3;
        req1.indexes[3] = 4;

        CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
        stream << req1;

        BlockTransactionsRequest req2;
        stream >> req2;

        BOOST_CHECK_EQUAL(req1.blockhash.ToString(), req2.blockhash.ToString());
        BOOST_CHECK_EQUAL(req1.indexes.size(), req2.indexes.size());
        BOOST_CHECK_EQUAL(req1.indexes[0], req2.indexes[0]);
        BOOST_CHECK_EQUAL(req1.indexes[1], req2.indexes[1]);
        BOOST_CHECK_EQUAL(req1.indexes[2], req2.indexes[2]);
        BOOST_CHECK_EQUAL(req1.indexes[3], req2.indexes[3]);
}

BOOST_AUTO_TEST_CASE(TransactionsRequestDeserializationMaxTest) {
        // Check that the highest legal index is decoded correctly
        BlockTransactionsRequest req0;
        req0.blockhash = InsecureRand256();
        req0.indexes.resize(1);
        req0.indexes[0] = 0xffff;
        CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
        stream << req0;

        BlockTransactionsRequest req1;
        stream >> req1;
        BOOST_CHECK_EQUAL(req0.indexes.size(), req1.indexes.size());
        BOOST_CHECK_EQUAL(req0.indexes[0], req1.indexes[0]);
}

BOOST_AUTO_TEST_CASE(TransactionsRequestDeserializationOverflowTest) {
        // Any set of index deltas that starts with N values that sum to
        // (2^32 - N) causes the edge-case overflow that was originally not
        // checked for. F-200 (4.5.2 Part A) widened indexes's element type
        // from uint16_t to uint32_t, so this boundary moved from 0x10000 to
        // 0x100000000 -- still exercising DifferenceFormatter's own generic
        // wraparound protection (templated on the element type, so it
        // adapts automatically), just at the new, wider boundary. Such a
        // request cannot be created by serializing a real
        // BlockTransactionsRequest due to the overflow, so here we'll
        // serialize from raw deltas.
        BlockTransactionsRequest req0;
        req0.blockhash = InsecureRand256();
        req0.indexes.resize(3);
        req0.indexes[0] = 0x70000000;
        req0.indexes[1] = (uint32_t)(0x100000000ULL - 0x70000000ULL - 2);
        req0.indexes[2] = 0;
        CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
        stream << req0.blockhash;
        WriteCompactSize(stream, req0.indexes.size());
        WriteCompactSize(stream, req0.indexes[0]);
        WriteCompactSize(stream, req0.indexes[1]);
        WriteCompactSize(stream, req0.indexes[2]);

        BlockTransactionsRequest req1;
        try {
            stream >> req1;
            // before patch: deserialize above succeeds and this check fails, demonstrating the overflow
            BOOST_CHECK(req1.indexes[1] < req1.indexes[2]);
            // this shouldn't be reachable before or after patch
            BOOST_CHECK(0);
        } catch(std::ios_base::failure &) {
            // deserialize should fail
        }
}

// F-200 (4.5.2 Part A): PrefilledTransaction::index and
// BlockTransactionsRequest::indexes were uint16_t (max 65,535) -- this
// branch's own MAX_DIP0001_BLOCK_SIZE (8,000,000, consensus/consensus.h)
// needs up to 250,000 identifiers, which overflows that outright. Widened to
// uint32_t. The first two cases pin the widened range directly at the point
// of assignment: a narrowing int->uint16_t conversion silently WRAPS rather
// than failing to compile, so the pre-widening bug reproduces as a wrong
// stored value, not a crash -- these are genuine RED before the widening.
BOOST_AUTO_TEST_CASE(PrefilledTransactionIndexWideningTest) {
        PrefilledTransaction pt;
        pt.index = 100000;
        BOOST_CHECK_EQUAL(pt.index, 100000U);
}

BOOST_AUTO_TEST_CASE(BlockTransactionsRequestIndexesWideningTest) {
        BlockTransactionsRequest req;
        req.indexes.push_back(100000);
        BOOST_CHECK_EQUAL(req.indexes[0], 100000U);
}

BOOST_AUTO_TEST_CASE(BlockTransactionsRequestWideIndexRoundTripTest) {
        // Exercises the actual wire path (DifferenceFormatter's own
        // WriteCompactSize/ReadCompactSize), not just in-memory storage: a
        // single index above the old uint16_t ceiling must serialize and
        // deserialize back to the same value.
        BlockTransactionsRequest req0;
        req0.blockhash = InsecureRand256();
        req0.indexes.resize(1);
        req0.indexes[0] = 100000;

        CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
        stream << req0;

        BlockTransactionsRequest req1;
        stream >> req1;
        BOOST_CHECK_EQUAL(req0.indexes.size(), req1.indexes.size());
        // Pinned against the literal, not just req0 -- req0 itself already
        // truncates silently under uint16_t, so comparing only against req0
        // would pass even before the widening (round-tripping whatever
        // wrong value got stored).
        BOOST_CHECK_EQUAL(req1.indexes[0], 100000U);
        BOOST_CHECK_EQUAL(req0.indexes[0], req1.indexes[0]);
}

// F-200 (4.5.2 Part A): CBlockHeaderAndShortTxIDs::BlockTxCount()'s
// deserialize-time overflow guard (blockencodings.h) must bind to the
// design's own consensus cap, COMMITMENT_BUDGET_MAX_INPUTS (700,000, D-19)
// -- not to the widened type's own incidental max (bounded in practice to
// ReadCompactSize's own MAX_SIZE, ~33.5 million, serialize.h -- not
// uint32_t's full ~4.29 billion range, since PrefilledTransaction::index is
// wire-encoded via COMPACTSIZE and ReadCompactSize rejects anything over
// MAX_SIZE before it ever reaches the field). The cases below pin that
// binding precisely: a count that fits the OLD uint16_t ceiling still
// passes; a count between the old ceiling and the cap (this branch's own
// 8 MB/250,000-identifier need -- impossible before this fix) now also
// passes; the cap itself passes exactly.
//
// F-202 (4.5.2 fix, review): the guard bound itself was off by one.
// COMMITMENT_BUDGET_MAX_INPUTS bounds aggregate NON-coinbase input count
// (consensus/consensus.h's own MaxBlockInputs doc comment: "the coinbase's
// own dummy input is excluded"), not total transaction count -- but
// BlockTxCount() counts every transaction, coinbase included. A maximal
// valid block is COMMITMENT_BUDGET_MAX_INPUTS single-input, non-coinbase
// transactions plus exactly one coinbase = COMMITMENT_BUDGET_MAX_INPUTS + 1
// total transactions, and the guard previously rejected exactly that block
// (BlockTxCount() > COMMITMENT_BUDGET_MAX_INPUTS, one short of the true
// maximum), disagreeing by one with PartiallyDownloadedBlock::InitData's own
// candidateindex bound (blockencodings.cpp), which already correctly
// permits a 0-based index up to COMMITMENT_BUDGET_MAX_INPUTS (the
// (COMMITMENT_BUDGET_MAX_INPUTS + 1)-th transaction). If the guard is ever
// reverted to the old, one-short bound, `...PlusOneWorks` below starts
// throwing -- that is the mutation this pair is built to catch.
static CBlockHeaderAndShortTxIDs RoundTripWithShortTxIdCount(size_t count) {
        TestHeaderAndShortIDs shortIDs(BuildBlockTestCase());
        shortIDs.shorttxids.assign(count, 0);
        shortIDs.prefilledtxn.clear();

        CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
        stream << shortIDs;

        CBlockHeaderAndShortTxIDs result;
        stream >> result;
        return result;
}

BOOST_AUTO_TEST_CASE(BlockTxCountUnderOldUint16CeilingStillWorks) {
        BOOST_CHECK_NO_THROW(RoundTripWithShortTxIdCount(60000));
}

BOOST_AUTO_TEST_CASE(BlockTxCountAboveOldUint16CeilingNowWorks) {
        // This branch's own MAX_DIP0001_BLOCK_SIZE=8,000,000 needs up to
        // 250,000 identifiers -- exceeded the old 65,535 ceiling outright.
        BOOST_CHECK_NO_THROW(RoundTripWithShortTxIdCount(250000));
}

BOOST_AUTO_TEST_CASE(BlockTxCountAtCommitmentBudgetMaxInputsWorks) {
        BOOST_CHECK_NO_THROW(RoundTripWithShortTxIdCount(COMMITMENT_BUDGET_MAX_INPUTS));
}

BOOST_AUTO_TEST_CASE(BlockTxCountAtCommitmentBudgetMaxInputsPlusOneWorks) {
        // F-202: the real maximum (700,000 non-coinbase + 1 coinbase) must
        // be ACCEPTED, not rejected -- this is the exact case the pre-fix
        // off-by-one guard dropped.
        BOOST_CHECK_NO_THROW(RoundTripWithShortTxIdCount(COMMITMENT_BUDGET_MAX_INPUTS + 1));
}

BOOST_AUTO_TEST_CASE(BlockTxCountOverCommitmentBudgetMaxInputsPlusOneThrows) {
        BOOST_CHECK_THROW(RoundTripWithShortTxIdCount(COMMITMENT_BUDGET_MAX_INPUTS + 2), std::ios_base::failure);
}

BOOST_AUTO_TEST_CASE(MaximalCommitmentBudgetBlockWithCoinbaseInitializesOk) {
        // F-202: the reviewer's own exact scenario, end to end -- not just
        // the header-level guard, but PartiallyDownloadedBlock::InitData too
        // -- a maximal 700,000-non-coinbase-input commitment-budget block,
        // 700,001 total transactions counting one coinbase. Pre-fix, this
        // never reached InitData at all: BlockTxCount() > COMMITMENT_BUDGET_
        // MAX_INPUTS threw at deserialization first.
        CTxMemPool pool;
        CBlock block(BuildBlockTestCase());
        TestHeaderAndShortIDs shortIDs(block);
        shortIDs.prefilledtxn.assign(1, PrefilledTransaction{0, block.vtx[0]});
        shortIDs.shorttxids.resize(COMMITMENT_BUDGET_MAX_INPUTS);
        for (size_t i = 0; i < shortIDs.shorttxids.size(); i++) {
            // Unique, not zero: InitData treats a colliding shorttxid as a
            // real short-ID collision (READ_STATUS_FAILED), which would
            // mask the guard-boundary behaviour this test exists to prove.
            shortIDs.shorttxids[i] = i + 1;
        }

        CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
        stream << shortIDs;
        CBlockHeaderAndShortTxIDs result;
        BOOST_REQUIRE_NO_THROW(stream >> result);
        BOOST_REQUIRE_EQUAL(result.BlockTxCount(), COMMITMENT_BUDGET_MAX_INPUTS + 1);

        PartiallyDownloadedBlock partialBlock(&pool);
        BOOST_CHECK_EQUAL(partialBlock.InitData(result, extra_txn), READ_STATUS_OK);
        BOOST_CHECK(partialBlock.IsTxAvailable(0));
}

// F-201 (4.5.2 Part B): SendBlockTransactions (net_processing.cpp) builds a
// single BLOCKTXN message with no size-based chunking -- at design
// throughput (~373 B/tx average, F-30), as few as ~8,000 requested
// transactions already exceeds MAX_PROTOCOL_MESSAGE_LENGTH (3 MB) and gets
// this node disconnected by the peer it's replying to. The size-vs-ceiling
// DECISION is factored out as a pure predicate (matching bodyrange.h's own
// ShouldDisconnectForBodyRangeAttempts precedent) so it is unit-testable
// without a CNode/CConnman network harness; net_processing.cpp wires it to
// the actual fallback (a CInv pushed to vRecvGetData, reusing
// MAX_BLOCKTXN_DEPTH's own existing full-block-fallback shape).
BOOST_AUTO_TEST_CASE(DoesNotDeclineWithinCeiling) {
        BOOST_CHECK(!ShouldDeclineBlockTransactionsForSize(100, 200));
}

BOOST_AUTO_TEST_CASE(DoesNotDeclineExactlyAtCeiling) {
        BOOST_CHECK(!ShouldDeclineBlockTransactionsForSize(200, 200));
}

BOOST_AUTO_TEST_CASE(DeclinesOneByteOverCeiling) {
        // If this trigger were ever reverted/disabled (e.g. hardcoded to
        // `return false`), this is the case that would silently pass again
        // -- the mutation this test is built to catch.
        BOOST_CHECK(ShouldDeclineBlockTransactionsForSize(201, 200));
}

BOOST_AUTO_TEST_CASE(DeclinesAtDesignPointScale) {
        // F-197's own worked example: ~8,000 missing transactions at F-30's
        // ~373 B average already exceeds a 2 MB ceiling.
        BOOST_CHECK(ShouldDeclineBlockTransactionsForSize(8000ULL * 373, 2 * 1024 * 1024));
}

BOOST_AUTO_TEST_SUITE_END()
