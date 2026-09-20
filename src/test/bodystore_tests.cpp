// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// 2.1.1: the body-store record format and file series, in isolation --
// nothing here touches CBlockIndex or AcceptBlock yet (that's a later 2.1
// step). See bodystore.h for the design.

#include <bodystore.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <test/test_raptoreum.h>
#include <tinyformat.h>
#include <txdb.h>
#include <uint256.h>
#include <util/system.h>

#include <boost/test/unit_test.hpp>

// validation.h's own declaration; redeclared here rather than including all
// of validation.h for one pointer (matches bodystore.cpp's own approach).
extern std::unique_ptr<CBlockTreeDB> pblocktree;

// BasicTestingSetup alone never points -datadir at its own temp root or
// clears GetBlocksDir()'s path cache (only TestingSetup does, alongside a lot
// of chain-init machinery this suite doesn't need) -- fine for tests that
// never touch a real file, which is every other BasicTestingSetup-based test
// in this tree, but these do. Without this, GetBlocksDir() can return a path
// left behind by whichever earlier test last set one, which by the time this
// suite runs has already been deleted by that test's own teardown -- or,
// worse, GetBlocksDir() falls back to the real default datadir and this suite
// writes into it (2.1.1 review finding #7 -- confirmed: an earlier run of
// this exact file, before this fixture existed, wrote real bdy*.dat files
// into a live node's own blocks/ directory).
//
// 2.1.2 also needs a real pblocktree -- an in-memory CBlockTreeDB, the same
// one TestingSetup constructs for its own much heavier chain-init, taken here
// on its own rather than pulling in all of TestingSetup.
struct BodyStoreTestingSetup : public BasicTestingSetup {
    BodyStoreTestingSetup() {
        SetDataDir("tempdir");
        ClearDatadirCache();
        pblocktree.reset(new CBlockTreeDB(1 << 20, /*fMemory=*/true));
    }
};

BOOST_FIXTURE_TEST_SUITE(bodystore_tests, BodyStoreTestingSetup)

// 2.1.1 review finding #6: every transaction the same length lets a
// write-side mutant (e.g. an offset table computed from the wrong body's
// size) pass every test, since any plausible wrong table equals the right
// one when all bodies are equal-sized. Padding scriptPubKey by `nonce` bytes
// gives every body a distinct, verifiable length.
static CTransactionRef MakeBodyTx(uint32_t nonce) {
    CMutableTransaction tx;
    tx.nVersion = 1;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint(uint256S(strprintf("%064x", nonce)), 0);
    tx.vout.resize(1);
    tx.vout[0].nValue = 1000 + nonce;
    tx.vout[0].scriptPubKey = CScript() << OP_TRUE << std::vector<unsigned char>(nonce, 0xAB);
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

// Writes `bodies` using GetBodyRecordSerializedSize for nAddSize -- the
// contract WriteBodyRecord's own doc comment requires -- rather than each
// test recomputing (and risking disagreeing with) the size by hand.
static FlatFilePos WriteBodies(const std::vector<CTransactionRef> &bodies) {
    FlatFilePos pos;
    BOOST_REQUIRE(FindBodyPos(pos, (unsigned int) GetBodyRecordSerializedSize(bodies)));
    BOOST_REQUIRE(WriteBodyRecord(pos, bodies));
    return pos;
}

BOOST_AUTO_TEST_CASE(roundtrip_empty_record) {
    FlatFilePos pos = WriteBodies({});

    std::vector<CTransactionRef> bodiesOut;
    BOOST_REQUIRE(ReadBodyRecord(pos, bodiesOut));
    BOOST_CHECK(bodiesOut.empty());

    unsigned int count = 12345;
    BOOST_REQUIRE(ReadBodyRecordCount(pos, count));
    BOOST_CHECK_EQUAL(count, 0U);
}

BOOST_AUTO_TEST_CASE(roundtrip_preserves_order_and_content) {
    std::vector<CTransactionRef> bodies = MakeBodies(5);
    FlatFilePos pos = WriteBodies(bodies);

    std::vector<CTransactionRef> bodiesOut;
    BOOST_REQUIRE(ReadBodyRecord(pos, bodiesOut));
    BOOST_REQUIRE_EQUAL(bodiesOut.size(), bodies.size());
    for (size_t i = 0; i < bodies.size(); i++) {
        BOOST_CHECK_EQUAL(bodiesOut[i]->GetHash().ToString(), bodies[i]->GetHash().ToString());
    }
}

BOOST_AUTO_TEST_CASE(read_body_record_count_matches_without_reading_bodies) {
    FlatFilePos pos = WriteBodies(MakeBodies(7));

    unsigned int count = 0;
    BOOST_REQUIRE(ReadBodyRecordCount(pos, count));
    BOOST_CHECK_EQUAL(count, 7U);
}

// The offset table's whole point (bodystore.h): every index must be directly
// reachable, not just the ones a sequential read happens to visit first.
BOOST_AUTO_TEST_CASE(read_body_at_returns_the_correct_transaction_for_every_index) {
    std::vector<CTransactionRef> bodies = MakeBodies(6);
    FlatFilePos pos = WriteBodies(bodies);

    // Deliberately out of order, including the first and last index, so a
    // mutation that only fixes index 0 (e.g. always reading from the start)
    // cannot pass by accident.
    for (unsigned int index : {3U, 0U, 5U, 1U, 4U, 2U}) {
        CTransactionRef txOut;
        BOOST_REQUIRE(ReadBodyAt(pos, index, txOut));
        BOOST_CHECK_EQUAL(txOut->GetHash().ToString(), bodies[index]->GetHash().ToString());
    }
}

// 2.1.1 review finding #3/#10: proves ReadBodyAt genuinely seeks past earlier
// bodies rather than deserializing (and so depending on) them -- corrupt body
// 0's on-disk bytes after writing, then read body 2 and confirm it is still
// exactly right. A sequential-decode implementation would throw or return
// garbage; a real seek is unaffected.
BOOST_AUTO_TEST_CASE(read_body_at_does_not_depend_on_earlier_bodies_being_valid) {
    std::vector<CTransactionRef> bodies = MakeBodies(3);
    FlatFilePos pos = WriteBodies(bodies);

    unsigned int count = 0;
    BOOST_REQUIRE(ReadBodyRecordCount(pos, count));
    long headerSize = (long) ::GetSizeOfCompactSize(count) + (long) count * 4;
    long dataStart = (long) pos.nPos + headerSize;

    // Stamp garbage over body 0's first bytes -- a sequential-decode
    // implementation of ReadBodyAt would throw or return garbage when asked
    // for body 2; a real seek is unaffected by it.
    FILE *f = OpenBodyFile(pos);
    BOOST_REQUIRE(f != nullptr);
    BOOST_REQUIRE_EQUAL(fseek(f, dataStart, SEEK_SET), 0);
    unsigned char garbage[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    BOOST_REQUIRE_EQUAL(fwrite(garbage, 1, sizeof(garbage), f), sizeof(garbage));
    fclose(f);

    CTransactionRef txOut;
    BOOST_REQUIRE(ReadBodyAt(pos, 2, txOut));
    BOOST_CHECK_EQUAL(txOut->GetHash().ToString(), bodies[2]->GetHash().ToString());
}

BOOST_AUTO_TEST_CASE(read_body_at_rejects_an_out_of_range_index) {
    FlatFilePos pos = WriteBodies(MakeBodies(3));

    CTransactionRef txOut;
    BOOST_CHECK(!ReadBodyAt(pos, 3, txOut));
    BOOST_CHECK(!ReadBodyAt(pos, 100, txOut));
}

// 2.1.1 review finding #5: GetBodyRecordSerializedSize must produce exactly
// what WriteBodyRecord writes, or a caller relying on it to size FindBodyPos
// (every test in this file, and eventually 2.1.4) silently overlaps the next
// record.
BOOST_AUTO_TEST_CASE(serialized_size_matches_what_write_actually_writes) {
    std::vector<CTransactionRef> bodies = MakeBodies(9);
    uint64_t predicted = GetBodyRecordSerializedSize(bodies);

    FlatFilePos pos;
    BOOST_REQUIRE(FindBodyPos(pos, (unsigned int) predicted));
    BOOST_REQUIRE(WriteBodyRecord(pos, bodies));

    FILE *f = OpenBodyFile(pos, true);
    BOOST_REQUIRE(f != nullptr);
    BOOST_REQUIRE_EQUAL(fseek(f, 0, SEEK_END), 0);
    long actualFileSize = ftell(f);
    fclose(f);

    BOOST_CHECK_EQUAL((uint64_t) actualFileSize - (uint64_t) pos.nPos, predicted);
}

// Two records back to back must not collide -- the self-delimiting property
// every design doc / commit message claims and nothing previously exercised
// (2.1.1 review finding #5).
BOOST_AUTO_TEST_CASE(adjacent_records_do_not_overlap) {
    std::vector<CTransactionRef> bodiesA = MakeBodies(4);
    std::vector<CTransactionRef> bodiesB = MakeBodies(6);

    FlatFilePos posA = WriteBodies(bodiesA);
    FlatFilePos posB = WriteBodies(bodiesB);
    BOOST_REQUIRE_EQUAL(posA.nFile, posB.nFile);
    BOOST_REQUIRE_EQUAL(posB.nPos, posA.nPos + GetBodyRecordSerializedSize(bodiesA));

    std::vector<CTransactionRef> outA, outB;
    BOOST_REQUIRE(ReadBodyRecord(posA, outA));
    BOOST_REQUIRE(ReadBodyRecord(posB, outB));
    BOOST_REQUIRE_EQUAL(outA.size(), bodiesA.size());
    BOOST_REQUIRE_EQUAL(outB.size(), bodiesB.size());
    for (size_t i = 0; i < bodiesA.size(); i++)
        BOOST_CHECK_EQUAL(outA[i]->GetHash().ToString(), bodiesA[i]->GetHash().ToString());
    for (size_t i = 0; i < bodiesB.size(); i++)
        BOOST_CHECK_EQUAL(outB[i]->GetHash().ToString(), bodiesB[i]->GetHash().ToString());
}

// 2.1.1 review finding #1: offsets used to be CompactSize, which
// ReadCompactSize refuses above 32 MiB (serialize.h's MAX_SIZE) -- well under
// COMMITMENT_BUDGET_BODY_BYTES (110 MB), so every record over 32 MiB of
// bodies was unreadable. One small transaction plus one large one crosses
// that boundary while keeping the test fast.
static CTransactionRef MakeMediumTx(uint32_t nonce, size_t scriptBytes) {
    CMutableTransaction tx;
    tx.nVersion = 1;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint(uint256S(strprintf("%064x", nonce)), 0);
    tx.vout.resize(1);
    tx.vout[0].nValue = 5000 + nonce;
    // A single vector element must itself stay under serialize.h's MAX_SIZE
    // (32 MiB) -- CScript's own length prefix is also a CompactSize, an
    // orthogonal, pre-existing limit this test must respect, not the one
    // finding #1 is about. Many medium transactions whose CUMULATIVE record
    // offset crosses 32 MiB is what actually exercises the fix.
    tx.vout[0].scriptPubKey = CScript() << OP_RETURN << std::vector<unsigned char>(scriptBytes, 0xCD);
    return MakeTransactionRef(std::move(tx));
}

BOOST_AUTO_TEST_CASE(roundtrip_a_record_over_the_compactsize_limit) {
    // 40 x ~900 KB =~ 36 MB total, comfortably over 32 MiB (33,554,432).
    std::vector<CTransactionRef> bodies;
    for (uint32_t i = 1; i <= 40; i++) {
        bodies.push_back(MakeMediumTx(i, 900000));
    }
    BOOST_REQUIRE_GT(GetBodyRecordSerializedSize(bodies), (uint64_t) 33554432);

    FlatFilePos pos = WriteBodies(bodies);

    CTransactionRef first, last;
    BOOST_REQUIRE(ReadBodyAt(pos, 0, first));
    BOOST_CHECK_EQUAL(first->GetHash().ToString(), bodies.front()->GetHash().ToString());
    BOOST_REQUIRE(ReadBodyAt(pos, (unsigned int) bodies.size() - 1, last));
    BOOST_CHECK_EQUAL(last->GetHash().ToString(), bodies.back()->GetHash().ToString());

    std::vector<CTransactionRef> bodiesOut;
    BOOST_REQUIRE(ReadBodyRecord(pos, bodiesOut));
    BOOST_REQUIRE_EQUAL(bodiesOut.size(), bodies.size());
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
// a write that would reach MAX_BODYFILE_SIZE starts a fresh file at position 0
// rather than splitting a record across two files. Pinned exactly (not just
// "some later file, position 0") so a `>` mutant of the `>=` rollover
// condition cannot survive (2.1.1 review finding #8).
BOOST_AUTO_TEST_CASE(find_body_pos_rolls_over_to_a_new_file_once_the_current_one_would_reach_the_limit) {
    // FindBodyPos's bookkeeping is process-global (bodystore.cpp's anonymous
    // namespace), shared across every test case in this suite in file order --
    // `a.nPos == 0` below holds because `MAX_BODYFILE_SIZE - 1` always either
    // rolls onto a fresh file (nPos 0) or was already sitting at nPos 0 of the
    // current one; it is not a fresh-process assumption.
    FlatFilePos anchor;
    BOOST_REQUIRE(FindBodyPos(anchor, MAX_BODYFILE_SIZE - 1));
    BOOST_REQUIRE_EQUAL(anchor.nPos, 0U);

    FlatFilePos a;
    BOOST_REQUIRE(FindBodyPos(a, MAX_BODYFILE_SIZE - 100));
    BOOST_REQUIRE_EQUAL(a.nFile, anchor.nFile + 1);
    BOOST_REQUIRE_EQUAL(a.nPos, 0U);

    FlatFilePos b;
    BOOST_REQUIRE(FindBodyPos(b, 99));
    BOOST_CHECK_EQUAL(b.nFile, a.nFile);
    BOOST_CHECK_EQUAL(b.nPos, MAX_BODYFILE_SIZE - 100);

    FlatFilePos c;
    BOOST_REQUIRE(FindBodyPos(c, 1));
    BOOST_CHECK_EQUAL(c.nFile, a.nFile + 1);
    BOOST_CHECK_EQUAL(c.nPos, 0U);

    // 2.1.1 review finding #4: the file just left behind (a.nFile) must be
    // truncated to its actual used size, not left at its full chunk-sized
    // preallocation forever -- FindBlockPos's own FlushBlockFile(finalize=true)
    // does this for blk*.dat, and the leaked bdy*.dat files this same review
    // found in a real datadir (F-126) were exactly-chunk-sized evidence this
    // step was missing.
    FlatFilePos endOfA(a.nFile, 0);
    FILE *f = OpenBodyFile(endOfA, true);
    BOOST_REQUIRE(f != nullptr);
    BOOST_REQUIRE_EQUAL(fseek(f, 0, SEEK_END), 0);
    long finishedFileSize = ftell(f);
    fclose(f);
    BOOST_CHECK_EQUAL((uint64_t) finishedFileSize, (uint64_t) (MAX_BODYFILE_SIZE - 100 + 99));
}

// 2.1.1 review finding #11: a corrupt or adversarial count must not drive an
// oversized allocation before a single real offset is read.
BOOST_AUTO_TEST_CASE(read_body_record_header_rejects_an_implausible_count) {
    FlatFilePos pos;
    BOOST_REQUIRE(FindBodyPos(pos, 1));
    {
        FILE *f = OpenBodyFile(pos);
        BOOST_REQUIRE(f != nullptr);
        BOOST_REQUIRE_EQUAL(fseek(f, pos.nPos, SEEK_SET), 0);
        CAutoFile fileout(f, SER_DISK, CLIENT_VERSION);
        // One more than COMMITMENT_BUDGET_MAX_INPUTS -- an implausible count no
        // legal record could ever have (F-115's own reasoning: every non-coinbase
        // tx needs >=1 input, so tx count can never exceed the input-count budget).
        WriteCompactSize(fileout, (uint64_t) COMMITMENT_BUDGET_MAX_INPUTS + 1);
    }

    unsigned int count = 0;
    BOOST_CHECK(!ReadBodyRecordCount(pos, count));
    CTransactionRef txOut;
    BOOST_CHECK(!ReadBodyAt(pos, 0, txOut));
}

// 2.1.1 review finding #2: an nAddSize that could never fit in one file must
// fail loudly rather than looping forever trying to roll over.
BOOST_AUTO_TEST_CASE(find_body_pos_rejects_a_write_that_could_never_fit_in_one_file) {
    FlatFilePos pos;
    BOOST_CHECK(!FindBodyPos(pos, MAX_BODYFILE_SIZE));
    BOOST_CHECK(!FindBodyPos(pos, MAX_BODYFILE_SIZE + 1));
}

// 2.1.2: LoadBodyFileInfo must not crash or misbehave on a database that has
// never had anything flushed to it -- a fresh datadir's first boot.
BOOST_AUTO_TEST_CASE(load_body_file_info_is_safe_on_an_empty_database) {
    TestOnlyResetBodyFileState();
    BOOST_REQUIRE(LoadBodyFileInfo());

    FlatFilePos pos;
    BOOST_REQUIRE(FindBodyPos(pos, 100));
    BOOST_CHECK_EQUAL(pos.nFile, 0);
    BOOST_CHECK_EQUAL(pos.nPos, 0U);
}

// The point of 2.1.2: FindBodyPos must continue from where a previous run
// left off, not silently restart at file 0 / position 0 and overwrite real
// data already there.
BOOST_AUTO_TEST_CASE(find_body_pos_continues_after_a_simulated_restart) {
    TestOnlyResetBodyFileState();
    BOOST_REQUIRE(LoadBodyFileInfo());

    std::vector<CTransactionRef> bodies = MakeBodies(4);
    FlatFilePos posBefore = WriteBodies(bodies);
    BOOST_REQUIRE(FlushBodyFileInfo());

    // Simulate a restart: the in-memory bookkeeping is gone, only what was
    // flushed to pblocktree survives.
    TestOnlyResetBodyFileState();
    BOOST_REQUIRE(LoadBodyFileInfo());

    FlatFilePos posAfter;
    BOOST_REQUIRE(FindBodyPos(posAfter, 1));
    BOOST_CHECK_EQUAL(posAfter.nFile, posBefore.nFile);
    BOOST_CHECK_EQUAL(posAfter.nPos, posBefore.nPos + GetBodyRecordSerializedSize(bodies));

    // And the record written before the "restart" is still there and correct
    // -- the reset/reload only affects in-memory bookkeeping, never the bytes
    // already on disk.
    std::vector<CTransactionRef> bodiesOut;
    BOOST_REQUIRE(ReadBodyRecord(posBefore, bodiesOut));
    BOOST_REQUIRE_EQUAL(bodiesOut.size(), bodies.size());
    for (size_t i = 0; i < bodies.size(); i++) {
        BOOST_CHECK_EQUAL(bodiesOut[i]->GetHash().ToString(), bodies[i]->GetHash().ToString());
    }
}

// FlushBodyFileInfo with nothing dirty must be a harmless no-op, not an error.
BOOST_AUTO_TEST_CASE(flush_body_file_info_is_a_no_op_when_nothing_is_dirty) {
    TestOnlyResetBodyFileState();
    BOOST_REQUIRE(LoadBodyFileInfo());
    BOOST_REQUIRE(FlushBodyFileInfo());
}

BOOST_AUTO_TEST_SUITE_END()
