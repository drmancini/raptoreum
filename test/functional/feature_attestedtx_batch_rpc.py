#!/usr/bin/env python3
# Copyright (c) 2026 The Raptoreum developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
from test_framework.messages import CTransaction, FromHex, ToHex
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

'''
feature_attestedtx_batch_rpc.py

5.4.4.2's own header comment (llmq/quorums_attestationbatch.h) named the wire transport as
"deliberately NOT built here ... this row's own next sub-step" -- build-plan.md's row repeated
the same gap. CAttestationBatchHandler itself has been built, reviewed and unit-tested since
F-245; what never existed was a way for anything outside this node's own process to reach
RequestAttestation/GetAttestation, or for the handler to even be running on a real node at all
(no Start()/Stop() call anywhere in init.cpp). F-251 closes both: the lifecycle wiring
(llmq/quorums_init.cpp, mirrors chainLocksHandler's own identical shape) and two new RPCs,
attestedtx_requestattestation / attestedtx_getattestation (rpc/rpcattestedtx.cpp).

This test covers what a single node, with no live quorum, actually can exercise -- the same
honest limit every other quorum-dependent test in this feature has carried since F-216:

1. A genuinely, correctly-signed TRANSACTION_ATTESTED-shaped transaction is accepted by
   requestattestation. This is the one behaviour that matters most to get right and that v1's
   own equivalent path never had: RequestAttestation runs a REAL per-input signature check at
   request time (the F-245 CRITICAL fix) -- unlike CheckAttestedTx itself, which never looks at
   scriptSig at all. A transaction carrying a well-formed-but-WRONG signature (same technique
   test/perf/badsig.py already established) must be REJECTED here, proving that check is real
   and not a no-op -- the inverse of attestedtx_activation.py's own point, and the test this
   project's own "a guard present is not a guard that works" lesson asks for directly.
2. Requesting the identical transaction a second time does not error (idempotent, per
   RequestAttestation's own doc comment).
3. getattestation on a transaction never submitted, or one still awaiting a tick, returns null
   -- not found is not an error.

No live quorum is constructible in this environment (F-216), so the "a batch actually recovers
a signature" path is NOT exercised here -- same limitation every other test in this feature
carries, and explicitly out of scope for a single-node functional test.
'''


class AttestedTxBatchRpcTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def build_signed_tx(self):
        node = self.nodes[0]
        addr = node.getnewaddress()
        rawtx = node.createrawtransaction([], {addr: 1})
        rawtx = node.fundrawtransaction(rawtx, {"feeRate": 0.01})["hex"]

        tx = FromHex(CTransaction(), rawtx)
        tx.nVersion = 3
        tx.nType = 11  # TRANSACTION_ATTESTED
        tx.vExtraPayload = b""

        signed = node.signrawtransactionwithwallet(ToHex(tx))
        assert signed["complete"], signed
        return signed["hex"]

    def corrupt_signature(self, tx_hex):
        """badsig.py's own established technique: replace the first input's own
        signature with a different, genuine signature over an unrelated digest --
        valid DER, low-S, wrong. Proves the request-time check is real, not that a
        malformed signature merely fails to parse."""
        import os
        import coincurve
        from test_framework.script import CScript

        tx = FromHex(CTransaction(), tx_hex)
        pushes = list(CScript(tx.vin[0].scriptSig))
        assert_equal(len(pushes), 2)
        _sig, pubkey = pushes
        wrong = coincurve.PrivateKey(os.urandom(32)).sign(os.urandom(32), hasher=None)
        tx.vin[0].scriptSig = CScript([wrong + bytes([1]), pubkey])  # SIGHASH_ALL = 1
        return ToHex(tx)

    def run_test(self):
        node = self.nodes[0]
        node.generate(110)

        good_hex = self.build_signed_tx()
        bad_hex = self.corrupt_signature(good_hex)

        # The real per-input signature check (F-245's own CRITICAL fix) rejects a
        # well-formed-but-wrong signature -- proving it is genuinely checked, not
        # merely present. This is the opposite of v1's own CheckAttestedTx, which
        # never looks at scriptSig at all.
        assert_raises_rpc_error(-26, None, node.attestedtx_requestattestation, bad_hex)

        # The genuinely-signed transaction is accepted.
        assert_equal(node.attestedtx_requestattestation(good_hex), True)

        # Requesting the identical transaction again is idempotent, not an error.
        assert_equal(node.attestedtx_requestattestation(good_hex), True)

        # No live quorum exists in this environment (F-216) -- nothing can have
        # recovered a signature yet, for either transaction.
        assert node.attestedtx_getattestation(good_hex) is None
        assert node.attestedtx_getattestation(bad_hex) is None


if __name__ == '__main__':
    AttestedTxBatchRpcTest().main()
