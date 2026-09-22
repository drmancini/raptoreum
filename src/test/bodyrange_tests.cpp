// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// 2.2.2 (F-143's accepted wire-format spec): the fetch protocol's request/
// response payload structs, the index-space helper, and the request-
// validation classification -- the wire contract only, no ProcessMessage
// dispatch and no serving (both 2.2.3). See bodyrange.h for the design.

#include <bodyrange.h>
#include <bodystore.h>
#include <chain.h>
#include <clientversion.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <streams.h>
#include <test/test_raptoreum.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

// F-147 (Fable review of F-146, HIGH): this suite's own validate_* tests
// call FindBodyPos/WriteBodyRecord, real file I/O -- at the time this was
// found, BasicTestingSetup alone never pointed -datadir at its own temp
// root or cleared GetBlocksDir()'s path cache, so those calls fell through
// to the REAL default datadir. This was not hypothetical: confirmed
// directly -- the original version of this file wrote a real 16 MiB
// bdy00000.dat into ~/.raptoreumcore/blocks/, the live datadir of an
// actually-running raptoreumd process, and left stale data there across
// runs that then made one of this file's own mutation-testing claims
// falsely pass (see F-147's own docs/findings.md entry). Exactly F-127's
// own precedent (bodystore_tests.cpp's BodyStoreTestingSetup) for exactly
// the same mistake, in a second file -- fixed here first with a one-off
// fixture, then (a second Fable review of this same fix, still F-147)
// fixed at the root in BasicTestingSetup itself (test_raptoreum.cpp) so a
// third file can't reintroduce it. That makes the one-off fixture this
// file used to define here redundant; plain BasicTestingSetup is enough.
BOOST_FIXTURE_TEST_SUITE(bodyrange_tests, BasicTestingSetup)

// F-127's own precedent (bodystore_tests.cpp): every transaction the same
// length lets a wrong-index bug pass by accident, since any plausible wrong
// answer equals the right one when all bodies are equal-sized.
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

BOOST_AUTO_TEST_CASE(cgetbodyrange_round_trips) {
    CGetBodyRange req;
    req.hashBlock = uint256S("0xaa");
    req.nStartIndex = 3;
    req.nCount = 7;

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << req;
    CGetBodyRange reqOut;
    ss >> reqOut;

    BOOST_CHECK(reqOut.hashBlock == req.hashBlock);
    BOOST_CHECK_EQUAL(reqOut.nStartIndex, req.nStartIndex);
    BOOST_CHECK_EQUAL(reqOut.nCount, req.nCount);
}

BOOST_AUTO_TEST_CASE(cbodyrange_round_trips_empty_vbodies) {
    CBodyRange resp;
    resp.hashBlock = uint256S("0xbb");
    resp.nStartIndex = 5;

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << resp;
    CBodyRange respOut;
    ss >> respOut;

    BOOST_CHECK(respOut.hashBlock == resp.hashBlock);
    BOOST_CHECK_EQUAL(respOut.nStartIndex, resp.nStartIndex);
    BOOST_CHECK(respOut.vBodies.empty());
}

BOOST_AUTO_TEST_CASE(cbodyrange_round_trips_preserving_order_and_content) {
    CBodyRange resp;
    resp.hashBlock = uint256S("0xcc");
    resp.nStartIndex = 4;
    resp.vBodies = {MakeBodyTx(1), MakeBodyTx(2), MakeBodyTx(3)};

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << resp;
    CBodyRange respOut;
    ss >> respOut;

    // F-147 (Fable review of F-146, LOW): the original version of this test
    // never checked hashBlock/nStartIndex here -- only the empty-vBodies
    // test did.
    BOOST_CHECK(respOut.hashBlock == resp.hashBlock);
    BOOST_CHECK_EQUAL(respOut.nStartIndex, resp.nStartIndex);
    BOOST_REQUIRE_EQUAL(respOut.vBodies.size(), resp.vBodies.size());
    for (size_t i = 0; i < resp.vBodies.size(); i++) {
        BOOST_CHECK(respOut.vBodies[i]->GetHash() == resp.vBodies[i]->GetHash());
    }
}

// F-147 (a second Fable review of F-147's own fix, LOW): the comment the
// previous version of this file carried on the test above -- that its new
// hashBlock/nStartIndex assertions would catch "a SERIALIZE_METHODS
// field-order swap between hashBlock and vBodies" -- is wrong.
// SERIALIZE_METHODS generates ONE ordered list that both the writer and the
// reader walk identically, so reordering it changes what byte range each
// field's bytes land in on BOTH sides at once; a round-trip test can never
// observe a pure reorder, since serializing then deserializing with the
// reordered code still recovers each field's own value correctly. Verified
// directly: swapping CBodyRange's field order to
// `vBodies, nStartIndex, hashBlock` and rebuilding still passes every
// round-trip test in this file. What a round-trip test genuinely catches is
// a field being DROPPED or a TYPE changing in an incompatible way (fewer/
// more bytes consumed) -- still worth having, just not for the reason
// previously claimed.
//
// The only test shape that actually pins the wire format is a golden byte
// string: this changing means the wire format changed, whether or not a
// round-trip still happens to recover the same C++ values. `CGetBodyRange`
// is used here since a fixed CTransactionRef test-fixture makes CBodyRange's
// own transaction-compression encoding a second moving part not worth
// coupling this test to; CBodyRange's message TYPE tag/dispatch is covered
// separately (protocol.h/.cpp's own message-type tests), not its full wire
// encoding.
BOOST_AUTO_TEST_CASE(cgetbodyrange_wire_format_is_pinned) {
    CGetBodyRange req;
    req.hashBlock = uint256S("0x1122334455667788");
    req.nStartIndex = 0x03020100;
    req.nCount = 0x07060504;

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << req;

    BOOST_CHECK_EQUAL(HexStr(ss),
        "88776655443322110000000000000000000000000000000000000000000000000001020304050607");
}

BOOST_AUTO_TEST_CASE(vtx_index_from_body_index_is_plus_one) {
    BOOST_CHECK_EQUAL(VtxIndexFromBodyIndex(0), 1U);
    BOOST_CHECK_EQUAL(VtxIndexFromBodyIndex(5), 6U);
}

BOOST_AUTO_TEST_CASE(body_index_from_vtx_index_is_minus_one) {
    BOOST_CHECK_EQUAL(BodyIndexFromVtxIndex(1), 0U);
    BOOST_CHECK_EQUAL(BodyIndexFromVtxIndex(6), 5U);
}

BOOST_AUTO_TEST_CASE(body_index_and_vtx_index_are_inverses) {
    for (uint32_t bodyIndex = 0; bodyIndex < 10; bodyIndex++) {
        BOOST_CHECK_EQUAL(BodyIndexFromVtxIndex(VtxIndexFromBodyIndex(bodyIndex)), bodyIndex);
    }
}

// F-143's own spec: BAN cases decided first, no cs_main, no I/O.
BOOST_AUTO_TEST_CASE(validate_bans_zero_count) {
    ResetBodyIndex();
    CGetBodyRange req;
    req.hashBlock = uint256S("0xdd");
    req.nStartIndex = 0;
    req.nCount = 0;

    FlatFilePos posOut;
    BOOST_CHECK(ValidateGetBodyRange(req, posOut) == GetBodyRangeValidation::BAN);
}

BOOST_AUTO_TEST_CASE(validate_bans_start_plus_count_overflow) {
    ResetBodyIndex();
    CGetBodyRange req;
    req.hashBlock = uint256S("0xee");
    req.nStartIndex = std::numeric_limits<uint32_t>::max() - 2;
    req.nCount = 5;

    FlatFilePos posOut;
    BOOST_CHECK(ValidateGetBodyRange(req, posOut) == GetBodyRangeValidation::BAN);
}

// F-147 (a second Fable review of F-147's own fix, LOW): the test above only
// ever tries a request that genuinely overflows -- nothing pinned the exact
// boundary, so the off-by-one mutant `nStartIndex > max - nCount` -> `>=`
// (which would BAN this legal, non-overflowing request too) survived every
// test in this file. `nStartIndex + nCount == UINT32_MAX` doesn't overflow
// uint32_t arithmetic and must not BAN; the hash is deliberately unknown, so
// a correct implementation reaches MISS (not OK, which would need a real
// serveable record) -- BAN is only possible here via the overflow check
// itself misfiring.
BOOST_AUTO_TEST_CASE(validate_does_not_ban_the_exact_non_overflowing_boundary) {
    ResetBodyIndex();
    CGetBodyRange req;
    req.hashBlock = uint256S("0xef");
    req.nCount = 5;
    req.nStartIndex = std::numeric_limits<uint32_t>::max() - req.nCount;

    FlatFilePos posOut;
    BOOST_CHECK(ValidateGetBodyRange(req, posOut) == GetBodyRangeValidation::MISS);
}

BOOST_AUTO_TEST_CASE(validate_misses_an_unknown_hash) {
    ResetBodyIndex();
    CGetBodyRange req;
    req.hashBlock = uint256S("0xff");
    req.nStartIndex = 0;
    req.nCount = 1;

    FlatFilePos posOut;
    BOOST_CHECK(ValidateGetBodyRange(req, posOut) == GetBodyRangeValidation::MISS);
    // F-147: must not leak a position for a hash that was never recorded.
    BOOST_CHECK(posOut.IsNull());
}

// F-147 (Fable review of F-146, HIGH): the original version of this test
// backed `posOut` with a HAND-BUILT FlatFilePos(0, 100) -- no real record
// ever written there. On a clean datadir, `ReadBodyRecordCount` opening
// that position fails for the SAME reason an unrecorded hash does (no
// readable bytes), so a mutant that drops the serveability check entirely
// (leaking straight through to `ValidateGetBodyRange`'s later tiers)
// coincidentally still landed on MISS -- via `ReadBodyRecordCount`'s own
// failure path, not via the serveability check this test exists to prove.
// Backing the withheld entry with a REAL, readable record is what makes
// this test actually exercise "found, but withheld" rather than
// "indistinguishable from unreadable garbage".
BOOST_AUTO_TEST_CASE(validate_misses_a_withheld_hash) {
    ResetBodyIndex();
    TestOnlyResetBodyFileState();

    std::vector<CTransactionRef> bodies = {MakeBodyTx(1)};
    FlatFilePos pos;
    BOOST_REQUIRE(FindBodyPos(pos, (unsigned int) GetBodyRecordSerializedSize(bodies)));
    BOOST_REQUIRE(WriteBodyRecord(pos, bodies));
    uint256 hash = uint256S("0x101");
    RecordBodyPositionByHash(hash, pos, /*fServeable=*/false);

    CGetBodyRange req;
    req.hashBlock = hash;
    req.nStartIndex = 0;
    req.nCount = 1;

    FlatFilePos posOut;
    BOOST_CHECK(ValidateGetBodyRange(req, posOut) == GetBodyRangeValidation::MISS);
    // F-147 (MEDIUM, bodystore.cpp): the withheld block's own REAL position
    // must never leak out of a call classified as a miss.
    BOOST_CHECK(posOut.IsNull());
}

// F-147 (Fable review of F-146, HIGH -- the corrupt-record tier the
// original mutation pass never tried): a serveable hash whose on-disk
// record cannot actually be read (corrupt count byte) must classify as a
// MISS, never a BAN -- the requester cannot tell "corrupt" from "server-side
// problem" and did nothing wrong by asking.
BOOST_AUTO_TEST_CASE(validate_misses_a_serveable_hash_with_a_corrupt_record) {
    ResetBodyIndex();
    TestOnlyResetBodyFileState();

    FlatFilePos pos;
    BOOST_REQUIRE(FindBodyPos(pos, 8));
    {
        // An implausible CompactSize count -- ReadBodyRecordCount's own
        // implausible-count guard (bodystore.cpp) must reject this, exactly
        // the F-127 precedent this record-corruption shape already covers
        // for ReadBodyRecordCount itself.
        CAutoFile fileout(OpenBodyFile(pos), SER_DISK, CLIENT_VERSION);
        BOOST_REQUIRE(!fileout.IsNull());
        WriteCompactSize(fileout, (uint64_t) COMMITMENT_BUDGET_MAX_INPUTS + 1);
    }
    uint256 hash = uint256S("0x104");
    RecordBodyPositionByHash(hash, pos, /*fServeable=*/true);

    CGetBodyRange req;
    req.hashBlock = hash;
    req.nStartIndex = 0;
    req.nCount = 1;

    FlatFilePos posOut;
    BOOST_CHECK(ValidateGetBodyRange(req, posOut) == GetBodyRangeValidation::MISS);
}

// F-147 (Fable review of F-146, LOW): a coinbase-only block's body record is
// empty (count == 0) -- every range request against it is a BAN by
// construction (nStartIndex >= 0 always holds), since an honest requester
// never has a reason to ask for a range of a block with no non-coinbase
// transactions at all. Not a gap; pinned explicitly so the reasoning isn't
// left implicit.
BOOST_AUTO_TEST_CASE(validate_bans_any_request_on_a_coinbase_only_record) {
    ResetBodyIndex();
    TestOnlyResetBodyFileState();

    FlatFilePos pos;
    BOOST_REQUIRE(FindBodyPos(pos, (unsigned int) GetBodyRecordSerializedSize({})));
    BOOST_REQUIRE(WriteBodyRecord(pos, {}));
    uint256 hash = uint256S("0x105");
    RecordBodyPositionByHash(hash, pos, /*fServeable=*/true);

    CGetBodyRange req;
    req.hashBlock = hash;
    req.nStartIndex = 0;
    req.nCount = 1;

    FlatFilePos posOut;
    BOOST_CHECK(ValidateGetBodyRange(req, posOut) == GetBodyRangeValidation::BAN);
}

BOOST_AUTO_TEST_CASE(validate_bans_an_out_of_range_start_on_a_serveable_block) {
    ResetBodyIndex();
    TestOnlyResetBodyFileState();
    std::vector<CTransactionRef> bodies = {MakeBodyTx(1), MakeBodyTx(2)};
    FlatFilePos pos;
    BOOST_REQUIRE(FindBodyPos(pos, (unsigned int) GetBodyRecordSerializedSize(bodies)));
    BOOST_REQUIRE(WriteBodyRecord(pos, bodies));
    uint256 hash = uint256S("0x102");
    RecordBodyPositionByHash(hash, pos, /*fServeable=*/true);

    CGetBodyRange req;
    req.hashBlock = hash;
    req.nStartIndex = 2;  // bodies.size() == 2, so index 2 is past the end
    req.nCount = 1;

    FlatFilePos posOut;
    BOOST_CHECK(ValidateGetBodyRange(req, posOut) == GetBodyRangeValidation::BAN);
}

BOOST_AUTO_TEST_CASE(validate_ok_for_a_real_in_range_serveable_request) {
    ResetBodyIndex();
    TestOnlyResetBodyFileState();
    std::vector<CTransactionRef> bodies = {MakeBodyTx(1), MakeBodyTx(2), MakeBodyTx(3)};
    FlatFilePos pos;
    BOOST_REQUIRE(FindBodyPos(pos, (unsigned int) GetBodyRecordSerializedSize(bodies)));
    BOOST_REQUIRE(WriteBodyRecord(pos, bodies));
    uint256 hash = uint256S("0x103");
    RecordBodyPositionByHash(hash, pos, /*fServeable=*/true);

    CGetBodyRange req;
    req.hashBlock = hash;
    req.nStartIndex = 1;
    req.nCount = 2;

    FlatFilePos posOut;
    BOOST_REQUIRE(ValidateGetBodyRange(req, posOut) == GetBodyRangeValidation::OK);
    BOOST_CHECK_EQUAL(posOut.nFile, pos.nFile);
    BOOST_CHECK_EQUAL(posOut.nPos, pos.nPos);
}

BOOST_AUTO_TEST_SUITE_END()
