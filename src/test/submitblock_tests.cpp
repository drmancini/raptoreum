// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// 4.4.2 (F-192): submitblock's dual-serialization support -- a new explicit
// "format" parameter ("full" default, "commitment" alternate), orchestration
// only, no new validation logic (build-plan.md's own row for this sub-step).
// miner_tests.cpp already covers MaterialiseSubmittedCommitmentBlock (the
// extracted body-resolution/materialise logic) directly against a
// standalone CTxMemPool. This file additionally exercises submitblock's own
// RPC-boundary wiring -- the "format" argument in the dispatch table, the
// decode/error-throwing paths, and a genuine end-to-end accept -- via real
// RPC dispatch (tableRPC.execute), the same pattern acceptancebit_tests.cpp's
// own CallRPCForTest already established on this fixture.

#include <chainparams.h>
#include <consensus/validation.h>
#include <evo/specialtx.h>
#include <key.h>
#include <keystore.h>
#include <llmq/quorums_blockprocessor.h>
#include <llmq/quorums_commitment.h>
#include <node/context.h>
#include <primitives/block.h>
#include <rpc/mining.h>
#include <rpc/protocol.h>
#include <rpc/server.h>
#include <script/script.h>
#include <script/sign.h>
#include <script/standard.h>
#include <streams.h>
#include <txmempool.h>
#include <util/ref.h>
#include <util/strencodings.h>
#include <validation.h>
#include <version.h>

#include <test/test_raptoreum.h>

#include <boost/test/unit_test.hpp>

#include <univalue.h>

namespace submitblock_tests {

static UniValue CallSubmitBlock(NodeContext &node, const std::string &hexdata, const UniValue &format) {
    util::Ref context{node};
    JSONRPCRequest request(context);
    request.strMethod = "submitblock";
    request.params = UniValue(UniValue::VARR);
    request.params.push_back(hexdata);
    request.params.push_back(NullUniValue); // dummy, BIP22 compat, ignored
    request.params.push_back(format);
    request.fHelp = false;
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    return tableRPC.execute(request);
}

static std::string HexEncode(const CCommitmentBlock &commitments) {
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << commitments;
    return HexStr(ss);
}

static std::string HexEncode(const CBlock &block) {
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << block;
    return HexStr(ss);
}

// 2.1.4/acceptancebit_tests.cpp's own precedent: a real, ECDSA-signed spend
// of a mature coinbase, so a commitment-mode block in this file names a
// genuine non-coinbase transaction, not just a coinbase-only block (which
// would leave the mempool-resolution loop untested).
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

// F-196 Part B: submitblock's own "commitment" format branch is now gated by
// the same serving-obligation check as getblocktemplate (4.4.3's own gate,
// widened here to cover this call site too). Every PRE-EXISTING
// commitment-mode test in this file exercises the resolution/materialisation
// code paths those tests were actually written for, not this new gate --
// they opt in via -servebodyrange for their own duration. Matches
// denialofservice_tests' own save/restore precedent, but via ForceRemoveArg
// (util/system.h) rather than ForceSetArg, since the flag starts genuinely
// unset in this binary and must end that way too.
struct ServeBodyRangeGuard {
    ServeBodyRangeGuard() { gArgs.ForceSetArg("-servebodyrange", "1"); }

    ~ServeBodyRangeGuard() { gArgs.ForceRemoveArg("-servebodyrange"); }
};

BOOST_FIXTURE_TEST_SUITE(submitblock_tests, TestChain100Setup)

// "full" format, unnamed (the default) and named explicitly, must behave
// identically -- both resubmit an ALREADY-ACCEPTED block, which
// submitblock's own pre-existing duplicate check answers without touching
// any of this sub-step's new code.
BOOST_AUTO_TEST_CASE(full_format_default_matches_explicit_full) {
    CBlock alreadyAccepted = CreateAndProcessBlock({}, coinbaseKey);
    const std::string hex = HexEncode(alreadyAccepted);

    UniValue withoutFormat = CallSubmitBlock(m_node, hex, NullUniValue);
    BOOST_REQUIRE(withoutFormat.isStr());
    BOOST_CHECK_EQUAL(withoutFormat.get_str(), "duplicate");

    UniValue withExplicitFull = CallSubmitBlock(m_node, hex, UniValue("full"));
    BOOST_REQUIRE(withExplicitFull.isStr());
    BOOST_CHECK_EQUAL(withExplicitFull.get_str(), "duplicate");
}

// Message-specific rather than "some UniValue was thrown" -- a "bogus" format
// string could in principle also fail later (e.g. a decode error), which
// would still be a UniValue throw but would mean the dispatch's own format
// validation never ran at all. Checking the message pins down which guard
// actually fired.
BOOST_AUTO_TEST_CASE(unknown_format_is_rejected) {
    CBlock alreadyAccepted = CreateAndProcessBlock({}, coinbaseKey);
    const std::string hex = HexEncode(alreadyAccepted);
    BOOST_CHECK_EXCEPTION(CallSubmitBlock(m_node, hex, UniValue("bogus")), UniValue,
                         [](const UniValue &e) {
                             return find_value(e, "message").get_str().find("Unknown \"format\"") !=
                                    std::string::npos;
                         });
}

// The real round trip: a candidate block built with a genuine non-coinbase
// spend, converted to commitment form, submitted via the RPC in
// "commitment" format, resolved from THIS node's own mempool, and accepted
// -- the chain tip actually advances to it, not just "did not throw".
//
// F-193: TestChain100Setup's own regtest params run with DIP0003Enabled
// unconditionally, so CreateNewBlock (miner.cpp) can insert a genuine
// TRANSACTION_QUORUM_COMMITMENT transaction directly from
// quorumBlockProcessor -- entirely bypassing the mempool, never a
// mempool-sourced transaction at all. A real commitment-mode block can
// legitimately contain one of these; every OTHER committed transaction here
// is staged into the mempool the same way a real miner's own selection
// would populate it, so this test resolves whatever candidate.vtx actually
// contains rather than assuming it is only the one spend.
BOOST_AUTO_TEST_CASE(commitment_format_round_trips_through_the_local_mempool) {
    ServeBodyRangeGuard servebodyrangeGuard; // F-196 Part B: opt in, not testing the gate here
    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);

    CBlock candidate = CreateBlock({spendTx}, coinbaseKey);

    TestMemPoolEntryHelper entry;
    for (size_t i = 1; i < candidate.vtx.size(); i++) {
        m_node.mempool->addUnchecked(entry.FromTx(candidate.vtx[i]));
    }

    CCommitmentBlock commitments = CommitmentsFromBlock(candidate);
    const std::string hex = HexEncode(commitments);

    UniValue result = CallSubmitBlock(m_node, hex, UniValue("commitment"));
    BOOST_CHECK(result.isNull());
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), candidate.GetHash().ToString());
}

// F-193: the quorum-commitment transaction present in candidate.vtx (confirmed
// empirically: TestChain100Setup's fixture always produces one, nType ==
// TRANSACTION_QUORUM_COMMITMENT) is deliberately NOT staged into the mempool here,
// unlike the round-trip test above -- only the ordinary spend is. Without the
// FindMineableCommitmentTxByHash fallback, this would throw "not found in local
// mempool" for the quorum-commitment transaction's hash; the block is still
// accepted, and the chain tip actually advances to it.
BOOST_AUTO_TEST_CASE(commitment_format_resolves_a_quorum_commitment_tx_the_mempool_never_held) {
    ServeBodyRangeGuard servebodyrangeGuard; // F-196 Part B: opt in, not testing the gate here
    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CBlock candidate = CreateBlock({spendTx}, coinbaseKey);

    bool sawQuorumCommitment = false;
    TestMemPoolEntryHelper entry;
    for (size_t i = 1; i < candidate.vtx.size(); i++) {
        if (candidate.vtx[i]->nType == TRANSACTION_QUORUM_COMMITMENT) {
            sawQuorumCommitment = true;
            continue; // the transaction under test: left out of the mempool on purpose
        }
        m_node.mempool->addUnchecked(entry.FromTx(candidate.vtx[i]));
    }
    // If the fixture ever stops producing one, this test would silently stop
    // testing anything -- fail loudly instead.
    BOOST_REQUIRE(sawQuorumCommitment);

    CCommitmentBlock commitments = CommitmentsFromBlock(candidate);
    const std::string hex = HexEncode(commitments);

    UniValue result = CallSubmitBlock(m_node, hex, UniValue("commitment"));
    BOOST_CHECK(result.isNull());
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), candidate.GetHash().ToString());
}

// F-195: FindMineableCommitmentTxByHash used to key its reconstruction to the
// LIVE CHAIN TIP at call time (::ChainActive().Tip()), not to the submitted
// block's own recorded parent (commitments.hashPrevBlock) -- so if the tip
// moves between template creation and submission (a miner losing a race is
// routine), the fallback reconstructs for the WRONG height and misses,
// throwing "not found in local mempool" for what would otherwise be a valid
// (if stale) block -- submitblock in "full" format would have accepted the
// same block fine. Reproduces the reviewer's own probe: build a candidate at
// tip N, let the tip advance to N+1 via a DIFFERENT block, then confirm the
// stale-but-valid sibling at height N+1 still resolves and is accepted.
BOOST_AUTO_TEST_CASE(commitment_format_resolves_a_stale_sibling_after_the_tip_moved) {
    ServeBodyRangeGuard servebodyrangeGuard; // F-196 Part B: opt in, not testing the gate here
    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    // Built against the CURRENT tip -- becomes a stale sibling the moment a
    // different block advances the tip below.
    CBlock staleCandidate = CreateBlock({spendTx}, coinbaseKey);

    bool sawQuorumCommitment = false;
    TestMemPoolEntryHelper entry;
    for (size_t i = 1; i < staleCandidate.vtx.size(); i++) {
        if (staleCandidate.vtx[i]->nType == TRANSACTION_QUORUM_COMMITMENT) {
            sawQuorumCommitment = true;
            continue; // the transaction under test: left out of the mempool on purpose
        }
        m_node.mempool->addUnchecked(entry.FromTx(staleCandidate.vtx[i]));
    }
    // If the fixture ever stops producing one, this test would silently stop
    // testing the fallback path at all -- fail loudly instead.
    BOOST_REQUIRE(sawQuorumCommitment);

    // A DIFFERENT block, extending the SAME parent, wins the race: the real
    // chain tip advances to height N+1 via a sibling of staleCandidate, not
    // staleCandidate itself.
    CBlock winner = CreateAndProcessBlock({}, coinbaseKey);
    BOOST_REQUIRE(winner.GetHash() != staleCandidate.GetHash());
    BOOST_REQUIRE(staleCandidate.hashPrevBlock == winner.hashPrevBlock);
    BOOST_REQUIRE_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), winner.GetHash().ToString());

    CCommitmentBlock commitments = CommitmentsFromBlock(staleCandidate);
    const std::string hex = HexEncode(commitments);

    // Resolution must succeed for the STALE sibling, keyed to its OWN
    // recorded parent, regardless of what the live tip has since become.
    // Deliberately not asserting result.isNull() or that the tip becomes
    // staleCandidate: staleCandidate and winner have EQUAL work, so which
    // one keeps the active tip is a first-seen tie-break unrelated to
    // F-195 -- winner (already connected) keeps it, and staleCandidate is
    // correctly accepted as a valid side branch ("inconclusive" is BIP22's
    // own honest answer for a block never run through full ConnectBlock,
    // not a rejection). What must NOT happen is the old bug: an uncaught
    // "not found in local mempool" JSONRPCError, or an explicit "invalid"
    // rejection. A real, indexed, non-failed block entry proves the block
    // was genuinely decoded, resolved and passed CheckBlock -- none of which
    // could happen if resolution had thrown.
    UniValue result = CallSubmitBlock(m_node, hex, UniValue("commitment"));
    if (result.isStr()) {
        BOOST_CHECK(result.get_str() != "invalid");
    }
    LOCK(cs_main);
    const CBlockIndex *pindex = LookupBlockIndex(staleCandidate.GetHash());
    BOOST_REQUIRE(pindex != nullptr);
    BOOST_CHECK(!(pindex->nStatus & BLOCK_FAILED_MASK));
}

// F-195 (second candidate): the currently-known-best commitment for a quorum
// session can change -- a better one arrives via AddMineableCommitment --
// between template creation and submission, so GetMineableCommitmentTx's
// CURRENT answer no longer matches what was actually mined into the
// template. The deterministic "null commitment" form the template
// originally used (this fixture never has a real DKG session, so it is
// always the null form) is still reconstructible via the new
// GetNullCommitmentTx and must be tried as a second candidate before giving
// up.
BOOST_AUTO_TEST_CASE(commitment_format_resolves_after_a_better_commitment_supersedes_the_null_one) {
    ServeBodyRangeGuard servebodyrangeGuard;
    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CBlock candidate = CreateBlock({spendTx}, coinbaseKey);

    CTransactionRef nullCommitmentTx;
    for (const auto &tx : candidate.vtx) {
        if (tx->nType == TRANSACTION_QUORUM_COMMITMENT) {
            nullCommitmentTx = tx;
            break;
        }
    }
    BOOST_REQUIRE(nullCommitmentTx != nullptr);

    llmq::CFinalCommitmentTxPayload originalPayload;
    BOOST_REQUIRE(GetTxPayload(*nullCommitmentTx, originalPayload));
    // The fixture never runs a real DKG session, so this is always the null
    // form -- if that ever stops holding, this test would silently stop
    // testing the fallback it claims to.
    BOOST_REQUIRE(originalPayload.commitment.IsNull());

    TestMemPoolEntryHelper entry;
    for (size_t i = 1; i < candidate.vtx.size(); i++) {
        if (candidate.vtx[i]->GetHash() == nullCommitmentTx->GetHash()) {
            continue; // resolved via the fallback under test, not the mempool
        }
        m_node.mempool->addUnchecked(entry.FromTx(candidate.vtx[i]));
    }

    // Simulate a real, better commitment arriving for the SAME session
    // (same llmqType/quorumHash) after candidate's template was built.
    // AddMineableCommitment (quorums_blockprocessor.cpp) is the real
    // mechanism a verified P2P QFCOMMITMENT message uses to register one;
    // called directly here since this fixture never runs a live DKG.
    llmq::CFinalCommitment better(Params().GetConsensus().llmqs.at(originalPayload.commitment.llmqType),
                             originalPayload.commitment.quorumHash);
    better.signers = {true};
    better.validMembers = {true};
    llmq::quorumBlockProcessor->AddMineableCommitment(better);

    CCommitmentBlock commitments = CommitmentsFromBlock(candidate);
    const std::string hex = HexEncode(commitments);

    // GetMineableCommitmentTx now answers with `better`, not the null form
    // candidate.vtx actually contains -- resolution must fall back to
    // reconstructing the null form directly rather than giving up.
    UniValue result = CallSubmitBlock(m_node, hex, UniValue("commitment"));
    BOOST_CHECK(result.isNull());
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), candidate.GetHash().ToString());
}

BOOST_AUTO_TEST_CASE(commitment_format_throws_a_clean_error_on_a_missing_body) {
    ServeBodyRangeGuard servebodyrangeGuard; // F-196 Part B: opt in, not testing the gate here
    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    // Deliberately never added to the mempool.
    CBlock candidate = CreateBlock({spendTx}, coinbaseKey);
    CCommitmentBlock commitments = CommitmentsFromBlock(candidate);
    const std::string hex = HexEncode(commitments);

    BOOST_CHECK_EXCEPTION(CallSubmitBlock(m_node, hex, UniValue("commitment")), UniValue,
                         [](const UniValue &e) {
                             return find_value(e, "message").get_str().find("not found in local mempool") !=
                                    std::string::npos;
                         });
    BOOST_CHECK(::ChainActive().Tip()->GetBlockHash().ToString() != candidate.GetHash().ToString());
}

// F-196 Part B: EnforceCommitmentModeServingObligation (4.4.3's own gate) was
// only ever called from getblocktemplate -- submitblock's own "commitment"
// format branch (4.4.2) incurs the exact same serving obligation (a
// commitment-mode block's assembly-time-only transactions are held by no
// other node until served) but was reachable without it: a client can build
// its own CCommitmentBlock from an ordinary full-mode template, or poll one
// node and submit to a different one, entirely bypassing a commitment-mode
// getblocktemplate call on the submitting node. Every committed transaction
// here IS staged into the mempool (unlike the missing-body test above) so
// that, absent this gate, the submission would fully resolve and succeed --
// isolating this test to the gate itself, not a resolution failure.
BOOST_AUTO_TEST_CASE(commitment_format_refuses_without_servebodyrange) {
    BOOST_REQUIRE(!gArgs.IsArgSet("-servebodyrange"));

    CMutableTransaction spendTx = MakeSpendOfCoinbase(m_coinbase_txns[0], coinbaseKey);
    CBlock candidate = CreateBlock({spendTx}, coinbaseKey);

    TestMemPoolEntryHelper entry;
    for (size_t i = 1; i < candidate.vtx.size(); i++) {
        m_node.mempool->addUnchecked(entry.FromTx(candidate.vtx[i]));
    }

    CCommitmentBlock commitments = CommitmentsFromBlock(candidate);
    const std::string hex = HexEncode(commitments);

    BOOST_CHECK_EXCEPTION(CallSubmitBlock(m_node, hex, UniValue("commitment")), UniValue,
                         [](const UniValue &e) {
                             const std::string msg = find_value(e, "message").get_str();
                             return msg.find("submitblock") != std::string::npos &&
                                    msg.find("-servebodyrange") != std::string::npos &&
                                    msg.find("must not run on a live/exposed network") != std::string::npos;
                         });
    // Never even reached ProcessNewBlock -- the gate refused before any
    // resolution/materialisation work, so the block was never accepted.
    BOOST_CHECK(::ChainActive().Tip()->GetBlockHash().ToString() != candidate.GetHash().ToString());

    // Same candidate, opted in: the gate is the ONLY thing that changed --
    // resolution/materialisation/acceptance still all work, confirming the
    // failure above was specifically the gate, not e.g. a body this test
    // forgot to stage.
    {
        ServeBodyRangeGuard servebodyrangeGuard;
        UniValue result = CallSubmitBlock(m_node, hex, UniValue("commitment"));
        BOOST_CHECK(result.isNull());
    }
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace submitblock_tests
