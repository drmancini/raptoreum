// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/merkle.h>
#include <primitives/block.h>
#include <protocol.h>
#include <chainparams.h>
#include <validation.h>
#include <streams.h>
#include <tinyformat.h>
#include <test/test_raptoreum.h>
#include <version.h>

#include <boost/test/unit_test.hpp>

// Build-plan item 1.1: the commitment form of a block. These are the properties the
// rest of the design leans on, so they are asserted rather than assumed.

BOOST_FIXTURE_TEST_SUITE(commitmentblock_tests, BasicTestingSetup)

static CTransactionRef MakeTx(uint32_t nonce, uint32_t nLockTime = 0) {
    CMutableTransaction tx;
    tx.nVersion = 1;
    tx.nLockTime = nLockTime;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint(uint256S(strprintf("%064x", nonce)), nonce);
    tx.vout.resize(1);
    tx.vout[0].nValue = 1000 + nonce;
    tx.vout[0].scriptPubKey = CScript() << OP_TRUE;
    return MakeTransactionRef(std::move(tx));
}

static CBlock MakeBlock(size_t nTx) {
    CBlock block;
    block.nVersion = 4;
    block.hashPrevBlock = uint256S("0xbeef");
    block.nTime = 1700000000;
    block.nBits = 0x207fffff;
    block.nNonce = 7;
    for (size_t i = 0; i < nTx; i++) {
        block.vtx.push_back(MakeTx(i + 1));
    }
    block.hashMerkleRoot = BlockMerkleRoot(block);
    return block;
}

// The whole design rests on this: a block's merkle root is computable from the
// identifiers alone, because that is all the tree ever hashed.
BOOST_AUTO_TEST_CASE(merkle_root_survives_the_commitment_form) {
    for (size_t n : {1, 2, 3, 4, 5, 17, 64}) {
        const CBlock block = MakeBlock(n);
        const CCommitmentBlock c = CommitmentsFromBlock(block);
        BOOST_CHECK_EQUAL(c.CommittedCount(), n);
        BOOST_CHECK(c.ComputeMerkleRoot() == BlockMerkleRoot(block));
        BOOST_CHECK(c.ComputeMerkleRoot() == block.hashMerkleRoot);
    }
}

// The coinbase is carried whole, so nothing extra is needed to bind it to the tree:
// its own hash is the first leaf. Change the coinbase and the root moves.
BOOST_AUTO_TEST_CASE(coinbase_is_bound_by_construction) {
    const CBlock block = MakeBlock(4);
    CCommitmentBlock c = CommitmentsFromBlock(block);
    const uint256 before = c.ComputeMerkleRoot();

    c.coinbase = MakeTx(999);
    BOOST_CHECK(c.ComputeMerkleRoot() != before);
}

BOOST_AUTO_TEST_CASE(round_trips_over_a_stream) {
    const CBlock block = MakeBlock(9);
    const CCommitmentBlock out = CommitmentsFromBlock(block);

    CDataStream ss(SER_NETWORK | SER_COMMITMENTS, PROTOCOL_VERSION);
    ss << out;
    CCommitmentBlock in;
    ss >> in;

    BOOST_CHECK(in.GetHash() == block.GetHash());
    BOOST_CHECK_EQUAL(in.CommittedCount(), out.CommittedCount());
    BOOST_CHECK(in.coinbase->GetHash() == block.vtx[0]->GetHash());
    BOOST_CHECK(in.vCommitments == out.vCommitments);
    BOOST_CHECK(in.ComputeMerkleRoot() == block.hashMerkleRoot);
}

// The saving is not a constant: it is the ratio of a body to 32 bytes, so it depends
// entirely on how big the transactions are. With the minimal 1-in/1-out transactions
// above -- about 61 bytes -- an identifier saves barely half, and the header plus the
// carried coinbase eat most of that back. An assertion written as "less than half the
// size" passes or fails on the fixture, not on the design.
//
// So assert the thing that is actually true of the format: each named transaction
// costs 32 bytes regardless of its size. Then show the consequence at the body size
// the design assumes.
BOOST_AUTO_TEST_CASE(each_named_transaction_costs_32_bytes) {
    const CBlock small = MakeBlock(8);
    const CBlock bigger = MakeBlock(9);
    const size_t a = ::GetSerializeSize(CommitmentsFromBlock(small), PROTOCOL_VERSION);
    const size_t b = ::GetSerializeSize(CommitmentsFromBlock(bigger), PROTOCOL_VERSION);
    BOOST_CHECK_EQUAL(b - a, 32U);      // one more identifier, 32 bytes, whatever it names
}

BOOST_AUTO_TEST_CASE(the_saving_grows_with_body_size) {
    // A transaction padded towards a realistic size. The design's storage figures
    // assume ~373 bytes for a 2-in/2-out payment, so that is the interesting regime
    // rather than the minimum-size one.
    auto padded = [](uint32_t nonce, size_t bytes) {
        CMutableTransaction tx;
        tx.nVersion = 1;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint(uint256S(strprintf("%064x", nonce)), nonce);
        tx.vout.resize(1);
        tx.vout[0].nValue = 1000;
        tx.vout[0].scriptPubKey = CScript() << std::vector<unsigned char>(bytes, 0x51);
        return MakeTransactionRef(std::move(tx));
    };
    CBlock block;
    block.nVersion = 4;
    block.nBits = 0x207fffff;
    for (size_t i = 0; i < 64; i++) {
        block.vtx.push_back(padded(i + 1, 340));
    }
    block.hashMerkleRoot = BlockMerkleRoot(block);

    const size_t full = ::GetSerializeSize(block, PROTOCOL_VERSION);
    const size_t committed = ::GetSerializeSize(CommitmentsFromBlock(block), PROTOCOL_VERSION);
    BOOST_CHECK(committed * 5 < full);   // at this body size, better than 5x
}

BOOST_AUTO_TEST_CASE(materialise_rebuilds_the_same_block) {
    const CBlock block = MakeBlock(6);
    const CCommitmentBlock c = CommitmentsFromBlock(block);
    const std::vector <CTransactionRef> bodies(block.vtx.begin() + 1, block.vtx.end());

    CBlock rebuilt;
    BOOST_CHECK(MaterialiseBlock(c, bodies, rebuilt));
    BOOST_CHECK_EQUAL(rebuilt.vtx.size(), block.vtx.size());
    BOOST_CHECK(BlockMerkleRoot(rebuilt) == block.hashMerkleRoot);
    BOOST_CHECK(rebuilt.GetHash() == block.GetHash());
}

// CheckBlock returns early on fChecked. A materialised block that inherited the flag
// from its commitment-only pass would never have its bodies checked, so the fresh
// object is a correctness requirement and not tidiness.
BOOST_AUTO_TEST_CASE(materialise_never_carries_fchecked) {
    const CBlock block = MakeBlock(3);
    const CCommitmentBlock c = CommitmentsFromBlock(block);
    const std::vector <CTransactionRef> bodies(block.vtx.begin() + 1, block.vtx.end());

    CBlock rebuilt;
    rebuilt.fChecked = true;            // as if reused from an earlier pass
    BOOST_CHECK(MaterialiseBlock(c, bodies, rebuilt));
    BOOST_CHECK(!rebuilt.fChecked);
}

BOOST_AUTO_TEST_CASE(materialise_rejects_a_body_that_does_not_match) {
    const CBlock block = MakeBlock(5);
    const CCommitmentBlock c = CommitmentsFromBlock(block);
    std::vector <CTransactionRef> bodies(block.vtx.begin() + 1, block.vtx.end());
    bodies[2] = MakeTx(4242);           // a peer answering with the wrong transaction

    CBlock rebuilt;
    BOOST_CHECK(!MaterialiseBlock(c, bodies, rebuilt));
    BOOST_CHECK(rebuilt.vtx.empty());   // and it leaves nothing half-built
}

BOOST_AUTO_TEST_CASE(materialise_rejects_the_wrong_number_of_bodies) {
    const CBlock block = MakeBlock(5);
    const CCommitmentBlock c = CommitmentsFromBlock(block);
    std::vector <CTransactionRef> bodies(block.vtx.begin() + 1, block.vtx.end());
    bodies.pop_back();

    CBlock rebuilt;
    BOOST_CHECK(!MaterialiseBlock(c, bodies, rebuilt));
}

// The merkle tree cannot answer this. ComputeMerkleRoot compares hashes[pos] against
// hashes[pos+1] for EVEN pos only, so a repeated identifier at an odd boundary is
// never compared and survives every check the tree performs. A commitment block must
// therefore test uniqueness itself.
BOOST_AUTO_TEST_CASE(duplicate_identifiers_are_detectable_when_the_tree_misses_them) {
    CBlock block = MakeBlock(4);
    // Positions 2 and 3 of the leaves are vtx[2] and vtx[3]; make them equal. With
    // the coinbase at leaf 0 that pair sits on an even boundary, so the tree DOES
    // see it -- assert that, to pin the behaviour the design relies on.
    block.vtx[3] = block.vtx[2];
    block.hashMerkleRoot = BlockMerkleRoot(block);
    bool mutated = false;
    BlockMerkleRoot(block, &mutated);
    BOOST_CHECK(mutated);
    BOOST_CHECK(CommitmentsFromBlock(block).HasDuplicateIdentifiers());

    // Now the case the tree misses: the repeat straddles an odd boundary.
    CBlock odd = MakeBlock(5);
    odd.vtx[3] = odd.vtx[2];            // leaves 2,3 equal -> even pair, seen
    odd.vtx[2] = MakeTx(77);            // break that, leaving 3,4 equal -> odd pair
    odd.vtx[4] = odd.vtx[3];
    odd.hashMerkleRoot = BlockMerkleRoot(odd);
    bool mutated2 = false;
    BlockMerkleRoot(odd, &mutated2);
    BOOST_CHECK(!mutated2);             // the tree does not notice
    BOOST_CHECK(CommitmentsFromBlock(odd).HasDuplicateIdentifiers());   // we do
}

BOOST_AUTO_TEST_CASE(an_empty_block_has_no_commitments) {
    CBlock block;
    const CCommitmentBlock c = CommitmentsFromBlock(block);
    BOOST_CHECK(c.IsNull());
    BOOST_CHECK_EQUAL(c.CommittedCount(), 0U);
    CBlock rebuilt;
    BOOST_CHECK(!MaterialiseBlock(c, {}, rebuilt));
}

// The selector is a service bit, not a header bit: whether a block can be carried as
// commitments is a property of the connection, not of the block. These pin the
// negotiation and, more importantly, the limit of what the bit is allowed to mean.
BOOST_AUTO_TEST_CASE(the_service_bit_chooses_the_serialization) {
    BOOST_CHECK(!CanReceiveCommitments(NODE_NETWORK));
    BOOST_CHECK(CanReceiveCommitments(ServiceFlags(NODE_NETWORK | NODE_COMMITMENTS)));

    BOOST_CHECK_EQUAL(BlockSerFlagsFor(NODE_NETWORK), SER_NETWORK);
    BOOST_CHECK_EQUAL(BlockSerFlagsFor(ServiceFlags(NODE_NETWORK | NODE_COMMITMENTS)),
                      SER_NETWORK | SER_COMMITMENTS);

    // Comparing against SER_COMMITMENTS is not enough on its own: if the constant
    // were zero the two assertions above would both still pass while the flag
    // selected nothing. Pin the bit, and pin that the two answers differ.
    BOOST_CHECK(SER_COMMITMENTS != 0);
    BOOST_CHECK_EQUAL(SER_COMMITMENTS & (SER_NETWORK | SER_DISK | SER_GETHASH), 0);
    BOOST_CHECK(BlockSerFlagsFor(ServiceFlags(NODE_NETWORK | NODE_COMMITMENTS)) !=
                BlockSerFlagsFor(NODE_NETWORK));

    // In the experimental range, so it cannot collide with an upstream assignment
    // while the format is unactivated.
    BOOST_CHECK(NODE_COMMITMENTS >= (1u << 24));
    // And it must not overlap anything already in use.
    const uint64_t inUse = NODE_NETWORK | NODE_GETUTXO | NODE_BLOOM | NODE_XTHIN | NODE_NETWORK_LIMITED;
    BOOST_CHECK_EQUAL(NODE_COMMITMENTS & inUse, 0U);
}

// F-85: sending the commitment-form getdata inv type to a peer who merely claims
// NODE_COMMITMENTS wedges them if the claim is a stray bit-24 collision rather than
// real support (ProcessGetData never erases an unknown front item). The handshake
// exists to require an explicit reply before that ever happens; this pins when we
// offer it in the first place -- to a peer who has claimed the bit, and only when
// we ourselves actually support it.
BOOST_AUTO_TEST_CASE(sendcommitments_is_offered_only_when_both_sides_claim_the_bit) {
    BOOST_CHECK(ShouldNegotiateCommitments(ServiceFlags(NODE_NETWORK | NODE_COMMITMENTS),
                                            ServiceFlags(NODE_NETWORK | NODE_COMMITMENTS)));

    // We don't support it ourselves -- nothing to negotiate.
    BOOST_CHECK(!ShouldNegotiateCommitments(NODE_NETWORK,
                                             ServiceFlags(NODE_NETWORK | NODE_COMMITMENTS)));

    // Peer hasn't claimed it -- no point offering.
    BOOST_CHECK(!ShouldNegotiateCommitments(ServiceFlags(NODE_NETWORK | NODE_COMMITMENTS),
                                             NODE_NETWORK));

    // Neither side claims it.
    BOOST_CHECK(!ShouldNegotiateCommitments(NODE_NETWORK, NODE_NETWORK));
}

// The flag is declared and negotiated, and NOTHING READS IT: no Serialize in the
// tree tests SER_COMMITMENTS (the GetType() consumers all test SER_GETHASH or
// SER_DISK). So today it selects nothing, and this pins that rather than letting
// the negotiation above read as working machinery. Whoever wires a real selector
// will see this fail and must update it deliberately.
//
// Recorded because the docs retract the stream-flag selector (F-80..F-82, R-28b)
// while the symbols are still here pending a decision on the replacement.
BOOST_AUTO_TEST_CASE(the_stream_flag_currently_selects_nothing) {
    const CBlock block = MakeBlock(4);

    CDataStream plain(SER_NETWORK, PROTOCOL_VERSION);
    plain << block;
    CDataStream flagged(SER_NETWORK | SER_COMMITMENTS, PROTOCOL_VERSION);
    flagged << block;

    BOOST_CHECK(plain.size() == flagged.size());
    BOOST_CHECK(std::equal(plain.begin(), plain.end(), flagged.begin()));
}

// A service bit is an unauthenticated advertisement. A peer may claim the bit and
// answer a body request with the wrong transaction, and the bit must buy it nothing:
// verification is against the identifiers, never against the claim.
BOOST_AUTO_TEST_CASE(advertising_the_bit_earns_no_trust) {
    const CBlock block = MakeBlock(4);
    const CCommitmentBlock c = CommitmentsFromBlock(block);
    std::vector <CTransactionRef> lies(block.vtx.begin() + 1, block.vtx.end());
    lies[1] = MakeTx(31337);

    BOOST_CHECK(CanReceiveCommitments(ServiceFlags(NODE_NETWORK | NODE_COMMITMENTS)));
    CBlock rebuilt;
    BOOST_CHECK(!MaterialiseBlock(c, lies, rebuilt));   // the claim changes nothing
}

// SetNull() and the default constructor leave coinbase null, and the shared_ptr
// serializer dereferences unconditionally -- so before the guard, putting a
// default-constructed instance on a stream segfaulted. It is the design's wire
// type; a fault is not an acceptable way to reject one.
BOOST_AUTO_TEST_CASE(a_null_commitment_block_inspects_safely_and_refuses_the_wire) {
    const CCommitmentBlock c = CommitmentsFromBlock(CBlock());
    BOOST_REQUIRE(c.IsNull());

    // Every accessor must tolerate it rather than deref.
    BOOST_CHECK_EQUAL(c.CommittedCount(), 0U);
    BOOST_CHECK(c.Identifiers().empty());
    bool mutated = false;
    BOOST_CHECK(c.ComputeMerkleRoot(&mutated) == uint256());
    BOOST_CHECK(!c.HasDuplicateIdentifiers());

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    BOOST_CHECK_THROW(ss << c, std::ios_base::failure);
}

// The count check has two sides and only "too few" was covered.
BOOST_AUTO_TEST_CASE(a_surplus_body_is_rejected_too) {
    const CBlock block = MakeBlock(4);
    const CCommitmentBlock c = CommitmentsFromBlock(block);

    std::vector <CTransactionRef> surplus(block.vtx.begin() + 1, block.vtx.end());
    surplus.push_back(MakeTx(999));
    BOOST_REQUIRE(surplus.size() == c.vCommitments.size() + 1);

    CBlock rebuilt;
    BOOST_CHECK(!MaterialiseBlock(c, surplus, rebuilt));
}

BOOST_AUTO_TEST_SUITE_END()

// The read API split, against a real chain on disk. The two reads differ by
// contract: a node holding no bodies must still answer for its commitments. If
// both reads fail together the split is decoration, so the test is the asymmetry
// itself rather than either read in isolation.
BOOST_FIXTURE_TEST_SUITE(commitmentblock_read_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(commitments_readable_when_bodies_are_not) {
    const CBlockIndex *pindex = ::ChainActive().Tip();
    const Consensus::Params &params = Params().GetConsensus();

    // Baseline: with bodies held, both reads answer and agree on the block.
    CBlock whole;
    BOOST_REQUIRE(ReadBlockFromDisk(whole, pindex, params));
    CCommitmentBlock c;
    BOOST_REQUIRE(ReadCommitmentBlockFromDisk(c, pindex, params));
    BOOST_CHECK_EQUAL(c.GetHash().ToString(), pindex->GetBlockHash().ToString());
    BOOST_CHECK_EQUAL(c.CommittedCount(), whole.vtx.size());  // coinbase included
    BOOST_CHECK_EQUAL(c.vCommitments.size(), whole.vtx.size() - 1);

    // Now withhold this block's bodies -- the probe's predicate, which is how
    // "commitments held, bodies missing" is reachable before the body store exists.
    g_perf_withhold_hashes.insert(pindex->GetBlockHash());

    CBlock denied;
    BOOST_CHECK(!ReadBlockFromDisk(denied, pindex, params));  // materialising read refused

    CCommitmentBlock still;
    BOOST_CHECK(ReadCommitmentBlockFromDisk(still, pindex, params));  // commitments still answer
    BOOST_CHECK_EQUAL(still.GetHash().ToString(), pindex->GetBlockHash().ToString());
    BOOST_CHECK(still.Identifiers() == c.Identifiers());

    g_perf_withhold_hashes.erase(pindex->GetBlockHash());
}

// A commitment read must carry enough to rebuild the block, so pair it with the
// bodies and check the round trip end to end through disk.
BOOST_AUTO_TEST_CASE(a_disk_commitment_read_rebuilds_the_block) {
    const CBlockIndex *pindex = ::ChainActive().Tip();
    const Consensus::Params &params = Params().GetConsensus();

    CBlock whole;
    BOOST_REQUIRE(ReadBlockFromDisk(whole, pindex, params));
    CCommitmentBlock c;
    BOOST_REQUIRE(ReadCommitmentBlockFromDisk(c, pindex, params));

    std::vector <CTransactionRef> bodies(whole.vtx.begin() + 1, whole.vtx.end());
    CBlock rebuilt;
    BOOST_REQUIRE(MaterialiseBlock(c, bodies, rebuilt));
    BOOST_CHECK_EQUAL(rebuilt.GetHash().ToString(), whole.GetHash().ToString());
    BOOST_CHECK_EQUAL(rebuilt.hashMerkleRoot.ToString(), whole.hashMerkleRoot.ToString());
    // ConnectBlock re-invokes CheckBlock and that early-returns on fChecked, so a
    // materialised block must arrive unchecked however it was obtained.
    BOOST_CHECK(!rebuilt.fChecked);
}

// ReadCommitmentBlockFromDisk skips the CBlockIndex overload (that is how it
// bypasses the withholding predicate) and re-adds that overload's
// hash-matches-index guard itself. Reading the tip can never exercise it, so point
// the tip's entry at another block's bytes and require the refusal.
BOOST_AUTO_TEST_CASE(a_commitment_read_refuses_bytes_that_are_not_the_indexed_block) {
    CBlockIndex *tip = const_cast<CBlockIndex *>(::ChainActive().Tip());
    const CBlockIndex *prev = tip->pprev;
    BOOST_REQUIRE(prev != nullptr);
    const Consensus::Params &params = Params().GetConsensus();

    const int savedFile = tip->nFile;
    const unsigned int savedPos = tip->nDataPos;
    tip->nFile = prev->nFile;
    tip->nDataPos = prev->nDataPos;

    CCommitmentBlock c;
    BOOST_CHECK(!ReadCommitmentBlockFromDisk(c, tip, params));

    tip->nFile = savedFile;
    tip->nDataPos = savedPos;
    // and the restore must leave the read working, or the check above proved nothing
    BOOST_CHECK(ReadCommitmentBlockFromDisk(c, tip, params));
}

BOOST_AUTO_TEST_SUITE_END()
