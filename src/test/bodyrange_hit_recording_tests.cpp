// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// F-207 (independent adversarial review of F-206, CONFIRMED HIGH): the
// BODYRANGE response handler's own hit-recording call sites, driven end to
// end through a real PeerLogicValidation::ProcessMessages pass -- matching
// blocktxn_tests.cpp's own ConnectedPeer/QueueMessage harness (this file's
// helpers of the same name/shape are deliberately duplicated from there,
// not shared, matching this project's own established per-file convention
// for this harness -- see denialofservice_tests.cpp for the same choice).
//
// This project's own established convention (bodyrange_tests.cpp's own doc
// comment, coveragetelemetry_tests.cpp's own doc comment) keeps
// net_processing.cpp's message-processing glue itself unit-untested,
// extracting only the pure decision predicates it depends on into
// bodyrange.h/coveragetelemetry.h for testing. That convention does not fit
// here: the F-207 bug IS the glue's own call ORDER (hit-recording happened
// right after ValidateBodyRangeResponse -- wire shape only -- instead of
// after ValidateBodyRangeChunkHashes -- actual content -- see
// net_processing.cpp's own F-207 doc comment at the fix), which no pure
// predicate captures; only a test that drives the real handler can catch a
// call-order regression in it. net_processing.cpp's SeedBodyRangeInFlightForTest
// (test-only, declared in net_processing.h) exists solely to make that
// possible without reimplementing SendMessages' own real fetch-selection
// machinery (-fetchbodyrange, NODE_COMMITMENTS, the announcer ring) a
// second time in a test.
//
// F-212 (rework, docs/findings.md): extended with the gap-2 completion-hook
// tests (first_chunk_alone_leaves_a_multi_chunk_height_at_partial_not_full/
// both_chunks_arriving_advances_a_multi_chunk_height_to_full below), and the
// existing three tests' assertions were ported from the old bucket-level
// BodyRangeTally read (CoverageTallyForHeight) to the new per-height
// HeightCoverageStatus read (CoverageStatusForHeight) -- see this file's own
// CoverageStatusForHeight doc comment. The completion tests need a pindex
// that genuinely LACKS its body yet (BLOCK_HAVE_DATA set, BLOCK_HAVE_BODIES
// clear) for ProcessFetchedBodyRange to do real work at all (validation.cpp's
// own guard: `!(nStatus & BLOCK_HAVE_DATA) || HaveBodies(pindex)` is a no-op
// return, not a failure) -- a block mined locally via CreateAndProcessBlock
// already has its body (the normal whole-block path), so those tests clear
// BLOCK_HAVE_BODIES on the pindex directly before exercising the fetch path
// -- matching acceptancebit_tests.cpp's own established convention (its own
// `pindex->nStatus &= ~BLOCK_HAVE_BODIES` call sites) of clearing exactly
// that bit and no other; BLOCK_HAVE_BODY_RECORD is deliberately left alone,
// since HaveBodies (validation.cpp) gates on BLOCK_HAVE_BODIES only, and
// this file's own first attempt at clearing both bits together produced an
// unrelated crash later in the same test binary run (a different suite's
// fixture teardown) -- consistent with disturbing the body-file store's own
// process-global bookkeeping (bodystore.cpp) in a way nothing in this
// codebase's existing tests does, so this file does not do it either.

#include <bodyrange.h>
#include <chain.h>
#include <chainparams.h>
#include <hash.h>
#include <net.h>
#include <net_processing.h>
#include <protocol.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <streams.h>
#include <uint256.h>
#include <util/time.h>
#include <validation.h>

#include <test/test_raptoreum.h>

#include <boost/test/unit_test.hpp>

namespace {

// Builds one spend of m_coinbase_txns[nCoinbaseIndex] to `spk`, signed by
// the fixture's own coinbaseKey.
CMutableTransaction BuildSpend(TestChainSetup &fixture, size_t nCoinbaseIndex, const CScript &spk) {
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint(fixture.m_coinbase_txns[nCoinbaseIndex]->GetHash(), 0);
    tx.vout.resize(1);
    tx.vout[0].nValue = fixture.m_coinbase_txns[nCoinbaseIndex]->vout[0].nValue - 1000;
    tx.vout[0].scriptPubKey = spk;

    const uint256 hash = SignatureHash(spk, tx, 0, SIGHASH_ALL, 0, SigVersion::BASE);
    std::vector<unsigned char> vchSig;
    BOOST_REQUIRE(fixture.coinbaseKey.Sign(hash, vchSig));
    vchSig.push_back((unsigned char) SIGHASH_ALL);
    tx.vin[0].scriptSig << vchSig;
    return tx;
}

// Mines `nFillerBlocks` empty (coinbase-only) blocks first, then one block
// with exactly one non-coinbase transaction (spending m_coinbase_txns[0]) --
// enough for CCommitmentBlock::vCommitments (block.h: "identifiers for
// vtx[1..]") to have a real entry at index 0 to attack.
//
// The filler-block count exists solely to give every test CASE in this file
// its own, non-colliding height: mapBodyRangeCoverage (F-212's own
// per-HEIGHT ledger, net_processing.cpp) is process-global state, never
// reset between test cases in the same test binary, while TestChainSetup
// gives each test case its own fresh, independent chain starting from the
// SAME height -- so two test cases in this file that both mined "the next
// block" would silently read and write the SAME height's coverage record.
// Each test case below passes its own distinct nFillerBlocks.
CBlock BuildOneTxBlock(TestChainSetup &fixture, int nFillerBlocks = 0) {
    for (int i = 0; i < nFillerBlocks; i++) {
        fixture.CreateAndProcessBlock({}, CScript() << OP_TRUE);
    }

    const CScript spk = CScript() << ToByteVector(fixture.coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    std::vector<CMutableTransaction> txns = {BuildSpend(fixture, 0, spk)};
    return fixture.CreateAndProcessBlock(txns, spk);
}

// Same idea, but with TWO non-coinbase transactions (spending
// m_coinbase_txns[0] and [1]) -- BodyRangeWantedCount(pindex) == 2, so a
// single GETBODYRANGE chunk of nCount=1 can never complete this block by
// itself. Used by the gap-2 completion tests below, which need a block that
// genuinely requires more than one chunk to reconstruct.
CBlock BuildTwoTxBlock(TestChainSetup &fixture, int nFillerBlocks = 0) {
    for (int i = 0; i < nFillerBlocks; i++) {
        fixture.CreateAndProcessBlock({}, CScript() << OP_TRUE);
    }

    const CScript spk = CScript() << ToByteVector(fixture.coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    std::vector<CMutableTransaction> txns = {BuildSpend(fixture, 0, spk), BuildSpend(fixture, 1, spk)};
    return fixture.CreateAndProcessBlock(txns, spk);
}

// A transaction guaranteed to differ from the block's own real committed
// tx -- the content a malicious or buggy peer might substitute into an
// otherwise shape-valid BODYRANGE chunk.
CTransactionRef MakeUnrelatedTx() {
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint(uint256S("0xdeadbeef"), 0);
    tx.vout.resize(1);
    tx.vout[0].nValue = 1;
    tx.vout[0].scriptPubKey = CScript() << OP_TRUE;
    return MakeTransactionRef(tx);
}

// Frame `payload` as a complete, checksummed P2P message and queue it on the
// node exactly as the socket thread would, via CNetMessage's own readHeader/
// readData parse path. Duplicated from blocktxn_tests.cpp's own helper of
// the same name/shape.
void QueueMessage(CNode &node, const char *command, const CDataStream &payload) {
    CMessageHeader hdr(Params().MessageStart(), command, payload.size());
    const uint256 checksum = Hash(payload.begin(), payload.end());
    memcpy(hdr.pchChecksum, checksum.begin(), CMessageHeader::CHECKSUM_SIZE);
    CDataStream hdrStream(SER_NETWORK, PROTOCOL_VERSION);
    hdrStream << hdr;

    CNetMessage msg(Params().MessageStart(), SER_NETWORK, PROTOCOL_VERSION);
    BOOST_REQUIRE_EQUAL(msg.readHeader(hdrStream.data(), hdrStream.size()), (int) hdrStream.size());
    BOOST_REQUIRE_EQUAL(msg.readData(payload.data(), payload.size()), (int) payload.size());
    BOOST_REQUIRE(msg.complete());
    msg.nTime = GetTimeMicros();

    LOCK(node.cs_vProcessMsg);
    node.nProcessQueueSize += msg.vRecv.size() + CMessageHeader::HEADER_SIZE;
    node.vProcessMsg.push_back(std::move(msg));
}

struct ConnectedPeer {
    CNode node;
    PeerLogicValidation &peerLogic;

    explicit ConnectedPeer(PeerLogicValidation &logic)
            : node(/*id=*/ 0, ServiceFlags(NODE_NETWORK), 0, INVALID_SOCKET,
                   CAddress(CService(CNetAddr(), Params().GetDefaultPort()), NODE_NONE),
                   0, 0, CAddress(), "", /*fInboundIn=*/ true),
              peerLogic(logic) {
        node.SetSendVersion(PROTOCOL_VERSION);
        node.SetRecvVersion(PROTOCOL_VERSION);
        peerLogic.InitializeNode(&node);
        node.nVersion = PROTOCOL_VERSION;
        node.fSuccessfullyConnected = true;
    }

    ~ConnectedPeer() {
        bool dummy;
        peerLogic.FinalizeNode(node.GetId(), dummy);
    }

    void Process() {
        std::atomic<bool> interrupt{false};
        peerLogic.ProcessMessages(&node, interrupt);
    }
};

// F-212: the cross-peer coverage LEDGER's own status for a given block
// height, read back through GetBodyRangeCoverageHeights --
// HeightCoverageStatus::NOT_OBSERVED if this run has not observed that
// exact height at all yet. mapBodyRangeCoverage is process-global
// (net_processing.cpp file-scope state, never reset between test cases in
// the same test binary), so every assertion below is against a height this
// test case mines FRESH (BuildOneTxBlock/BuildMultiChunkBlock mine a new
// block each call), never a height reused across test cases -- unlike the
// pre-F-212 version of this helper (bucket-level, so a second test case
// sharing a bucket needed "check the CHANGE, not the absolute count"), a
// per-height read has no such contamination risk as long as each test uses
// its own height, which every test case here already does by construction.
HeightCoverageStatus CoverageStatusForHeight(int nHeight) {
    std::vector<BodyRangeCoverageHeightEntry> vHeights;
    GetBodyRangeCoverageHeights(nHeight, nHeight, vHeights);
    if (vHeights.empty()) {
        return HeightCoverageStatus::NOT_OBSERVED;
    }
    BOOST_REQUIRE_EQUAL(vHeights.size(), 1U);
    BOOST_REQUIRE_EQUAL(vHeights[0].nHeight, nHeight);
    return vHeights[0].status;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(bodyrange_hit_recording_tests, TestChain100Setup)

// The regression this suite exists to catch (F-207): a shape-valid,
// content-INVALID chunk must never be counted as a hit, in either the
// per-peer tally or the cross-peer coverage bucket -- it must instead get
// the peer banned (Misbehaving) and leave both telemetry structures
// completely untouched. Not recorded as a miss either: the peer claimed to
// have the range and answered with garbage, which is not the same signal
// as an honest "I don't have this."
BOOST_AUTO_TEST_CASE(hash_invalid_chunk_is_never_recorded_as_a_hit) {
    const CBlock block = BuildOneTxBlock(*this);
    CBlockIndex *pindex = LookupBlockIndex(block.GetHash());
    BOOST_REQUIRE(pindex != nullptr);
    // At least coinbase + the one spend -- CreateBlock (test_raptoreum.cpp)
    // also re-inserts any LLMQ commitment special tx the template produced,
    // so vtx[1] is not necessarily the spend added by BuildOneTxBlock; it
    // does not need to be -- any real committed non-coinbase tx exercises
    // ValidateBodyRangeChunkHashes identically.
    BOOST_REQUIRE(block.vtx.size() >= 2);

    const CTransactionRef bogus = MakeUnrelatedTx();
    BOOST_REQUIRE(bogus->GetHash() != block.vtx[1]->GetHash());

    // F-212: this height must genuinely never have been observed before
    // this test's own action -- a bucket-level "check the change" baseline
    // (the pre-F-212 version of this test) is no longer needed since
    // GetBodyRangeCoverageHeights reads this exact height, but confirming
    // it starts NOT_OBSERVED still guards against an unexpected earlier
    // write to this specific height.
    BOOST_REQUIRE(CoverageStatusForHeight(pindex->nHeight) == HeightCoverageStatus::NOT_OBSERVED);

    ConnectedPeer peer(*m_node.peer_logic);
    SeedBodyRangeInFlightForTest(block.GetHash(), peer.node.GetId(), /*nStartIndex=*/0, /*nCount=*/1);

    // Shape-valid (right hash, right nStartIndex, one non-null body, within
    // the requested count) but content-invalid: `bogus` is not block.vtx[1].
    CBodyRange resp;
    resp.hashBlock = block.GetHash();
    resp.nStartIndex = 0;
    resp.vBodies = {bogus};

    CDataStream payload(SER_NETWORK, PROTOCOL_VERSION);
    payload << resp;
    QueueMessage(peer.node, NetMsgType::BODYRANGE, payload);
    peer.Process();

    CNodeStateStats stats;
    BOOST_REQUIRE(GetNodeStateStats(peer.node.GetId(), stats));
    BOOST_CHECK_GT(stats.nMisbehavior, 0);          // banned for the invalid content
    BOOST_CHECK_EQUAL(stats.nBodyRangeHits, 0U);    // the actual F-207 assertion
    BOOST_CHECK_EQUAL(stats.nBodyRangeMisses, 0U);  // not a miss either

    // F-207's own guarantee, now expressed against the F-212 ledger: hash-
    // invalid content must leave this height's cross-peer coverage status
    // completely untouched -- not recorded as PARTIAL (a hit) or MISS.
    BOOST_CHECK(CoverageStatusForHeight(pindex->nHeight) == HeightCoverageStatus::NOT_OBSERVED);
}

// Sanity companion: a genuinely valid chunk (real content, matching
// commitments) IS still recorded as a hit post-fix -- confirms the fix
// moved the recording rather than deleting it.
BOOST_AUTO_TEST_CASE(hash_valid_chunk_is_still_recorded_as_a_hit) {
    const CBlock block = BuildOneTxBlock(*this, /*nFillerBlocks=*/1);
    CBlockIndex *pindex = LookupBlockIndex(block.GetHash());
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(block.vtx.size() >= 2);

    // See this file's own top-of-file F-212 doc comment: this test needs a
    // real completion to happen, which needs a pindex that does not already
    // have its body (unlike a normally-mined-and-connected test block).
    {
        LOCK(cs_main);
        pindex->nStatus &= ~BLOCK_HAVE_BODIES;
    }

    BOOST_REQUIRE(CoverageStatusForHeight(pindex->nHeight) == HeightCoverageStatus::NOT_OBSERVED);

    // Every non-coinbase body in ONE chunk -- block.vtx.size() - 1 is
    // BodyRangeWantedCount's own real value for this block (net_processing.cpp),
    // whatever it turns out to be (CreateAndProcessBlock's own template can
    // add more than just BuildOneTxBlock's one requested spend, e.g. an
    // automatic LLMQ commitment special transaction at this height -- this
    // test does not depend on which). Delivering all of it in a single
    // response keeps this test's own point (one validated, COMPLETE chunk)
    // independent of that count.
    const std::vector<CTransactionRef> vAllBodies(block.vtx.begin() + 1, block.vtx.end());

    ConnectedPeer peer(*m_node.peer_logic);
    SeedBodyRangeInFlightForTest(block.GetHash(), peer.node.GetId(), /*nStartIndex=*/0,
                                  /*nCount=*/(uint32_t) vAllBodies.size());

    CBodyRange resp;
    resp.hashBlock = block.GetHash();
    resp.nStartIndex = 0;
    resp.vBodies = vAllBodies;

    CDataStream payload(SER_NETWORK, PROTOCOL_VERSION);
    payload << resp;
    QueueMessage(peer.node, NetMsgType::BODYRANGE, payload);
    peer.Process();

    CNodeStateStats stats;
    BOOST_REQUIRE(GetNodeStateStats(peer.node.GetId(), stats));
    BOOST_CHECK_EQUAL(stats.nMisbehavior, 0);
    BOOST_CHECK_EQUAL(stats.nBodyRangeHits, 1U);
    BOOST_CHECK_EQUAL(stats.nBodyRangeMisses, 0U);

    // This single chunk carries EVERY body the block wants, so it both
    // validates AND completes the fetch in one round trip -- the cross-peer
    // ledger jumps straight to FULL, not just PARTIAL, proving both the
    // first-chunk recording site AND the new completion hook (F-212's own
    // gap-2 fix) fired for this height.
    BOOST_CHECK(CoverageStatusForHeight(pindex->nHeight) == HeightCoverageStatus::FULL);
}

// Companion: an honest miss (empty response) is unaffected by this fix --
// still recorded immediately, since there is no content for
// ValidateBodyRangeChunkHashes to check on zero bodies.
BOOST_AUTO_TEST_CASE(empty_response_is_still_recorded_as_a_miss) {
    const CBlock block = BuildOneTxBlock(*this, /*nFillerBlocks=*/2);
    CBlockIndex *pindex = LookupBlockIndex(block.GetHash());
    BOOST_REQUIRE(pindex != nullptr);

    BOOST_REQUIRE(CoverageStatusForHeight(pindex->nHeight) == HeightCoverageStatus::NOT_OBSERVED);

    ConnectedPeer peer(*m_node.peer_logic);
    SeedBodyRangeInFlightForTest(block.GetHash(), peer.node.GetId(), /*nStartIndex=*/0, /*nCount=*/1);

    CBodyRange resp;
    resp.hashBlock = block.GetHash();
    resp.nStartIndex = 0;
    // resp.vBodies left empty -- a miss.

    CDataStream payload(SER_NETWORK, PROTOCOL_VERSION);
    payload << resp;
    QueueMessage(peer.node, NetMsgType::BODYRANGE, payload);
    peer.Process();

    CNodeStateStats stats;
    BOOST_REQUIRE(GetNodeStateStats(peer.node.GetId(), stats));
    BOOST_CHECK_EQUAL(stats.nMisbehavior, 0);
    BOOST_CHECK_EQUAL(stats.nBodyRangeHits, 0U);
    BOOST_CHECK_EQUAL(stats.nBodyRangeMisses, 1U);

    BOOST_CHECK(CoverageStatusForHeight(pindex->nHeight) == HeightCoverageStatus::MISS);
}

// F-212 (gap 2): a peer serving the FIRST chunk of a multi-chunk block and
// then stalling (never actually delivering the rest) must leave that
// height's cross-peer status at PARTIAL, never FULL -- the exact scenario
// F-209's review found invisible in F-208's original design (a peer serving
// chunk 0 of a large block and silently withholding everything else used to
// read as a clean hit, with zero visible signal that the body was never
// actually completed).
BOOST_AUTO_TEST_CASE(first_chunk_alone_leaves_a_multi_chunk_height_at_partial_not_full) {
    const CBlock block = BuildTwoTxBlock(*this, /*nFillerBlocks=*/3);
    CBlockIndex *pindex = LookupBlockIndex(block.GetHash());
    BOOST_REQUIRE(pindex != nullptr);
    // At least the two real spends BuildTwoTxBlock asked for -- possibly
    // more (CreateAndProcessBlock's own template can add an automatic LLMQ
    // commitment special transaction at this height; this test does not
    // depend on the exact count, only that it takes more than one chunk).
    BOOST_REQUIRE_GE(block.vtx.size(), 3U);

    {
        LOCK(cs_main);
        pindex->nStatus &= ~BLOCK_HAVE_BODIES;
    }

    BOOST_REQUIRE(CoverageStatusForHeight(pindex->nHeight) == HeightCoverageStatus::NOT_OBSERVED);

    ConnectedPeer peer(*m_node.peer_logic);
    SeedBodyRangeInFlightForTest(block.GetHash(), peer.node.GetId(), /*nStartIndex=*/0, /*nCount=*/1);

    // Only chunk 0 of 2 arrives -- a genuinely incomplete fetch.
    CBodyRange resp;
    resp.hashBlock = block.GetHash();
    resp.nStartIndex = 0;
    resp.vBodies = {block.vtx[1]};

    CDataStream payload(SER_NETWORK, PROTOCOL_VERSION);
    payload << resp;
    QueueMessage(peer.node, NetMsgType::BODYRANGE, payload);
    peer.Process();

    CNodeStateStats stats;
    BOOST_REQUIRE(GetNodeStateStats(peer.node.GetId(), stats));
    BOOST_CHECK_EQUAL(stats.nBodyRangeHits, 1U);  // this one chunk DID validate

    // The old (pre-F-212) design would have recorded this as a clean,
    // indistinguishable-from-complete hit. This is the actual regression
    // guard for gap 2.
    BOOST_CHECK(CoverageStatusForHeight(pindex->nHeight) == HeightCoverageStatus::PARTIAL);
}

// Companion: once the SECOND chunk also arrives and validates, completion
// fires (net_processing.cpp's own new F-212 hook, right after
// ProcessFetchedBodyRange succeeds) and the height advances to FULL --
// confirming the completion signal genuinely reflects reconstruction, not
// merely "another chunk arrived."
BOOST_AUTO_TEST_CASE(both_chunks_arriving_advances_a_multi_chunk_height_to_full) {
    const CBlock block = BuildTwoTxBlock(*this, /*nFillerBlocks=*/4);
    CBlockIndex *pindex = LookupBlockIndex(block.GetHash());
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE_GE(block.vtx.size(), 3U);

    {
        LOCK(cs_main);
        pindex->nStatus &= ~BLOCK_HAVE_BODIES;
    }

    ConnectedPeer peer(*m_node.peer_logic);

    SeedBodyRangeInFlightForTest(block.GetHash(), peer.node.GetId(), /*nStartIndex=*/0, /*nCount=*/1);
    CBodyRange resp1;
    resp1.hashBlock = block.GetHash();
    resp1.nStartIndex = 0;
    resp1.vBodies = {block.vtx[1]};
    CDataStream payload1(SER_NETWORK, PROTOCOL_VERSION);
    payload1 << resp1;
    QueueMessage(peer.node, NetMsgType::BODYRANGE, payload1);
    peer.Process();

    BOOST_REQUIRE(CoverageStatusForHeight(pindex->nHeight) == HeightCoverageStatus::PARTIAL);

    // Everything else the block wants, in one second chunk -- whatever that
    // remainder turns out to be (see the previous test's own doc comment on
    // why this does not hardcode a count).
    const std::vector<CTransactionRef> vRemaining(block.vtx.begin() + 2, block.vtx.end());
    BOOST_REQUIRE(!vRemaining.empty());

    SeedBodyRangeInFlightForTest(block.GetHash(), peer.node.GetId(), /*nStartIndex=*/1,
                                  /*nCount=*/(uint32_t) vRemaining.size());
    CBodyRange resp2;
    resp2.hashBlock = block.GetHash();
    resp2.nStartIndex = 1;
    resp2.vBodies = vRemaining;
    CDataStream payload2(SER_NETWORK, PROTOCOL_VERSION);
    payload2 << resp2;
    QueueMessage(peer.node, NetMsgType::BODYRANGE, payload2);
    peer.Process();

    BOOST_CHECK(CoverageStatusForHeight(pindex->nHeight) == HeightCoverageStatus::FULL);
}

BOOST_AUTO_TEST_SUITE_END()
