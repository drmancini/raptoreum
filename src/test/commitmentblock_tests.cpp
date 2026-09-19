// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <evo/evodb.h>
#include <node/context.h>
#include <pow.h>
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

// F6 (test review precedent, txvalidation_tests.cpp/blockbudget_tests.cpp): a
// thrown BOOST_REQUIRE between setting g_commitmentBudgetActive = true and
// resetting it would leave the global on for every later test in the process.
struct CommitmentBudgetGuard {
    ~CommitmentBudgetGuard() { g_commitmentBudgetActive = false; }
};

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

// 1.3.2 (F-83, Mike, 2026-09-19): CheckCommitmentBlock/ContextualCheckCommitmentBlock,
// the validation entry point that was entirely missing -- ComputeMerkleRoot()
// and HasDuplicateIdentifiers() had zero production callers until this. Real,
// validly-mined blocks from TestChain100Setup are used throughout rather than
// hand-built ones, so every test starts from a genuinely DIP3-correct
// coinbase (nType, nVersion) and a real founder-payment-exempt height
// (regtest's founder payment only starts at 500) -- getting those details
// right by hand, for every test, would risk the tests passing for the wrong
// reason.
BOOST_FIXTURE_TEST_SUITE(checkcommitmentblock_tests, TestChain100Setup)

static CCommitmentBlock RealCommitmentBlock(int *outHeight) {
    const CBlockIndex *pindex = ::ChainActive().Tip();
    CBlock whole;
    BOOST_REQUIRE(ReadBlockFromDisk(whole, pindex, Params().GetConsensus()));
    *outHeight = pindex->nHeight;
    return CommitmentsFromBlock(whole);
}

BOOST_AUTO_TEST_CASE(checkcommitmentblock_accepts_a_genuine_block) {
    int height;
    CCommitmentBlock c = RealCommitmentBlock(&height);

    CValidationState state;
    BOOST_CHECK(CheckCommitmentBlock(c, state, Params().GetConsensus(), height,
                                     /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "");

    const CBlockIndex *pindexPrev = ::ChainActive().Tip()->pprev;
    CValidationState state2;
    BOOST_CHECK(ContextualCheckCommitmentBlock(c, state2, Params().GetConsensus(), pindexPrev));
    BOOST_CHECK_EQUAL(state2.GetRejectReason(), "");
}

BOOST_AUTO_TEST_CASE(checkcommitmentblock_rejects_a_null_block) {
    CCommitmentBlock c;   // default-constructed: SetNull(), coinbase == nullptr
    BOOST_REQUIRE(c.IsNull());

    // fCheckPOW=false: a null header's zero-valued fields fail the PoW check
    // first, which would test CheckBlockHeader's own reject reason ("high-hash")
    // rather than the bad-cb-missing rule this test targets.
    CValidationState state;
    BOOST_CHECK(!CheckCommitmentBlock(c, state, Params().GetConsensus(), 1,
                                      /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cb-missing");
}

// A malicious peer controls the wire form of this field -- the C++ type
// alone does not guarantee it. Checked AFTER the merkle root (H-1, Fable
// review, 2026-09-19: the original ordering had this before the root, with
// corruption=false -- inverting CheckBlock's own "corruption checks first"
// invariant, since a wrong-coinbase pairing under a genuine header is
// corruption, not the header's fault). The root is recomputed here so the
// pairing is otherwise self-consistent and the ONLY failure reachable is
// the IsCoinBase() guard itself, not an (now upstream) root mismatch.
BOOST_AUTO_TEST_CASE(checkcommitmentblock_rejects_a_non_coinbase_in_the_coinbase_field) {
    int height;
    CCommitmentBlock c = RealCommitmentBlock(&height);

    CMutableTransaction notCoinbase;
    notCoinbase.vin.resize(1);
    notCoinbase.vin[0].prevout = COutPoint(uint256S("1"), 0);   // non-null: not a coinbase
    notCoinbase.vout.resize(1);
    notCoinbase.vout[0].nValue = 1000;
    notCoinbase.vout[0].scriptPubKey = CScript() << OP_TRUE;
    c.coinbase = MakeTransactionRef(notCoinbase);
    BOOST_REQUIRE(!c.coinbase->IsCoinBase());
    c.hashMerkleRoot = c.ComputeMerkleRoot();   // self-consistent: isolates the IsCoinBase() check

    // fCheckPOW=false: the recomputed root changes the header hash (same
    // reason as the wrong-merkle-root test below).
    CValidationState state;
    BOOST_CHECK(!CheckCommitmentBlock(c, state, Params().GetConsensus(), height,
                                      /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cb-missing");
}

// H-1's actual scenario, not merely a differently-worded repeat of the test
// above: a genuine header (this real block's own hashMerkleRoot, left
// UNCHANGED) paired with a coinbase that does not belong to it. The reject
// REASON alone cannot distinguish check ordering here -- ANY ordering that
// rejects this input can say "bad-cb-missing" or "bad-txnmrklroot" and both
// are "rejected". What ordering actually controls is the corruption flag:
// mismatched data under an otherwise-genuine header is corruption
// (state.CorruptionPossible() must be true, matching CheckBlock's own
// hashMerkleRoot-mismatch treatment), not a fault of the header itself. The
// H-1 bug had the IsCoinBase() check firing FIRST with corruption=false,
// which would have made AcceptBlock-shaped wiring permanently invalidate a
// perfectly genuine header over a coinbase substitution that was never its
// fault.
BOOST_AUTO_TEST_CASE(checkcommitmentblock_treats_a_mismatched_pairing_as_corruption) {
    int height;
    CCommitmentBlock c = RealCommitmentBlock(&height);

    CMutableTransaction notCoinbase;
    notCoinbase.vin.resize(1);
    notCoinbase.vin[0].prevout = COutPoint(uint256S("1"), 0);
    notCoinbase.vout.resize(1);
    notCoinbase.vout[0].nValue = 1000;
    notCoinbase.vout[0].scriptPubKey = CScript() << OP_TRUE;
    c.coinbase = MakeTransactionRef(notCoinbase);
    // hashMerkleRoot deliberately NOT recomputed: this header is genuine and
    // unrelated to the swap, which is exactly the pairing a peer controls.

    CValidationState state;
    BOOST_CHECK(!CheckCommitmentBlock(c, state, Params().GetConsensus(), height,
                                      /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/true));
    BOOST_CHECK(state.CorruptionPossible());
}

BOOST_AUTO_TEST_CASE(checkcommitmentblock_rejects_a_wrong_merkle_root) {
    int height;
    CCommitmentBlock c = RealCommitmentBlock(&height);
    c.hashMerkleRoot = uint256S("dead");

    // fCheckPOW=false: hashMerkleRoot is part of what the header hash covers,
    // so mutating it invalidates the block's real PoW too -- fCheckPOW=true
    // would fail on "high-hash" before ever reaching the check under test.
    CValidationState state;
    BOOST_CHECK(!CheckCommitmentBlock(c, state, Params().GetConsensus(), height,
                                      /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txnmrklroot");
}

// F-43b: [coinbase, id, id, ...] puts a duplicate at merkle positions 1,2 --
// ComputeMerkleRoot's malleation check compares (0,1),(2,3),... (EVEN
// positions only), so this duplicate is never compared and malleation
// passes. The root is recomputed over the mutated list so the ONLY failure
// exercised is the new HasDuplicateIdentifiers rule, not a root mismatch.
BOOST_AUTO_TEST_CASE(checkcommitmentblock_rejects_duplicate_ids_the_merkle_check_misses) {
    int height;
    CCommitmentBlock c = RealCommitmentBlock(&height);
    // Test review (2026-09-19): CheckCommitmentBlock never resolves an
    // identifier to a body, so any fabricated uint256 pair tests the same
    // thing without depending on this fixture's real commitment list --
    // the original version relied on c.vCommitments[0] existing, which is
    // only true because regtest height 100 happens to fall in an LLMQ DKG
    // mining window (dkgInterval=30, window 10..18) and so carries a
    // quorum-commitment identifier; a 99- or 101-block fixture would have
    // broken this test with no hint why.
    c.vCommitments = {uint256S("aa"), uint256S("aa")};

    bool mutated = false;
    c.hashMerkleRoot = c.ComputeMerkleRoot(&mutated);
    BOOST_REQUIRE(!mutated);   // confirms the scenario: malleation does NOT see this duplicate
    BOOST_REQUIRE(c.HasDuplicateIdentifiers());

    // fCheckPOW=false: the recomputed root changes the header hash, same
    // reason as checkcommitmentblock_rejects_a_wrong_merkle_root above.
    CValidationState state;
    BOOST_CHECK(!CheckCommitmentBlock(c, state, Params().GetConsensus(), height,
                                      /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cmt-duplicate-ids");
    // Fable review, 2026-09-19: reached only after the merkle root already
    // matched, so this is a fact about what the header itself commits to,
    // not corruption in transit -- corruption=true here would mean
    // AcceptBlock never stamps BLOCK_FAILED_VALID, and every peer whose best
    // chain includes this header would re-request an unfillable block
    // forever.
    BOOST_CHECK(!state.CorruptionPossible());
}

// M-1 (Fable review, 2026-09-19): ContextualCheckCommitmentBlock also calls
// ContextualCheckTransaction on the coinbase, mirroring ContextualCheckBlock's
// own per-tx loop -- and that function's tx.IsCoinBase() && nType!=COINBASE
// check (reject reason bad-txns-cb-type) fires before this function's own,
// separately-named bad-cb-type check ever gets reached, exactly as it does
// in ContextualCheckBlock (the loop runs before the block's own trailing
// bad-cb-type check). The separate check is therefore reachable only when
// DIP3 is off, in which case ContextualCheckTransaction's equivalent check
// is skipped identically -- kept for symmetry with ContextualCheckBlock's
// shape and to document SS2.4's row, not because it fires first here.
BOOST_AUTO_TEST_CASE(contextualcheckcommitmentblock_rejects_a_non_cbtx_coinbase) {
    int height;
    CCommitmentBlock c = RealCommitmentBlock(&height);

    CMutableTransaction mutCoinbase(*c.coinbase);
    mutCoinbase.nType = TRANSACTION_NORMAL;   // was TRANSACTION_COINBASE
    c.coinbase = MakeTransactionRef(mutCoinbase);
    BOOST_REQUIRE(c.coinbase->IsCoinBase());   // still a coinbase by null-prevout; just the wrong DIP3 type
    // Self-consistent: a real accept pipeline would reach ContextualCheckCommitmentBlock
    // only after CheckCommitmentBlock's own merkle check already passed.
    c.hashMerkleRoot = c.ComputeMerkleRoot();

    const CBlockIndex *pindexPrev = ::ChainActive().Tip()->pprev;
    CValidationState state;
    BOOST_CHECK(!ContextualCheckCommitmentBlock(c, state, Params().GetConsensus(), pindexPrev));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-cb-type");
}

BOOST_AUTO_TEST_CASE(contextualcheckcommitmentblock_rejects_a_nonfinal_coinbase) {
    int height;
    CCommitmentBlock c = RealCommitmentBlock(&height);

    CMutableTransaction mutCoinbase(*c.coinbase);
    mutCoinbase.nLockTime = 999999;      // height-based, far beyond any test chain height
    mutCoinbase.vin[0].nSequence = 1;    // not SEQUENCE_FINAL -- required for nLockTime to bind
    c.coinbase = MakeTransactionRef(mutCoinbase);
    // Self-consistent: nLockTime/nSequence change the coinbase's own hash
    // (Identifiers()'s leaf 0), same reachability reasoning as the
    // bad-cb-type test above.
    c.hashMerkleRoot = c.ComputeMerkleRoot();

    const CBlockIndex *pindexPrev = ::ChainActive().Tip()->pprev;
    CValidationState state;
    BOOST_CHECK(!ContextualCheckCommitmentBlock(c, state, Params().GetConsensus(), pindexPrev));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-nonfinal");
}

BOOST_AUTO_TEST_SUITE_END()

// 1.3.2 (F-44, Mike, 2026-09-19): the connect-time relocation of the two
// rules ContextualCheckBlock enforces but ConnectBlock, by design, never
// re-checks (D-16's own reasoning: BLOCK_VALID_TRANSACTIONS is granted
// without re-invoking ContextualCheckBlock, so these need a connect-time
// home or they silently stop being enforced). IMPORTANT SCOPE NOTE: in
// TODAY's accept pipeline, AcceptBlock always calls the full CheckBlock +
// ContextualCheckBlock on the complete block BEFORE ever reaching connect,
// regardless of g_commitmentBudgetActive -- so these relocated checks are
// currently REDUNDANT with (shadowed by) ContextualCheckBlock's own per-tx
// loop on every real path (ProcessNewBlock, TestBlockValidity). They become
// load-bearing only once a future step makes AcceptBlock skip the full
// per-tx ContextualCheckBlock work for a genuine commitment-only arrival
// (phase 2's body store), which is why they are built now per D-16 but
// exercised here by calling ConnectBlock DIRECTLY -- the only way to reach
// them without ContextualCheckBlock intercepting first, today.
BOOST_FIXTURE_TEST_SUITE(connectblock_relocated_rules_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(connectblock_enforces_nonfinal_coinbase_when_relocation_is_active) {
    CBlock block = CreateBlock({}, coinbaseKey);

    CMutableTransaction mutCoinbase(*block.vtx[0]);
    mutCoinbase.nLockTime = 999999;      // height-based, far beyond this chain's height
    mutCoinbase.vin[0].nSequence = 1;    // not SEQUENCE_FINAL -- required for nLockTime to bind
    block.vtx[0] = MakeTransactionRef(mutCoinbase);
    // Deliberately NOT re-solving PoW or the merkle root: ConnectBlock's own
    // internal CheckBlock re-check runs with fCheckPOW=fCheckMerkleRoot=false
    // under fJustCheck=true, so neither is exercised at this level.

    CBlockIndex *pindexPrev = ::ChainActive().Tip();
    CCoinsViewCache viewNew(&::ChainstateActive().CoinsTip());
    uint256 block_hash(block.GetHash());
    CBlockIndex indexDummy(block);
    indexDummy.pprev = pindexPrev;
    indexDummy.nHeight = pindexPrev->nHeight + 1;
    indexDummy.phashBlock = &block_hash;

    LOCK(cs_main);
    auto dbTx = evoDb->BeginTransaction();   // rolled back when dbTx goes out of scope

    CommitmentBudgetGuard guard;
    g_commitmentBudgetActive = true;
    CValidationState state;
    bool ok = ::ChainstateActive().ConnectBlock(block, state, &indexDummy, viewNew, Params(), passetsCache.get(),
                                                /*fJustCheck=*/true);

    BOOST_CHECK(!ok);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-nonfinal");
}

// Test review, 2026-09-19 (MEDIUM): the sibling test above only exercises
// IsFinalTx -- deleting the ContextualCheckTransaction call entirely would
// still pass it. This isolates that call specifically, on a NON-coinbase
// transaction (the "every transaction including the coinbase" claim's other
// half, also untested until now), using bad-txns-type (a v1/v2 transaction
// carrying a nonzero nType) since it needs no real spendable UTXO: the fake
// prevout is never reached because the relocated check now runs FIRST in
// the loop body, before CheckTxInputs.
BOOST_AUTO_TEST_CASE(connectblock_enforces_type_check_on_a_non_coinbase_tx) {
    CBlock block = CreateBlock({}, coinbaseKey);

    CMutableTransaction badTypeTx;
    badTypeTx.nVersion = 1;   // pre-DIP2: nType must be TRANSACTION_NORMAL
    badTypeTx.nType = 99;     // wire-reachable regardless of nVersion -- transaction.h
                             // unpacks nType from the high 16 bits unconditionally
    badTypeTx.vin.resize(1);
    badTypeTx.vin[0].prevout = COutPoint(uint256S("1"), 0);   // never resolved: rejected before CheckTxInputs
    badTypeTx.vout.resize(1);
    badTypeTx.vout[0].nValue = 1000;
    badTypeTx.vout[0].scriptPubKey = CScript() << OP_TRUE;
    block.vtx.push_back(MakeTransactionRef(badTypeTx));

    CBlockIndex *pindexPrev = ::ChainActive().Tip();
    CCoinsViewCache viewNew(&::ChainstateActive().CoinsTip());
    uint256 block_hash(block.GetHash());
    CBlockIndex indexDummy(block);
    indexDummy.pprev = pindexPrev;
    indexDummy.nHeight = pindexPrev->nHeight + 1;
    indexDummy.phashBlock = &block_hash;

    LOCK(cs_main);
    auto dbTx = evoDb->BeginTransaction();

    CommitmentBudgetGuard guard;
    g_commitmentBudgetActive = true;
    CValidationState state;
    bool ok = ::ChainstateActive().ConnectBlock(block, state, &indexDummy, viewNew, Params(), passetsCache.get(),
                                                /*fJustCheck=*/true);

    BOOST_CHECK(!ok);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-type");
    // Pin the DoS level too: ContextualCheckBlock's per-tx loop (the path
    // this shadows on every real accept today) and this relocated site both
    // score bad-txns-type at 100 via ContextualCheckTransaction directly --
    // distinct from the relocated nLockTime check's own DoS(10), so this
    // also confirms the two are genuinely different call sites, not the same
    // check reporting under two names.
    int nDoS = 0;
    BOOST_REQUIRE(state.IsInvalid(nDoS));
    BOOST_CHECK_EQUAL(nDoS, 100);
}

BOOST_AUTO_TEST_CASE(connectblock_does_not_enforce_relocated_rules_when_flag_is_off) {
    // Control: the identical mutated block, with the flag off, must connect
    // -- proving the relocation is genuinely gated, not accidentally always-on.
    CBlock block = CreateBlock({}, coinbaseKey);

    CMutableTransaction mutCoinbase(*block.vtx[0]);
    mutCoinbase.nLockTime = 999999;
    mutCoinbase.vin[0].nSequence = 1;
    block.vtx[0] = MakeTransactionRef(mutCoinbase);

    CBlockIndex *pindexPrev = ::ChainActive().Tip();
    CCoinsViewCache viewNew(&::ChainstateActive().CoinsTip());
    uint256 block_hash(block.GetHash());
    CBlockIndex indexDummy(block);
    indexDummy.pprev = pindexPrev;
    indexDummy.nHeight = pindexPrev->nHeight + 1;
    indexDummy.phashBlock = &block_hash;

    LOCK(cs_main);
    auto dbTx = evoDb->BeginTransaction();

    BOOST_REQUIRE(!g_commitmentBudgetActive);   // explicit: this is the off state
    CValidationState state;
    bool ok = ::ChainstateActive().ConnectBlock(block, state, &indexDummy, viewNew, Params(), passetsCache.get(),
                                                /*fJustCheck=*/true);

    BOOST_CHECK_NE(state.GetRejectReason(), "bad-txns-nonfinal");
    BOOST_CHECK(ok);
}

BOOST_AUTO_TEST_SUITE_END()

// 1.3.2 (F-83, Mike, 2026-09-19): proves AcceptBlock's new CheckCommitmentBlock/
// ContextualCheckCommitmentBlock call is actually reached, using the one rule
// that is a genuine ADDITION over CheckBlock rather than a reachable-anyway
// duplicate: bad-cmt-duplicate-ids (F-43b). A block naming the same
// transaction twice, at an ODD merkle position, passes CheckBlock/
// ContextualCheckBlock cleanly -- merkle malleation only compares EVEN
// positions, and no within-block double-spend check exists at CheckBlock's
// level (that's ConnectBlock/CheckTxInputs, never reached here) -- so a
// rejection with exactly this reason can only have come from the new call.
BOOST_FIXTURE_TEST_SUITE(acceptblock_commitment_wiring_tests, TestChain100Setup)

// Test review, 2026-09-19: mines past regtest's DKG window (dkgInterval=30,
// window 10..18), returning a coinbase-only block at the resulting tip + 1.
// Bounded rather than `while (true)`, and asserts the tip actually advances
// each iteration -- an unbounded loop over a call whose return value is
// discarded (CreateAndProcessBlock) would spin forever on a CI hang, not a
// red test, if any mined block were ever rejected.
static CBlock MineToOutsideDkgWindowAndBuild(TestChainSetup &fixture, const CKey &coinbaseKey) {
    for (int i = 0; i < 30; i++) {
        int nextHeightMod = (::ChainActive().Tip()->nHeight + 1) % 30;
        if (nextHeightMod < 10 || nextHeightMod > 18) break;
        int heightBefore = ::ChainActive().Tip()->nHeight;
        fixture.CreateAndProcessBlock({}, coinbaseKey);
        BOOST_REQUIRE_GT(::ChainActive().Tip()->nHeight, heightBefore);
    }
    CBlock block = fixture.CreateBlock({}, coinbaseKey);
    BOOST_REQUIRE_EQUAL(block.vtx.size(), 1U);   // coinbase only, confirming the window is cleared
    return block;
}

BOOST_AUTO_TEST_CASE(acceptblock_calls_checkcommitmentblock) {
    CBlock block = MineToOutsideDkgWindowAndBuild(*this, coinbaseKey);

    CTransactionRef dupTx = commitmentblock_tests::MakeTx(1);
    block.vtx.push_back(dupTx);
    block.vtx.push_back(dupTx);   // SAME transaction twice -> positions 1,2: odd boundary
    block.hashMerkleRoot = BlockMerkleRoot(block);
    while (!CheckProofOfWork(block.GetPOWHash(), block.nBits, Params().GetConsensus())) ++block.nNonce;

    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
    CBlockIndex *pindex = nullptr;

    // AcceptBlock directly (not ProcessNewBlock, which manages its own
    // internal CValidationState and doesn't expose it) so the reject reason
    // is observable -- proving THIS specific check fired, not merely that
    // the block was rejected for some unrelated reason.
    LOCK(cs_main);
    CommitmentBudgetGuard guard;
    g_commitmentBudgetActive = true;
    CValidationState state;
    bool ok = ::ChainstateActive().AcceptBlock(shared_pblock, state, Params(), &pindex, /*fRequested=*/true,
                                               nullptr, nullptr);

    BOOST_CHECK(!ok);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cmt-duplicate-ids");

    // Test review, 2026-09-19 (HIGH): the commit's actual claim is ORDERING
    // -- this check must run before nTx/nChainTx/HAVE_DATA are recorded --
    // which the reject-reason assertion alone does not prove (it would pass
    // identically even if the new check ran AFTER SaveBlockToDisk /
    // ReceivedBlockTransactions). Assert the index entry directly.
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_CHECK_EQUAL(pindex->nTx, 0U);
    BOOST_CHECK(!(pindex->nStatus & BLOCK_HAVE_DATA));
    // bad-cmt-duplicate-ids is corruption=false (this header commits to an
    // unfillable list, its own fault -- see the fix above), so this is NOT
    // asserting "never marked failed"; it specifically proves AcceptBlock's
    // failure-handling stamped BLOCK_FAILED_VALID as the correct response to
    // a non-corruption-possible rejection.
    BOOST_CHECK(pindex->nStatus & BLOCK_FAILED_VALID);
}

// Test review, 2026-09-19 (MEDIUM): the commit message's central claim --
// "without this wiring the identical block is fully ACCEPTED" -- was
// asserted only in prose, not pinned as a test. Sibling to the test above,
// flag off, otherwise identical.
BOOST_AUTO_TEST_CASE(acceptblock_does_not_call_checkcommitmentblock_when_flag_is_off) {
    CBlock block = MineToOutsideDkgWindowAndBuild(*this, coinbaseKey);

    CTransactionRef dupTx = commitmentblock_tests::MakeTx(2);
    block.vtx.push_back(dupTx);
    block.vtx.push_back(dupTx);
    block.hashMerkleRoot = BlockMerkleRoot(block);
    while (!CheckProofOfWork(block.GetPOWHash(), block.nBits, Params().GetConsensus())) ++block.nNonce;

    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
    CBlockIndex *pindex = nullptr;

    LOCK(cs_main);
    BOOST_REQUIRE(!g_commitmentBudgetActive);   // explicit: this is the off state
    CValidationState state;
    bool ok = ::ChainstateActive().AcceptBlock(shared_pblock, state, Params(), &pindex, /*fRequested=*/true,
                                               nullptr, nullptr);

    BOOST_CHECK(ok);
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_CHECK(pindex->nStatus & BLOCK_HAVE_DATA);
    BOOST_CHECK_NE(state.GetRejectReason(), "bad-cmt-duplicate-ids");
}

BOOST_AUTO_TEST_SUITE_END()
