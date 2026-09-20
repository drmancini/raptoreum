// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// 2.1.1: the body-store record format and file series, in isolation --
// nothing here touches CBlockIndex or AcceptBlock yet (that's a later 2.1
// step). See bodystore.h for the design.

#include <bodystore.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/test_raptoreum.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/system.h>

#include <boost/test/unit_test.hpp>

// BasicTestingSetup alone never points -datadir at its own temp root or
// clears GetBlocksDir()'s path cache (only TestingSetup does, alongside a lot
// of chain-init machinery this suite doesn't need) -- fine for tests that
// never touch a real file, which is every other BasicTestingSetup-based test
// in this tree, but these do. Without this, GetBlocksDir() can return a path
// left behind by whichever earlier test last set one, which by the time this
// suite runs has already been deleted by that test's own teardown.
struct BodyStoreTestingSetup : public BasicTestingSetup {
    BodyStoreTestingSetup() {
        SetDataDir("tempdir");
        ClearDatadirCache();
    }
};

BOOST_FIXTURE_TEST_SUITE(bodystore_tests, BodyStoreTestingSetup)

static CTransactionRef MakeBodyTx(uint32_t nonce) {
    CMutableTransaction tx;
    tx.nVersion = 1;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint(uint256S(strprintf("%064x", nonce)), 0);
    tx.vout.resize(1);
    tx.vout[0].nValue = 1000 + nonce;
    tx.vout[0].scriptPubKey = CScript() << OP_TRUE;
    return MakeTransactionRef(std::move(tx));
}

static std::vector<CTransactionRef> MakeBodies(size_t n) {
    std::vector<CTransactionRef> bodies;
    bodies.reserve(n);
    for (size_t i = 0; i < n; i++) {
        bodies.push_back(MakeBodyTx((uint32_t) i + 1));
    }
    return bodies;
}

BOOST_AUTO_TEST_CASE(roundtrip_empty_record) {
    FlatFilePos pos;
    BOOST_REQUIRE(FindBodyPos(pos, 1)); // 1-byte CompactSize(0) header
    BOOST_REQUIRE(WriteBodyRecord(pos, {}));

    std::vector<CTransactionRef> bodiesOut;
    BOOST_REQUIRE(ReadBodyRecord(pos, bodiesOut));
    BOOST_CHECK(bodiesOut.empty());

    unsigned int count = 12345;
    BOOST_REQUIRE(ReadBodyRecordCount(pos, count));
    BOOST_CHECK_EQUAL(count, 0U);
}

BOOST_AUTO_TEST_CASE(roundtrip_preserves_order_and_content) {
    std::vector<CTransactionRef> bodies = MakeBodies(5);

    FlatFilePos pos;
    unsigned int addSize = 0;
    for (const auto &tx : bodies) addSize += ::GetSerializeSize(*tx, SER_DISK, CLIENT_VERSION) + 4;
    BOOST_REQUIRE(FindBodyPos(pos, addSize));
    BOOST_REQUIRE(WriteBodyRecord(pos, bodies));

    std::vector<CTransactionRef> bodiesOut;
    BOOST_REQUIRE(ReadBodyRecord(pos, bodiesOut));
    BOOST_REQUIRE_EQUAL(bodiesOut.size(), bodies.size());
    for (size_t i = 0; i < bodies.size(); i++) {
        BOOST_CHECK_EQUAL(bodiesOut[i]->GetHash().ToString(), bodies[i]->GetHash().ToString());
    }
}

BOOST_AUTO_TEST_CASE(read_body_record_count_matches_without_reading_bodies) {
    std::vector<CTransactionRef> bodies = MakeBodies(7);

    FlatFilePos pos;
    BOOST_REQUIRE(FindBodyPos(pos, 100000));
    BOOST_REQUIRE(WriteBodyRecord(pos, bodies));

    unsigned int count = 0;
    BOOST_REQUIRE(ReadBodyRecordCount(pos, count));
    BOOST_CHECK_EQUAL(count, 7U);
}

// The offset table's whole point (bodystore.h): every index must be directly
// reachable, not just the ones a sequential read happens to visit first.
BOOST_AUTO_TEST_CASE(read_body_at_returns_the_correct_transaction_for_every_index) {
    std::vector<CTransactionRef> bodies = MakeBodies(6);

    FlatFilePos pos;
    BOOST_REQUIRE(FindBodyPos(pos, 100000));
    BOOST_REQUIRE(WriteBodyRecord(pos, bodies));

    // Deliberately out of order, including the first and last index, so a
    // mutation that only fixes index 0 (e.g. always reading from the start)
    // cannot pass by accident.
    for (unsigned int index : {3U, 0U, 5U, 1U, 4U, 2U}) {
        CTransactionRef txOut;
        BOOST_REQUIRE(ReadBodyAt(pos, index, txOut));
        BOOST_CHECK_EQUAL(txOut->GetHash().ToString(), bodies[index]->GetHash().ToString());
    }
}

BOOST_AUTO_TEST_CASE(read_body_at_rejects_an_out_of_range_index) {
    std::vector<CTransactionRef> bodies = MakeBodies(3);

    FlatFilePos pos;
    BOOST_REQUIRE(FindBodyPos(pos, 100000));
    BOOST_REQUIRE(WriteBodyRecord(pos, bodies));

    CTransactionRef txOut;
    BOOST_CHECK(!ReadBodyAt(pos, 3, txOut));
    BOOST_CHECK(!ReadBodyAt(pos, 100, txOut));
}

BOOST_AUTO_TEST_CASE(find_body_pos_is_contiguous_within_one_file) {
    FlatFilePos pos1, pos2, pos3;
    BOOST_REQUIRE(FindBodyPos(pos1, 100));
    BOOST_REQUIRE(FindBodyPos(pos2, 200));
    BOOST_REQUIRE(FindBodyPos(pos3, 50));

    BOOST_CHECK_EQUAL(pos1.nFile, pos2.nFile);
    BOOST_CHECK_EQUAL(pos2.nFile, pos3.nFile);
    BOOST_CHECK_EQUAL(pos2.nPos, pos1.nPos + 100);
    BOOST_CHECK_EQUAL(pos3.nPos, pos2.nPos + 200);
}

// Mirrors FindBlockPos's own rollover behaviour for blk*.dat (validation.cpp):
// a write that would cross MAX_BODYFILE_SIZE starts a fresh file at position 0
// rather than splitting a record across two files.
BOOST_AUTO_TEST_CASE(find_body_pos_rolls_over_to_a_new_file_once_the_current_one_is_full) {
    FlatFilePos posNearEnd;
    BOOST_REQUIRE(FindBodyPos(posNearEnd, MAX_BODYFILE_SIZE - 100));

    FlatFilePos posOverflow;
    BOOST_REQUIRE(FindBodyPos(posOverflow, 200));

    BOOST_CHECK_EQUAL(posOverflow.nFile, posNearEnd.nFile + 1);
    BOOST_CHECK_EQUAL(posOverflow.nPos, 0U);
}

BOOST_AUTO_TEST_SUITE_END()
