// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// 2.1.1-2.1.3: the body-store record format and file series (2.1.1), its
// persistence via pblocktree (2.1.2), and CBlockIndex's own body-record
// position fields (2.1.3) -- nothing here touches AcceptBlock yet (that's
// 2.1.4). See bodystore.h for the design.

#include <bodystore.h>
#include <chain.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <streams.h>
#include <test/test_raptoreum.h>
#include <tinyformat.h>
#include <txdb.h>
#include <uint256.h>
#include <util/strencodings.h>
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
// writes into it (F-127 -- confirmed: an earlier run of
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

    // Symmetry with TestingSetup::~TestingSetup (test_raptoreum.cpp), which
    // also resets pblocktree on teardown -- without this, this fixture's own
    // in-memory CBlockTreeDB outlives it until some other fixture replaces it.
    ~BodyStoreTestingSetup() { pblocktree.reset(); }
};

BOOST_FIXTURE_TEST_SUITE(bodystore_tests, BodyStoreTestingSetup)

// F-127: every transaction the same length lets a
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

// F-127: proves ReadBodyAt genuinely seeks past earlier
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

// F-127: GetBodyRecordSerializedSize must produce exactly
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
// (F-127).
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

// F-127: offsets used to be CompactSize, which
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
// condition cannot survive (F-127).
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

    // F-127: the file just left behind (a.nFile) must be
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

// F-127: a corrupt or adversarial count must not drive an
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

// F-127: an nAddSize that could never fit in one file must
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
// 2.1.2/2.1.3 review: the original version of this test never left file 0, so
// it couldn't tell a correctly-persisted nLastBodyFile from one hardcoded to
// 0 -- multiple real mutants of LoadBodyFileInfo/FlushBodyFileInfo (dropping
// the ReadLastBodyFile call, persisting a constant instead of the real last
// file, or only reloading the LAST file's own size) all still passed it. This
// version forces a genuine rollover to file 1 first -- the actual steady
// state of a real node, once the first ~128 MiB fills -- so "continues from
// the persisted position" is only true if every piece of 2.1.2's persistence
// actually worked, not merely if file 0 happens to still be current.
BOOST_AUTO_TEST_CASE(find_body_pos_continues_after_a_simulated_restart) {
    TestOnlyResetBodyFileState();
    BOOST_REQUIRE(LoadBodyFileInfo());

    // Fill file 0 to 1 byte short of the limit, then write a real record that
    // can only fit by rolling into a fresh file 1.
    FlatFilePos fillPos;
    BOOST_REQUIRE(FindBodyPos(fillPos, MAX_BODYFILE_SIZE - 1));
    BOOST_REQUIRE_EQUAL(fillPos.nFile, 0);

    std::vector<CTransactionRef> bodies = MakeBodies(4);
    uint64_t recordSize = GetBodyRecordSerializedSize(bodies);
    FlatFilePos posBefore;
    BOOST_REQUIRE(FindBodyPos(posBefore, (unsigned int) recordSize));
    BOOST_REQUIRE_EQUAL(posBefore.nFile, 1);
    BOOST_REQUIRE_EQUAL(posBefore.nPos, 0U);
    BOOST_REQUIRE(WriteBodyRecord(posBefore, bodies));
    // F-132: FlushBodyFileInfo's separate batch is gone -- this is
    // FlushStateToDisk's own real sequence now, gather then fold into
    // WriteBatchSync's atomic batch.
    std::vector<std::pair<int, CBodyFileInfo>> vFilesToFlush;
    int nLastFileToFlush = -1;
    GetDirtyBodyFileInfo(vFilesToFlush, nLastFileToFlush);
    BOOST_REQUIRE(pblocktree->WriteBatchSync({}, 0, {}, vFilesToFlush, nLastFileToFlush));

    // Simulate a restart: the in-memory bookkeeping is gone, only what was
    // flushed to pblocktree survives.
    TestOnlyResetBodyFileState();
    BOOST_REQUIRE(LoadBodyFileInfo());

    // File 0's own size must have survived too, not just the last file's --
    // LoadBodyFileInfo reads every file 0..nLastBodyFile, and this is the
    // only way that's observable (FindBodyPos itself never looks at a file
    // it isn't currently writing into).
    BOOST_CHECK_EQUAL(TestOnlyGetBodyFileSize(0), MAX_BODYFILE_SIZE - 1);
    BOOST_CHECK_EQUAL(TestOnlyGetBodyFileSize(1), (unsigned int) recordSize);

    FlatFilePos posAfter;
    BOOST_REQUIRE(FindBodyPos(posAfter, 1));
    BOOST_CHECK_EQUAL(posAfter.nFile, posBefore.nFile);
    BOOST_CHECK_EQUAL(posAfter.nPos, posBefore.nPos + recordSize);

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

// F-133: FlushBodyFile must be a harmless no-op before LoadBodyFileInfo has
// ever run (vinfoBodyFile still empty) -- indexing vinfoBodyFile[nLastBodyFile]
// unconditionally here would be undefined behaviour on a fresh in-memory
// state, not a caught error, since operator[] does not bounds-check.
BOOST_AUTO_TEST_CASE(flush_body_file_is_a_no_op_before_anything_is_loaded) {
    TestOnlyResetBodyFileState();
    BOOST_CHECK(FlushBodyFile());
}

// 2.1.4: FlushBodyFile is FindBodyPos's analogue of FlushBlockFile -- proves
// it actually flushes the CURRENT file (not the wrong one, and not a no-op
// once something real exists to flush). F-135 (2.1.4 review): the first
// version of this test never left file 0, so a hardcoded FlatFilePos(0, ...)
// inside FlushBodyFile would have survived it -- rolled to file 1 first,
// matching find_body_pos_continues_after_a_simulated_restart's own reasoning,
// so a wrong-file mutant fails to open/flush the right file instead of
// silently passing.
BOOST_AUTO_TEST_CASE(flush_body_file_flushes_the_current_file_after_a_real_write) {
    TestOnlyResetBodyFileState();
    BOOST_REQUIRE(LoadBodyFileInfo());

    FlatFilePos fillPos;
    BOOST_REQUIRE(FindBodyPos(fillPos, MAX_BODYFILE_SIZE - 1));
    BOOST_REQUIRE_EQUAL(fillPos.nFile, 0);

    std::vector<CTransactionRef> bodies = MakeBodies(3);
    uint64_t recordSize = GetBodyRecordSerializedSize(bodies);
    FlatFilePos pos;
    BOOST_REQUIRE(FindBodyPos(pos, (unsigned int) recordSize));
    BOOST_REQUIRE_EQUAL(pos.nFile, 1);
    BOOST_REQUIRE(WriteBodyRecord(pos, bodies));

    BOOST_CHECK(FlushBodyFile());

    // The record must still read back correctly after the flush -- FlushBodyFile
    // truncates/fsyncs, it must never touch the bytes it's flushing.
    std::vector<CTransactionRef> bodiesOut;
    BOOST_REQUIRE(ReadBodyRecord(pos, bodiesOut));
    BOOST_REQUIRE_EQUAL(bodiesOut.size(), bodies.size());
    for (size_t i = 0; i < bodies.size(); i++) {
        BOOST_CHECK_EQUAL(bodiesOut[i]->GetHash().ToString(), bodies[i]->GetHash().ToString());
    }
}

// GetDirtyBodyFileInfo with nothing touched since load must report no dirty
// files -- a harmless empty gather, not an error.
BOOST_AUTO_TEST_CASE(get_dirty_body_file_info_is_empty_when_nothing_is_dirty) {
    TestOnlyResetBodyFileState();
    BOOST_REQUIRE(LoadBodyFileInfo());

    std::vector<std::pair<int, CBodyFileInfo>> vFiles;
    int nLastFileOut = -1;
    GetDirtyBodyFileInfo(vFiles, nLastFileOut);
    BOOST_CHECK(vFiles.empty());
}

// GetDirtyBodyFileInfo is what FlushStateToDisk actually calls (validation.cpp)
// -- it must report every file FindBodyPos touched since the last call, the
// current last-body-file unconditionally (dirty or not, matching how
// nLastBlockFile is always passed to WriteBatchSync whether or not any block
// file is dirty), and clear the dirty set so a second call in a row reports
// no files newly dirty.
BOOST_AUTO_TEST_CASE(get_dirty_body_file_info_gathers_and_clears) {
    TestOnlyResetBodyFileState();
    BOOST_REQUIRE(LoadBodyFileInfo());

    FlatFilePos pos;
    BOOST_REQUIRE(FindBodyPos(pos, 500));

    std::vector<std::pair<int, CBodyFileInfo>> vFiles;
    int nLastFileOut = -1;
    GetDirtyBodyFileInfo(vFiles, nLastFileOut);
    BOOST_REQUIRE_EQUAL(vFiles.size(), 1U);
    BOOST_CHECK_EQUAL(vFiles[0].first, 0);
    BOOST_CHECK_EQUAL(vFiles[0].second.nSize, 500U);
    BOOST_CHECK_EQUAL(nLastFileOut, 0);

    // Nothing new touched since the gather above -- the dirty SET must come
    // back empty, though the last-body-file is still reported unconditionally
    // (it isn't itself a "dirty" concept, same as nLastBlockFile).
    std::vector<std::pair<int, CBodyFileInfo>> vFilesAgain;
    int nLastFileOutAgain = -1;
    GetDirtyBodyFileInfo(vFilesAgain, nLastFileOutAgain);
    BOOST_CHECK(vFilesAgain.empty());
    BOOST_CHECK_EQUAL(nLastFileOutAgain, 0);
}

// F-132: body-file info now flows through CBlockTreeDB::WriteBatchSync's own
// atomic batch instead of a separate WriteBodyFileInfoBatch call -- this is
// the write path FlushStateToDisk (validation.cpp) actually uses now.
BOOST_AUTO_TEST_CASE(write_batch_sync_persists_body_file_info) {
    CBodyFileInfo info0;
    info0.nSize = 1234;
    CBodyFileInfo info1;
    info1.nSize = 5678;
    std::vector<std::pair<int, CBodyFileInfo>> vFiles = {
        std::make_pair(0, info0), std::make_pair(1, info1)};

    BOOST_REQUIRE(pblocktree->WriteBatchSync({}, 0, {}, vFiles, /*nLastBodyFile=*/1));

    CBodyFileInfo readBack0, readBack1;
    BOOST_REQUIRE(pblocktree->ReadBodyFileInfo(0, readBack0));
    BOOST_REQUIRE(pblocktree->ReadBodyFileInfo(1, readBack1));
    BOOST_CHECK_EQUAL(readBack0.nSize, info0.nSize);
    BOOST_CHECK_EQUAL(readBack1.nSize, info1.nSize);

    int nLastFileOut = -1;
    BOOST_REQUIRE(pblocktree->ReadLastBodyFile(nLastFileOut));
    BOOST_CHECK_EQUAL(nLastFileOut, 1);
}

// F-133 (review of F-132): every other test's first real flush already left
// file 0 (nLastBodyFile ends up >= 1 by the time WriteBatchSync is called),
// so nothing proved the `nLastBodyFile >= 0` guard is INCLUSIVE at the file-0
// boundary -- a `> 0` mutant passed every prior test silently, which in
// production would mean a real node persists NO body-file info at all for
// its entire first ~128 MiB of bodies (every flush before the first rollover
// would silently skip both writes). This exercises the real
// GetDirtyBodyFileInfo -> WriteBatchSync pipeline while file 0 is still
// current, the way most of a node's actual runtime is spent.
BOOST_AUTO_TEST_CASE(write_batch_sync_persists_body_file_info_while_file_zero_is_current) {
    TestOnlyResetBodyFileState();
    BOOST_REQUIRE(LoadBodyFileInfo());

    FlatFilePos pos;
    BOOST_REQUIRE(FindBodyPos(pos, 250));

    std::vector<std::pair<int, CBodyFileInfo>> vFiles;
    int nLastFileOut = -1;
    GetDirtyBodyFileInfo(vFiles, nLastFileOut);
    BOOST_REQUIRE_EQUAL(nLastFileOut, 0);
    BOOST_REQUIRE(pblocktree->WriteBatchSync({}, 0, {}, vFiles, nLastFileOut));

    int nLastFileRead = -1;
    BOOST_REQUIRE(pblocktree->ReadLastBodyFile(nLastFileRead));
    BOOST_CHECK_EQUAL(nLastFileRead, 0);
    CBodyFileInfo readBack;
    BOOST_REQUIRE(pblocktree->ReadBodyFileInfo(0, readBack));
    BOOST_CHECK_EQUAL(readBack.nSize, 250U);
}

// The one call site F-132 deliberately left untouched (LoadBlockIndexDB's
// pre-1.3 bodiesmigrated migration, validation.cpp) calls WriteBatchSync with
// no body-file arguments at all. The sentinel default (nLastBodyFile = -1)
// must make that a complete no-op for the body-file keys -- not silently
// persist a fabricated last-body-file of 0 that would stomp a real,
// already-persisted value the next time a real flush runs.
//
// F-133 (review of F-132): the original version of this test wrote nothing
// real first, so ReadBodyFileInfo/ReadLastBodyFile failing proved nothing --
// they fail on a fresh database regardless of what the 3-argument call does.
// Seeds a REAL persisted value first so the assertions below only pass if
// the 3-argument call genuinely left it untouched, not merely absent.
BOOST_AUTO_TEST_CASE(write_batch_sync_without_body_args_touches_no_body_file_keys) {
    CBodyFileInfo seeded;
    seeded.nSize = 999;
    BOOST_REQUIRE(pblocktree->WriteBatchSync({}, 0, {}, {std::make_pair(3, seeded)}, 3));

    BOOST_REQUIRE(pblocktree->WriteBatchSync({}, 0, {}));

    int nLastFileOut = -1;
    BOOST_REQUIRE(pblocktree->ReadLastBodyFile(nLastFileOut));
    BOOST_CHECK_EQUAL(nLastFileOut, 3);
    CBodyFileInfo readBack;
    BOOST_REQUIRE(pblocktree->ReadBodyFileInfo(3, readBack));
    BOOST_CHECK_EQUAL(readBack.nSize, 999U);
}

// 2.1.3: CBlockIndex/CDiskBlockIndex's own nBodyFile/nBodyPos + BLOCK_HAVE_BODY_RECORD.
// No migration needed (see the bit's own comment in chain.h) -- these tests exist to
// prove that claim, not just assert it: an entry with the bit unset must round-trip
// identically to one that has never heard of the new fields at all.

BOOST_AUTO_TEST_CASE(get_body_pos_is_null_until_the_bit_is_set) {
    CBlockIndex index;
    BOOST_CHECK(index.GetBodyPos().IsNull());

    index.nBodyFile = 3;
    index.nBodyPos = 12345;
    // Fields alone, bit still unset -- GetBlockPos()/GetUndoPos() use exactly
    // this same convention, so GetBodyPos() must too.
    BOOST_CHECK(index.GetBodyPos().IsNull());

    index.nStatus |= BLOCK_HAVE_BODY_RECORD;
    BOOST_CHECK(!index.GetBodyPos().IsNull());
    BOOST_CHECK_EQUAL(index.GetBodyPos().nFile, 3);
    BOOST_CHECK_EQUAL(index.GetBodyPos().nPos, 12345U);
}

static CBlockIndex MakeBlockIndexForDiskTest() {
    CBlockIndex index;
    index.nHeight = 100;
    index.nStatus = BLOCK_HAVE_DATA | BLOCK_VALID_TRANSACTIONS;
    index.nTx = 5;
    index.nFile = 7;
    index.nDataPos = 999;
    index.nVersion = 4;
    index.hashMerkleRoot = uint256S("0xabc123");
    index.nTime = 1700000000;
    index.nBits = 0x207fffff;
    index.nNonce = 42;
    return index;
}

BOOST_AUTO_TEST_CASE(body_record_position_round_trips_through_disk_index_serialization) {
    CBlockIndex index = MakeBlockIndexForDiskTest();
    index.nStatus |= BLOCK_HAVE_BODY_RECORD;
    index.nBodyFile = 2;
    index.nBodyPos = 54321;

    CDiskBlockIndex diskIndex(&index);
    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << diskIndex;

    CDiskBlockIndex diskIndexOut;
    ss >> diskIndexOut;

    BOOST_CHECK(diskIndexOut.nStatus & BLOCK_HAVE_BODY_RECORD);
    BOOST_CHECK_EQUAL(diskIndexOut.nBodyFile, 2);
    BOOST_CHECK_EQUAL(diskIndexOut.nBodyPos, 54321U);
    // The rest of the entry must survive unaffected -- proves the new fields'
    // bytes didn't shift anything that comes after them.
    BOOST_CHECK_EQUAL(diskIndexOut.nHeight, index.nHeight);
    BOOST_CHECK_EQUAL(diskIndexOut.nTx, index.nTx);
    BOOST_CHECK_EQUAL(diskIndexOut.nFile, index.nFile);
    BOOST_CHECK_EQUAL(diskIndexOut.nDataPos, index.nDataPos);
    BOOST_CHECK_EQUAL(diskIndexOut.nVersion, index.nVersion);
    BOOST_CHECK(diskIndexOut.hashMerkleRoot == index.hashMerkleRoot);
    BOOST_CHECK_EQUAL(diskIndexOut.nTime, index.nTime);
    BOOST_CHECK_EQUAL(diskIndexOut.nBits, index.nBits);
    BOOST_CHECK_EQUAL(diskIndexOut.nNonce, index.nNonce);
}

// The actual "no migration needed" claim: every entry on disk today has this
// bit unset, so it must decode exactly as if the new fields didn't exist --
// zero-byte cost, and no misalignment of whatever comes after them.
BOOST_AUTO_TEST_CASE(body_record_position_is_absent_when_the_bit_is_unset) {
    CBlockIndex indexWithout = MakeBlockIndexForDiskTest();
    // Bit deliberately left unset, even though the fields hold garbage --
    // exactly what an upgraded binary sees for a real historical entry: the
    // in-memory default (0) after SetNull(), never touched because nothing
    // before 2.1.4 ever sets this bit.
    indexWithout.nBodyFile = 999;
    indexWithout.nBodyPos = 999;

    CBlockIndex indexBaseline = MakeBlockIndexForDiskTest();

    CDataStream ssWithout(SER_DISK, CLIENT_VERSION);
    ssWithout << CDiskBlockIndex(&indexWithout);
    CDataStream ssBaseline(SER_DISK, CLIENT_VERSION);
    ssBaseline << CDiskBlockIndex(&indexBaseline);

    // Byte-identical: the garbage in nBodyFile/nBodyPos never reaches the wire
    // when the bit is unset, so two otherwise-identical entries serialize the
    // same regardless of what those fields happen to hold in memory.
    BOOST_CHECK_EQUAL(HexStr(std::vector<unsigned char>(ssWithout.begin(), ssWithout.end())),
                     HexStr(std::vector<unsigned char>(ssBaseline.begin(), ssBaseline.end())));

    CDiskBlockIndex diskIndexOut;
    ssWithout >> diskIndexOut;
    BOOST_CHECK(!(diskIndexOut.nStatus & BLOCK_HAVE_BODY_RECORD));
    BOOST_CHECK(diskIndexOut.GetBodyPos().IsNull());
    BOOST_CHECK_EQUAL(diskIndexOut.nHeight, indexWithout.nHeight);
    BOOST_CHECK_EQUAL(diskIndexOut.nFile, indexWithout.nFile);
    BOOST_CHECK_EQUAL(diskIndexOut.nDataPos, indexWithout.nDataPos);
    BOOST_CHECK(diskIndexOut.hashMerkleRoot == indexWithout.hashMerkleRoot);
}

// 2.2.1 (F-140): the body-store-owned height+hash index, entirely decoupled
// from CBlockIndex/cs_main -- see bodystore.h's file-level comment for the
// design (two asymmetric halves, one dedicated leaf-most lock).

BOOST_AUTO_TEST_CASE(lookup_body_position_by_hash_returns_false_when_never_recorded) {
    ResetBodyIndex();

    FlatFilePos posOut;
    BOOST_CHECK(!LookupBodyPositionByHash(uint256S("0x1"), posOut));
}

BOOST_AUTO_TEST_CASE(record_and_lookup_body_position_by_hash_round_trips) {
    ResetBodyIndex();

    uint256 hash = uint256S("0xaa");
    FlatFilePos pos(3, 12345);
    RecordBodyPositionByHash(hash, pos);

    FlatFilePos posOut;
    BOOST_REQUIRE(LookupBodyPositionByHash(hash, posOut));
    BOOST_CHECK_EQUAL(posOut.nFile, pos.nFile);
    BOOST_CHECK_EQUAL(posOut.nPos, pos.nPos);

    // A different hash never recorded must still miss -- proves the lookup is
    // keyed on the hash, not just "something was recorded".
    FlatFilePos other;
    BOOST_CHECK(!LookupBodyPositionByHash(uint256S("0xbb"), other));
}

// Side-chain blocks are accepted and persisted but never connected at all
// (transaction-decoupling.md SS5) -- the hash index must never require a
// connect to see an entry. This test doesn't exercise validation.cpp's own
// AcceptBlock wiring (that's the integration test in acceptancebit_tests.cpp);
// it proves the index itself imposes no such ordering.
BOOST_AUTO_TEST_CASE(record_body_position_by_hash_needs_no_height_entry_at_all) {
    ResetBodyIndex();

    uint256 hash = uint256S("0xcc");
    FlatFilePos pos(0, 500);
    RecordBodyPositionByHash(hash, pos);

    FlatFilePos posOut;
    uint256 hashOut;
    // This height was never recorded via RecordBodyPositionAtHeight -- the
    // by-hash entry must not have implicitly created one.
    BOOST_CHECK(!LookupBodyPositionAtHeight(0, posOut, hashOut));

    FlatFilePos byHashOut;
    BOOST_REQUIRE(LookupBodyPositionByHash(hash, byHashOut));
    BOOST_CHECK_EQUAL(byHashOut.nPos, 500U);
}

BOOST_AUTO_TEST_CASE(lookup_body_position_at_height_returns_false_when_never_recorded) {
    ResetBodyIndex();

    FlatFilePos posOut;
    uint256 hashOut;
    BOOST_CHECK(!LookupBodyPositionAtHeight(0, posOut, hashOut));
    BOOST_CHECK(!LookupBodyPositionAtHeight(100, posOut, hashOut));
}

BOOST_AUTO_TEST_CASE(record_and_lookup_body_position_at_height_round_trips) {
    ResetBodyIndex();

    uint256 hash = uint256S("0xdd");
    FlatFilePos pos(1, 999);
    RecordBodyPositionAtHeight(42, hash, pos);

    FlatFilePos posOut;
    uint256 hashOut;
    BOOST_REQUIRE(LookupBodyPositionAtHeight(42, posOut, hashOut));
    BOOST_CHECK_EQUAL(posOut.nFile, pos.nFile);
    BOOST_CHECK_EQUAL(posOut.nPos, pos.nPos);
    BOOST_CHECK(hashOut == hash);

    // A neighbouring height never recorded must still miss.
    FlatFilePos other;
    uint256 otherHash;
    BOOST_CHECK(!LookupBodyPositionAtHeight(41, other, otherHash));
    BOOST_CHECK(!LookupBodyPositionAtHeight(43, other, otherHash));
}

// A reorg's own ConnectTip for the winning branch is what corrects a stale
// height entry -- RecordBodyPositionAtHeight must overwrite, not refuse or
// duplicate.
BOOST_AUTO_TEST_CASE(record_body_position_at_height_overwrites_an_existing_entry) {
    ResetBodyIndex();

    uint256 hashA = uint256S("0xa1");
    RecordBodyPositionAtHeight(10, hashA, FlatFilePos(0, 100));

    uint256 hashB = uint256S("0xb2");
    RecordBodyPositionAtHeight(10, hashB, FlatFilePos(0, 200));

    FlatFilePos posOut;
    uint256 hashOut;
    BOOST_REQUIRE(LookupBodyPositionAtHeight(10, posOut, hashOut));
    BOOST_CHECK(hashOut == hashB);
    BOOST_CHECK_EQUAL(posOut.nPos, 200U);
}

// DisconnectTip's own contract (bodystore.h): remove the entry outright, never
// redirect it -- a reader landing in the gap mid-reorg must see a clean miss,
// not the disconnected branch's stale data.
BOOST_AUTO_TEST_CASE(erase_body_position_at_height_removes_the_entry) {
    ResetBodyIndex();

    RecordBodyPositionAtHeight(7, uint256S("0xe1"), FlatFilePos(0, 300));
    FlatFilePos posOut;
    uint256 hashOut;
    BOOST_REQUIRE(LookupBodyPositionAtHeight(7, posOut, hashOut));

    EraseBodyPositionAtHeight(7);
    BOOST_CHECK(!LookupBodyPositionAtHeight(7, posOut, hashOut));
}

// Erasing a height that was never recorded (or already erased) must be a
// harmless no-op, not a crash -- DisconnectTip has no way to know in advance
// whether an entry exists.
BOOST_AUTO_TEST_CASE(erase_body_position_at_height_is_a_no_op_when_nothing_was_recorded) {
    ResetBodyIndex();
    EraseBodyPositionAtHeight(999);

    FlatFilePos posOut;
    uint256 hashOut;
    BOOST_CHECK(!LookupBodyPositionAtHeight(999, posOut, hashOut));
}

// The real startup path (the free UnloadBlockIndex(CTxMemPool*),
// validation.cpp -- F-141: NOT LoadChainTip, which only rebuilds the height
// half) calls this once before repopulating from CBlockIndex -- must
// discard BOTH halves, not just one, or a stale by-hash entry from before a
// reindex would silently keep answering for a block the rebuild never
// re-recorded.
BOOST_AUTO_TEST_CASE(reset_body_index_clears_both_halves) {
    ResetBodyIndex();

    uint256 hash = uint256S("0xf1");
    RecordBodyPositionByHash(hash, FlatFilePos(0, 1));
    RecordBodyPositionAtHeight(5, hash, FlatFilePos(0, 1));

    ResetBodyIndex();

    FlatFilePos posOut;
    uint256 hashOut;
    BOOST_CHECK(!LookupBodyPositionByHash(hash, posOut));
    BOOST_CHECK(!LookupBodyPositionAtHeight(5, posOut, hashOut));
}

BOOST_AUTO_TEST_SUITE_END()
