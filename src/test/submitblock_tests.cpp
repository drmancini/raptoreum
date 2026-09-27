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
#include <key.h>
#include <keystore.h>
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

BOOST_AUTO_TEST_CASE(commitment_format_throws_a_clean_error_on_a_missing_body) {
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

BOOST_AUTO_TEST_SUITE_END()

} // namespace submitblock_tests
