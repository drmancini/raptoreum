#!/usr/bin/env python3
# Copyright (c) 2026 The Raptoreum developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
import struct

from test_framework.messages import CTransaction, FromHex, ToHex
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_raises_rpc_error

'''
feature_attestedtx_activation.py

5.4.3 (F-242) named this gap explicitly and left it open: "no test/functional coverage
exists for this consensus change ... regtest's own structurally-unreachable placeholder
height makes one impossible without a new -testactivationheight-style CLI knob." That knob
(F-249, -testactivationheight=<bit>:<height>) was built the same week for a live demo --
this closes the gap it was built to close.

Exercises the real production entry point (sendrawtransaction -> AcceptToMemoryPoolWorker
-> ContextualCheckTransaction's whitelist -> CheckSpecialTx -> CheckAttestedTx) through a
real node's own mempool, reached by genuine mining past a genuine activation height -- not
an in-process C++ fixture and not a forced-activation override (both already covered by
attestedtx_activation_tests.cpp/txvalidation_tests.cpp; this is the real-node gap those
cannot close).

No live quorum is needed, and the point is not that a garbage signature gets accepted --
it never does, in either phase. The point is WHICH check rejects the identical transaction
shape changes once the bit activates: bad-txns-type (the whitelist itself, before -- proof
CheckAttestedTx is never even reached) vs bad-attested-tx-attestation (CheckAttestedTx's own
quorum verification, after -- proof the real dispatch path is genuinely live).
'''

TRANSACTION_ATTESTED = 11
SPECIAL_TX_VERSION = 3
ATTESTED_TX_BIT = 4


class AttestedTxActivationTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-testactivationheight=%d:100" % ATTESTED_TX_BIT]]

    def activate_attested_tx(self):
        """Mine until EUpdate::ATTESTED_TX reports active -- mirrors activate_v17's own
        pattern exactly, against the new per-bit knob instead of the hardcoded v17 height."""
        node = self.nodes[0]
        while node.getblockchaininfo()["rip1_softforks"]["Attested Transaction"]["status"] != "active":
            self.bump_mocktime(1)
            node.generate(10)

    def build_attested_tx(self, sign_height):
        """A real wallet-funded, wallet-signed transaction, reshaped into a
        TRANSACTION_ATTESTED carrying a deliberately all-zero signature -- the
        signature's own validity is never what this test is about (see module
        docstring)."""
        node = self.nodes[0]
        addr = node.getnewaddress()
        rawtx = node.createrawtransaction([], {addr: 1})
        # A generous explicit feeRate: fundrawtransaction sizes the fee against this
        # transaction's CURRENT size, before vExtraPayload (99 bytes) gets appended below,
        # and the default rate alone is not comfortably above min relay fee once it is.
        rawtx = node.fundrawtransaction(rawtx, {"feeRate": 0.01})["hex"]

        tx = FromHex(CTransaction(), rawtx)
        tx.nVersion = SPECIAL_TX_VERSION
        tx.nType = TRANSACTION_ATTESTED
        # CAttestationPayload v1: nVersion(u16 LE) + nSignHeight(i32 LE) + sig(96 bytes).
        tx.vExtraPayload = struct.pack("<H", 1) + struct.pack("<i", sign_height) + b"\x00" * 96

        signed = node.signrawtransactionwithwallet(ToHex(tx))
        assert signed["complete"], signed
        return signed["hex"]

    def run_test(self):
        node = self.nodes[0]
        node.generate(110)

        # Before activation: the whitelist itself rejects the type outright.
        # CheckAttestedTx -- and the garbage signature inside -- is never reached.
        tx_hex = self.build_attested_tx(node.getblockcount())
        assert_raises_rpc_error(-26, "bad-txns-type", node.sendrawtransaction, tx_hex)

        self.activate_attested_tx()

        # After activation: the identical shape now passes the whitelist and reaches
        # CheckAttestedTx's own quorum verification, rejected there specifically --
        # proof the real production dispatch path is genuinely live on a real node.
        tx_hex = self.build_attested_tx(node.getblockcount())
        assert_raises_rpc_error(-26, "bad-attested-tx-attestation", node.sendrawtransaction, tx_hex)


if __name__ == '__main__':
    AttestedTxActivationTest().main()
