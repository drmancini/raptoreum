// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// 1.3.1 (Mike, 2026-09-19): the real per-block BLOCK_HAVE_BODIES bit, ported
// and hardened from probe/acceptance-layer (a2d5f68c9, 37a7c37a0) -- the
// commitment block is always written (BLOCK_HAVE_DATA stays honest), and
// BLOCK_HAVE_BODIES is set separately by ReceivedBlockBodies. F-100 corrected
// the plan draft that described a vacuous encoding where the bit collapsed
// onto BLOCK_HAVE_DATA; these tests exercise the real, distinct bit.

#include <algorithm>
#include <amount.h>
#include <bodyrange.h>
#include <bodystore.h>
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/validation.h>
#include <index/txindex.h>
#include <key.h>
#include <keystore.h>
#include <llmq/quorums_blockprocessor.h>
#include <node/context.h>
#include <primitives/block.h>
#include <pubkey.h>
#include <fs.h>
#include <interfaces/chain.h>
#include <rpc/client.h>
#include <rpc/server.h>
#include <script/script.h>
#include <script/sign.h>
#include <script/standard.h>
#include <streams.h>
#include <txmempool.h>
#include <util/ref.h>
#include <util/system.h>
#include <validation.h>
#include <validationinterface.h>
#include <test/test_raptoreum.h>

#include <boost/algorithm/string.hpp>
#include <boost/test/unit_test.hpp>

// F7 (test review precedent, txvalidation_tests.cpp/blockbudget_tests.cpp):
// a thrown BOOST_REQUIRE between inserting into g_perf_withhold_* and erasing
// would leave the flag set for every later test in the process.
struct PerfWithholdGuard {
    uint256 hash;

    explicit PerfWithholdGuard(const uint256 &hashIn) : hash(hashIn) {
        g_perf_withhold_hashes.insert(hash);
    }

    ~PerfWithholdGuard() { g_perf_withhold_hashes.erase(hash); }
};

// 4.1.3: build-plan.md's own 4.1.3 row claims NewPoWValidBlock (fast
// compact-block announce, net_processing.cpp:1781) and UpdatedBlockTip's
// INV/header announce (:1826) are "safe by construction" against a
// commitment-only block -- both only ever run against a pindex/pblock that
// is already fully connected or fully in memory. This spy records every
// real call so a test can confirm that, not just reason about it.
// NewPoWValidBlock fires synchronously (CMainSignals::NewPoWValidBlock
// calls straight through, validationinterface.cpp); UpdatedBlockTip is
// queued through CMainSignals' own scheduler client instead (unlike
// NewPoWValidBlock, its dispatcher wraps the call in
// m_schedulerClient.AddToProcessQueue) -- callers must
// SyncWithValidationInterfaceQueue() before reading
// spy.updatedBlockTipCalls, or a real call can still be sitting in the
// queue, unobserved (found the hard way: an earlier version of this test
// read updatedBlockTipCalls without draining the queue first and its own
// positive control failed as a result).
class RelayAtomicitySpy : public CValidationInterface {
public:
    std::vector<const CBlockIndex *> newPoWValidBlockCalls;
    std::vector<const CBlockIndex *> updatedBlockTipCalls;

protected:
    void NewPoWValidBlock(const CBlockIndex *pindex, const std::shared_ptr<const CBlock> &) override {
        newPoWValidBlockCalls.push_back(pindex);
    }

    void UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *, bool) override {
        updatedBlockTipCalls.push_back(pindexNew);
    }
};

// F-168 (4.1.1 closeout): a minimal CallRPC, same technique as
// rpc_tests.cpp's RPCTestingSetup::CallRPC (CLI-style string args ->
// RPCConvertValues -> tableRPC.execute), reimplemented here rather than
// shared because that class is private to rpc_tests.cpp and this file's
// fixture is TestChain100Setup, not RPCTestingSetup -- TestingSetup's own
// constructor already calls RegisterAllCoreRPCCommands(tableRPC), which
// TestChain100Setup inherits transitively, so no separate registration is
// needed here.
static UniValue CallRPCForTest(NodeContext &node, const std::string &args) {
    std::vector<std::string> vArgs;
    boost::split(vArgs, args, boost::is_any_of(" \t"));
    std::string strMethod = vArgs[0];
    vArgs.erase(vArgs.begin());
    util::Ref context{node};
    JSONRPCRequest request(context);
    request.strMethod = strMethod;
    request.params = RPCConvertValues(strMethod, vArgs);
    request.fHelp = false;
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    try {
        return tableRPC.execute(request);
    } catch (const UniValue &objError) {
        throw std::runtime_error(find_value(objError, "message").get_str());
    }
}

BOOST_FIXTURE_TEST_SUITE(acceptancebit_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(have_bodies_is_true_by_default) {
    const CBlockIndex *tip = ::ChainActive().Tip();
    BOOST_REQUIRE(tip != nullptr);
    BOOST_CHECK(HaveBodies(tip));
    BOOST_CHECK(tip->nStatus & BLOCK_HAVE_BODIES);
}

BOOST_AUTO_TEST_CASE(have_bodies_is_false_for_a_null_index) {
    BOOST_CHECK(!HaveBodies(nullptr));
}

// The core mechanism: a withheld block is accepted with BLOCK_HAVE_DATA set
// but BLOCK_HAVE_BODIES clear (commitment-only); once the body is offered
// again -- unrequested, exactly F-36's trap -- it must be accepted and the
// bit set, not silently discarded.
BOOST_AUTO_TEST_CASE(unrequested_arrival_fills_a_commitment_only_block) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    CBlock block = CreateBlock({}, coinbaseKey);
    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
    uint256 hash = block.GetHash();

    {
        PerfWithholdGuard guard(hash);
        bool fNewBlock = false;
        BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblock, /*fForceProcessing=*/true, &fNewBlock));
        BOOST_REQUIRE(fNewBlock);

        const CBlockIndex *pindex = LookupBlockIndex(hash);
        BOOST_REQUIRE(pindex != nullptr);
        BOOST_CHECK(pindex->nStatus & BLOCK_HAVE_DATA);
        BOOST_CHECK(pindex->nTx > 0);
        BOOST_CHECK(!HaveBodies(pindex));
        BOOST_CHECK(!(pindex->nStatus & BLOCK_HAVE_BODIES));
    }
    // Guard destructor cleared the withhold -- PerfWithholdBodies(pindex) is
    // now false, but HaveBodies(pindex) stays false: it reads the PERSISTED
    // bit, not the current withhold-flag state, exactly the distinction
    // F-100 corrected the plan draft over.
    const CBlockIndex *pindex = LookupBlockIndex(hash);
    BOOST_REQUIRE(!HaveBodies(pindex));
    // Test review (2026-09-19): capture where the commitment record already
    // lives, so a re-offer that wrongly writes a SECOND copy (exactly what
    // ReceivedBlockBodies exists to avoid -- see the "already-stored,
    // still-withheld" comment in AcceptBlock) is caught, not just the bit.
    const int nFileBefore = pindex->nFile;
    const unsigned int nDataPosBefore = pindex->nDataPos;

    // Re-offer the identical block, UNREQUESTED (fForceProcessing = false).
    // Before the F-36 fix, AcceptBlock's "if (pindex->nTx != 0) return true"
    // would discard this -- indistinguishable from a pruned block -- and the
    // bit would never be set.
    bool fNewBlock2 = false;
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblock, /*fForceProcessing=*/false, &fNewBlock2));
    // AcceptBlock sets *fNewBlock unconditionally once past the requested/
    // work/height gates and CheckBlock, before it knows whether this is a
    // genuinely new block or bodies filling in an already-known one --
    // pre-existing behaviour, not something this fix changes. What matters
    // here is that we reached this line at all: before the F-36 fix this
    // call returned early with *fNewBlock left false.
    BOOST_CHECK(fNewBlock2);

    BOOST_CHECK(HaveBodies(pindex));
    BOOST_CHECK(pindex->nStatus & BLOCK_HAVE_BODIES);
    BOOST_CHECK(pindex->nStatus & BLOCK_HAVE_DATA);

    // Test review (2026-09-19), HIGH: the mechanism's actual point -- a
    // commitment-only block whose bodies arrive must become connectable, not
    // just carry the right bits. Deleting ReceivedBlockBodies's candidate
    // re-arm walk (the "put back everything the arrival makes eligible
    // again" loop) would leave every assertion above passing while the tip
    // never advances.
    BOOST_CHECK(::ChainActive().Tip()->GetBlockHash() == hash);
    // And no second copy was written -- the record's own file position is
    // unchanged, not merely re-created identically.
    BOOST_CHECK_EQUAL(pindex->nFile, nFileBefore);
    BOOST_CHECK_EQUAL(pindex->nDataPos, nDataPosBefore);
}

// The fix must not touch pruning's own, deliberate "ignore a re-offered
// pruned block" behaviour (feature_characterise_pruned_arrival.py's pinned
// baseline): a block whose BLOCK_HAVE_DATA has been cleared (nTx != 0 stays,
// exactly what pruning leaves behind) must still be discarded when offered
// back unrequested.
BOOST_AUTO_TEST_CASE(genuinely_pruned_arrival_is_still_discarded) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    CBlock block = CreateBlock({}, coinbaseKey);
    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
    uint256 hash = block.GetHash();

    bool fNewBlock = false;
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblock, /*fForceProcessing=*/true, &fNewBlock));
    BOOST_REQUIRE(fNewBlock);

    CBlockIndex *pindex = LookupBlockIndex(hash);
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(pindex->nTx > 0);

    // Simulate pruning directly: clear HAVE_DATA (and HAVE_BODIES, which
    // PruneOneBlockFile clears alongside it) but leave nTx untouched -- the
    // exact index shape pruning leaves behind. CheckBlockIndex (called
    // unconditionally at the top of every AcceptBlock) asserts
    // !HAVE_DATA == (nTx==0) whenever fHavePruned is false, so this
    // simulation must also set the global fHavePruned flag the way real
    // pruning does, or the very next AcceptBlock call trips that assertion.
    struct FHavePrunedGuard {
        bool saved = fHavePruned;
        ~FHavePrunedGuard() { fHavePruned = saved; }
    } fHavePrunedGuard;
    fHavePruned = true;
    pindex->nStatus &= ~BLOCK_HAVE_DATA;
    pindex->nStatus &= ~BLOCK_HAVE_BODIES;
    pindex->nStatus &= ~BLOCK_HAVE_UNDO; // PruneOneBlockFile clears this too; HAVE_UNDO implies HAVE_DATA

    bool fNewBlock2 = true;
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblock, /*fForceProcessing=*/false, &fNewBlock2));
    BOOST_CHECK(!fNewBlock2);

    // Must still be discarded: HAVE_DATA was not restored by the unrequested
    // arrival, matching the characterisation test's pinned pruning behaviour.
    BOOST_CHECK(!(pindex->nStatus & BLOCK_HAVE_DATA));
    BOOST_CHECK(!HaveBodies(pindex));
}

// 1.3.4 (F-35/F-42, Mike, 2026-09-19): the equal-work tie-break. Settled by
// construction when ReceivedBlockBodies was built (1.3.1) -- it deliberately
// does NOT re-stamp nSequenceId, on the reasoning that first-seen order has
// not changed when a body merely catches up to already-held commitments.
// This proves it: a block (B) whose COMMITMENTS arrive first, but whose BODY
// arrives second (after an equal-work rival (A) has arrived whole and become
// tip), must still WIN the tie once its body fills in.
//
// L-1 (F-107 correction, Fable adversarial review, 2026-09-19): this does
// NOT match unmodified Bitcoin's tie-break -- it's a genuine, disclosed
// behaviour change (see F-42). Unmodified Bitcoin only assigns nSequenceId
// once a block's FULL data arrives, so with this exact arrival order (B's
// commitments, then A whole, then B's body) A keeps the tip upstream; only a
// first-seen-whole-and-connectable block can ever win a tie there. This test
// proves the opposite side of F-42's open question: a commitment-first,
// body-second block CAN displace an already-connected equal-work rival,
// something F-42 explicitly says unmodified code never does. F-35's bug was
// the reverse of what decoupling now deliberately does: re-stamping on body
// arrival would have moved B's sequence id LATER than A's, making B lose a
// tie this design means for it to win.
BOOST_AUTO_TEST_CASE(equal_work_tiebreak_survives_bodies_arriving_second) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    // Two distinct children of the same tip -- equal chainwork by
    // construction (same nBits at the same height) -- built with different
    // coinbase scriptPubKeys so they are genuinely different blocks, not the
    // same bytes twice. NOTE: TestChainSetup::CreateBlock's CKey overload
    // ignores its argument and always uses the fixture's own coinbaseKey (a
    // pre-existing quirk in test_raptoreum.cpp, out of scope here) -- use
    // the CScript overload directly instead.
    CKey keyB;
    keyB.MakeNewKey(true);
    CScript scriptPubKeyB = CScript() << ToByteVector(keyB.GetPubKey()) << OP_CHECKSIG;
    CBlock blockB = CreateBlock({}, scriptPubKeyB);
    CBlock blockA = CreateBlock({}, coinbaseKey);
    BOOST_REQUIRE(blockA.GetHash() != blockB.GetHash());

    std::shared_ptr<const CBlock> shared_pblockB = std::make_shared<const CBlock>(blockB);
    std::shared_ptr<const CBlock> shared_pblockA = std::make_shared<const CBlock>(blockA);
    uint256 hashA = blockA.GetHash();
    uint256 hashB = blockB.GetHash();

    // B's commitments arrive first, bodies withheld -- not yet connectable,
    // so it cannot become tip regardless of its (currently low) sequence id.
    {
        PerfWithholdGuard guard(hashB);
        BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblockB, /*fForceProcessing=*/true, nullptr));
    }
    const CBlockIndex *pindexB = LookupBlockIndex(hashB);
    BOOST_REQUIRE(pindexB != nullptr);
    BOOST_REQUIRE(!HaveBodies(pindexB));

    // A arrives whole, second, and becomes tip -- the only connectable
    // candidate at this point.
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblockA, /*fForceProcessing=*/true, nullptr));
    const CBlockIndex *pindexA = LookupBlockIndex(hashA);
    BOOST_REQUIRE(pindexA != nullptr);
    BOOST_REQUIRE(::ChainActive().Tip()->GetBlockHash() == hashA);
    BOOST_REQUIRE(pindexA->nChainWork == pindexB->nChainWork);   // genuinely a tie
    BOOST_REQUIRE_LT(pindexB->nSequenceId, pindexA->nSequenceId);   // B really was seen first

    // B's body now arrives, unrequested -- ReceivedBlockBodies must NOT
    // re-stamp nSequenceId, so B's (earlier) id is unchanged and it wins the
    // tie against A, becoming the new tip.
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblockB, /*fForceProcessing=*/false, nullptr));
    BOOST_CHECK(HaveBodies(pindexB));
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashB.ToString());
}

// 1.3.3 (F-25j, Mike, 2026-09-19): out-of-order arrival is pre-existing
// Bitcoin Core behaviour -- ReceivedBlockTransactions parks a block in
// m_blocks_unlinked whenever ITS OWN parent is not yet HaveTxsDownloaded(),
// "regardless of bodies" (F-25j's own wording). Decoupling does not touch
// that guard, only what "downloaded" now records (commitments vs bodies).
// This proves the guard still holds with the commitment layer wired in: a
// fully-valid, fully-downloaded CHILD whose PARENT looks header-only must be
// parked, not connected -- and must drain correctly once the parent's
// commitments genuinely arrive.
//
// Building that PARENT/CHILD pair is not free: DIP3's CbTx merkle roots
// (CalcCbTxMerkleRootMNList/Quorums, src/evo/cbtx.cpp) require the
// deterministic masternode list to already be built for whatever pindexPrev
// the child extends. That list is built incrementally, only by really
// connecting a block -- there is no way to construct a DIP3-valid CHILD on
// top of a parent that was never really connected (a fake CChain::SetTip
// onto a header-only index, the upstream miner_tests.cpp trick for plain
// subsidy math, reaches "*** Found EvoDB inconsistency, you must reindex to
// continue" here, a fatal abort). So this builds the pair the only way DIP3
// allows -- by really connecting the parent first -- and then rolls the
// parent back to header-only via the real DisconnectTip (which correctly
// reverses its coin/evoDB effects, unlike hand-editing nStatus over live
// state), leaving only its own nTx/nChainTx bookkeeping to reset directly.
struct CommitmentBudgetGuard {
    ~CommitmentBudgetGuard() { g_commitmentBudgetActive = false; }
};

BOOST_AUTO_TEST_CASE(out_of_order_arrival_parks_a_child_until_its_parent_downloads) {
    CommitmentBudgetGuard guard;
    g_commitmentBudgetActive = true;

    ChainstateManager &chainman = EnsureChainman(m_node);
    CChainState &chainstate = ::ChainstateActive();
    const CChainParams &chainparams = Params();
    CChain &active = ::ChainActive();

    CBlockIndex *pindexRealTip = active.Tip();
    BOOST_REQUIRE(pindexRealTip != nullptr);
    uint256 hashRealTip = pindexRealTip->GetBlockHash();

    // Really connect the parent -- DIP3's deterministic MN list is now
    // genuinely cached for it, so a child built on top of it is DIP3-valid.
    CBlock parent = CreateAndProcessBlock({}, coinbaseKey);
    uint256 hashParent = parent.GetHash();
    CBlockIndex *pindexParent = LookupBlockIndex(hashParent);
    BOOST_REQUIRE(pindexParent != nullptr);
    BOOST_REQUIRE_EQUAL(active.Tip()->GetBlockHash().ToString(), hashParent.ToString());

    // Build the child on top of the (still really connected) parent tip --
    // ordinary construction, no fakery, so its own CbTx is genuinely correct.
    CBlock child = CreateBlock({}, coinbaseKey);
    BOOST_REQUIRE_EQUAL(child.hashPrevBlock.ToString(), hashParent.ToString());

    // Roll the parent back off the active chain through the real mechanism,
    // which correctly reverses its coin/evoDB effects. DisconnectTip queues
    // every one of the block's txs (coinbase included) for mempool
    // resurrection -- deciding which ones actually get resurrected is the
    // real reorg caller's (UpdateMempoolForReorg's) job, done elsewhere, so
    // here it is enough to drain the queue directly: a coinbase can never be
    // mempool-eligible, and DisconnectedBlockTransactions's own destructor
    // asserts the queue is empty.
    {
        LOCK2(cs_main, m_node.mempool->cs);
        CValidationState dcState;
        DisconnectedBlockTransactions disconnectpool;
        BOOST_REQUIRE(chainstate.DisconnectTip(dcState, chainparams, &disconnectpool));
        disconnectpool.clear();
    }
    BOOST_REQUIRE_EQUAL(active.Tip()->GetBlockHash().ToString(), hashRealTip.ToString());

    // setBlockIndexCandidates only keeps entries "as good as the current tip
    // or better" -- real tip pruned it out once the (strictly better) parent
    // overtook it, on the assumption (true for a real reorg, not here) that
    // ActivateBestChain would immediately follow within the same call and
    // reconnect something at least as good. CheckBlockIndex asserts the
    // active tip is always a member, so restore that invariant by hand.
    chainstate.setBlockIndexCandidates.insert(pindexRealTip);

    // DisconnectTip reverses the parent's EFFECTS but leaves its own
    // "this block's transactions were downloaded" bookkeeping untouched --
    // correctly so, since DisconnectTip models "no longer active", not
    // "never downloaded". Reset exactly that bookkeeping by hand (the same
    // kind of direct nStatus surgery genuinely_pruned_arrival_is_still_discarded
    // uses to simulate pruning) so the parent now looks exactly like a
    // genuine headers-first registration: known, ordered, but never given to
    // ReceivedBlockTransactions.
    chainstate.setBlockIndexCandidates.erase(pindexParent);
    pindexParent->nStatus &= ~(BLOCK_HAVE_DATA | BLOCK_HAVE_BODIES | BLOCK_HAVE_UNDO);
    pindexParent->nStatus = (pindexParent->nStatus & ~BLOCK_VALID_MASK) | BLOCK_VALID_TREE;
    pindexParent->nTx = 0;
    pindexParent->nChainTx = 0;
    // nSequenceId is only ever assigned inside ReceivedBlockTransactions, so
    // a genuine header-only block always reads 0 -- CheckBlockIndex asserts
    // exactly this for every not-yet-linked block.
    pindexParent->nSequenceId = 0;
    BOOST_REQUIRE(!pindexParent->HaveTxsDownloaded());
    BOOST_REQUIRE(!(pindexParent->nStatus & BLOCK_HAVE_DATA));

    // The child arrives, fully formed. Its own parent is (as far as the
    // index can tell) still only header-known.
    std::shared_ptr<const CBlock> shared_pchild = std::make_shared<const CBlock>(child);
    bool fNewBlock = false;
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pchild, /*fForceProcessing=*/true, &fNewBlock));
    BOOST_REQUIRE(fNewBlock);

    // The child is fully downloaded on its own -- HAVE_DATA is set -- but it
    // must NOT become the tip, because its own parent has never had
    // ReceivedBlockTransactions called on it. "Regardless of bodies"
    // (F-25j): the child carries a full, decoded body and it still cannot
    // connect.
    CBlockIndex *pindexChild = LookupBlockIndex(child.GetHash());
    BOOST_REQUIRE(pindexChild != nullptr);
    BOOST_CHECK(pindexChild->nStatus & BLOCK_HAVE_DATA);
    BOOST_CHECK_EQUAL(active.Tip()->GetBlockHash().ToString(), hashRealTip.ToString());
    // Direct evidence of the parked state itself (Fable review, 2026-09-19:
    // the mutation-tested crash above proves SOME invariant broke, not that
    // these specific facts are what the test is actually checking).
    BOOST_CHECK(!pindexChild->HaveTxsDownloaded());
    BOOST_CHECK_EQUAL(chainstate.setBlockIndexCandidates.count(pindexChild), 0U);
    auto rangeUnlinked = chainman.m_blockman.m_blocks_unlinked.equal_range(pindexParent);
    bool childFoundUnlinked = std::any_of(rangeUnlinked.first, rangeUnlinked.second,
                                          [&](const auto &kv) { return kv.second == pindexChild; });
    BOOST_CHECK(childFoundUnlinked);

    // Now the parent's commitments arrive, genuinely, through the normal
    // path. ReceivedBlockTransactions's own drain loop (validation.cpp) must
    // find the child in m_blocks_unlinked and connect both blocks in one go.
    std::shared_ptr<const CBlock> shared_pparent = std::make_shared<const CBlock>(parent);
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pparent, /*fForceProcessing=*/true, nullptr));

    BOOST_CHECK_EQUAL(active.Tip()->GetBlockHash().ToString(), child.GetHash().ToString());
    BOOST_CHECK_EQUAL(active.Height(), pindexRealTip->nHeight + 2);
}

// 1.3.3, take two (Fable adversarial review, 2026-09-19): the test above
// exercises ReceivedBlockTransactions's generic parent-guard, which is
// unmodified Bitcoin Core code -- it passes identically with
// g_commitmentBudgetActive off, so it proves nothing decoupling-specific.
// The real decoupling-era gap lives in FindMostWorkChain (validation.cpp,
// the "PERF" comment on fMissingData): a candidate whose OWN bodies are
// present can still have an ANCESTOR that is commitment-only, and
// FindMostWorkChain must recognise the whole candidate chain as unusable
// and re-park it in m_blocks_unlinked -- not silently drop it, and not let
// it become tip (ConnectTip cannot execute a block whose parent's UTXO
// effects were never applied). This is the shape F-25j actually named
// ("regardless of bodies") that the first test never built: a PARENT that
// is genuinely commitment-only, extended by a fully-bodied CHILD.
//
// Building a commitment-only PARENT with a DIP3-valid CHILD on top of it
// has the same constraint the first test's comment already documents:
// DIP3's deterministic MN list is only ever built by really connecting a
// block, so this builds the pair by really connecting the parent (getting
// a real list cached for it and a real child built on top), then
// downgrades the parent to commitment-only by hand -- clearing only
// BLOCK_HAVE_BODIES/BLOCK_HAVE_UNDO and the validity rung down to
// BLOCK_VALID_TRANSACTIONS (what a genuine commitment-only arrival grants,
// per D-16), while leaving nTx/nChainTx/nSequenceId untouched, since its
// commitments really were downloaded. The child is never submitted until
// after this downgrade, so it carries no stale index/candidate state of
// its own to account for.
BOOST_AUTO_TEST_CASE(commitment_only_ancestor_parks_a_fully_bodied_descendant) {
    CommitmentBudgetGuard guard;
    g_commitmentBudgetActive = true;

    ChainstateManager &chainman = EnsureChainman(m_node);
    CChainState &chainstate = ::ChainstateActive();
    const CChainParams &chainparams = Params();
    CChain &active = ::ChainActive();

    CBlockIndex *pindexRealTip = active.Tip();
    BOOST_REQUIRE(pindexRealTip != nullptr);
    uint256 hashRealTip = pindexRealTip->GetBlockHash();

    // Really connect the parent (DIP3 list genuinely cached for it), build
    // the child on top of it (also DIP3-valid), but do not submit the child
    // yet -- it must not acquire any index state before the parent is
    // downgraded below.
    CBlock parent = CreateAndProcessBlock({}, coinbaseKey);
    uint256 hashParent = parent.GetHash();
    CBlockIndex *pindexParent = LookupBlockIndex(hashParent);
    BOOST_REQUIRE(pindexParent != nullptr);
    BOOST_REQUIRE_EQUAL(active.Tip()->GetBlockHash().ToString(), hashParent.ToString());

    CBlock child = CreateBlock({}, coinbaseKey);
    BOOST_REQUIRE_EQUAL(child.hashPrevBlock.ToString(), hashParent.ToString());

    // Roll the parent back off the active chain through the real mechanism.
    {
        LOCK2(cs_main, m_node.mempool->cs);
        CValidationState dcState;
        DisconnectedBlockTransactions disconnectpool;
        BOOST_REQUIRE(chainstate.DisconnectTip(dcState, chainparams, &disconnectpool));
        disconnectpool.clear();
    }
    BOOST_REQUIRE_EQUAL(active.Tip()->GetBlockHash().ToString(), hashRealTip.ToString());
    chainstate.setBlockIndexCandidates.insert(pindexRealTip);

    // Downgrade the parent to genuinely commitment-only: bodies withdrawn,
    // rung lowered to what a real commitment-only arrival grants, tx-level
    // bookkeeping (nTx/nChainTx/nSequenceId) untouched since commitments
    // really were downloaded for it.
    chainstate.setBlockIndexCandidates.erase(pindexParent);
    pindexParent->nStatus &= ~(BLOCK_HAVE_BODIES | BLOCK_HAVE_UNDO);
    pindexParent->nStatus = (pindexParent->nStatus & ~BLOCK_VALID_MASK) | BLOCK_VALID_TRANSACTIONS;
    BOOST_REQUIRE(pindexParent->nStatus & BLOCK_HAVE_DATA);
    BOOST_REQUIRE(pindexParent->HaveTxsDownloaded());
    BOOST_REQUIRE(!HaveBodies(pindexParent));

    // The fully-bodied child arrives. Its own data is complete, but its
    // parent's bodies are not -- FindMostWorkChain must refuse to connect
    // it and must park it, not lose it.
    std::shared_ptr<const CBlock> shared_pchild = std::make_shared<const CBlock>(child);
    bool fNewBlock = false;
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pchild, /*fForceProcessing=*/true, &fNewBlock));
    BOOST_REQUIRE(fNewBlock);

    CBlockIndex *pindexChild = LookupBlockIndex(child.GetHash());
    BOOST_REQUIRE(pindexChild != nullptr);
    BOOST_CHECK(HaveBodies(pindexChild));
    BOOST_CHECK_EQUAL(active.Tip()->GetBlockHash().ToString(), hashRealTip.ToString());
    BOOST_CHECK_EQUAL(chainstate.setBlockIndexCandidates.count(pindexChild), 0U);
    auto rangeUnlinked = chainman.m_blockman.m_blocks_unlinked.equal_range(pindexParent);
    bool childFoundUnlinked = std::any_of(rangeUnlinked.first, rangeUnlinked.second,
                                          [&](const auto &kv) { return kv.second == pindexChild; });
    BOOST_CHECK(childFoundUnlinked);

    // The parent's body now arrives, genuinely. Its own ReceivedBlockBodies
    // re-arm walk (1.3.1) must find the child in m_blocks_unlinked and
    // connect both blocks.
    std::shared_ptr<const CBlock> shared_pparent = std::make_shared<const CBlock>(parent);
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pparent, /*fForceProcessing=*/false, nullptr));

    BOOST_CHECK(HaveBodies(pindexParent));
    BOOST_CHECK_EQUAL(active.Tip()->GetBlockHash().ToString(), child.GetHash().ToString());
    BOOST_CHECK_EQUAL(active.Height(), pindexRealTip->nHeight + 2);
}

// 1.3.5 (F-110, Mike, 2026-09-20): VerifyDB/reindex/startup re-pointing at the
// real bit. Investigation (Fable adversarial review) found most of the
// original plan already built and correct, but the mechanism is not what the
// plan assumed: ConnectTip's own !HaveBodies(pindexNew) guard (the "PERF"
// comment near SetBodiesMissing) and init.cpp:1193's BodiesMissing() handling
// are BOTH dead code today -- FindMostWorkChain's own fMissingData ancestor
// walk (F-109's mechanism) already excludes any bodies-missing candidate
// BEFORE ConnectTip is ever called on it, so ConnectTip's guard never fires
// in practice. This test pins that fact directly, so if FindMostWorkChain's
// filter is ever weakened, this assertion (not a crash) is the tripwire that
// says the OTHER guard has become load-bearing.
BOOST_AUTO_TEST_CASE(startup_activation_is_non_fatal_past_a_commitment_only_best_block) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    CChainState &chainstate = ::ChainstateActive();
    const CChainParams &chainparams = Params();

    CBlockIndex *pindexRealTip = ::ChainActive().Tip();
    BOOST_REQUIRE(pindexRealTip != nullptr);
    uint256 hashRealTip = pindexRealTip->GetBlockHash();

    CBlock blockB = CreateBlock({}, coinbaseKey);
    uint256 hashB = blockB.GetHash();
    std::shared_ptr<const CBlock> shared_pblockB = std::make_shared<const CBlock>(blockB);

    {
        PerfWithholdGuard guard(hashB);
        BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblockB, /*fForceProcessing=*/true, nullptr));
    }
    CBlockIndex *pindexB = LookupBlockIndex(hashB);
    BOOST_REQUIRE(pindexB != nullptr);
    BOOST_REQUIRE(!HaveBodies(pindexB));
    BOOST_REQUIRE_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashRealTip.ToString());

    // FindMostWorkChain already excluded B from setBlockIndexCandidates
    // during the ProcessNewBlock call above, so calling ActivateBestChain
    // again here would trivially find nothing to do -- not what a real
    // restart faces. LoadBlockIndex (validation.cpp) re-inserts every
    // BLOCK_VALID_TRANSACTIONS block with HaveTxsDownloaded() as a
    // candidate regardless of bodies (a commitment-only block satisfies
    // both), so at actual startup B genuinely IS a candidate again and
    // FindMostWorkChain's filter is what has to exclude it a second time.
    // Reproduce that real precondition directly (Fable review, 2026-09-20).
    chainstate.setBlockIndexCandidates.insert(pindexB);

    // The exact call shape init.cpp:1190-1208 makes at startup -- one loop,
    // used for both -reindex and -reindex-chainstate, no special-casing.
    CValidationState state;
    BOOST_REQUIRE(chainstate.ActivateBestChain(state, chainparams, nullptr));
    BOOST_CHECK(state.IsValid());
    BOOST_CHECK(!state.BodiesMissing());
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashRealTip.ToString());
    BOOST_CHECK_EQUAL(chainstate.setBlockIndexCandidates.count(pindexB), 0U);
}

// The bodies-gap guard in VerifyDB (validation.cpp, "Same shape as the
// pruning case") is only load-bearing when the block's bytes are genuinely
// unreadable -- under today's storage format (F-110) a block's full bytes are
// always written to disk regardless of BLOCK_HAVE_BODIES, so clearing the bit
// alone leaves ReadBlockFromDisk succeeding and the guard proves nothing.
// g_perf_withhold_hashes is what actually makes ReadBlockFromDisk's
// CBlockIndex overload refuse the read (precedent: commitmentblock_tests.cpp's
// materialising-read test) -- combining both is what reproduces "commitments
// held, bodies missing" the way F-25's original bug needed to be caught.
BOOST_AUTO_TEST_CASE(verifydb_stops_at_a_bodies_gap_instead_of_reporting_corruption) {
    CChainState &chainstate = ::ChainstateActive();
    const CChainParams &chainparams = Params();

    CBlockIndex *pindexTip = ::ChainActive().Tip();
    BOOST_REQUIRE(pindexTip != nullptr);
    CBlockIndex *pindexG = pindexTip->GetAncestor(pindexTip->nHeight - 2);
    BOOST_REQUIRE(pindexG != nullptr);
    BOOST_REQUIRE(HaveBodies(pindexG));

    PerfWithholdGuard guard(pindexG->GetBlockHash());
    CBlock unreadable;
    BOOST_REQUIRE(!ReadBlockFromDisk(unreadable, pindexG, chainparams.GetConsensus()));  // sanity: withhold really blocks the read

    pindexG->nStatus &= ~BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(!HaveBodies(pindexG));

    // The guard (validation.cpp's !HaveBodies check, ahead of the doomed
    // read) stops the walk gracefully at both check level 3 (the default
    // path) and level 4 (the "try reconnecting" path) -- level 4 never
    // revisits G itself, only bodied blocks above it (F-110), but prove it
    // rather than assume it.
    BOOST_CHECK(CVerifyDB().VerifyDB(chainparams, &chainstate.CoinsTip(), /*nCheckLevel=*/3, /*nCheckDepth=*/10));
    BOOST_CHECK(CVerifyDB().VerifyDB(chainparams, &chainstate.CoinsTip(), /*nCheckLevel=*/4, /*nCheckDepth=*/10));

    // Control: restore the bit while the bytes are STILL unreadable -- this
    // is F-25's original bug shape (the index claims bodies are held; they
    // are not). Without the guard catching it earlier, VerifyDB attempts the
    // read, it fails, and the caller (init.cpp) reports "Corrupted block
    // database detected" -- exactly the false diagnosis 1.3.5 exists to
    // prevent for a genuine gap, reproduced here to prove the guard (not the
    // bytes) is what saves the true case above.
    pindexG->nStatus |= BLOCK_HAVE_BODIES;
    BOOST_CHECK(!CVerifyDB().VerifyDB(chainparams, &chainstate.CoinsTip(), /*nCheckLevel=*/3, /*nCheckDepth=*/10));

    // Restore real state before the withhold guard lifts.
    BOOST_REQUIRE(HaveBodies(pindexG));
}

// The -reindex/-loadblock limb: LoadExternalBlockFile's M-2 fix
// ((pindex->nStatus & BLOCK_HAVE_DATA) == 0 || !HaveBodies(pindex), instead of
// HAVE_DATA alone) has zero test coverage despite being named in 1.3.5's own
// title. Under today's storage format a blk*.dat entry is always a complete
// block (F-110) -- offering one for an index entry that is merely
// commitment-only is exactly how -reindex heals it back to fully-bodied,
// which is what this proves.
BOOST_AUTO_TEST_CASE(loadblock_fills_a_commitment_only_block_and_startup_connects_it) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    CBlockIndex *pindexRealTip = ::ChainActive().Tip();
    BOOST_REQUIRE(pindexRealTip != nullptr);
    uint256 hashRealTip = pindexRealTip->GetBlockHash();

    CBlock blockB = CreateBlock({}, coinbaseKey);
    uint256 hashB = blockB.GetHash();
    std::shared_ptr<const CBlock> shared_pblockB = std::make_shared<const CBlock>(blockB);

    {
        PerfWithholdGuard guard(hashB);
        BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblockB, /*fForceProcessing=*/true, nullptr));
    }
    CBlockIndex *pindexB = LookupBlockIndex(hashB);
    BOOST_REQUIRE(pindexB != nullptr);
    BOOST_REQUIRE(!HaveBodies(pindexB));
    int nFileBefore = pindexB->nFile;
    unsigned int nDataPosBefore = pindexB->nDataPos;

    // Build a scratch blk*.dat-format file for B, the same framing
    // WriteBlockToDisk uses (message-start bytes, serialized size, the block).
    fs::path scratchPath = GetDataDir() / "scratch_blk_for_test.dat";
    {
        CAutoFile fileout(fsbridge::fopen(scratchPath, "wb+"), SER_DISK, CLIENT_VERSION);
        BOOST_REQUIRE(!fileout.IsNull());
        unsigned int nSize = GetSerializeSize(fileout, blockB);
        fileout << chainparams.MessageStart() << nSize;
        fileout << blockB;
    }
    FILE *fileIn = fsbridge::fopen(scratchPath, "rb");
    BOOST_REQUIRE(fileIn != nullptr);
    LoadExternalBlockFile(chainparams, fileIn);  // takes ownership, closes fileIn

    BOOST_CHECK(HaveBodies(pindexB));
    // No second copy written -- the existing commitment record's own file
    // position is reused, not duplicated (same discipline as F-101's
    // unrequested-arrival test).
    BOOST_CHECK_EQUAL(pindexB->nFile, nFileBefore);
    BOOST_CHECK_EQUAL(pindexB->nDataPos, nDataPosBefore);

    // LoadExternalBlockFile only self-activates for genesis; B is not
    // genesis, so the startup sequence's own separate ActivateBestChain call
    // (init.cpp:1192) is what actually connects it.
    CValidationState state;
    BOOST_REQUIRE(::ChainstateActive().ActivateBestChain(state, chainparams, nullptr));
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashB.ToString());
    BOOST_CHECK_EQUAL(::ChainActive().Height(), pindexRealTip->nHeight + 1);
}

// 1.3.7 (Mike, 2026-09-20): InvalidateBlock / MarkConflictingBlock /
// EnforceBestChainLock -- audit, not full hardening (build-plan's own
// framing: confirm safe under the new state, leave full graceful
// degradation to 4.1). Scoped with a Fable review before writing anything,
// given this reaches into the LLMQ ChainLocks subsystem this session
// hadn't otherwise touched.
//
// Two findings from that review, both negative results worth recording
// rather than fixing:
//
// (1) The "rebuild setBlockIndexCandidates" loop InvalidateBlock and
// MarkConflictingBlock share checks HaveTxsDownloaded() but not
// HaveBodies() (validation.cpp), so it CAN insert a commitment-only block
// into the candidate set. This is not new risk -- ReceivedBlockTransactions
// and LoadBlockIndex already insert commitment-only blocks as candidates
// on arrival and at restart with the identical check (the
// startup_activation_... test above already relies on that).
// FindMostWorkChain (F-109) is the one, already-tested filter, and nothing
// else reads the set unsafely: no RPC, no net_processing.cpp reference: no
// consumer of setBlockIndexCandidates outside FindMostWorkChain,
// PruneBlockIndexCandidates, ResetBlockFailureFlags and PreciousBlock, all
// of which use the identical insertion check.
//
// (2) EnforceBestChainLock's (llmq/quorums_chainlocks.cpp) backward pprev
// walk terminates safely at genesis (always on ChainActive, so the loop's
// own !ChainActive().Contains(pindex) condition stops it before a null
// pprev is ever dereferenced), and its only assert(false) path
// (MarkConflictingBlock returning false) requires DisconnectTip to fail,
// reachable only for a block that m_chain.Contains() -- which a
// commitment-only block can never be (ConnectTip refuses to connect one,
// F-109). The row's own named "ReadBlockFromDisk nullptr fallback reaching
// an IS-conflict-detection caller" is TrySignChainTip's GetBlockTxs walk
// (same file) -- rooted at ::ChainActive().Tip() and never walking more
// than 5 blocks back, so by the same invariant (the active chain is never
// commitment-only) it can never reach one either. Safe by construction;
// this comment is that written conclusion, not a test, since there is
// nothing reachable to construct a test around.
//
// What IS worth testing directly, below: that InvalidateBlock and
// MarkConflictingBlock genuinely don't crash or corrupt CheckBlockIndex's
// invariants with a commitment-only sibling in play, on either side of the
// call -- and that marking a full sibling conflicting (MarkConflictingBlock's
// own shape, the exact operation EnforceBestChainLock performs on a losing
// sibling) doesn't prevent a commitment-only rival from later healing in.
//
// Shared shape: T is the real tip; A is a full child of T that connects and
// becomes tip; B is an equal-work commitment-only child of T that never
// connects (same construction as equal_work_tiebreak_survives_bodies_
// arriving_second above). MarkConflictingBlock's own docs annotate
// UpdateMempoolForReorg's requirement on ::mempool.cs, unlike
// InvalidateBlock which takes it internally -- a pre-existing annotation
// gap (MarkConflictingBlock never takes the lock itself) that costs
// nothing to build around here, so every call below is under LOCK2.
// CreateBlock is a TestChainSetup fixture member, unreachable from a free
// function -- callers build blockA/blockB themselves (CreateBlock({},
// coinbaseKey) and CreateBlock({}, an independent scriptPubKey) so they are
// genuinely distinct, equal-work children of the real tip) and this helper
// only drives the shared process-and-assert sequence.
static void ProcessEqualWorkSiblingsAWinsBWithheld(ChainstateManager &chainman, const CChainParams &chainparams,
                                                   const CBlock &blockA, const CBlock &blockB, uint256 &hashA,
                                                   uint256 &hashB, std::shared_ptr<const CBlock> &shared_pblockB) {
    BOOST_REQUIRE(blockA.GetHash() != blockB.GetHash());

    shared_pblockB = std::make_shared<const CBlock>(blockB);
    std::shared_ptr<const CBlock> shared_pblockA = std::make_shared<const CBlock>(blockA);
    hashA = blockA.GetHash();
    hashB = blockB.GetHash();

    {
        PerfWithholdGuard guard(hashB);
        BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblockB, /*fForceProcessing=*/true, nullptr));
    }
    BOOST_REQUIRE(!HaveBodies(LookupBlockIndex(hashB)));

    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblockA, /*fForceProcessing=*/true, nullptr));
    BOOST_REQUIRE_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashA.ToString());
}

static CBlock CreateSiblingBlock(TestChainSetup &fixture) {
    CKey key;
    key.MakeNewKey(true);
    CScript scriptPubKey = CScript() << ToByteVector(key.GetPubKey()) << OP_CHECKSIG;
    return fixture.CreateBlock({}, scriptPubKey);
}

BOOST_AUTO_TEST_CASE(invalidateblock_is_safe_with_a_commitment_only_sibling) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    CChainState &chainstate = ::ChainstateActive();
    const CChainParams &chainparams = Params();

    CBlockIndex *pindexRealTip = ::ChainActive().Tip();
    uint256 hashRealTip = pindexRealTip->GetBlockHash();

    uint256 hashA, hashB;
    std::shared_ptr<const CBlock> shared_pblockB;
    CBlock blockB = CreateSiblingBlock(*this);
    CBlock blockA = CreateBlock({}, coinbaseKey);
    ProcessEqualWorkSiblingsAWinsBWithheld(chainman, chainparams, blockA, blockB, hashA, hashB, shared_pblockB);
    CBlockIndex *pindexB = LookupBlockIndex(hashB);

    LOCK2(cs_main, m_node.mempool->cs);
    CValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, chainparams, LookupBlockIndex(hashA)));
    // The rebuild loop's own known gap (finding (1) above): B, still
    // commitment-only, is reinstated as a candidate anyway. Recorded, not
    // asserted against -- it is what the comment above says is harmless.
    BOOST_CHECK_EQUAL(chainstate.setBlockIndexCandidates.count(pindexB), 1U);
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashRealTip.ToString());

    // FindMostWorkChain must still refuse to select B despite it being a
    // (wrongly, but harmlessly) reinstated candidate.
    CValidationState state2;
    BOOST_REQUIRE(chainstate.ActivateBestChain(state2, chainparams, nullptr));
    BOOST_CHECK(!state2.BodiesMissing());
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashRealTip.ToString());
    BOOST_CHECK_EQUAL(chainstate.setBlockIndexCandidates.count(pindexB), 0U);
}

BOOST_AUTO_TEST_CASE(markconflictingblock_is_safe_with_a_commitment_only_sibling) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    CChainState &chainstate = ::ChainstateActive();
    const CChainParams &chainparams = Params();

    CBlockIndex *pindexRealTip = ::ChainActive().Tip();
    uint256 hashRealTip = pindexRealTip->GetBlockHash();

    uint256 hashA, hashB;
    std::shared_ptr<const CBlock> shared_pblockB;
    CBlock blockB = CreateSiblingBlock(*this);
    CBlock blockA = CreateBlock({}, coinbaseKey);
    ProcessEqualWorkSiblingsAWinsBWithheld(chainman, chainparams, blockA, blockB, hashA, hashB, shared_pblockB);
    CBlockIndex *pindexA = LookupBlockIndex(hashA);
    CBlockIndex *pindexB = LookupBlockIndex(hashB);

    LOCK2(cs_main, m_node.mempool->cs);
    CValidationState state;
    BOOST_REQUIRE(chainstate.MarkConflictingBlock(state, chainparams, pindexA));
    BOOST_CHECK(pindexA->nStatus & BLOCK_CONFLICT_CHAINLOCK);
    BOOST_CHECK(!(pindexB->nStatus & BLOCK_CONFLICT_CHAINLOCK));
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashRealTip.ToString());

    CValidationState state2;
    BOOST_REQUIRE(chainstate.ActivateBestChain(state2, chainparams, nullptr));
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashRealTip.ToString());
    BOOST_CHECK_EQUAL(chainstate.setBlockIndexCandidates.count(pindexB), 0U);
}

// The build-plan row's own literal wording: these two functions invoked
// directly ON a commitment-only block, off the active chain entirely --
// the pure flag-setting path (pindex_was_in_chain stays false, so neither
// function ever reaches DisconnectTip at all).
BOOST_AUTO_TEST_CASE(invalidate_and_markconflicting_are_safe_called_directly_on_a_commitment_only_block) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    CChainState &chainstate = ::ChainstateActive();
    const CChainParams &chainparams = Params();

    uint256 hashA, hashB;
    std::shared_ptr<const CBlock> shared_pblockB;
    CBlock blockB = CreateSiblingBlock(*this);
    CBlock blockA = CreateBlock({}, coinbaseKey);
    ProcessEqualWorkSiblingsAWinsBWithheld(chainman, chainparams, blockA, blockB, hashA, hashB, shared_pblockB);
    CBlockIndex *pindexB = LookupBlockIndex(hashB);
    BOOST_REQUIRE(!::ChainActive().Contains(pindexB));

    LOCK2(cs_main, m_node.mempool->cs);
    CValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, chainparams, pindexB));
    BOOST_CHECK(pindexB->nStatus & BLOCK_FAILED_VALID);
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashA.ToString());

    // Reset and repeat for MarkConflictingBlock on a second, freshly-built
    // commitment-only sibling C, so the two calls don't interfere.
    ::ResetBlockFailureFlags(pindexB);

    CKey keyC;
    keyC.MakeNewKey(true);
    CScript scriptPubKeyC = CScript() << ToByteVector(keyC.GetPubKey()) << OP_CHECKSIG;
    CBlock blockC = CreateBlock({}, scriptPubKeyC);
    std::shared_ptr<const CBlock> shared_pblockC = std::make_shared<const CBlock>(blockC);
    uint256 hashC = blockC.GetHash();
    BOOST_REQUIRE(hashC != hashA && hashC != hashB);
    {
        PerfWithholdGuard guard(hashC);
        BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblockC, /*fForceProcessing=*/true, nullptr));
    }
    CBlockIndex *pindexC = LookupBlockIndex(hashC);
    BOOST_REQUIRE(!HaveBodies(pindexC));

    CValidationState state2;
    BOOST_REQUIRE(chainstate.MarkConflictingBlock(state2, chainparams, pindexC));
    BOOST_CHECK(pindexC->nStatus & BLOCK_CONFLICT_CHAINLOCK);
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashA.ToString());
}

// EnforceBestChainLock's own operation on a losing sibling, reproduced
// directly (driving the real CChainLocksHandler needs an enabled spork, a
// BLS quorum and a signed recovered signature -- disproportionate scaffolding
// for what is, per the Fable review, entirely expressible through
// MarkConflictingBlock's own behaviour: EnforceBestChainLock's loop calls
// exactly this, on exactly a losing sibling of its chain-locked target).
// The commitment-only side of the story: A (full, the loser here) gets
// marked conflicting exactly as EnforceBestChainLock would when a ChainLock
// commits to a DIFFERENT chain than the one A extends -- and B (commitment-
// only) must still be able to heal in afterward once its real body arrives,
// proving MarkConflictingBlock's bookkeeping doesn't leave B permanently
// stranded alongside its now-conflicting rival.
BOOST_AUTO_TEST_CASE(a_commitment_only_block_still_heals_after_its_sibling_is_marked_conflicting) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    CChainState &chainstate = ::ChainstateActive();
    const CChainParams &chainparams = Params();

    CBlockIndex *pindexRealTip = ::ChainActive().Tip();
    uint256 hashRealTip = pindexRealTip->GetBlockHash();

    uint256 hashA, hashB;
    std::shared_ptr<const CBlock> shared_pblockB;
    CBlock blockB = CreateSiblingBlock(*this);
    CBlock blockA = CreateBlock({}, coinbaseKey);
    ProcessEqualWorkSiblingsAWinsBWithheld(chainman, chainparams, blockA, blockB, hashA, hashB, shared_pblockB);
    CBlockIndex *pindexA = LookupBlockIndex(hashA);
    CBlockIndex *pindexB = LookupBlockIndex(hashB);

    {
        LOCK2(cs_main, m_node.mempool->cs);
        CValidationState state;
        BOOST_REQUIRE(chainstate.MarkConflictingBlock(state, chainparams, pindexA));
    }
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashRealTip.ToString());
    // Same known, harmless gap as invalidateblock_is_safe_...: the rebuild
    // loop reinstates B (still commitment-only) regardless of bodies.
    BOOST_CHECK_EQUAL(chainstate.setBlockIndexCandidates.count(pindexB), 1U);

    // B's real body now arrives, unrequested -- must still heal in, not be
    // stranded by its now-conflicting rival's bookkeeping.
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblockB, /*fForceProcessing=*/false, nullptr));
    BOOST_CHECK(HaveBodies(pindexB));
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), hashB.ToString());
    // A must never come back, having been marked conflicting.
    BOOST_CHECK(pindexA->nStatus & BLOCK_CONFLICT_CHAINLOCK);
}

// M-5 (full-arc adversarial review, F-118): the pre-1.3 BLOCK_HAVE_BODIES
// migration in LoadBlockIndexDB (validation.cpp) had zero test coverage.
// Reproduces the on-disk shape a pre-1.3 datadir actually has -- HAVE_DATA
// set, HAVE_BODIES clear, exactly what every entry looked like before this
// bit existed -- by clearing HAVE_BODIES directly on already-connected
// entries (their bytes are genuinely on disk, matching a real pre-1.3 node)
// rather than via g_perf_withhold_hashes (which also makes ReadBlockFromDisk
// refuse the read -- the wrong shape here; a pre-1.3 block was never
// withheld, it just predates the bit).
BOOST_AUTO_TEST_CASE(bodiesmigrated_migration_restores_have_bodies_on_pre_1_3_entries) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    CBlockIndex *pindexTip = ::ChainActive().Tip();
    BOOST_REQUIRE(pindexTip != nullptr);
    CBlockIndex *pindex1 = pindexTip->GetAncestor(pindexTip->nHeight - 5);
    CBlockIndex *pindex2 = pindexTip->GetAncestor(pindexTip->nHeight - 2);
    BOOST_REQUIRE(pindex1 != nullptr);
    BOOST_REQUIRE(pindex2 != nullptr);
    BOOST_REQUIRE(pindex1->nStatus & BLOCK_HAVE_DATA);
    BOOST_REQUIRE(pindex2->nStatus & BLOCK_HAVE_DATA);

    {
        LOCK(cs_main);
        pindex1->nStatus &= ~BLOCK_HAVE_BODIES;
        pindex2->nStatus &= ~BLOCK_HAVE_BODIES;
        BOOST_REQUIRE(pblocktree->WriteFlag("bodiesmigrated", false));
    }
    BOOST_REQUIRE(!HaveBodies(pindex1));
    BOOST_REQUIRE(!HaveBodies(pindex2));

    {
        LOCK(cs_main);
        BOOST_REQUIRE(LoadBlockIndexDB(chainman, chainparams));
    }

    BOOST_CHECK(HaveBodies(pindex1));
    BOOST_CHECK(HaveBodies(pindex2));
    bool fBodiesMigrated = false;
    pblocktree->ReadFlag("bodiesmigrated", fBodiesMigrated);
    BOOST_CHECK(fBodiesMigrated);
}

// The downgrade/upgrade corruption risk the migration's own comment names:
// once "bodiesmigrated" is true, a GENUINE commitment-only entry (Phase 2)
// must never be swept up by a later, redundant migration run. Simulated here
// by clearing HAVE_BODIES on a fresh entry AFTER the flag is already set --
// the shape a real commitment-only block has the moment Phase 2 exists.
BOOST_AUTO_TEST_CASE(bodiesmigrated_migration_does_not_touch_a_later_genuine_commitment_only_entry) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    CBlockIndex *pindexTip = ::ChainActive().Tip();
    BOOST_REQUIRE(pindexTip != nullptr);
    CBlockIndex *pindexGenuine = pindexTip->GetAncestor(pindexTip->nHeight - 3);
    BOOST_REQUIRE(pindexGenuine != nullptr);
    BOOST_REQUIRE(pindexGenuine->nStatus & BLOCK_HAVE_DATA);

    {
        // First pass: an empty/true-fresh datadir migrates nothing (nothing
        // is HAVE_DATA-without-HAVE_BODIES yet) but still sets the flag,
        // matching what a real first boot after upgrading to this binary does.
        LOCK(cs_main);
        BOOST_REQUIRE(LoadBlockIndexDB(chainman, chainparams));
    }
    bool fBodiesMigrated = false;
    pblocktree->ReadFlag("bodiesmigrated", fBodiesMigrated);
    BOOST_REQUIRE(fBodiesMigrated);

    {
        LOCK(cs_main);
        pindexGenuine->nStatus &= ~BLOCK_HAVE_BODIES;
    }
    BOOST_REQUIRE(!HaveBodies(pindexGenuine));

    {
        LOCK(cs_main);
        BOOST_REQUIRE(LoadBlockIndexDB(chainman, chainparams));
    }

    // Must still be genuinely bodies-missing -- the flag being set is what
    // stops the second pass from wrongly re-stamping it.
    BOOST_CHECK(!HaveBodies(pindexGenuine));
}

// 2.1.4: the body store's own write side, wired into AcceptBlock. Builds a
// real spend of a mature coinbase so the block carries a genuine non-coinbase
// body, not just a coinbase-only block (which would leave GetBodyRecordSerializedSize's
// count-only path untested here).
static CMutableTransaction MakeSpendOfCoinbase(const CTransactionRef &coinbase, const CKey &coinbaseKeyIn) {
    CBasicKeyStore keystore;
    keystore.AddKey(coinbaseKeyIn);
    CMutableTransaction spendTx;
    spendTx.vin.resize(1);
    spendTx.vin[0].prevout = COutPoint(coinbase->GetHash(), 0);
    spendTx.vout.resize(1);
    spendTx.vout[0].nValue = 10 * CENT;
    spendTx.vout[0].scriptPubKey = GetScriptForDestination(coinbaseKeyIn.GetPubKey().GetID());
    BOOST_REQUIRE(SignSignature(keystore, *coinbase, spendTx, 0, SIGHASH_ALL));
    return spendTx;
}

// The core mechanism: an accepted block's non-coinbase transactions must
// actually be readable back from the body store, at the exact position the
// index now records -- not just "some bit got set".
BOOST_AUTO_TEST_CASE(accepted_block_writes_its_bodies_to_the_body_store) {
    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);

    CBlock block = CreateAndProcessBlock({spendTx}, coinbaseKey);
    CBlockIndex *pindex = LookupBlockIndex(block.GetHash());
    BOOST_REQUIRE(pindex != nullptr);
    // F-135 (2.1.4 review): the body store is written and the status bit set
    // during AcceptBlock, before ConnectBlock ever runs -- without this
    // check, an invalid spend would still pass every assertion below.
    BOOST_REQUIRE_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), block.GetHash().ToString());

    BOOST_CHECK(pindex->nStatus & BLOCK_HAVE_BODY_RECORD);
    BOOST_REQUIRE(!pindex->GetBodyPos().IsNull());

    std::vector<CTransactionRef> bodiesOut;
    BOOST_REQUIRE(ReadBodyRecord(pindex->GetBodyPos(), bodiesOut));
    // Index 0 is vtx[1] -- the coinbase is never stored here (bodystore.h's
    // own indexing convention).
    BOOST_REQUIRE_EQUAL(bodiesOut.size(), block.vtx.size() - 1);
    for (size_t i = 0; i < bodiesOut.size(); i++) {
        BOOST_CHECK(bodiesOut[i]->GetHash() == block.vtx[i + 1]->GetHash());
    }
}

// F-135 (2.1.4 review): BLOCK_HAVE_BODY_RECORD is NOT BLOCK_HAVE_BODIES's
// sibling -- it is BLOCK_HAVE_DATA's own analogue for the body store ("we
// know where these bytes are"), unconditional the instant SaveBodyToDisk
// succeeds. The first version of this test asserted the OPPOSITE (the record
// bit deferred like BLOCK_HAVE_BODIES) and passed -- because gating
// CDiskBlockIndex's conditional serialization of nBodyFile/nBodyPos on that
// same, deliberately-withheld bit meant a withheld block's real, already-
// written position was never persisted to disk at all, only held in memory
// for the life of the process. A restart between a withheld accept and the
// bodies "arriving" would then stamp BLOCK_HAVE_BODY_RECORD onto whatever a
// fresh CBlockIndex defaults to (0,0) instead of the real position -- this
// test cannot reach across a restart (body_store_bookkeeping_survives_a_
// real_loadblockindexdb_reload below does, now that both are fixed), but it
// still proves the corrected, non-deferred semantics within one process:
// GetBodyPos() is valid and correct on the FIRST (withheld) accept, and only
// BLOCK_HAVE_BODIES -- the deliberately-withholdable "we claim to serve real
// content" bit -- lags until the bodies "arrive" (a later unrequested
// re-offer).
BOOST_AUTO_TEST_CASE(body_record_bit_is_unconditional_even_when_bodies_are_withheld) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CBlock block = CreateBlock({spendTx}, coinbaseKey);
    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
    uint256 hash = block.GetHash();

    FlatFilePos bodyPosAfterFirstAccept;
    {
        PerfWithholdGuard guard(hash);
        bool fNewBlock = false;
        BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblock, /*fForceProcessing=*/true, &fNewBlock));
        BOOST_REQUIRE(fNewBlock);

        const CBlockIndex *pindex = LookupBlockIndex(hash);
        BOOST_REQUIRE(pindex != nullptr);
        // The position is known and correct immediately -- withholding defers
        // only the SEPARATE "claim to hold real content" bit below.
        BOOST_CHECK(pindex->nStatus & BLOCK_HAVE_BODY_RECORD);
        BOOST_REQUIRE(!pindex->GetBodyPos().IsNull());
        BOOST_CHECK(!(pindex->nStatus & BLOCK_HAVE_BODIES));
        bodyPosAfterFirstAccept = pindex->GetBodyPos();

        std::vector<CTransactionRef> bodiesOut;
        BOOST_REQUIRE(ReadBodyRecord(bodyPosAfterFirstAccept, bodiesOut));
        BOOST_REQUIRE_EQUAL(bodiesOut.size(), block.vtx.size() - 1);
        BOOST_CHECK(bodiesOut[0]->GetHash() == block.vtx[1]->GetHash());
    }

    bool fNewBlock2 = false;
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblock, /*fForceProcessing=*/false, &fNewBlock2));
    BOOST_CHECK(fNewBlock2);

    const CBlockIndex *pindex = LookupBlockIndex(hash);
    BOOST_CHECK(pindex->nStatus & BLOCK_HAVE_BODIES);
    BOOST_CHECK(pindex->nStatus & BLOCK_HAVE_BODY_RECORD);
    // No second write: the position is unchanged from the withheld accept.
    BOOST_CHECK_EQUAL(pindex->GetBodyPos().nFile, bodyPosAfterFirstAccept.nFile);
    BOOST_CHECK_EQUAL(pindex->GetBodyPos().nPos, bodyPosAfterFirstAccept.nPos);
}

// F-143 (2.2.2 spec, retroactive amendment to 2.2.1): the body-store index's
// own serveability flag must track BLOCK_HAVE_BODIES through a REAL
// withhold-then-arrival cycle, not just in a hand-fed unit test -- this is
// exactly the real call sequence ReceivedBlockTransactions (withheld=false)
// then ReceivedBlockBodies (arrival) drives.
// F-145 (Fable review of F-144, MEDIUM): the original version of this test
// never asserted anything about a NORMAL, never-withheld block -- a mutant
// that made ReceivedBlockTransactions always record fServeable=false (the
// production consequence: a future 2.2.3 handler refuses every block until
// the node restarts) survived the whole suite, since the withheld/arrival
// pair alone never observes that call site's own real value. Fixed with a
// control block, a position-stability check across arrival, and a direct
// cross-check against HaveBodies() at both checkpoints, so the index's own
// claim is verified against the CBlockIndex fact it's supposed to mirror,
// not just against itself.
BOOST_AUTO_TEST_CASE(body_index_serveability_flag_tracks_a_real_withhold_then_arrival_cycle) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    // Control: a normal block, never withheld, must be serveable immediately
    // after accept -- no mutant that quietly ignores `bodies_held` here can
    // hide behind the withheld/arrival pair below.
    CBlock controlBlock = CreateAndProcessBlock({}, coinbaseKey);
    uint256 controlHash = controlBlock.GetHash();
    FlatFilePos controlPosOut;
    bool fControlServeableOut = false;
    BOOST_REQUIRE(LookupBodyPositionByHash(controlHash, controlPosOut, &fControlServeableOut));
    BOOST_CHECK(fControlServeableOut);
    BOOST_CHECK_EQUAL(fControlServeableOut, HaveBodies(LookupBlockIndex(controlHash)));

    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CBlock block = CreateBlock({spendTx}, coinbaseKey);
    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
    uint256 hash = block.GetHash();

    FlatFilePos posOut;
    bool fServeableOut = true;
    FlatFilePos posBeforeArrival;

    {
        PerfWithholdGuard guard(hash);
        BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblock, /*fForceProcessing=*/true, nullptr));

        BOOST_REQUIRE(LookupBodyPositionByHash(hash, posOut, &fServeableOut));
        BOOST_CHECK(!fServeableOut);
        BOOST_CHECK_EQUAL(fServeableOut, HaveBodies(LookupBlockIndex(hash)));
        posBeforeArrival = posOut;
    }

    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblock, /*fForceProcessing=*/false, nullptr));

    BOOST_REQUIRE(LookupBodyPositionByHash(hash, posOut, &fServeableOut));
    BOOST_CHECK(fServeableOut);
    BOOST_CHECK_EQUAL(fServeableOut, HaveBodies(LookupBlockIndex(hash)));
    // The position must not move when bodies merely arrive for an
    // already-accepted block -- SaveBodyToDisk only ever runs once, at the
    // original accept.
    BOOST_CHECK_EQUAL(posOut.nFile, posBeforeArrival.nFile);
    BOOST_CHECK_EQUAL(posOut.nPos, posBeforeArrival.nPos);
}

// F-133's second recorded constraint, proven rather than merely argued: the
// write side (AcceptBlock -> SaveBodyToDisk -> FindBodyPos) and the load side
// (LoadBodyFileInfo, now wired into LoadBlockIndexDB) must actually agree
// after a real restart, not just each work in isolation the way
// bodystore_tests.cpp's own restart test already proved with hand-fed data.
// This drives the SAME real LoadBlockIndexDB call
// bodiesmigrated_migration_restores_have_bodies_on_pre_1_3_entries above
// uses, so it is a real reload, not a simulation of one.
//
// F-135 (2.1.4 review): this proves the body-FILE bookkeeping side only
// (vinfoBodyFile's per-file sizes, via LoadBodyFileInfo) -- it never persists
// the CBlockIndex itself, so it says nothing about the per-block POSITION
// side. See body_position_on_the_index_survives_a_real_loadblockindexdb_reload
// below for that half; the two are independent and BOTH were needed (the
// second one caught a real bug this one couldn't see).
BOOST_AUTO_TEST_CASE(body_file_bookkeeping_survives_a_real_loadblockindexdb_reload) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CBlock block = CreateAndProcessBlock({spendTx}, coinbaseKey);
    CBlockIndex *pindex = LookupBlockIndex(block.GetHash());
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(pindex->nStatus & BLOCK_HAVE_BODY_RECORD);
    FlatFilePos bodyPos = pindex->GetBodyPos();

    // The real flush pipeline's body-file half: GetDirtyBodyFileInfo folded
    // into WriteBatchSync (F-132/F-133), for an entry FindBodyPos actually
    // marked dirty during a real AcceptBlock -- not a hand-built
    // CBodyFileInfo the way bodystore_tests.cpp's own tests use. (Calling the
    // full CChainState::FlushStateToDisk directly from this fixture is not
    // this test's job and is unsafe here -- it assumes a shutdown sequence
    // this fixture does not run, unrelated to the body store.)
    std::vector<std::pair<int, CBodyFileInfo>> vBodyFiles;
    int nLastBodyFileOut = -1;
    GetDirtyBodyFileInfo(vBodyFiles, nLastBodyFileOut);
    BOOST_REQUIRE(pblocktree->WriteBatchSync({}, 0, {}, vBodyFiles, nLastBodyFileOut));

    // Simulate a restart of bodystore.cpp's own in-memory bookkeeping (the
    // bytes on disk and the CBlockIndex entries are untouched -- only the
    // process-global FindBodyPos state resets, the same thing a real process
    // restart would do).
    TestOnlyResetBodyFileState();
    {
        LOCK(cs_main);
        BOOST_REQUIRE(LoadBlockIndexDB(chainman, chainparams));
    }

    // FindBodyPos must continue from the persisted position, not silently
    // restart at file 0 and overwrite the block this test just accepted.
    // Computed independent of the reload (from the block's own content, not
    // from any post-reload state) so a mutant that drops LoadBodyFileInfo's
    // wiring can't pass by having both sides of the comparison collapse to
    // the same (wrong) zero together.
    uint64_t expectedRecordSize = GetBodyRecordSerializedSize(
            std::vector<CTransactionRef>(block.vtx.begin() + 1, block.vtx.end()));
    unsigned int expectedFileSizeAfterThisBlock = bodyPos.nPos + (unsigned int) expectedRecordSize;
    BOOST_REQUIRE_GT(expectedFileSizeAfterThisBlock, 0U);

    unsigned int nSizeAfterReload = TestOnlyGetBodyFileSize(bodyPos.nFile);
    BOOST_CHECK_EQUAL(nSizeAfterReload, expectedFileSizeAfterThisBlock);

    FlatFilePos posAfter;
    BOOST_REQUIRE(FindBodyPos(posAfter, 1));
    BOOST_CHECK_EQUAL(posAfter.nFile, bodyPos.nFile);
    BOOST_CHECK_EQUAL(posAfter.nPos, expectedFileSizeAfterThisBlock);

    // And the accepted block's own bytes are still correct through the
    // reloaded state -- the reload only affects in-memory bookkeeping.
    std::vector<CTransactionRef> bodiesOut;
    BOOST_REQUIRE(ReadBodyRecord(bodyPos, bodiesOut));
    BOOST_REQUIRE_EQUAL(bodiesOut.size(), block.vtx.size() - 1);
    BOOST_CHECK(bodiesOut[0]->GetHash() == block.vtx[1]->GetHash());
}

// F-135 (2.1.4 review): the test above proves the body-FILE bookkeeping
// survives a reload; it never proves the per-block POSITION does, because it
// never persists the CBlockIndex itself through WriteBatchSync and never
// disturbs pindex's own nBodyFile/nBodyPos in memory. That gap hid a real
// HIGH bug: LoadBlockIndexGuts (txdb.cpp) copied nFile/nDataPos/nUndoPos/
// nStatus/nTx from CDiskBlockIndex but never nBodyFile/nBodyPos, so a
// correctly PERSISTED position (CDiskBlockIndex's own conditional
// serialization, chain.h, writes it whenever BLOCK_HAVE_BODY_RECORD is set)
// was silently dropped on every restart regardless. Proven here by
// persisting the real CBlockIndex, clobbering the in-memory fields to
// exactly what a genuinely fresh CBlockIndex::SetNull() leaves (0,0) --
// what every restart's newly constructed entry starts as before the loader
// runs -- and confirming the reload restores the real position, not the
// clobbered one.
BOOST_AUTO_TEST_CASE(body_position_on_the_index_survives_a_real_loadblockindexdb_reload) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CBlock block = CreateAndProcessBlock({spendTx}, coinbaseKey);
    CBlockIndex *pindex = LookupBlockIndex(block.GetHash());
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(pindex->nStatus & BLOCK_HAVE_BODY_RECORD);
    FlatFilePos bodyPos = pindex->GetBodyPos();
    BOOST_REQUIRE(!bodyPos.IsNull());

    // Persist the real CBlockIndex entry -- CDiskBlockIndex's own conditional
    // serialization (chain.h) writes nBodyFile/nBodyPos because the status
    // bit is set.
    BOOST_REQUIRE(pblocktree->WriteBatchSync({}, 0, {pindex}));

    // Clobber the in-memory fields to what a genuinely fresh restart's newly
    // constructed CBlockIndex starts as, before the loader has run.
    pindex->nBodyFile = 0;
    pindex->nBodyPos = 0;
    BOOST_CHECK_EQUAL(pindex->GetBodyPos().nPos, 0U);

    {
        LOCK(cs_main);
        BOOST_REQUIRE(LoadBlockIndexDB(chainman, chainparams));
    }

    // The reload must restore the REAL position, not leave the clobbered one.
    BOOST_CHECK_EQUAL(pindex->GetBodyPos().nFile, bodyPos.nFile);
    BOOST_CHECK_EQUAL(pindex->GetBodyPos().nPos, bodyPos.nPos);

    std::vector<CTransactionRef> bodiesOut;
    BOOST_REQUIRE(ReadBodyRecord(pindex->GetBodyPos(), bodiesOut));
    BOOST_REQUIRE_EQUAL(bodiesOut.size(), block.vtx.size() - 1);
    BOOST_CHECK(bodiesOut[0]->GetHash() == block.vtx[1]->GetHash());
}

// 2.2.1 (F-140): the body-store-owned height+hash index's real wiring --
// bodystore_tests.cpp already proves the index functions themselves are
// correct against hand-fed positions; these prove the actual integration
// points (ReceivedBlockTransactions, ConnectTip, DisconnectTip, LoadChainTip)
// call them, which is exactly the kind of wiring gap F-135 found for the
// per-block position fields (a correctly-implemented function nothing ever
// called).
BOOST_AUTO_TEST_CASE(body_index_by_hash_and_height_are_populated_by_a_real_accept_and_connect) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    CChainState &chainstate = ::ChainstateActive();

    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CBlock block = CreateAndProcessBlock({spendTx}, coinbaseKey);
    CBlockIndex *pindex = LookupBlockIndex(block.GetHash());
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(pindex == chainstate.m_chain.Tip());
    FlatFilePos bodyPos = pindex->GetBodyPos();
    BOOST_REQUIRE(!bodyPos.IsNull());

    FlatFilePos posOut;
    BOOST_REQUIRE(LookupBodyPositionByHash(block.GetHash(), posOut));
    BOOST_CHECK_EQUAL(posOut.nFile, bodyPos.nFile);
    BOOST_CHECK_EQUAL(posOut.nPos, bodyPos.nPos);

    uint256 hashOut;
    BOOST_REQUIRE(LookupBodyPositionAtHeight(pindex->nHeight, posOut, hashOut));
    BOOST_CHECK_EQUAL(posOut.nFile, bodyPos.nFile);
    BOOST_CHECK_EQUAL(posOut.nPos, bodyPos.nPos);
    BOOST_CHECK(hashOut == block.GetHash());
}

// A disconnected block is still fully accepted/persisted data (side-chain
// blocks are never connected at all, so the hash half must not treat
// disconnection as if it were "never accepted") -- only the ACTIVE CHAIN's
// height entry is removed.
BOOST_AUTO_TEST_CASE(body_index_height_entry_is_erased_by_a_real_disconnecttip_hash_entry_survives) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    CChainState &chainstate = ::ChainstateActive();
    const CChainParams &chainparams = Params();

    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CBlock block = CreateAndProcessBlock({spendTx}, coinbaseKey);
    CBlockIndex *pindex = LookupBlockIndex(block.GetHash());
    BOOST_REQUIRE(pindex != nullptr);
    int disconnectedHeight = pindex->nHeight;
    uint256 disconnectedHash = block.GetHash();

    FlatFilePos posOut;
    uint256 hashOut;
    BOOST_REQUIRE(LookupBodyPositionAtHeight(disconnectedHeight, posOut, hashOut));

    {
        LOCK2(cs_main, m_node.mempool->cs);
        CValidationState dcState;
        DisconnectedBlockTransactions disconnectpool;
        BOOST_REQUIRE(chainstate.DisconnectTip(dcState, chainparams, &disconnectpool));
        disconnectpool.clear();
    }
    // CheckBlockIndex's own invariant (see out_of_order_arrival_parks_a_child_
    // until_its_parent_downloads above): the disconnected tip must remain a
    // block-index candidate, since nothing else is being connected in its
    // place within this test.
    chainstate.setBlockIndexCandidates.insert(pindex);

    BOOST_CHECK(!LookupBodyPositionAtHeight(disconnectedHeight, posOut, hashOut));
    // The hash half must NOT have been touched -- the block's bytes are still
    // exactly where they were, on disk, whether or not it is on the active
    // chain right now.
    BOOST_REQUIRE(LookupBodyPositionByHash(disconnectedHash, posOut));
}

// F-135's own reload pattern, applied to LoadChainTip's new rebuild: a
// process restart must repopulate BOTH halves of the index from CBlockIndex's
// own already-persisted state -- calling LoadChainTip again while the tip
// already matches the coins view's best block would hit its early return and
// prove nothing (the exact "early return skip logic" gap F-135 warned about
// for a different reload path). Forcing m_chain's tip to null first is what
// makes this a genuine exercise of the rebuild loop, not a no-op call.
BOOST_AUTO_TEST_CASE(body_index_survives_a_real_loadchaintip_reload) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    CChainState &chainstate = ::ChainstateActive();
    const CChainParams &chainparams = Params();

    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CBlock block = CreateAndProcessBlock({spendTx}, coinbaseKey);
    CBlockIndex *pindex = LookupBlockIndex(block.GetHash());
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(pindex == chainstate.m_chain.Tip());
    FlatFilePos bodyPos = pindex->GetBodyPos();
    BOOST_REQUIRE(!bodyPos.IsNull());
    int height = pindex->nHeight;
    uint256 hash = block.GetHash();

    // Simulate a fresh process: the in-memory index is empty and m_chain has
    // no tip set yet -- LoadChainTip's own genuine "never loaded" precondition
    // (validation.cpp's own init.cpp call site), not merely calling it again
    // once a tip is already current. Both real init.cpp calls are exercised,
    // in the same order production makes them (F-141, Fable review of F-140:
    // the hash half rebuilds in LoadBlockIndexDB, the height half in
    // LoadChainTip -- calling only the latter, as this test originally did,
    // passed while silently never exercising the hash-half rebuild path at
    // all once it moved out of LoadChainTip).
    ResetBodyIndex();
    chainstate.m_chain.SetTip(nullptr);

    FlatFilePos posOut;
    uint256 hashOut;
    BOOST_REQUIRE(!LookupBodyPositionByHash(hash, posOut));
    BOOST_REQUIRE(!LookupBodyPositionAtHeight(height, posOut, hashOut));

    {
        LOCK(cs_main);
        BOOST_REQUIRE(LoadBlockIndexDB(chainman, chainparams));
        BOOST_REQUIRE(chainstate.LoadChainTip(chainparams));
    }

    BOOST_REQUIRE(LookupBodyPositionByHash(hash, posOut));
    BOOST_CHECK_EQUAL(posOut.nFile, bodyPos.nFile);
    BOOST_CHECK_EQUAL(posOut.nPos, bodyPos.nPos);

    BOOST_REQUIRE(LookupBodyPositionAtHeight(height, posOut, hashOut));
    BOOST_CHECK_EQUAL(posOut.nFile, bodyPos.nFile);
    BOOST_CHECK_EQUAL(posOut.nPos, bodyPos.nPos);
    BOOST_CHECK(hashOut == hash);

    // The genesis block (height 0) must also have rebuilt correctly -- proves
    // the rebuild loop covers the whole chain, not just the block this test
    // happened to add last.
    const CBlockIndex *pindexGenesis = chainstate.m_chain[0];
    BOOST_REQUIRE(pindexGenesis != nullptr);
    FlatFilePos genesisBodyPos = pindexGenesis->GetBodyPos();
    BOOST_REQUIRE(!genesisBodyPos.IsNull());
    FlatFilePos genesisPosOut;
    uint256 genesisHashOut;
    BOOST_REQUIRE(LookupBodyPositionAtHeight(0, genesisPosOut, genesisHashOut));
    BOOST_CHECK_EQUAL(genesisPosOut.nFile, genesisBodyPos.nFile);
    BOOST_CHECK_EQUAL(genesisPosOut.nPos, genesisBodyPos.nPos);
    BOOST_CHECK(genesisHashOut == pindexGenesis->GetBlockHash());
}

// F-141 (Fable review of F-140, HIGH): production's `-reindex-chainstate`
// path calls LoadBlockIndexDB but deliberately SKIPS LoadChainTip when the
// coins view isn't empty at that point (init.cpp's own `is_coinsview_empty`
// gate: `fReset || fReindexChainState || ...` -- `-reindex-chainstate` alone
// sets `fReindexChainState`, a genuinely distinct flag from `-reindex`'s
// `fReindex`, confirmed directly against init.cpp). Before this fix, the
// hash half's ONLY rebuild lived inside LoadChainTip, so a real
// `-reindex-chainstate` run left it permanently empty: every existing block
// gets reconnected via ConnectTip (populating only the height half) with no
// ReceivedBlockTransactions call for any of them (they're already accepted,
// not re-read from blk*.dat the way a full -reindex would). Per 2.2.2's own
// planned NOTFOUND rule, a node in this state would misbehavior-flag itself
// answering every hash-form tip-path request for its whole pre-existing
// chain. Proven here by calling LoadBlockIndexDB ALONE, deliberately never
// calling LoadChainTip, matching exactly what `-reindex-chainstate` does.
BOOST_AUTO_TEST_CASE(body_index_hash_half_is_rebuilt_by_loadblockindexdb_alone) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CBlock block = CreateAndProcessBlock({spendTx}, coinbaseKey);
    uint256 hash = block.GetHash();
    CBlockIndex *pindex = LookupBlockIndex(hash);
    BOOST_REQUIRE(pindex != nullptr);
    FlatFilePos bodyPos = pindex->GetBodyPos();
    BOOST_REQUIRE(!bodyPos.IsNull());

    ResetBodyIndex();
    FlatFilePos posOut;
    BOOST_REQUIRE(!LookupBodyPositionByHash(hash, posOut));

    {
        LOCK(cs_main);
        BOOST_REQUIRE(LoadBlockIndexDB(chainman, chainparams));
        // Deliberately NOT calling LoadChainTip -- the real
        // -reindex-chainstate path never does either.
    }

    BOOST_REQUIRE(LookupBodyPositionByHash(hash, posOut));
    BOOST_CHECK_EQUAL(posOut.nFile, bodyPos.nFile);
    BOOST_CHECK_EQUAL(posOut.nPos, bodyPos.nPos);
}

// F-143 (2.2.2 spec): the SAME rebuild loop must also restore the
// serveability flag correctly -- for a normal, fully-held block AND for a
// genuinely withheld one, in the same reload, so a rebuild can never
// silently promote a withheld block to serveable (or vice versa) on restart.
BOOST_AUTO_TEST_CASE(body_index_serveability_is_rebuilt_correctly_by_loadblockindexdb) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    // The pre-1.3 "bodiesmigrated" migration (bodiesmigrated_migration_does_
    // not_touch_a_later_genuine_commitment_only_entry's own established
    // pattern) treats any HAVE_DATA-without-HAVE_BODIES entry as a migration
    // candidate on ITS OWN first run against a fresh pblocktree -- which
    // would wrongly stamp BLOCK_HAVE_BODIES onto this test's own withheld
    // block. Setting the flag directly (rather than running a full
    // LoadBlockIndexDB now, which reprocesses m_blocks_unlinked/nChainTx
    // against the fixture's already-live chain state and corrupts
    // CheckBlockIndex's own invariants once a new block is processed
    // afterward -- confirmed by triggering exactly that assertion failure)
    // gets the same effect with none of the side effects on live state.
    BOOST_REQUIRE(pblocktree->WriteFlag("bodiesmigrated", true));

    CBlock heldBlock = CreateAndProcessBlock({}, coinbaseKey);
    uint256 heldHash = heldBlock.GetHash();

    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CBlock withheldBlock = CreateBlock({spendTx}, coinbaseKey);
    std::shared_ptr<const CBlock> shared_pwithheld = std::make_shared<const CBlock>(withheldBlock);
    uint256 withheldHash = withheldBlock.GetHash();
    {
        PerfWithholdGuard guard(withheldHash);
        BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pwithheld, /*fForceProcessing=*/true, nullptr));
    }
    BOOST_REQUIRE(LookupBlockIndex(withheldHash)->nStatus & BLOCK_HAVE_BODY_RECORD);
    BOOST_REQUIRE(!(LookupBlockIndex(withheldHash)->nStatus & BLOCK_HAVE_BODIES));

    ResetBodyIndex();
    {
        LOCK(cs_main);
        BOOST_REQUIRE(LoadBlockIndexDB(chainman, chainparams));
    }

    FlatFilePos posOut;
    bool fServeableOut = false;
    BOOST_REQUIRE(LookupBodyPositionByHash(heldHash, posOut, &fServeableOut));
    BOOST_CHECK(fServeableOut);

    fServeableOut = true;
    BOOST_REQUIRE(LookupBodyPositionByHash(withheldHash, posOut, &fServeableOut));
    BOOST_CHECK(!fServeableOut);
}

// F-145 (Fable review of F-144, HIGH): F-144 wired the serveability flag at
// all three BLOCK_HAVE_BODIES WRITE sites but missed the one CLEAR site --
// PruneOneBlockFile -- leaving the index answering "serveable" for a block
// HaveBodies() now says is gone, until a restart happens to rebuild it
// correctly. Drives the REAL PruneOneBlockFile directly (not a hand-cleared
// bit simulation, matching what CheckBlockIndex/fHavePruned bookkeeping
// genuinely needs) so this is a genuine regression test, not merely a
// characterisation of intent.
BOOST_AUTO_TEST_CASE(body_index_serveability_flag_is_cleared_by_a_real_prune) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    CBlock block = CreateAndProcessBlock({}, coinbaseKey);
    uint256 hash = block.GetHash();
    CBlockIndex *pindex = LookupBlockIndex(hash);
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(HaveBodies(pindex));

    FlatFilePos posOut;
    bool fServeableOut = false;
    BOOST_REQUIRE(LookupBodyPositionByHash(hash, posOut, &fServeableOut));
    BOOST_CHECK(fServeableOut);

    struct FHavePrunedGuard {
        bool saved = fHavePruned;
        ~FHavePrunedGuard() { fHavePruned = saved; }
    } fHavePrunedGuard;
    fHavePruned = true;
    {
        LOCK(cs_main);
        chainman.PruneOneBlockFile(pindex->nFile);
    }
    BOOST_REQUIRE(!HaveBodies(pindex));

    // The index must now agree with HaveBodies() -- still found (the bytes
    // are still really there, no body-file pruning exists yet), but no
    // longer serveable, with no restart required.
    fServeableOut = true;
    BOOST_REQUIRE(LookupBodyPositionByHash(hash, posOut, &fServeableOut));
    BOOST_CHECK(!fServeableOut);
}

// F-145 (Fable review of F-144, LOW): ReceivedBlockBodies's own comment
// claims GetBodyPos() "is always valid here" because BLOCK_HAVE_BODY_RECORD
// is set unconditionally by ReceivedBlockTransactions -- true for any block
// THIS binary accepted, but chain.h's own migration note is explicit that no
// pre-2.1.4 entry has ever had a real body-store record. A pre-2.1.4-style
// withheld block that reaches this function (matching F-141's own
// "connecttip_never_records_a_null_body_position_for_a_pre_2_1_4_style_entry"
// clobber technique) must not poison the index with nFile=-1.
BOOST_AUTO_TEST_CASE(receivedblockbodies_never_records_a_null_body_position_for_a_pre_2_1_4_style_entry) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    CBlock block = CreateBlock({}, coinbaseKey);
    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
    uint256 hash = block.GetHash();

    {
        PerfWithholdGuard guard(hash);
        BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblock, /*fForceProcessing=*/true, nullptr));
    }
    CBlockIndex *pindex = LookupBlockIndex(hash);
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(pindex->nStatus & BLOCK_HAVE_BODY_RECORD);
    BOOST_REQUIRE(!(pindex->nStatus & BLOCK_HAVE_BODIES));

    // Simulate a genuinely pre-2.1.4 entry: the record bit unset, matching
    // F-141's own technique. Also reset the index -- the first (withheld)
    // accept above already wrote a REAL, valid entry for this hash via
    // ReceivedBlockTransactions; clearing it here isolates what
    // ReceivedBlockBodies itself does with a null GetBodyPos(), rather than
    // observing whatever the earlier, unrelated write left behind.
    pindex->nStatus &= ~BLOCK_HAVE_BODY_RECORD;
    BOOST_REQUIRE(pindex->GetBodyPos().IsNull());
    ResetBodyIndex();

    // Re-offer the body -- ReceivedBlockBodies runs.
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblock, /*fForceProcessing=*/false, nullptr));

    // Must be a clean miss, never a poisoned "found, serveable" entry with
    // nFile == -1.
    FlatFilePos posOut;
    BOOST_CHECK(!LookupBodyPositionByHash(hash, posOut));
}

// F-141 (Fable review of F-140, HIGH-adjacent/LOW): the free
// UnloadBlockIndex(CTxMemPool*) -- called both by every TestingSetup's own
// teardown (test_raptoreum.cpp) and by init.cpp's real reindex retry loop --
// already resets vinfoBlockFile/ResetBodyFileState but never reset this
// index, so a reindex retried within one process (or two test cases sharing
// one binary) silently inherited stale entries from before the reset, which
// then pointed at bytes the retried reindex was about to overwrite from
// file 0.
BOOST_AUTO_TEST_CASE(unloadblockindex_clears_the_body_index) {
    EnsureChainman(m_node);
    const CBlockIndex *tip = ::ChainActive().Tip();
    BOOST_REQUIRE(tip != nullptr);
    uint256 tipHash = tip->GetBlockHash();

    FlatFilePos posOut;
    BOOST_REQUIRE(LookupBodyPositionByHash(tipHash, posOut));

    UnloadBlockIndex(m_node.mempool);

    BOOST_CHECK(!LookupBodyPositionByHash(tipHash, posOut));
}

// F-141 (Fable review of F-140, MEDIUM): ConnectTip recorded
// pindexNew->GetBodyPos() unconditionally. Every block THIS binary accepts
// always has BLOCK_HAVE_BODY_RECORD set (F-135: unconditional in
// ReceivedBlockTransactions), but chain.h's own migration note is explicit
// that "no pre-2.1.4 entry has ever had a real body-store record written" --
// a `-reindex-chainstate` replay of any datadir synced before this project's
// 2.1.4 commit reconnects such entries directly (ConnectTip's `!pblock`
// branch only requires HaveBodies(), the OLDER, already-migrated
// BLOCK_HAVE_BODIES bit -- NOT BLOCK_HAVE_BODY_RECORD), reaching
// RecordBodyPositionAtHeight with a null position. Before this fix, that
// poisoned the height map with nFile == -1: LookupBodyPositionAtHeight
// returned true for a position OpenBodyFile would reject, rather than the
// clean miss the rebuild path (LoadChainTip, which DOES guard on IsNull())
// produces for the exact same chain. Reproduced by clobbering the bit off a
// really-accepted, really-disconnected block (matching
// body_position_on_the_index_survives_a_real_loadblockindexdb_reload's own
// "clobber to what an old entry actually looks like" technique) and
// reconnecting it directly through the real ConnectTip, via the same
// `!pblock` branch a genuine historical replay uses.
BOOST_AUTO_TEST_CASE(connecttip_never_records_a_null_body_position_for_a_pre_2_1_4_style_entry) {
    EnsureChainman(m_node);
    CChainState &chainstate = ::ChainstateActive();
    const CChainParams &chainparams = Params();

    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CBlock block = CreateAndProcessBlock({spendTx}, coinbaseKey);
    CBlockIndex *pindex = LookupBlockIndex(block.GetHash());
    BOOST_REQUIRE(pindex != nullptr);
    int height = pindex->nHeight;
    BOOST_REQUIRE(pindex->nStatus & BLOCK_HAVE_BODIES);

    FlatFilePos posOut;
    uint256 hashOut;
    BOOST_REQUIRE(LookupBodyPositionAtHeight(height, posOut, hashOut));

    {
        LOCK2(cs_main, m_node.mempool->cs);
        CValidationState dcState;
        DisconnectedBlockTransactions disconnectpool;
        BOOST_REQUIRE(chainstate.DisconnectTip(dcState, chainparams, &disconnectpool));
        disconnectpool.clear();
    }
    chainstate.setBlockIndexCandidates.insert(pindex);
    BOOST_REQUIRE(!LookupBodyPositionAtHeight(height, posOut, hashOut));

    // Simulate a genuinely pre-2.1.4 entry: BLOCK_HAVE_BODY_RECORD unset,
    // BLOCK_HAVE_BODIES still set (already migrated by the older, pre-2.1.3
    // "bodiesmigrated" pass) -- exactly what LoadBlockIndexDB reads off a
    // datadir synced before this commit.
    pindex->nStatus &= ~BLOCK_HAVE_BODY_RECORD;
    BOOST_REQUIRE(pindex->GetBodyPos().IsNull());
    BOOST_REQUIRE(HaveBodies(pindex));

    // ConnectTrace is file-local to validation.cpp (only forward-declared in
    // the header), so this drives the real reconnection the same way
    // ThreadImport/other tests in this file do -- ActivateBestChain, called
    // without cs_main held (its own requirement), constructs ConnectTrace
    // itself and calls ConnectTip internally with pblock=nullptr for a
    // candidate it found via FindMostWorkChain rather than one freshly
    // handed to it -- exactly the `!pblock` branch a real historical replay
    // uses.
    CValidationState state;
    BOOST_REQUIRE(chainstate.ActivateBestChain(state, chainparams, nullptr));
    BOOST_REQUIRE(chainstate.m_chain.Tip() == pindex);

    // Must be a clean miss, never a poisoned entry with nFile == -1.
    BOOST_CHECK(!LookupBodyPositionAtHeight(height, posOut, hashOut));
}

// F-141 (Fable review of F-140, MEDIUM/test-coverage; corrected by a SECOND
// review of F-141's own fix): every prior integration test used a strictly
// linear chain, so BlockIndex() (every known block) and m_chain[0..Height()]
// (active chain only) always named exactly the same set -- a mutant feeding
// the height loop from BlockIndex() instead of m_chain would still pass
// every one of them.
//
// The first version of this test tried to prove that with a same-height
// EQUAL-WORK SIBLING pair (A connects, B doesn't) -- but BlockMap is keyed by
// BlockHasher's raw GetCheapHash() of each block's own (unsalted, unordered)
// hash, and both blocks' hashes are effectively random per run (fresh
// coinbase keys / PoW). Under the loop-source-swap mutant, whichever of A/B
// the map happened to visit LAST would win the height entry -- a coin flip
// independent of which block the mutant is supposed to get wrong. Simulated:
// A landed last (silently "passing" under the mutant) in ~60% of runs. A
// same-height competitor can never fix this: whoever is genuinely correct
// might also be the mutant's own accidental last-write.
//
// Fixed by removing the competition entirely: C is a genuine, fully valid
// child of the CURRENT tip whose body is WITHHELD (PerfWithholdGuard) --
// F-135's own "BLOCK_HAVE_BODY_RECORD is set unconditionally the moment
// ReceivedBlockTransactions runs, independent of bodies_held" means C is
// still genuinely indexed by hash, but ConnectTip's HaveBodies() gate
// (F-101/F-104/F-117) means C can NEVER connect -- no other block will ever
// occupy heightC in this test. Under the loop-source-swap mutant, ANY write
// to heightC is wrong, regardless of iteration order -- there is no
// competing legitimate entry for an unlucky mutant to coincidentally produce.
BOOST_AUTO_TEST_CASE(body_index_hash_half_covers_a_side_chain_block_the_height_half_never_does) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();
    CChainState &chainstate = ::ChainstateActive();

    const CBlockIndex *tipBefore = chainstate.m_chain.Tip();
    BOOST_REQUIRE(tipBefore != nullptr);
    int heightC = tipBefore->nHeight + 1;

    CBlock blockC = CreateBlock({}, coinbaseKey);
    std::shared_ptr<const CBlock> shared_pblockC = std::make_shared<const CBlock>(blockC);
    uint256 hashC = blockC.GetHash();

    {
        PerfWithholdGuard guard(hashC);
        BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblockC, /*fForceProcessing=*/true, nullptr));
    }

    const CBlockIndex *pindexC = LookupBlockIndex(hashC);
    BOOST_REQUIRE(pindexC != nullptr);
    BOOST_REQUIRE(pindexC->nStatus & BLOCK_HAVE_BODY_RECORD);
    BOOST_REQUIRE_EQUAL(pindexC->nHeight, heightC);
    BOOST_REQUIRE(chainstate.m_chain.Tip() == tipBefore);   // never connected

    FlatFilePos posOut;
    uint256 hashOut;
    BOOST_REQUIRE(LookupBodyPositionByHash(hashC, posOut));
    BOOST_CHECK_EQUAL(posOut.nFile, pindexC->GetBodyPos().nFile);
    BOOST_CHECK_EQUAL(posOut.nPos, pindexC->GetBodyPos().nPos);

    // No active-chain block occupies heightC at all.
    BOOST_CHECK(!LookupBodyPositionAtHeight(heightC, posOut, hashOut));

    // A real reload (LoadBlockIndexDB rebuilds the hash half from EVERY known
    // block, LoadChainTip rebuilds the height half from the active chain
    // only) must preserve exactly this distinction, deterministically.
    ResetBodyIndex();
    chainstate.m_chain.SetTip(nullptr);
    BOOST_REQUIRE(!LookupBodyPositionByHash(hashC, posOut));

    {
        LOCK(cs_main);
        BOOST_REQUIRE(LoadBlockIndexDB(chainman, chainparams));
        BOOST_REQUIRE(chainstate.LoadChainTip(chainparams));
    }

    BOOST_REQUIRE(LookupBodyPositionByHash(hashC, posOut));
    BOOST_CHECK_EQUAL(posOut.nFile, pindexC->GetBodyPos().nFile);
    BOOST_CHECK_EQUAL(posOut.nPos, pindexC->GetBodyPos().nPos);

    BOOST_CHECK(!LookupBodyPositionAtHeight(heightC, posOut, hashOut));
}

// F-143 (2.2.2 spec deliverable): pins the index-space invariant
// bodyrange.h's VtxIndexFromBodyIndex/BodyIndexFromVtxIndex document but
// bodyrange_tests.cpp can only assert as bare arithmetic -- against a REAL
// accepted block's own body store, not a hand-built fixture, matching
// F-115's own precedent for exactly this off-by-one class of bug. For every
// body index `i`, `ReadBodyAt(pos, i)` must be the transaction at
// `block.vtx[VtxIndexFromBodyIndex(i)]`, and asking one index past the end
// must fail cleanly.
BOOST_AUTO_TEST_CASE(vtx_index_from_body_index_matches_a_real_accepted_block) {
    CMutableTransaction spendTx1 = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CMutableTransaction spendTx2 = MakeSpendOfCoinbase(m_coinbase_txns[1], coinbaseKey);
    CBlock block = CreateAndProcessBlock({spendTx1, spendTx2}, coinbaseKey);
    CBlockIndex *pindex = LookupBlockIndex(block.GetHash());
    BOOST_REQUIRE(pindex != nullptr);
    FlatFilePos pos = pindex->GetBodyPos();
    BOOST_REQUIRE(!pos.IsNull());

    unsigned int count = 0;
    BOOST_REQUIRE(ReadBodyRecordCount(pos, count));
    // block.vtx[0] is the coinbase, never stored in the body store.
    BOOST_REQUIRE_EQUAL(count, block.vtx.size() - 1);

    for (uint32_t bodyIndex = 0; bodyIndex < count; bodyIndex++) {
        CTransactionRef txOut;
        BOOST_REQUIRE(ReadBodyAt(pos, bodyIndex, txOut));
        uint32_t vtxIndex = VtxIndexFromBodyIndex(bodyIndex);
        // F-147 (Fable review of F-146, LOW): an off-by-one mutant of
        // VtxIndexFromBodyIndex would otherwise index block.vtx
        // out-of-bounds here and "die" via UB/a crash rather than a clean
        // assertion failure.
        BOOST_REQUIRE_LT(vtxIndex, block.vtx.size());
        BOOST_CHECK(txOut->GetHash() == block.vtx[vtxIndex]->GetHash());
        BOOST_CHECK_EQUAL(BodyIndexFromVtxIndex(vtxIndex), bodyIndex);
    }

    CTransactionRef txOut;
    BOOST_CHECK(!ReadBodyAt(pos, count, txOut));
}

// 2.2.4 (build-plan.md's 2.2 row, F-139's own spec): the body-ARRIVAL write
// path -- ProcessFetchedBodyRange turns wire-fetched bodies into a durably
// stored, indexed, connectable block for a pindex whose commitments are
// known but whose body record has never been written at all (unlike every
// existing withhold-then-arrival test above, which always starts from a
// record SaveBodyToDisk already wrote unconditionally at accept -- the only
// shape the -perfwithholdbody harness produces). No such pindex is
// reachable via any real accept path today (a genuine commitment-only
// relay/accept is 4.1's own still-unbuilt job) -- simulated directly here,
// the same "clobber the index to a state only a future real path would
// produce" technique this file's own reload/prune/pre-2.1.4 tests already
// use (F-135/F-141/F-145).
static void SimulateBodyRecordNeverWritten(CBlockIndex *pindex) {
    pindex->nStatus &= ~(BLOCK_HAVE_BODY_RECORD | BLOCK_HAVE_BODIES);
    pindex->nBodyFile = -1;
    pindex->nBodyPos = 0;
    RecordBodyPositionByHash(pindex->GetBlockHash(), FlatFilePos(), /*fServeable=*/false);
}

BOOST_AUTO_TEST_CASE(process_fetched_body_range_writes_and_connects_a_genuinely_unwritten_body) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CBlock block = CreateBlock({spendTx}, coinbaseKey);
    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
    uint256 hash = block.GetHash();

    CBlockIndex *pindex;
    {
        // PerfWithholdGuard gets a real, commitment-only-shaped accept (no
        // connect) without this test having to hand-roll AcceptBlockHeader
        // plumbing -- SimulateBodyRecordNeverWritten then erases the body
        // record the harness still wrote underneath it (F-134/F-135: always
        // unconditional), leaving exactly 2.2.4's real target precondition.
        PerfWithholdGuard guard(hash);
        BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblock, /*fForceProcessing=*/true, nullptr));
    }
    pindex = LookupBlockIndex(hash);
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(pindex->nStatus & BLOCK_HAVE_DATA);
    SimulateBodyRecordNeverWritten(pindex);
    BOOST_REQUIRE(pindex->GetBodyPos().IsNull());
    BOOST_REQUIRE(!(pindex->nStatus & BLOCK_HAVE_BODY_RECORD));
    BOOST_REQUIRE(!HaveBodies(pindex));

    CCommitmentBlock commitments = CommitmentsFromBlock(block);
    std::vector<CTransactionRef> bodies(block.vtx.begin() + 1, block.vtx.end());

    CValidationState state;
    BOOST_REQUIRE(::ChainstateActive().ProcessFetchedBodyRange(pindex, commitments, bodies, state, chainparams));

    BOOST_CHECK(pindex->nStatus & BLOCK_HAVE_BODY_RECORD);
    BOOST_CHECK(pindex->nStatus & BLOCK_HAVE_BODIES);
    BOOST_CHECK(HaveBodies(pindex));
    FlatFilePos pos = pindex->GetBodyPos();
    BOOST_REQUIRE(!pos.IsNull());

    std::vector<CTransactionRef> bodiesOut;
    BOOST_REQUIRE(ReadBodyRecord(pos, bodiesOut));
    BOOST_REQUIRE_EQUAL(bodiesOut.size(), bodies.size());
    for (size_t i = 0; i < bodies.size(); i++) {
        BOOST_CHECK(bodiesOut[i]->GetHash() == bodies[i]->GetHash());
    }

    FlatFilePos posOut;
    BOOST_REQUIRE(LookupServeableBodyPositionByHash(hash, posOut));
    BOOST_CHECK_EQUAL(posOut.nFile, pos.nFile);
    BOOST_CHECK_EQUAL(posOut.nPos, pos.nPos);

    // The point of the mechanism: the block must become connectable, not
    // just carry the right bits (same reasoning as
    // unrequested_arrival_fills_a_commitment_only_block above).
    CValidationState activateState;
    BOOST_REQUIRE(::ChainstateActive().ActivateBestChain(activateState, chainparams, nullptr));
    BOOST_CHECK(::ChainActive().Tip()->GetBlockHash() == hash);
}

// A peer that answers with bodies not matching the block's committed
// identifiers is THAT PEER's fault, not a fact about the block -- must not
// permanently fail the block (BLOCK_FAILED_VALID), unlike a genuine
// body-dependent consensus violation (the next test).
BOOST_AUTO_TEST_CASE(process_fetched_body_range_rejects_a_hash_mismatch_without_failing_the_block) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CBlock block = CreateBlock({spendTx}, coinbaseKey);
    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
    uint256 hash = block.GetHash();

    CBlockIndex *pindex;
    {
        PerfWithholdGuard guard(hash);
        BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblock, /*fForceProcessing=*/true, nullptr));
    }
    pindex = LookupBlockIndex(hash);
    BOOST_REQUIRE(pindex != nullptr);
    SimulateBodyRecordNeverWritten(pindex);

    CCommitmentBlock commitments = CommitmentsFromBlock(block);
    // A body that does not match the committed identifier at index 0 --
    // MaterialiseBlock's own hash check must reject this.
    CMutableTransaction wrongTx = MakeSpendOfCoinbase(m_coinbase_txns[1], coinbaseKey);
    std::vector<CTransactionRef> wrongBodies = {MakeTransactionRef(wrongTx)};

    CValidationState state;
    BOOST_CHECK(!::ChainstateActive().ProcessFetchedBodyRange(pindex, commitments, wrongBodies, state, chainparams));
    BOOST_CHECK(state.IsInvalid());
    // This peer's own answer being wrong is not proof the BLOCK is invalid.
    BOOST_CHECK(state.CorruptionPossible());
    BOOST_CHECK(!(pindex->nStatus & BLOCK_FAILED_VALID));
    // Nothing was written.
    BOOST_CHECK(pindex->GetBodyPos().IsNull());
    BOOST_CHECK(!(pindex->nStatus & BLOCK_HAVE_BODY_RECORD));
    BOOST_CHECK(!HaveBodies(pindex));
}

// Unlike a hash mismatch (one specific peer's answer being wrong -- not a
// fact about the block), a genuine body-dependent consensus violation IS a
// fact about the block. This is the BLOCK_FAILED_VALID branch of
// ProcessFetchedBodyRange -- untested until this case, despite the
// hash-mismatch test's own comment above promising it ("the next test").
//
// Getting here needs care: AcceptBlock's own accept-time pipeline ALWAYS
// runs the full CheckBlock/ContextualCheckBlock against today's only real
// local CBlock, REGARDLESS of PerfWithholdGuard (which only defers the body
// WRITE/claim, per validation.cpp's own comment on this -- "not actually
// before any per-transaction work runs on today's only real path"). So a
// block carrying a genuinely invalid non-coinbase tx can never reach
// ProcessNewBlock's return true at all -- there is no way, with today's test
// infrastructure, to get an INVALID block's own pindex into the index via
// the normal accept path (matching F-139's still-open point: nothing yet
// delivers a genuine standalone CCommitmentBlock over the wire, so a
// genuinely commitment-only, never-locally-validated accept isn't
// constructible today either).
//
// Instead: accept a real, VALID block (giving pindex a realistic chain
// context -- pprev, height, time -- for ContextualCheckBlock to check
// against), then hand ProcessFetchedBodyRange a COMMITMENTS/BODIES pair that
// is internally self-consistent (MaterialiseBlock's hash check passes) but
// commits to a structurally invalid transaction instead of the block's real
// one -- isolating the test to exactly ProcessFetchedBodyRange's own error
// handling for a genuine CheckBlock failure, independent of how that
// mismatch could arise in a not-yet-built real commitment-only accept.
// ProcessFetchedBodyRange itself does not (and structurally cannot, without
// a real commitment-only accept path to compare against) cross-check
// `commitments` against `pindexNew`'s own true original content -- the real
// production caller (net_processing.cpp) always sources `commitments` via
// ReadCommitmentBlockFromDisk(pindex), which guarantees this pairing by
// construction; this test relies on the SAME latitude the hash-mismatch
// test above already takes (a deliberately mismatched bodies/commitments
// pair), just breaking a different half of the pairing.
BOOST_AUTO_TEST_CASE(process_fetched_body_range_fails_the_block_on_a_genuine_body_dependent_violation) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CBlock block = CreateBlock({spendTx}, coinbaseKey);
    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
    uint256 hash = block.GetHash();

    CBlockIndex *pindex;
    {
        PerfWithholdGuard guard(hash);
        BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblock, /*fForceProcessing=*/true, nullptr));
    }
    pindex = LookupBlockIndex(hash);
    BOOST_REQUIRE(pindex != nullptr);
    SimulateBodyRecordNeverWritten(pindex);

    // The real header + real coinbase (both already accepted and valid),
    // but the ONE committed identifier swapped for a structurally invalid
    // transaction's hash -- CheckTransaction's own "bad-txns-vout-negative"
    // (consensus/tx_check.cpp), body-dependent since nothing about a
    // non-coinbase tx's own field values is ever visible from the
    // commitment view alone. Mutating after signing doesn't matter here:
    // CheckBlock/ContextualCheckBlock never run script/signature
    // verification (ConnectBlock's own job).
    CCommitmentBlock commitments = CommitmentsFromBlock(block);
    CMutableTransaction invalidTx(spendTx);
    invalidTx.vout[0].nValue = -1;
    CTransactionRef invalidTxRef = MakeTransactionRef(invalidTx);
    // CreateBlock may add more than just spendTx (e.g. a founder/masternode
    // payment) -- find spendTx's own committed slot rather than assume
    // index 0, and substitute the invalid tx only there.
    std::vector<CTransactionRef> bodies(block.vtx.begin() + 1, block.vtx.end());
    bool fFoundSpendTx = false;
    for (size_t i = 0; i < bodies.size(); i++) {
        if (bodies[i]->GetHash() == spendTx.GetHash()) {
            commitments.vCommitments[i] = invalidTxRef->GetHash();
            bodies[i] = invalidTxRef;
            fFoundSpendTx = true;
            break;
        }
    }
    BOOST_REQUIRE(fFoundSpendTx);

    CValidationState state;
    BOOST_CHECK(!::ChainstateActive().ProcessFetchedBodyRange(pindex, commitments, bodies, state, chainparams));
    BOOST_CHECK(state.IsInvalid());
    // The critical distinction from the hash-mismatch test above: this is a
    // real consensus violation, not a single peer's bad answer -- it must be
    // stamped permanently invalid, not excused as corruption.
    BOOST_CHECK(!state.CorruptionPossible());
    BOOST_CHECK(pindex->nStatus & BLOCK_FAILED_VALID);
    // Nothing was written -- the failure is caught before any disk write.
    BOOST_CHECK(pindex->GetBodyPos().IsNull());
    BOOST_CHECK(!(pindex->nStatus & BLOCK_HAVE_BODY_RECORD));
    BOOST_CHECK(!HaveBodies(pindex));
}

// Defensive no-op: a pindex that already has bodies (an unsolicited/stale/
// duplicate arrival, or simply the wrong call site) must not be touched --
// the caller's own in-flight bookkeeping is what should have filtered this
// out; this is a second, independent guard, not the only one.
BOOST_AUTO_TEST_CASE(process_fetched_body_range_is_a_noop_when_bodies_are_already_held) {
    const CChainParams &chainparams = Params();

    CBlock block = CreateAndProcessBlock({}, coinbaseKey);
    uint256 hash = block.GetHash();
    CBlockIndex *pindex = LookupBlockIndex(hash);
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(HaveBodies(pindex));
    FlatFilePos posBefore = pindex->GetBodyPos();

    CCommitmentBlock commitments = CommitmentsFromBlock(block);
    std::vector<CTransactionRef> bodies(block.vtx.begin() + 1, block.vtx.end());

    CValidationState state;
    BOOST_CHECK(!::ChainstateActive().ProcessFetchedBodyRange(pindex, commitments, bodies, state, chainparams));
    BOOST_CHECK(state.IsValid());  // a no-op, not a rejection
    BOOST_CHECK_EQUAL(pindex->GetBodyPos().nFile, posBefore.nFile);
    BOOST_CHECK_EQUAL(pindex->GetBodyPos().nPos, posBefore.nPos);
}

// F-161 (4.1.1, the F-160 audit's own highest-severity finding):
// CQuorumBlockProcessor::UpgradeDB (llmq/quorums_blockprocessor.cpp) walks
// the WHOLE active chain from height 1, gating its per-block
// ReadBlockFromDisk on `fPruneMode && !(pindex->nStatus & BLOCK_HAVE_DATA)`
// -- the wrong bit (a commitment-only block also has BLOCK_HAVE_DATA set,
// the same F-36-class conflation already fixed everywhere else in this
// project) AND the wrong condition (only checked under fPruneMode at all,
// so a non-traditionally-pruned node with a body-missing active-chain
// block -- not reachable via any real accept path today, F-109's own
// exclusion filter keeps a commitment-only block off ::ChainActive()
// entirely, but exactly what a future body-retention window would
// produce -- skips the check and crashes). Constructed the way this
// file's own precedent handles a state no real path produces yet
// (SimulateBodyRecordNeverWritten's "clobber to a future-real-path state"
// technique): accept a block for real (genuine bodies, genuinely
// connected), then retroactively withhold it (PerfWithholdGuard makes the
// underlying ReadBlockFromDisk fail, matching what a real body-retention
// eviction would produce) while clearing BLOCK_HAVE_BODIES to match --
// the block stays on the active chain throughout, since clearing a status
// bit after connection doesn't disconnect it.
BOOST_AUTO_TEST_CASE(quorum_upgrade_db_does_not_crash_on_a_body_missing_active_chain_block) {
    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CBlock block = CreateAndProcessBlock({spendTx}, coinbaseKey);
    uint256 hash = block.GetHash();
    CBlockIndex *pindex = LookupBlockIndex(hash);
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(::ChainActive().Contains(pindex));
    BOOST_REQUIRE(HaveBodies(pindex));

    PerfWithholdGuard guard(hash);
    pindex->nStatus &= ~BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(!HaveBodies(pindex));
    BOOST_REQUIRE(::ChainActive().Contains(pindex));

    // UpgradeDB has never run in this fresh fixture's evoDb (no test
    // anywhere calls it -- confirmed by grep, only init.cpp's real startup
    // path does), so this genuinely walks from height 1 rather than
    // early-returning on an already-set marker.
    BOOST_CHECK(!llmq::quorumBlockProcessor->UpgradeDB());
}

// F-163 (4.1.1, F-160's own named A1 site): GetTransaction (validation.cpp)
// read straight from blk*.dat with no HaveBodies guard -- not a crash risk
// today (ReadBlockFromDisk already fails gracefully on a genuine miss), but
// a real correctness gap: blk*.dat always holds the real bytes regardless
// of BLOCK_HAVE_BODIES today (SaveBlockToDisk writes unconditionally,
// F-134/F-135), so the pre-fix code would happily materialise and return a
// transaction from a block the index claims NOT to hold bodies for --
// proven directly here, not just asserted: the same real bytes are still
// on disk throughout this test (nothing touches blk*.dat), so a fix
// regression would make this test's own final check fail by finding the
// transaction anyway, not by crashing.
BOOST_AUTO_TEST_CASE(get_transaction_respects_have_bodies_even_though_the_read_would_succeed) {
    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CBlock block = CreateAndProcessBlock({spendTx}, coinbaseKey);
    uint256 hash = block.GetHash();
    uint256 txHash = spendTx.GetHash();
    CBlockIndex *pindex = LookupBlockIndex(hash);
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(HaveBodies(pindex));

    uint256 hashBlockOut;
    CTransactionRef found = GetTransaction(pindex, nullptr, txHash, Params().GetConsensus(), hashBlockOut);
    BOOST_REQUIRE(found != nullptr);
    BOOST_CHECK(found->GetHash() == txHash);

    // Clear the bit -- the real bytes are untouched, matching what a
    // future body-retention window withdrawing bodies from an
    // already-connected block would look like from GetTransaction's own
    // vantage point.
    pindex->nStatus &= ~BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(!HaveBodies(pindex));

    uint256 hashBlockOut2;
    CTransactionRef foundAfter = GetTransaction(pindex, nullptr, txHash, Params().GetConsensus(), hashBlockOut2);
    BOOST_CHECK(foundAfter == nullptr);
}

// F-168 (4.1.1 closeout, item 2): gettxoutproof (rpc/rawtransaction.cpp)
// reads straight from blk*.dat with no HaveBodies guard ahead of it -- same
// shape and same fix as F-163's GetTransaction, and the same reason a
// PerfWithholdGuard isn't needed to prove it: blk*.dat holds the real bytes
// unconditionally today (F-110), so clearing BLOCK_HAVE_BODIES alone is
// enough to distinguish "checks the bit" from "happened to fail because the
// bytes were genuinely gone" -- nothing here touches blk*.dat, so a fix
// regression would make the final call succeed again, not crash.
BOOST_AUTO_TEST_CASE(gettxoutproof_respects_have_bodies_even_though_the_read_would_succeed) {
    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CBlock block = CreateAndProcessBlock({spendTx}, coinbaseKey);
    uint256 hash = block.GetHash();
    uint256 txHash = spendTx.GetHash();
    CBlockIndex *pindex = LookupBlockIndex(hash);
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(HaveBodies(pindex));

    const std::string cmd = "gettxoutproof [\"" + txHash.ToString() + "\"] " + hash.ToString();
    BOOST_CHECK_NO_THROW(CallRPCForTest(m_node, cmd));

    pindex->nStatus &= ~BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(!HaveBodies(pindex));

    BOOST_CHECK_EXCEPTION(CallRPCForTest(m_node, cmd), std::runtime_error,
                          [](const std::runtime_error &e) {
                              return std::string(e.what()).find("bodies not held") != std::string::npos;
                          });

    // Restore before the fixture tears down -- matches this file's own
    // convention (verifydb_stops_at_a_bodies_gap_..., quorum_upgrade_db_...)
    // of leaving the index consistent with the still-present real bytes.
    pindex->nStatus |= BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(HaveBodies(pindex));
}

// F-168 (4.1.1 closeout, item 3): the `smartnode payments` RPC
// (rpc/smartnode.cpp) has the identical unguarded-read shape, same fix, same
// no-PerfWithholdGuard-needed reasoning as gettxoutproof above.
BOOST_AUTO_TEST_CASE(smartnode_payments_respects_have_bodies_even_though_the_read_would_succeed) {
    CBlock block = CreateAndProcessBlock({}, coinbaseKey);
    uint256 hash = block.GetHash();
    CBlockIndex *pindex = LookupBlockIndex(hash);
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(HaveBodies(pindex));

    const std::string cmd = "smartnode payments " + hash.ToString() + " 1";
    BOOST_CHECK_NO_THROW(CallRPCForTest(m_node, cmd));

    pindex->nStatus &= ~BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(!HaveBodies(pindex));

    BOOST_CHECK_EXCEPTION(CallRPCForTest(m_node, cmd), std::runtime_error,
                          [](const std::runtime_error &e) {
                              return std::string(e.what()).find("bodies not held") != std::string::npos;
                          });

    pindex->nStatus |= BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(HaveBodies(pindex));
}

// F-169 (independent review of F-160/4.1.1, CONFIRMED HIGH): NodeRoundVoting::
// GetVote (update/update.cpp) called ReadBlockFromDisk with no HaveBodies
// guard and a hard assert(r) -- reachable from Updates().State()'s own
// round-voting walk, itself called from 39 non-test sites including
// consensus code (validation.cpp:2198 IsActive/IsAssetsActive, evo/
// providertx.cpp, llmq/*). Never found by F-160's original audit at all.
// Unlike F-163/F-168/F-171/F-172's "wrong yes" class, this genuinely
// crashes, so proving it needs a real read failure, not just a cleared
// status bit (blk*.dat's real bytes stay in place under Phase 1, F-110, and
// the read would silently succeed with the bit alone) -- PerfWithholdGuard
// + a bit-clear, matching F-161's own SIGABRT-reproduction technique.
// RED verified manually, not as part of this committed test (which asserts
// the FIXED graceful behaviour, since an assert() failure aborts the whole
// test binary and can't be caught in-process): the fix was reverted, this
// test was run, and it reproduced a genuine `Assertion 'r' failed` SIGABRT
// -- matching the independent reviewer's own reproduction exactly -- before
// the fix was restored.
BOOST_AUTO_TEST_CASE(node_round_voting_getvote_respects_have_bodies_instead_of_crashing) {
    // Regtest's ROUND_VOTING params (chainparams.cpp): bit=1, roundSize=10,
    // startHeight=100 -- NodeRoundVoting::GetVote's own early-return guards
    // (partial-round rejection, before-start rejection) only clear once the
    // round block being evaluated reaches height startHeight+roundSize=110,
    // so mine 10 further blocks past TestChain100Setup's own 100.
    for (int i = 0; i < 10; i++) {
        CreateAndProcessBlock({}, coinbaseKey);
    }
    CBlockIndex *pindex = ::ChainActive().Tip();
    BOOST_REQUIRE_EQUAL(pindex->nHeight, 110);

    // Withhold a block genuinely inside the round the walk covers
    // (GetVote(pindex@110) walks pprev 10 times -> heights 109 down to 100).
    CBlockIndex *pindexWithheld = pindex->GetAncestor(105);
    BOOST_REQUIRE(pindexWithheld != nullptr);
    BOOST_REQUIRE(HaveBodies(pindexWithheld));
    uint256 withheldHash = pindexWithheld->GetBlockHash();

    PerfWithholdGuard guard(withheldHash);
    CBlock unreadable;
    BOOST_REQUIRE(!ReadBlockFromDisk(unreadable, pindexWithheld, Params().GetConsensus()));  // sanity
    pindexWithheld->nStatus &= ~BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(!HaveBodies(pindexWithheld));

    // A standalone UpdateManager, not the global Updates() singleton:
    // ordinary block connection already calls Updates().IsAssetsActive()
    // (validation.cpp) on every block as it's accepted, including all 110
    // mined above, which populates BOTH UpdateManager's own per-round
    // `states` cache AND NodeRoundVoting's own per-(update,blockIndex)
    // `cache` -- by the time a test manually withholds a block and calls
    // the global Updates() afterwards, the vote for that exact round may
    // already be cached from when the block genuinely had its bodies,
    // masking the gap instead of reproducing it. A fresh UpdateManager has
    // never queried this update/blockIndex pair, so it genuinely walks and
    // reads, exactly like a node computing this round's vote for the first
    // time (e.g. right after IBD, or Updates()'s own worst case).
    // votingPeriod=1, not regtest's real 10: NodeUpdateVoting::GetVote (the
    // wrapper State() actually calls) has its OWN, even stricter threshold
    // -- StartHeight() + RoundSize()*VotingPeriod() -- before it ever calls
    // into NodeRoundVoting::GetVote at all. With regtest's real
    // votingPeriod=10 that threshold is height 200, needing 100 further
    // mined blocks just to reach it; votingPeriod=1 drops it to exactly 110,
    // matching NodeRoundVoting::GetVote's own StartHeight()+RoundSize()
    // guard, so the round mined above is enough.
    UpdateManager testUpdates;
    testUpdates.Add(Update(EUpdate::ROUND_VOTING, std::string("Test Round Voting"), 1, 10, 100, 1, 100, 10, false,
                           VoteThreshold(95, 95, 5), VoteThreshold(0, 0, 1)));

    // Before the fix this SIGABRTs (Assertion 'r' failed); after it, State()
    // completes without crashing. Not asserting a specific State value --
    // with no real vote bits set anywhere in this synthetic chain, Voting is
    // the expected shape, but the point of this test is "did not crash",
    // matching this round's own priority (graceful degradation over a
    // specific vote outcome).
    StateInfo si = testUpdates.State(EUpdate::ROUND_VOTING, pindex);
    BOOST_CHECK(si.State == EUpdateState::Voting || si.State == EUpdateState::Defined);

    pindexWithheld->nStatus |= BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(HaveBodies(pindexWithheld));
}

// F-173 (independent review of F-160/4.1.1, LOW): getmerkleblocks
// (rpc/blockchain.cpp) only guards its FIRST block, via GetBlockChecked
// (F-164) -- the loop that walks forward from there via ::ChainActive().Next()
// reads every further block with its own separate, unguarded
// ReadBlockFromDisk call. Proves the loop's OWN read, not GetBlockChecked's
// already-covered one: the withheld block here is the 5th block from the
// range's start, never touched by GetBlockChecked at all.
BOOST_AUTO_TEST_CASE(getmerkleblocks_respects_have_bodies_for_every_block_not_just_the_first) {
    CBlockIndex *pindexTip = ::ChainActive().Tip();
    BOOST_REQUIRE(pindexTip != nullptr);
    CBlockIndex *pindexStart = pindexTip->GetAncestor(pindexTip->nHeight - 10);
    CBlockIndex *pindexWithheld = pindexTip->GetAncestor(pindexTip->nHeight - 5);
    BOOST_REQUIRE(pindexStart != nullptr);
    BOOST_REQUIRE(pindexWithheld != nullptr);
    BOOST_REQUIRE(HaveBodies(pindexWithheld));

    const std::string filterHex =
        "2303028005802040100040000008008400048141010000f8400420800080025004000004130000000000000001";
    const std::string cmd = "getmerkleblocks " + filterHex + " " + pindexStart->GetBlockHash().ToString() + " 10";
    BOOST_CHECK_NO_THROW(CallRPCForTest(m_node, cmd));

    pindexWithheld->nStatus &= ~BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(!HaveBodies(pindexWithheld));

    BOOST_CHECK_EXCEPTION(CallRPCForTest(m_node, cmd), std::runtime_error,
                          [](const std::runtime_error &e) {
                              return std::string(e.what()).find("bodies not held") != std::string::npos;
                          });

    pindexWithheld->nStatus |= BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(HaveBodies(pindexWithheld));
}

// F-174 (independent review of F-160/4.1.1, LOW): interfaces::Chain::findBlock
// (interfaces/chain.cpp) -- used by wallet rescan and listsinceblock -- had
// no HaveBodies guard, unlisted anywhere by F-160's own original audit.
// Already fails gracefully by its own existing contract (SetNull() the
// output block, still return true -- callers are expected to check
// block.IsNull()), so this proves the fix extends that SAME contract to a
// body-not-held block, not a new behaviour.
BOOST_AUTO_TEST_CASE(findblock_respects_have_bodies_even_though_the_read_would_succeed) {
    CBlockIndex *pindexTip = ::ChainActive().Tip();
    BOOST_REQUIRE(pindexTip != nullptr);
    BOOST_REQUIRE(HaveBodies(pindexTip));

    CBlock block;
    BOOST_REQUIRE(m_node.chain->findBlock(pindexTip->GetBlockHash(), &block));
    BOOST_REQUIRE(!block.IsNull());

    pindexTip->nStatus &= ~BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(!HaveBodies(pindexTip));

    CBlock block2;
    bool found = m_node.chain->findBlock(pindexTip->GetBlockHash(), &block2);
    BOOST_CHECK(found);
    BOOST_CHECK(block2.IsNull());

    pindexTip->nStatus |= BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(HaveBodies(pindexTip));
}

// F-178 (independent review of F-171/F-175, CONFIRMED MEDIUM): smartnode_
// payments (rpc/smartnode.cpp) computes each non-coinbase transaction's fee
// by resolving every input's previous transaction via
// GetTransaction(nullptr, node.mempool, ...) and dereferencing the result --
// `CTransactionRef txPrev = GetTransaction(...); nValueIn +=
// txPrev->vout[...].nValue;` -- with no null check, unlike every other
// GetTransaction call site in this codebase (rest.cpp, rawtransaction.cpp,
// rpcevo.cpp, quorums_instantsend.cpp, ...). GetTransaction can always
// return nullptr in principle, but F-171's own HaveBodies guard on
// TxIndex::FindTx (the -txindex fallback GetTransaction's nullptr-block_index
// branch relies on) makes it a real, reachable crash today: a windowed node
// querying smartnode payments for a block whose spending transaction's
// input comes from a body-less block now genuinely gets nullptr back
// instead of the transaction.
// RED verified manually, not encoded in this committed test (a null-pointer
// dereference is undefined behaviour -- a process-ending SIGSEGV, not a C++
// exception BOOST_CHECK_EXCEPTION could catch in-process, matching F-169's
// own SIGABRT-can't-be-caught precedent): the fix was reverted, this test
// was run standalone, and it reproduced a genuine SIGSEGV inside
// smartnode_payments' own fee-calculation loop -- before the fix was
// restored.
BOOST_AUTO_TEST_CASE(smartnode_payments_null_checks_previous_transaction_instead_of_crashing) {
    // m_coinbase_txns[0]'s own containing block -- found via the global
    // g_txindex (already synced by TestChain100Setup, see
    // test_raptoreum.cpp's TestChainSetup ctor) while its bodies are still
    // held, before this test withholds them.
    uint256 spentBlockHash;
    CTransactionRef spentCoinbase;
    BOOST_REQUIRE(g_txindex->FindTx(m_coinbase_txns[0]->GetHash(), spentBlockHash, spentCoinbase));
    CBlockIndex *pindexSpent = LookupBlockIndex(spentBlockHash);
    BOOST_REQUIRE(pindexSpent != nullptr);
    BOOST_REQUIRE(HaveBodies(pindexSpent));

    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CBlock blockQ = CreateAndProcessBlock({spendTx}, coinbaseKey);
    uint256 hashQ = blockQ.GetHash();
    CBlockIndex *pindexQ = LookupBlockIndex(hashQ);
    BOOST_REQUIRE(pindexQ != nullptr);
    BOOST_REQUIRE(HaveBodies(pindexQ));

    // Sanity: with both blocks' bodies held, the RPC succeeds today.
    const std::string cmd = "smartnode payments " + hashQ.ToString() + " 1";
    BOOST_CHECK_NO_THROW(CallRPCForTest(m_node, cmd));

    // Withhold the SPENT block's bodies -- not blockQ's own -- so
    // smartnode_payments' own HaveBodies guard on blockQ still passes (it
    // reads blockQ fine) but resolving spendTx's input's previous
    // transaction, which lives in the now body-less block, fails.
    pindexSpent->nStatus &= ~BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(!HaveBodies(pindexSpent));
    uint256 blockHashTmp;
    // Sanity: GetTransaction genuinely returns nullptr now (via the
    // -txindex fallback, F-171's guard), not just "this test's premise is
    // wrong".
    BOOST_REQUIRE(!GetTransaction(nullptr, m_node.mempool, m_coinbase_txns[0]->GetHash(),
                                  Params().GetConsensus(), blockHashTmp));

    BOOST_CHECK_EXCEPTION(CallRPCForTest(m_node, cmd), std::runtime_error,
                          [](const std::runtime_error &e) {
                              return std::string(e.what()).find("not available") != std::string::npos;
                          });

    pindexSpent->nStatus |= BLOCK_HAVE_BODIES;
    BOOST_REQUIRE(HaveBodies(pindexSpent));
}

// 4.1.3 (F-183): relay atomicity -- "relay only what you can assemble"
// (transaction-decoupling.md §14.1 rule 1). Confirms, rather than just
// re-derives from source, that a commitment-only (withheld) block never
// reaches either fast-relay signal. NewPoWValidBlock's own call site
// (validation.cpp, F-117) already gates on HaveBodies(pindex) before firing
// -- this proves that guard actually holds under a real PerfWithholdGuard
// accept, not just that the source reads that way. UpdatedBlockTip is
// proven safe by the OTHER route the design doc names: it only ever fires
// for a block that genuinely became the new tip, and FindMostWorkChain's
// own ancestor-walk guard (F-109) never lets a body-missing block become a
// selectable candidate in the first place -- so pindexNew is never
// body-missing at every real call this test observes, not merely never
// equal to this one withheld block. UpdatedBlockTip is delivered via
// CMainSignals' own scheduler queue (validationinterface.cpp), unlike
// NewPoWValidBlock's direct/synchronous signal -- SyncWithValidationInterfaceQueue
// drains it before this test reads spy.updatedBlockTipCalls.
BOOST_AUTO_TEST_CASE(relay_signals_never_fire_for_a_commitment_only_block) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    RelayAtomicitySpy spy;
    RegisterValidationInterface(&spy);

    CBlock block = CreateBlock({}, coinbaseKey);
    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
    uint256 hash = block.GetHash();

    {
        PerfWithholdGuard guard(hash);
        bool fNewBlock = false;
        BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblock, /*fForceProcessing=*/true, &fNewBlock));
        BOOST_REQUIRE(fNewBlock);
    }
    SyncWithValidationInterfaceQueue();

    const CBlockIndex *pindex = LookupBlockIndex(hash);
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(!HaveBodies(pindex));
    // Never actually became the tip -- FindMostWorkChain's own guard, not
    // this test's own construction, is what kept it out.
    BOOST_REQUIRE(::ChainActive().Tip()->GetBlockHash() != hash);

    for (const CBlockIndex *called: spy.newPoWValidBlockCalls) {
        BOOST_CHECK(called != pindex);
    }
    for (const CBlockIndex *tipCalled: spy.updatedBlockTipCalls) {
        BOOST_CHECK(HaveBodies(tipCalled));
    }

    UnregisterValidationInterface(&spy);
}

// Positive control for the test above, in its own fixture instance (a fresh
// TestChain100Setup chain, not a continuation) rather than a second phase
// of the same test: an ordinary block on top of the current tip DOES fire
// NewPoWValidBlock and DOES become the new UpdatedBlockTip target. Without
// this, relay_signals_never_fire_for_a_commitment_only_block's own checks
// would pass vacuously if the signals never fired at all in this harness.
BOOST_AUTO_TEST_CASE(relay_signals_fire_for_an_ordinary_block) {
    ChainstateManager &chainman = EnsureChainman(m_node);
    const CChainParams &chainparams = Params();

    RelayAtomicitySpy spy;
    RegisterValidationInterface(&spy);

    CBlock block = CreateBlock({}, coinbaseKey);
    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
    uint256 hash = block.GetHash();
    bool fNewBlock = false;
    BOOST_REQUIRE(chainman.ProcessNewBlock(chainparams, shared_pblock, /*fForceProcessing=*/true, &fNewBlock));
    BOOST_REQUIRE(fNewBlock);
    SyncWithValidationInterfaceQueue();

    const CBlockIndex *pindex = LookupBlockIndex(hash);
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_REQUIRE(HaveBodies(pindex));
    BOOST_REQUIRE(::ChainActive().Tip()->GetBlockHash() == hash);

    BOOST_CHECK(std::find(spy.newPoWValidBlockCalls.begin(), spy.newPoWValidBlockCalls.end(), pindex)
                != spy.newPoWValidBlockCalls.end());
    BOOST_CHECK(std::find(spy.updatedBlockTipCalls.begin(), spy.updatedBlockTipCalls.end(), pindex)
                != spy.updatedBlockTipCalls.end());

    UnregisterValidationInterface(&spy);
}

BOOST_AUTO_TEST_SUITE_END()
