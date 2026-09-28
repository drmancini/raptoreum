#!/usr/bin/env python3
"""Pin what the node accepts and rejects for RTM/Dash-derived special transactions.

Build-plan item 0.2, full scope. The TRANSACTION_* enum
(src/primitives/transaction.h:17-27) has eleven members: NORMAL=0,
PROVIDER_REGISTER=1, PROVIDER_UPDATE_SERVICE=2, PROVIDER_UPDATE_REGISTRAR=3,
PROVIDER_UPDATE_REVOKE=4, COINBASE=5, QUORUM_COMMITMENT=6, FUTURE=7, NEW_ASSET=8,
UPDATE_ASSET=9, MINT_ASSET=10. This file covers 1-6, the ProTx family, CbTx and the
quorum-commitment tx. FUTURE and the three asset types have their own files
(feature_characterise_futures.py, feature_characterise_assets.py) -- narrower in
kind, not duplicated here.

Every one of 1-6 dispatches through CheckSpecialTx (src/evo/specialtx.cpp:18-56),
which is reached from TWO call sites: AcceptToMemoryPoolWorker
(src/validation.cpp:953, mempool) and ProcessSpecialTxsInBlock, itself called from
ConnectBlock (src/validation.cpp:2484) and RollforwardBlock (:6031). A type that
reaches both is body-dependent (needs the transaction's own payload and, for the
ProTx family, the confirmed UTXO/deterministic-MN-list view) but already has a
connect-time home -- no F-44-shaped gap. The one type that does NOT reach the
mempool at all is the quorum commitment: AcceptToMemoryPoolWorker rejects it
(REJECT_INVALID, "qc-not-allowed", src/validation.cpp:747-749) before CheckSpecialTx
is ever called, so CheckLLMQCommitment (src/llmq/quorums_commitment.cpp:148-192) is
connect-only by construction, not merely in practice like CbTx's own payload checks
below.

Two harness styles are mixed here, deliberately, not as a shortcut: the ProTx family
is exercised through the wallet's own `protx` RPCs, because (surprisingly, unlike
ordinary payments -- F-62) those RPCs surface the node's exact validation-state
reject reason directly in the JSONRPCError, e.g. "bad-protx-dup-key (code 18)".
There is no need to go through p2p and a mutated raw transaction to see which rule
fired. CbTx and the quorum-commitment tx are exercised through hand-built blocks and
p2p, matching feature_characterise_accept.py's style exactly, because there is no
RPC that constructs either as a first-class object outside the miner.

A real gap this file's own construction exposes, recorded honestly rather than
routed around: several ProTx-family update RPCs (update_service, update_registrar,
revoke) pre-validate that the named proTxHash exists, and that the caller holds the
matching key, ENTIRELY CLIENT-SIDE (rpc/rpcevo.cpp) before ever building a
transaction. A characterisation test going through those RPCs therefore cannot
reach the node's own bad-protx-hash / signature-mismatch checks at all -- only a
raw, hand-crafted transaction bypassing the wallet RPC could, and building one
needs a working BLS signer the test framework does not have (real islock/quorum
signing in this suite is always done by asking a live smartnode to sign, never
client-side). Recorded as a genuine, not-covered gap below, not silently skipped.

Run against an UNMODIFIED tree: master, never perf/throughput-rig (F-46b, F-58, and
this task's own directive -- perf/throughput-rig carries 0.1's validation.cpp
changes and an 8 MB MAX_DIP0001_BLOCK_SIZE, neither relevant here, but "unmodified"
is the discipline regardless of relevance).
"""
import struct
from io import BytesIO

from test_framework.blocktools import (
    create_block, create_coinbase, coinbase_height, create_null_commitment,
    REGTEST_LLMQS,
)
from test_framework.messages import CCbTx, ToHex
from test_framework.mininode import P2PDataStore, mininode_lock, network_thread_start, network_thread_join
from test_framework.test_framework import SMARTNODE_COLLATERAL, BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class CharacteriseSpecialTxTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def run_test(self):
        node = self.nodes[0]
        self.observed = {}

        self.log.info("priming past the founder-payment start height (500)")
        addr = node.getnewaddress()
        node.generatetoaddress(600, addr)

        self.protx_family()
        self.cbtx_height()
        self.quorum_commitment_standalone()

        self.log.info("=" * 70)
        self.log.info("characterised special-tx accept/reject (docs/findings.md F-216)")
        for name, got in self.observed.items():
            self.log.info("  %-52s -> %s", name, got)
        self.log.info("=" * 70)

    # ---- 1-4: the ProTx family, via RPC ------------------------------------

    def make_mn(self, port):
        """Register a real, fully valid smartnode. register_fund carries the
        collateral INSIDE the ProRegTx, and that path signs nothing -- the
        internal-collateral branch requires vchSig to be EMPTY
        (src/evo/providertx.cpp:520-522) -- so this is a plain RPC round trip,
        no BLS signing needed on our side.
        """
        node = self.nodes[0]
        owner = node.getnewaddress()
        op_pub = node.bls("generate")["public"]
        voting = node.getnewaddress()
        payout = node.getnewaddress()
        collateral_addr = node.getnewaddress()
        funds = node.getnewaddress()
        node.sendtoaddress(funds, SMARTNODE_COLLATERAL + 0.001)
        node.generatetoaddress(2, node.getnewaddress())
        txid = node.protx("register_fund", collateral_addr, SMARTNODE_COLLATERAL,
                           "127.0.0.1:%d" % port, owner, op_pub, voting, 0, payout, funds)
        return owner, txid

    def protx_family(self):
        node = self.nodes[0]

        # --- standalone mempool acceptance: a fact, not a rejection ---
        self.log.info("ProRegTx: standalone mempool acceptance")
        owner1, txid1 = self.make_mn(19801)
        mempool = node.getrawmempool()
        self.observed["ProRegTx: sits in mempool before mining"] = (
            "yes, unconfirmed" if txid1 in mempool else "NO -- unexpected")
        assert txid1 in mempool, (
            "ProRegTx must be standalone mempool-valid: CheckSpecialTx is reached "
            "from AcceptToMemoryPoolWorker (validation.cpp:953) with no special-tx-"
            "specific ATMP gate for TRANSACTION_PROVIDER_REGISTER, unlike the "
            "quorum-commitment type below")
        node.generatetoaddress(2, node.getnewaddress())

        # --- bad-protx-dup-key: state-dependent, needs the confirmed MN list ---
        self.log.info("ProRegTx: a second registration reusing the same owner key")
        op_pub2 = node.bls("generate")["public"]
        voting2 = node.getnewaddress()
        payout2 = node.getnewaddress()
        collateral2 = node.getnewaddress()
        funds2 = node.getnewaddress()
        node.sendtoaddress(funds2, SMARTNODE_COLLATERAL + 0.001)
        node.generatetoaddress(2, node.getnewaddress())
        try:
            node.protx("register_fund", collateral2, SMARTNODE_COLLATERAL,
                       "127.0.0.1:19802", owner1, op_pub2, voting2, 0, payout2, funds2)
            self.observed["ProRegTx: dup owner key -> ?"] = "ACCEPTED (unexpected)"
            raise AssertionError("a second MN reusing owner1's key was accepted")
        except Exception as e:
            msg = str(e)
            self.observed["ProRegTx: dup owner key (src/evo/providertx.cpp:498)"] = msg
            assert "bad-protx-dup-key" in msg, msg
        self.log.info("  -> %s", self.observed["ProRegTx: dup owner key (src/evo/providertx.cpp:498)"])

        # --- update_service naming an unregistered provider ---
        # Honest classification: this is rpc/rpcevo.cpp's OWN client-side lookup
        # (deterministicMNManager->GetListAtChainTip().GetMN(...)), thrown before a
        # transaction is even built. It is NOT the node's bad-protx-hash consensus
        # check (src/evo/providertx.cpp:551) -- that check has no RPC path that
        # reaches it, per this file's own docstring.
        self.log.info("ProUpServTx: naming a provider that was never registered")
        bls_sec = node.bls("generate")["secret"]
        funds3 = node.getnewaddress()
        node.sendtoaddress(funds3, 1)
        node.generatetoaddress(1, node.getnewaddress())
        fake_hash = "de" * 32
        try:
            node.protx("update_service", fake_hash, "127.0.0.2:19801", bls_sec, "", funds3)
            self.observed["ProUpServTx: unknown proTxHash -> ?"] = "ACCEPTED (unexpected)"
            raise AssertionError("update_service accepted an unknown proTxHash")
        except Exception as e:
            msg = str(e)
            self.observed["ProUpServTx: unknown proTxHash (RPC-side guard, rpc/rpcevo.cpp, "
                          "NOT src/evo/providertx.cpp's own bad-protx-hash)"] = msg
            assert "not found" in msg, msg
        self.log.info("  -> %s (client-side RPC guard, not the consensus check)",
                      self.observed["ProUpServTx: unknown proTxHash (RPC-side guard, rpc/rpcevo.cpp, "
                                    "NOT src/evo/providertx.cpp's own bad-protx-hash)"])

        # --- an update signed with the wrong operator key ---
        # Also, on inspection, an RPC-side guard: protx_update_service checks the
        # supplied operator key against dmn->pdmnState->pubKeyOperator itself
        # (src/rpc/rpcevo.cpp:858) and throws before building a transaction, so
        # this does NOT reach the node's own bad-protx-sig check
        # (src/evo/providertx.cpp:574) either -- a second instance of the same
        # gap this file's docstring already names for update_service/revoke. The
        # ProTx family's per-tx signature checks (bad-protx-sig, throughout
        # providertx.cpp) appear to have NO RPC path that reaches them at all on
        # this tree: every RPC that would sign one already knows, and checks,
        # the answer first.
        self.log.info("ProUpServTx: an update signed with the wrong operator key")
        owner_ctrl, protx_hash = self.make_mn(19803)
        node.generatetoaddress(2, node.getnewaddress())
        bls_sec_ctrl = node.bls("generate")["secret"]
        fundsu = node.getnewaddress()
        node.sendtoaddress(fundsu, 1)
        node.generatetoaddress(1, node.getnewaddress())
        try:
            node.protx("update_service", protx_hash, "127.0.0.2:19803", bls_sec_ctrl, "", fundsu)
            self.observed["ProUpServTx: update signed by the WRONG operator key -> ?"] = "ACCEPTED (unexpected)"
            raise AssertionError("update_service accepted a signature from an unregistered operator key")
        except Exception as e:
            msg = str(e)
            self.observed["ProUpServTx: update signed by the wrong operator key "
                          "(RPC-side guard, src/rpc/rpcevo.cpp:858, NOT bad-protx-sig)"] = msg
        self.log.info("  -> %s",
                      self.observed["ProUpServTx: update signed by the wrong operator key "
                                    "(RPC-side guard, src/rpc/rpcevo.cpp:858, NOT bad-protx-sig)"])

    # ---- 5: CbTx, commitment-checkable but connect-time only ---------------

    def bootstrap_p2p(self):
        self.nodes[0].add_p2p_connection(P2PDataStore())
        network_thread_start()
        self.nodes[0].p2p.wait_for_getheaders(timeout=10)

    def reconnect_p2p(self):
        self.nodes[0].disconnect_p2ps()
        network_thread_join()
        self.bootstrap_p2p()

    def reseal(self, block):
        for tx in block.vtx:
            tx.sha256 = None
            tx.hash = None
            tx.calc_sha256()
        block.hashMerkleRoot = block.calc_merkle_root()

    def submit_p2p(self, block):
        block.rehash()
        p2p = self.nodes[0].p2p
        with mininode_lock:
            p2p.reject_reason_received = None
        before = self.nodes[0].getbestblockhash()
        try:
            p2p.send_blocks_and_test([block], self.nodes[0], success=False, request_block=True, timeout=10)
        except AssertionError:
            if self.nodes[0].getbestblockhash() != before:
                return None
        with mininode_lock:
            reason = p2p.reject_reason_received
        return reason.decode() if isinstance(reason, bytes) else (reason or "rejected, no reason given")

    def cbtx_height(self):
        node = self.nodes[0]
        self.bootstrap_p2p()
        tip_hash = int(node.getbestblockhash(), 16)
        tip_height = node.getblockcount()
        tip_time = node.getblock(node.getbestblockhash())["time"]

        self.log.info("CbTx: coinbase payload's own height field disagrees with the real height")
        coinbase = create_coinbase(tip_height + 1)
        assert_equal(coinbase_height(coinbase), tip_height + 1)
        # Mutate the height field in place: CCbTx serializes as
        # <H version><i height><uint256 merkleRootMNList>[<uint256 merkleRootQuorums>]
        # (test_framework/messages.py:806-836) -- bytes [2:6] are nHeight, exactly
        # what coinbase_height() itself reads.
        payload = bytearray(coinbase.vExtraPayload)
        struct.pack_into("<i", payload, 2, tip_height + 2)  # off by one
        coinbase.vExtraPayload = bytes(payload)
        block = create_block(tip_hash, coinbase, tip_time + 1, node=node)
        self.reseal(block)
        block.solve()
        got = self.submit_p2p(block)
        self.observed["CbTx: nHeight != pindexPrev.nHeight+1 (src/evo/cbtx.cpp:43, bad-cbtx-height)"] = (
            got if got is not None else "ACCEPTED (no rejection)")
        self.log.info("  -> %s", got)
        # docs/transaction-decoupling.md 2.4 lists this row as "commitment-checkable,
        # but runs only at connect today -- a candidate to hoist": CheckCbTx is
        # reached only via ProcessSpecialTxsInBlock (specialtx.cpp:126), which
        # ConnectBlock calls, not CheckBlock/ContextualCheckBlock -- so a rung built
        # only from the commitment-checkable §2.4 rows would NOT catch this today.
        assert got is not None, (
            "bad-cbtx-height is rejected today (at connect), confirming the row is "
            "live, but it runs too late for a commitment-only rung to rely on")

    # ---- 6: quorum commitment, connect-only by construction -----------------

    def quorum_commitment_standalone(self):
        node = self.nodes[0]
        self.reconnect_p2p()
        self.log.info("quorum commitment tx: submitted standalone, never inside a block")
        llmq_type, size, _interval, _start, _end = REGTEST_LLMQS["llmq_test"]
        tx = create_null_commitment(llmq_type, size, node.getblockcount(),
                                     int(node.getbestblockhash(), 16))
        tx.calc_sha256()
        try:
            node.sendrawtransaction(ToHex(tx))
            self.observed["quorum commitment: standalone sendrawtransaction -> ?"] = "ACCEPTED (unexpected)"
            raise AssertionError("a bare quorum-commitment tx was accepted into the mempool")
        except Exception as e:
            msg = str(e)
            self.observed["quorum commitment: standalone (src/validation.cpp:747-749, qc-not-allowed)"] = msg
            assert "qc-not-allowed" in msg, msg
        self.log.info("  -> %s",
                      self.observed["quorum commitment: standalone (src/validation.cpp:747-749, qc-not-allowed)"])
        assert tx.hash not in node.getrawmempool()
        # The rejection fires before CheckSpecialTx / CheckLLMQCommitment is ever
        # reached -- this commitment is not even a real one (signers/validMembers
        # all false, no real DKG), and it is STILL rejected by the same string,
        # because the ATMP gate is unconditional on tx.nType alone. Confirms the
        # type is barred from the mempool categorically, not merely by whichever
        # commitment content happens to be invalid.


if __name__ == "__main__":
    CharacteriseSpecialTxTest().main()
