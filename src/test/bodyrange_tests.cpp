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

#include <chrono>
#include <limits>

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

// 2.2.3 (serving handler): BuildBodyRangeResponse is the pure, cs_main-free
// response-building logic -- given a position ValidateGetBodyRange already
// classified OK, read bodies and assemble the wire response. The actual
// net_processing.cpp ProcessMessage dispatch arm that calls this (deserialize
// -> ValidateGetBodyRange -> Misbehaving on BAN / this function on OK ->
// PushMessage) has no unit coverage, matching this tree's own established
// convention for message-processing glue (SENDCMPCT is the same way) -- but
// every byte of actual chunking/truncation LOGIC lives here instead, where it
// is fully testable without CNode/CConnman scaffolding.
BOOST_AUTO_TEST_CASE(build_response_echoes_hash_and_start_index) {
    ResetBodyIndex();
    TestOnlyResetBodyFileState();
    std::vector<CTransactionRef> bodies = {MakeBodyTx(1)};
    FlatFilePos pos;
    BOOST_REQUIRE(FindBodyPos(pos, (unsigned int) GetBodyRecordSerializedSize(bodies)));
    BOOST_REQUIRE(WriteBodyRecord(pos, bodies));

    CGetBodyRange req;
    req.hashBlock = uint256S("0x201");
    req.nStartIndex = 0;
    req.nCount = 1;

    CBodyRange resp;
    BuildBodyRangeResponse(req, pos, std::numeric_limits<uint64_t>::max(), resp);
    BOOST_CHECK(resp.hashBlock == req.hashBlock);
    BOOST_CHECK_EQUAL(resp.nStartIndex, req.nStartIndex);
}

BOOST_AUTO_TEST_CASE(build_response_respects_ncount_when_the_ceiling_is_no_limit) {
    ResetBodyIndex();
    TestOnlyResetBodyFileState();
    std::vector<CTransactionRef> bodies = {MakeBodyTx(1), MakeBodyTx(2), MakeBodyTx(3), MakeBodyTx(4)};
    FlatFilePos pos;
    BOOST_REQUIRE(FindBodyPos(pos, (unsigned int) GetBodyRecordSerializedSize(bodies)));
    BOOST_REQUIRE(WriteBodyRecord(pos, bodies));

    CGetBodyRange req;
    req.hashBlock = uint256S("0x202");
    req.nStartIndex = 1;
    req.nCount = 2;

    CBodyRange resp;
    BuildBodyRangeResponse(req, pos, std::numeric_limits<uint64_t>::max(), resp);
    BOOST_REQUIRE_EQUAL(resp.vBodies.size(), 2U);
    BOOST_CHECK(resp.vBodies[0]->GetHash() == bodies[1]->GetHash());
    BOOST_CHECK(resp.vBodies[1]->GetHash() == bodies[2]->GetHash());
}

BOOST_AUTO_TEST_CASE(build_response_truncates_when_the_record_runs_out_before_ncount) {
    ResetBodyIndex();
    TestOnlyResetBodyFileState();
    std::vector<CTransactionRef> bodies = {MakeBodyTx(1), MakeBodyTx(2)};
    FlatFilePos pos;
    BOOST_REQUIRE(FindBodyPos(pos, (unsigned int) GetBodyRecordSerializedSize(bodies)));
    BOOST_REQUIRE(WriteBodyRecord(pos, bodies));

    // ValidateGetBodyRange's own OK classification only guarantees
    // nStartIndex < count -- nothing guarantees nStartIndex + nCount <=
    // count, so a caller (correctly) getting OK can still ask for more
    // bodies than the record actually has past nStartIndex.
    CGetBodyRange req;
    req.hashBlock = uint256S("0x203");
    req.nStartIndex = 0;
    req.nCount = 10;

    CBodyRange resp;
    BuildBodyRangeResponse(req, pos, std::numeric_limits<uint64_t>::max(), resp);
    BOOST_REQUIRE_EQUAL(resp.vBodies.size(), 2U);
    BOOST_CHECK(resp.vBodies[0]->GetHash() == bodies[0]->GetHash());
    BOOST_CHECK(resp.vBodies[1]->GetHash() == bodies[1]->GetHash());
}

BOOST_AUTO_TEST_CASE(build_response_always_includes_the_first_body_even_over_a_tiny_ceiling) {
    ResetBodyIndex();
    TestOnlyResetBodyFileState();
    std::vector<CTransactionRef> bodies = {MakeBodyTx(1), MakeBodyTx(2)};
    FlatFilePos pos;
    BOOST_REQUIRE(FindBodyPos(pos, (unsigned int) GetBodyRecordSerializedSize(bodies)));
    BOOST_REQUIRE(WriteBodyRecord(pos, bodies));

    CGetBodyRange req;
    req.hashBlock = uint256S("0x204");
    req.nStartIndex = 0;
    req.nCount = 2;

    CBodyRange resp;
    // A 1-byte ceiling is smaller than any real transaction -- if the loop
    // refused to ever exceed the ceiling, it would return zero bodies for a
    // request that HAS at least one, making no forward progress at all. The
    // contract is: the first body is always included regardless of its own
    // size (see the static_assert in net_processing.cpp for why this can
    // never overflow MAX_PROTOCOL_MESSAGE_LENGTH in the real deployment).
    BuildBodyRangeResponse(req, pos, /*nByteCeiling=*/1, resp);
    BOOST_REQUIRE_EQUAL(resp.vBodies.size(), 1U);
    BOOST_CHECK(resp.vBodies[0]->GetHash() == bodies[0]->GetHash());
}

BOOST_AUTO_TEST_CASE(build_response_stops_at_the_ceiling_once_it_already_has_one_body) {
    ResetBodyIndex();
    TestOnlyResetBodyFileState();
    CTransactionRef tx1 = MakeBodyTx(1);
    std::vector<CTransactionRef> bodies = {tx1, MakeBodyTx(2), MakeBodyTx(3)};
    FlatFilePos pos;
    BOOST_REQUIRE(FindBodyPos(pos, (unsigned int) GetBodyRecordSerializedSize(bodies)));
    BOOST_REQUIRE(WriteBodyRecord(pos, bodies));

    CGetBodyRange req;
    req.hashBlock = uint256S("0x205");
    req.nStartIndex = 0;
    req.nCount = 3;

    // A ceiling that fits exactly the first body and nothing more -- proves
    // the SECOND body is excluded once the running total would exceed the
    // ceiling, not just that the FIRST is force-included.
    uint64_t nOneBodyCeiling = GetSerializeSize(*tx1, SER_NETWORK, PROTOCOL_VERSION);
    CBodyRange resp;
    BuildBodyRangeResponse(req, pos, nOneBodyCeiling, resp);
    BOOST_REQUIRE_EQUAL(resp.vBodies.size(), 1U);
    BOOST_CHECK(resp.vBodies[0]->GetHash() == tx1->GetHash());
}

// F-150 (2.2.3a's own Fable review): this test originally guarded against a
// `ReadBodyAt`-in-a-loop mutant (`break` -> `continue` on a failed read) --
// but the SAME review found the un-mutated code itself had an equivalent
// real cost problem: ReadBodyAt re-reads the whole offset table on every
// call, so even correct `break`-on-failure code was O(nCount x the
// record's own real body count) for the honest "ran past the end" case
// too. `BuildBodyRangeResponse` no longer loops over `ReadBodyAt` at
// all -- it delegates to `bodystore.h`'s `ReadBodyRange`, which opens the
// record and reads its header exactly once. That function's own dedicated,
// properly-bounded performance regression test lives in
// `bodystore_tests.cpp` (`read_body_range_opens_the_record_once_not_once_per_body`,
// a real 50,000-body record) -- unlike this one, it does not rely on
// `nCount = UINT32_MAX` to make a quadratic mutant's cost enormous, so a
// mutant fails it with a clean, fast assertion rather than a multi-hour
// hang (this test's own `nCount = UINT32_MAX` would hang, not fail
// cleanly, against a reintroduced per-call-reopen bug -- known and
// accepted here, since the real guard against that shape now lives one
// layer down). What THIS test still usefully covers: `BuildBodyRangeResponse`
// itself stays fast and correct at the extreme end of the legal `nCount`
// range once it delegates -- a narrower, still-real property, just not the
// same one it originally guarded.
BOOST_AUTO_TEST_CASE(build_response_does_not_hammer_reads_past_the_records_real_end) {
    ResetBodyIndex();
    TestOnlyResetBodyFileState();
    std::vector<CTransactionRef> bodies = {MakeBodyTx(1)};
    FlatFilePos pos;
    BOOST_REQUIRE(FindBodyPos(pos, (unsigned int) GetBodyRecordSerializedSize(bodies)));
    BOOST_REQUIRE(WriteBodyRecord(pos, bodies));

    CGetBodyRange req;
    req.hashBlock = uint256S("0x206");
    req.nStartIndex = 0;
    req.nCount = std::numeric_limits<uint32_t>::max();

    CBodyRange resp;
    auto start = std::chrono::steady_clock::now();
    BuildBodyRangeResponse(req, pos, std::numeric_limits<uint64_t>::max(), resp);
    auto elapsed = std::chrono::steady_clock::now() - start;

    BOOST_REQUIRE_EQUAL(resp.vBodies.size(), 1U);
    BOOST_CHECK(resp.vBodies[0]->GetHash() == bodies[0]->GetHash());
    // A generous bound: normally microseconds; billions of doomed opens
    // would take far, far longer than this on any real machine.
    BOOST_CHECK(elapsed < std::chrono::seconds(5));
}

// 2.2.4 (build-plan.md's 2.2 row): the fetching CLIENT's own response-shape
// check, before MaterialiseBlock ever sees the bodies. Pure, no I/O, no
// cs_main -- matches this file's own established split.
BOOST_AUTO_TEST_CASE(validate_response_accepts_a_well_formed_answer) {
    CGetBodyRange req;
    req.hashBlock = uint256S("0x301");
    req.nStartIndex = 0;
    req.nCount = 2;

    CBodyRange resp;
    resp.hashBlock = req.hashBlock;
    resp.nStartIndex = req.nStartIndex;
    resp.vBodies = {MakeBodyTx(1)};  // fewer than nCount -- legal truncation

    BOOST_CHECK(ValidateBodyRangeResponse(req, resp));
}

BOOST_AUTO_TEST_CASE(validate_response_accepts_an_honest_empty_miss) {
    CGetBodyRange req;
    req.hashBlock = uint256S("0x302");
    req.nStartIndex = 0;
    req.nCount = 5;

    CBodyRange resp;
    resp.hashBlock = req.hashBlock;
    resp.nStartIndex = req.nStartIndex;
    // vBodies left empty.

    BOOST_CHECK(ValidateBodyRangeResponse(req, resp));
}

BOOST_AUTO_TEST_CASE(validate_response_rejects_a_hash_mismatch) {
    CGetBodyRange req;
    req.hashBlock = uint256S("0x303");
    req.nStartIndex = 0;
    req.nCount = 1;

    CBodyRange resp;
    resp.hashBlock = uint256S("0x999");  // a different block entirely
    resp.nStartIndex = req.nStartIndex;
    resp.vBodies = {MakeBodyTx(1)};

    BOOST_CHECK(!ValidateBodyRangeResponse(req, resp));
}

BOOST_AUTO_TEST_CASE(validate_response_rejects_a_start_index_mismatch) {
    CGetBodyRange req;
    req.hashBlock = uint256S("0x304");
    req.nStartIndex = 3;
    req.nCount = 1;

    CBodyRange resp;
    resp.hashBlock = req.hashBlock;
    resp.nStartIndex = 0;  // does not echo the request
    resp.vBodies = {MakeBodyTx(1)};

    BOOST_CHECK(!ValidateBodyRangeResponse(req, resp));
}

// The over-delivery check's own boundary: exactly nCount bodies is a full,
// non-truncated delivery and must be ACCEPTED, not rejected -- proves the
// check is `>` (strictly more than asked), not `>=` (mutation-found gap:
// every other passing-response test here uses fewer than nCount, so a `>=`
// mutant that wrongly rejects an exact, legitimate full delivery survived
// undetected until this case was added).
BOOST_AUTO_TEST_CASE(validate_response_accepts_exactly_ncount_bodies) {
    CGetBodyRange req;
    req.hashBlock = uint256S("0x3045");
    req.nStartIndex = 0;
    req.nCount = 2;

    CBodyRange resp;
    resp.hashBlock = req.hashBlock;
    resp.nStartIndex = req.nStartIndex;
    resp.vBodies = {MakeBodyTx(1), MakeBodyTx(2)};  // exactly nCount, not fewer

    BOOST_CHECK(ValidateBodyRangeResponse(req, resp));
}

BOOST_AUTO_TEST_CASE(validate_response_rejects_over_delivery) {
    CGetBodyRange req;
    req.hashBlock = uint256S("0x305");
    req.nStartIndex = 0;
    req.nCount = 1;

    CBodyRange resp;
    resp.hashBlock = req.hashBlock;
    resp.nStartIndex = req.nStartIndex;
    resp.vBodies = {MakeBodyTx(1), MakeBodyTx(2)};  // 2 > nCount==1

    BOOST_CHECK(!ValidateBodyRangeResponse(req, resp));
}

BOOST_AUTO_TEST_CASE(validate_response_rejects_a_null_body_entry) {
    CGetBodyRange req;
    req.hashBlock = uint256S("0x306");
    req.nStartIndex = 0;
    req.nCount = 2;

    CBodyRange resp;
    resp.hashBlock = req.hashBlock;
    resp.nStartIndex = req.nStartIndex;
    resp.vBodies = {MakeBodyTx(1), CTransactionRef()};  // a hole

    BOOST_CHECK(!ValidateBodyRangeResponse(req, resp));
}

// 2.2.4: the aggregate-cap-and-eligibility decision, pure and testable
// without CNodeState/CAnnouncerRing/g_body_retry_state scaffolding.
BOOST_AUTO_TEST_CASE(should_request_allows_an_eligible_ready_uncapped_candidate) {
    BOOST_CHECK(ShouldRequestBodyRange(/*fWasAnnounced=*/true, /*nNow=*/1000, /*nNextAttempt=*/500,
                                         /*nInFlight=*/2, /*nMaxInFlight=*/10));
}

BOOST_AUTO_TEST_CASE(should_request_refuses_a_peer_that_never_announced) {
    BOOST_CHECK(!ShouldRequestBodyRange(/*fWasAnnounced=*/false, /*nNow=*/1000, /*nNextAttempt=*/500,
                                          /*nInFlight=*/2, /*nMaxInFlight=*/10));
}

BOOST_AUTO_TEST_CASE(should_request_refuses_before_the_backoff_deadline) {
    BOOST_CHECK(!ShouldRequestBodyRange(/*fWasAnnounced=*/true, /*nNow=*/499, /*nNextAttempt=*/500,
                                          /*nInFlight=*/2, /*nMaxInFlight=*/10));
}

BOOST_AUTO_TEST_CASE(should_request_allows_exactly_at_the_backoff_deadline) {
    // Boundary: nNow == nNextAttempt must be allowed, not just nNow > nNextAttempt --
    // matches net_processing.cpp's existing whole-block retry boundary
    // (`nNowRetry < retry.nNextAttempt` skips; the equal case falls through).
    BOOST_CHECK(ShouldRequestBodyRange(/*fWasAnnounced=*/true, /*nNow=*/500, /*nNextAttempt=*/500,
                                         /*nInFlight=*/2, /*nMaxInFlight=*/10));
}

BOOST_AUTO_TEST_CASE(should_request_refuses_once_the_aggregate_cap_is_saturated) {
    BOOST_CHECK(!ShouldRequestBodyRange(/*fWasAnnounced=*/true, /*nNow=*/1000, /*nNextAttempt=*/500,
                                          /*nInFlight=*/10, /*nMaxInFlight=*/10));
}

BOOST_AUTO_TEST_CASE(should_request_allows_one_below_the_aggregate_cap) {
    BOOST_CHECK(ShouldRequestBodyRange(/*fWasAnnounced=*/true, /*nNow=*/1000, /*nNextAttempt=*/500,
                                         /*nInFlight=*/9, /*nMaxInFlight=*/10));
}

// 2.2.4: NextBodyRetryBackoffMicros -- the exact arithmetic 1.3.6/H-2's
// original whole-block g_body_retry_state site used inline, factored out
// so 2.2.4's own GETBODYRANGE call site (net_processing.cpp) reuses it
// rather than duplicating the shift/clamp a second time.
BOOST_AUTO_TEST_CASE(backoff_doubles_per_attempt_before_the_shift_cap) {
    int64_t base = 1000000;
    int64_t max = 30000000;
    BOOST_CHECK_EQUAL(NextBodyRetryBackoffMicros(1, base, max), base);          // 1s
    BOOST_CHECK_EQUAL(NextBodyRetryBackoffMicros(2, base, max), base * 2);      // 2s
    BOOST_CHECK_EQUAL(NextBodyRetryBackoffMicros(3, base, max), base * 4);      // 4s
    BOOST_CHECK_EQUAL(NextBodyRetryBackoffMicros(4, base, max), base * 8);      // 8s
    BOOST_CHECK_EQUAL(NextBodyRetryBackoffMicros(5, base, max), base * 16);     // 16s
}

// Attempt 6 and beyond shift by the same clamped 5 -- base*32 (32s) here,
// with the ceiling raised so it's the SHIFT clamp under test, not the max.
BOOST_AUTO_TEST_CASE(backoff_shift_is_clamped_past_six_attempts) {
    int64_t base = 1000000;
    int64_t max = 60000000;
    BOOST_CHECK_EQUAL(NextBodyRetryBackoffMicros(6, base, max), base * 32);
    BOOST_CHECK_EQUAL(NextBodyRetryBackoffMicros(7, base, max), base * 32);
    BOOST_CHECK_EQUAL(NextBodyRetryBackoffMicros(100, base, max), base * 32);
}

BOOST_AUTO_TEST_CASE(backoff_is_clamped_to_the_max) {
    int64_t base = 1000000;
    int64_t max = 30000000;
    BOOST_CHECK_EQUAL(NextBodyRetryBackoffMicros(5, base, max), 16000000); // under max, unclamped
    BOOST_CHECK_EQUAL(NextBodyRetryBackoffMicros(6, base, max), max);     // 32s would exceed 30s max
}

// 2.2.4: IsBodyRangeRequestStale -- the reaper check for a connected-but-
// silent peer holding an aggregate-cap slot forever (no existing mechanism
// catches this: FinalizeNode only frees it on disconnect, and the
// whole-block stalling timeouts don't apply to mapBodyRangeInFlight at all).
BOOST_AUTO_TEST_CASE(stale_check_allows_a_fresh_request_to_stand) {
    BOOST_CHECK(!IsBodyRangeRequestStale(/*nRequestTime=*/1000, /*nNow=*/1500,
                                          /*nStaleAfterMicros=*/30000000));
}

BOOST_AUTO_TEST_CASE(stale_check_allows_a_request_exactly_at_the_boundary) {
    // Boundary: nNow - nRequestTime == nStaleAfterMicros must NOT be stale
    // yet -- only strictly past it is (matches ShouldRequestBodyRange's own
    // "equal is not yet expired" convention for its backoff boundary).
    BOOST_CHECK(!IsBodyRangeRequestStale(/*nRequestTime=*/0, /*nNow=*/30000000,
                                          /*nStaleAfterMicros=*/30000000));
}

BOOST_AUTO_TEST_CASE(stale_check_reaps_a_request_one_micro_past_the_boundary) {
    BOOST_CHECK(IsBodyRangeRequestStale(/*nRequestTime=*/0, /*nNow=*/30000001,
                                         /*nStaleAfterMicros=*/30000000));
}

BOOST_AUTO_TEST_CASE(stale_check_reaps_a_long_silent_request) {
    BOOST_CHECK(IsBodyRangeRequestStale(/*nRequestTime=*/0, /*nNow=*/600000000,
                                         /*nStaleAfterMicros=*/30000000));
}

// F-157 (Fable review of F-155/F-156): FindNextBlocksToDownload's own
// staller-detection blind spot -- extracted as a pure predicate so the fix
// is testable without net_processing.cpp's own untested scaffolding.
BOOST_AUTO_TEST_CASE(outstanding_work_true_when_only_whole_block_candidates_exist) {
    BOOST_CHECK(HasOutstandingBlockDownloadWork(/*nWholeBlockCandidates=*/3, /*nBodyRangeCandidates=*/0));
}

BOOST_AUTO_TEST_CASE(outstanding_work_true_when_only_body_range_candidates_exist) {
    // The regression this whole predicate exists to fix: real outstanding
    // work via the newer GETBODYRANGE path alone must count.
    BOOST_CHECK(HasOutstandingBlockDownloadWork(/*nWholeBlockCandidates=*/0, /*nBodyRangeCandidates=*/1));
}

BOOST_AUTO_TEST_CASE(outstanding_work_true_when_both_kinds_exist) {
    BOOST_CHECK(HasOutstandingBlockDownloadWork(/*nWholeBlockCandidates=*/2, /*nBodyRangeCandidates=*/2));
}

BOOST_AUTO_TEST_CASE(outstanding_work_false_when_neither_kind_exists) {
    BOOST_CHECK(!HasOutstandingBlockDownloadWork(/*nWholeBlockCandidates=*/0, /*nBodyRangeCandidates=*/0));
}

BOOST_AUTO_TEST_SUITE_END()
