// Copyright (c) 2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// 4.5.2 Part B (F-201, redesigned F-202 per adversarial review) integration
// coverage: drives net_processing.cpp's own GETBLOCKTXN dispatch arm through
// PeerLogicValidation::ProcessMessages with a real CNode and a real block on
// the regtest chain, so the size-sum plus decline wiring inside
// SendBlockTransactions is exercised end to end -- not just the extracted
// ShouldDeclineBlockTransactionsForSize predicate. The harness shape (CNode
// with INVALID_SOCKET, InitializeNode, inspect vSendMsg) is
// denialofservice_tests.cpp's own.
//
// F-202: the review found the original fallback (decline -> queue a full-
// block CInv{MSG_BLOCK} GETDATA) was worse than doing nothing -- a full
// block is never smaller than the subset of it being requested, so if the
// subset didn't fit, the full block provably doesn't either. The fix does
// two things: (1) derives the size ceiling from MAX_PROTOCOL_MESSAGE_LENGTH
// itself (net.h, K-7) rather than a hardcoded 2 MB, so nothing that could
// actually have fit on the wire is declined -- the old 2 MB ceiling
// regressed the (2 MB, 3 MB] range, exactly what
// getblocktxn_between_old_and_new_ceiling_now_answers_with_blocktxn below
// reproduces; (2) on a genuine decline (over the new, real ceiling), does
// NOT queue a full-block fallback -- it logs and returns, mirroring this
// same function's existing "peer sent us a getblocktxn for a block we don't
// have" pattern (net_processing.cpp) exactly, leaving the requester's own
// existing block-download stall/timeout machinery
// (BLOCK_STALLING_TIMEOUT/BLOCK_DOWNLOAD_TIMEOUT_BASE/_PER_PEER, validation.h)
// to recover, the same way it already does for any other block that never
// arrives.

#include <blockencodings.h>
#include <chainparams.h>
#include <consensus/consensus.h>
#include <hash.h>
#include <net.h>
#include <net_processing.h>
#include <policy/policy.h>
#include <protocol.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <streams.h>
#include <validation.h>

#include <test/test_raptoreum.h>

#include <boost/test/unit_test.hpp>

namespace {

// A regtest block whose non-coinbase transactions serialize to well over
// 2 MiB in total (numTx chained ~89 KB transactions, each under the
// 100,000-byte DIP0001 consensus ceiling), spending the one coinbase that is
// mature at height 101. Accepted to the active chain, so GETBLOCKTXN can
// find it. numTx defaults to 25 (~2.2 MB of non-coinbase transactions --
// over the OLD hardcoded 2 MB ceiling, comfortably under the new,
// MAX_PROTOCOL_MESSAGE_LENGTH-derived one); callers needing a request that
// exceeds the NEW ceiling too pass a larger count.
CBlock BuildBlockOverBlockTxnCeiling(TestChainSetup &fixture, int numTx = 25) {
    const CScript spk = CScript() << ToByteVector(fixture.coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    const std::vector<unsigned char> filler(9900, 0x42);

    std::vector<CMutableTransaction> txns;
    uint256 prevHash = fixture.m_coinbase_txns[0]->GetHash();
    const CAmount value = fixture.m_coinbase_txns[0]->vout[0].nValue;
    for (int i = 0; i < numTx; ++i) {
        CMutableTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint(prevHash, 0);
        tx.vout.resize(10);
        tx.vout[0].nValue = value;
        tx.vout[0].scriptPubKey = spk;
        for (size_t j = 1; j < tx.vout.size(); ++j) {
            tx.vout[j].nValue = 0;
            tx.vout[j].scriptPubKey = CScript() << OP_RETURN << filler;
        }
        std::vector<unsigned char> vchSig;
        const uint256 hash = SignatureHash(spk, tx, 0, SIGHASH_ALL, 0, SigVersion::BASE);
        BOOST_REQUIRE(fixture.coinbaseKey.Sign(hash, vchSig));
        vchSig.push_back((unsigned char) SIGHASH_ALL);
        tx.vin[0].scriptSig << vchSig;
        prevHash = tx.GetHash();
        txns.push_back(tx);
    }
    return fixture.CreateAndProcessBlock(txns, spk);
}

uint64_t SumSerializedSize(const CBlock &block, const BlockTransactionsRequest &req) {
    uint64_t total = 0;
    for (uint32_t idx: req.indexes) total += GetSerializeSize(*block.vtx[idx], SER_NETWORK, PROTOCOL_VERSION);
    return total;
}

// Frame `payload` as a complete, checksummed P2P message and queue it on the
// node exactly as the socket thread would, via CNetMessage's own readHeader/
// readData parse path.
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

// PushMessage queues the header and the payload as two consecutive vSendMsg
// entries (net.cpp); return the command of the first queued message and its
// payload bytes.
std::pair<std::string, std::vector<unsigned char>> FirstSentMessage(CNode &node) {
    LOCK(node.cs_vSend);
    BOOST_REQUIRE(!node.vSendMsg.empty());
    CDataStream hdrStream(node.vSendMsg.front(), SER_NETWORK, PROTOCOL_VERSION);
    CMessageHeader hdr(Params().MessageStart());
    hdrStream >> hdr;
    std::vector<unsigned char> payload;
    if (hdr.nMessageSize > 0) {
        BOOST_REQUIRE(node.vSendMsg.size() >= 2);
        payload = *std::next(node.vSendMsg.begin());
    }
    return {hdr.GetCommand(), payload};
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

} // namespace

BOOST_FIXTURE_TEST_SUITE(blocktxn_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(getblocktxn_within_ceiling_answers_with_blocktxn) {
        const CBlock block = BuildBlockOverBlockTxnCeiling(*this);
        BOOST_REQUIRE_EQUAL(::ChainActive().Tip()->GetBlockHash(), block.GetHash());
        BOOST_REQUIRE(block.vtx.size() >= 3);

        BlockTransactionsRequest req;
        req.blockhash = block.GetHash();
        req.indexes = {1, 2};
        BOOST_REQUIRE(SumSerializedSize(block, req) < 2 * 1024 * 1024);

        ConnectedPeer peer(*m_node.peer_logic);
        CDataStream payload(SER_NETWORK, PROTOCOL_VERSION);
        payload << req;
        QueueMessage(peer.node, NetMsgType::GETBLOCKTXN, payload);
        peer.Process();

        // Answered directly: no full-block fallback queued...
        BOOST_CHECK(peer.node.vRecvGetData.empty());
        // ...and exactly the requested transactions went out as one BLOCKTXN.
        const auto sent = FirstSentMessage(peer.node);
        BOOST_CHECK_EQUAL(sent.first, NetMsgType::BLOCKTXN);
        CDataStream respStream(sent.second, SER_NETWORK, PROTOCOL_VERSION);
        BlockTransactions resp;
        respStream >> resp;
        BOOST_CHECK_EQUAL(resp.blockhash, block.GetHash());
        BOOST_REQUIRE_EQUAL(resp.txn.size(), 2U);
        BOOST_CHECK_EQUAL(resp.txn[0]->GetHash(), block.vtx[1]->GetHash());
        BOOST_CHECK_EQUAL(resp.txn[1]->GetHash(), block.vtx[2]->GetHash());
}

// F-202: net_processing.cpp derives its real ceiling as
// MAX_PROTOCOL_MESSAGE_LENGTH minus a fixed margin (the same 4096 B margin
// this file's own DEFAULT_MAX_BODYRANGE_BYTES/MAX_STANDARD_TX_SIZE
// static_asserts already use) -- duplicated here rather than included, since
// MAX_BLOCKTXN_RESPONSE_BYTES itself is a net_processing.cpp file-local
// static with no header declaration, matching this test file's own existing
// convention of pinning against a literal rather than the production
// constant (the original 2 MB ceiling was pinned the same way above).
static const uint64_t BLOCKTXN_RESPONSE_MARGIN = 4096;
static const uint64_t DERIVED_BLOCKTXN_CEILING = MAX_PROTOCOL_MESSAGE_LENGTH - BLOCKTXN_RESPONSE_MARGIN;

BOOST_AUTO_TEST_CASE(getblocktxn_between_old_and_new_ceiling_now_answers_with_blocktxn) {
        // The reviewer's own exact regression: a request over the OLD,
        // hardcoded 2 MB ceiling that would still have fit comfortably
        // under a correctly MAX_PROTOCOL_MESSAGE_LENGTH-derived one. Pre-fix,
        // this got wrongly declined and "fell back" to a full block that was
        // itself no smaller -- the exact bug the review found (a 3,576,716 B
        // block, a 2,414,195 B subset request, served as the full 3,576,716 B
        // block anyway). Post-fix it must be answered directly with BLOCKTXN.
        const CBlock block = BuildBlockOverBlockTxnCeiling(*this);
        BOOST_REQUIRE_EQUAL(::ChainActive().Tip()->GetBlockHash(), block.GetHash());

        BlockTransactionsRequest req;
        req.blockhash = block.GetHash();
        for (size_t i = 1; i < block.vtx.size(); ++i) req.indexes.push_back(i);
        const uint64_t requested = SumSerializedSize(block, req);
        BOOST_REQUIRE(requested > 2 * 1024 * 1024);
        BOOST_REQUIRE(requested < DERIVED_BLOCKTXN_CEILING);

        ConnectedPeer peer(*m_node.peer_logic);
        CDataStream payload(SER_NETWORK, PROTOCOL_VERSION);
        payload << req;
        QueueMessage(peer.node, NetMsgType::GETBLOCKTXN, payload);
        peer.Process();

        // Answered directly: no fallback of any kind queued...
        BOOST_CHECK(peer.node.vRecvGetData.empty());
        // ...and exactly the requested transactions went out as one BLOCKTXN,
        // not the full block.
        const auto sent = FirstSentMessage(peer.node);
        BOOST_CHECK_EQUAL(sent.first, NetMsgType::BLOCKTXN);
        CDataStream respStream(sent.second, SER_NETWORK, PROTOCOL_VERSION);
        BlockTransactions resp;
        respStream >> resp;
        BOOST_CHECK_EQUAL(resp.blockhash, block.GetHash());
        BOOST_REQUIRE_EQUAL(resp.txn.size(), req.indexes.size());
}

BOOST_AUTO_TEST_CASE(getblocktxn_over_derived_ceiling_declines_without_full_block_fallback) {
        // A request that is genuinely too big even for the corrected,
        // derived ceiling. The subset is a SUBSET of the block, so the full
        // block is provably at least as large -- falling back to it would
        // reproduce the exact bug this fix removes. Correct behaviour:
        // decline silently (log only), same shape as this same handler's
        // existing "getblocktxn for a block we don't have" branch -- no
        // BLOCKTXN, no full-block GETDATA, nothing queued or sent at all.
        const CBlock block = BuildBlockOverBlockTxnCeiling(*this, /*numTx=*/40);
        BOOST_REQUIRE_EQUAL(::ChainActive().Tip()->GetBlockHash(), block.GetHash());

        BlockTransactionsRequest req;
        req.blockhash = block.GetHash();
        for (size_t i = 1; i < block.vtx.size(); ++i) req.indexes.push_back(i);
        const uint64_t requested = SumSerializedSize(block, req);
        // The premise: over the real, derived ceiling -- not merely the old
        // hardcoded one, and the whole block (subset + coinbase) is bigger
        // still, so a full-block fallback would not have helped either.
        BOOST_REQUIRE(requested > DERIVED_BLOCKTXN_CEILING);

        ConnectedPeer peer(*m_node.peer_logic);
        CDataStream payload(SER_NETWORK, PROTOCOL_VERSION);
        payload << req;
        QueueMessage(peer.node, NetMsgType::GETBLOCKTXN, payload);
        peer.Process();

        // Nothing sent, nothing queued as a getdata fallback either.
        {
            LOCK(peer.node.cs_vSend);
            BOOST_CHECK(peer.node.vSendMsg.empty());
        }
        BOOST_CHECK(peer.node.vRecvGetData.empty());

        // A further pass of the message loop changes nothing -- there is
        // nothing left queued to drain.
        peer.Process();
        {
            LOCK(peer.node.cs_vSend);
            BOOST_CHECK(peer.node.vSendMsg.empty());
        }
        BOOST_CHECK(peer.node.vRecvGetData.empty());
}

BOOST_AUTO_TEST_SUITE_END()
