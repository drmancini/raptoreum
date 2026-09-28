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

#include <bodyrange.h>
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

// Mines one block on top of the fixture's mature chain with exactly one
// non-coinbase transaction (spending m_coinbase_txns[0]) -- enough for
// CCommitmentBlock::vCommitments (block.h: "identifiers for vtx[1..]") to
// have a real entry at index 0 to attack.
CBlock BuildOneTxBlock(TestChainSetup &fixture) {
    const CScript spk = CScript() << ToByteVector(fixture.coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint(fixture.m_coinbase_txns[0]->GetHash(), 0);
    tx.vout.resize(1);
    tx.vout[0].nValue = fixture.m_coinbase_txns[0]->vout[0].nValue - 1000;
    tx.vout[0].scriptPubKey = spk;

    const uint256 hash = SignatureHash(spk, tx, 0, SIGHASH_ALL, 0, SigVersion::BASE);
    std::vector<unsigned char> vchSig;
    BOOST_REQUIRE(fixture.coinbaseKey.Sign(hash, vchSig));
    vchSig.push_back((unsigned char) SIGHASH_ALL);
    tx.vin[0].scriptSig << vchSig;

    std::vector<CMutableTransaction> txns = {tx};
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

// The coverage bucket a given block height falls into, read back from
// GetBodyRangeCoverageStats -- BodyRangeTally{0,0} (the struct's own
// default) if this run has not observed that bucket at all yet.
// mapBodyRangeCoverage is process-global (net_processing.cpp file-scope
// state, never reset between test cases in the same test binary), so every
// assertion below is against the CHANGE this test's own action causes,
// never an absolute count -- a second test case sharing this suite's own
// block heights (TestChain100Setup always mines to the same regtest
// heights) must not be read as contamination.
BodyRangeTally CoverageTallyForHeight(int nHeight) {
    std::vector<BodyRangeCoverageEntry> vStats;
    GetBodyRangeCoverageStats(vStats);
    for (const auto &entry: vStats) {
        if (nHeight >= entry.nRangeStartHeight && nHeight <= entry.nRangeEndHeightInclusive) {
            return entry.stats.tally;
        }
    }
    return BodyRangeTally();
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

    const BodyRangeTally baselineCoverage = CoverageTallyForHeight(pindex->nHeight);

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

    const BodyRangeTally afterCoverage = CoverageTallyForHeight(pindex->nHeight);
    BOOST_CHECK_EQUAL(afterCoverage.nHits, baselineCoverage.nHits);
    BOOST_CHECK_EQUAL(afterCoverage.nMisses, baselineCoverage.nMisses);
}

// Sanity companion: a genuinely valid chunk (real content, matching
// commitments) IS still recorded as a hit post-fix -- confirms the fix
// moved the recording rather than deleting it.
BOOST_AUTO_TEST_CASE(hash_valid_chunk_is_still_recorded_as_a_hit) {
    const CBlock block = BuildOneTxBlock(*this);
    CBlockIndex *pindex = LookupBlockIndex(block.GetHash());
    BOOST_REQUIRE(pindex != nullptr);

    const BodyRangeTally baselineCoverage = CoverageTallyForHeight(pindex->nHeight);

    ConnectedPeer peer(*m_node.peer_logic);
    SeedBodyRangeInFlightForTest(block.GetHash(), peer.node.GetId(), /*nStartIndex=*/0, /*nCount=*/1);

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
    BOOST_CHECK_EQUAL(stats.nMisbehavior, 0);
    BOOST_CHECK_EQUAL(stats.nBodyRangeHits, 1U);
    BOOST_CHECK_EQUAL(stats.nBodyRangeMisses, 0U);

    const BodyRangeTally afterCoverage = CoverageTallyForHeight(pindex->nHeight);
    BOOST_CHECK_EQUAL(afterCoverage.nHits, baselineCoverage.nHits + 1);
    BOOST_CHECK_EQUAL(afterCoverage.nMisses, baselineCoverage.nMisses);
}

// Companion: an honest miss (empty response) is unaffected by this fix --
// still recorded immediately, since there is no content for
// ValidateBodyRangeChunkHashes to check on zero bodies.
BOOST_AUTO_TEST_CASE(empty_response_is_still_recorded_as_a_miss) {
    const CBlock block = BuildOneTxBlock(*this);
    CBlockIndex *pindex = LookupBlockIndex(block.GetHash());
    BOOST_REQUIRE(pindex != nullptr);

    const BodyRangeTally baselineCoverage = CoverageTallyForHeight(pindex->nHeight);

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

    const BodyRangeTally afterCoverage = CoverageTallyForHeight(pindex->nHeight);
    BOOST_CHECK_EQUAL(afterCoverage.nHits, baselineCoverage.nHits);
    BOOST_CHECK_EQUAL(afterCoverage.nMisses, baselineCoverage.nMisses + 1);
}

BOOST_AUTO_TEST_SUITE_END()
