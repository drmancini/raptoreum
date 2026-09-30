#!/usr/bin/env python3
# Copyright (c) 2026 The Raptoreum developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Confirm assets and futures actually work in a genuinely decoupled block --
not reasoned from docs/transaction-decoupling.md 2.4's classification
("special transactions ... already runs at connect"), proven live.

Mike, 2026-09-30: "does our chain fully support assets and futures ... I'm
curious if they will work in decoupled blocks." 2.4's table classifies every
special-tx check as body-dependent, and ConnectBlock (which calls
ProcessSpecialTxsInBlock) never cares how a block's bodies arrived -- but
that conclusion had never been exercised against a real asset or future
transaction sitting inside a real commitment-only block, recovered via a
real GETBODYRANGE/BODYRANGE round trip between two independent node
processes. Every existing asset test (feature_assets.py, feature_assets_
rules.py, feature_characterise_assets.py) and every existing futures test
(feature_futures.py, feature_characterise_futures.py) runs with no
decoupling flags at all; every existing decoupling test (feature_bodyrange_
e2e.py and its siblings) never constructs a real asset or future payload.
This is the first test crossing both.

Built on feature_bodyrange_e2e.py's own proven two-node shape (F-159's own
noted gap, closed there for a plain transaction) rather than re-deriving it:
withhold one node's own view of a block via -perfwithholdheight/-perfwithhold
count=1 (2+ collides with ProcessFetchedBodyRange's own completion path, see
that file's own module doc for why), let the OTHER node serve it via
-servebodyrange, and require a genuine GETBODYRANGE/BODYRANGE round trip
(assert_debug_log on both sides) rather than the whole-block GETDATA
fallback.

Chain height and the withheld height both have real constraints, not
arbitrary round numbers: CHAIN_HEIGHT=830 is feature_assets.py's own bar for
"Round Voting active, launch-subsidy blocks matured, enough spendable RTM to
pay the 100 RTM asset fee" (regtest's own subsidy ramp pays only 4 RTM/block
until height 720). WITHHELD_HEIGHT=CHAIN_HEIGHT+1=831 must also sit outside
llmq_test's own DKG mining window (bodyrange_e2e.py's own DKG_INTERVAL=30/
MINING_WINDOW=(10,18) constants) or a quorum-commitment tx competes for the
same block; 831 % 30 == 21, outside the window.
"""
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, connect_nodes, disconnect_nodes, wait_until

DKG_INTERVAL = 30         # llmq_test, quorums_parameters.h
MINING_WINDOW = (10, 18)  # dkgMiningWindowStart .. End
CHAIN_HEIGHT = 830        # feature_assets.py's own bar: RIP1 active, coins matured, fee affordable
WITHHELD_HEIGHT = CHAIN_HEIGHT + 1
WITHHOLD_COUNT = 1        # see feature_bodyrange_e2e.py's own module doc for why not 2+

SPORK22_FEE_100 = 25600   # second byte = fee in whole RTM (feature_assets.py's own constant)
ASSET_NAME = "DECOUPLED"

TRANSACTION_NEW_ASSET = 8   # src/primitives/transaction.h
TRANSACTION_FUTURE = 7      # src/primitives/transaction.h


class FeatureDecoupledAssetsFuturesTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            [
                # fetcher: never sees the real body except via GETBODYRANGE
                "-fetchbodyrange=1",
                "-perfwithholdheight=%d" % WITHHELD_HEIGHT,
                "-perfwithholdcount=%d" % WITHHOLD_COUNT,
                "-assetindex",
                "-txindex",
            ],
            [
                # server: mines the real content and answers GETBODYRANGE
                "-commitmentblocks=1",
                "-servebodyrange=1",
                "-assetindex",
                "-txindex",
                "-sporkkey=cVpnZj4dZvRXmBf7Jze1GjpLQb25iKP92GDXUsKdUJTXhXRo2RFA",
            ],
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        fetcher, server = self.nodes

        self.log.info("priming the server alone to a height past the subsidy ramp, with mature coins")
        owner = server.getnewaddress()
        while server.getblockcount() < CHAIN_HEIGHT:
            server.generatetoaddress(min(100, CHAIN_HEIGHT - server.getblockcount()), owner)
        stage = WITHHELD_HEIGHT % DKG_INTERVAL
        assert not (MINING_WINDOW[0] <= stage <= MINING_WINDOW[1]), \
            "WITHHELD_HEIGHT must sit outside the DKG mining window"

        self.log.info("connecting the fetcher and letting it IBD-sync the primed chain normally")
        connect_nodes(fetcher, 1)
        wait_until(lambda: fetcher.getblockcount() == CHAIN_HEIGHT, timeout=30)
        assert_equal(fetcher.getbestblockhash(), server.getbestblockhash())

        self.log.info("opening the special-tx fee spork on the server, waiting for both nodes to see it")
        server.spork("SPORK_22_SPECIAL_TX_FEE", SPORK22_FEE_100)
        wait_until(lambda: all(n.spork("show")["SPORK_22_SPECIAL_TX_FEE"] == SPORK22_FEE_100
                               for n in self.nodes), timeout=30)

        self.log.info("disconnecting before mining -- an already-synced, still-connected peer gets the "
                      "withheld block via a LIVE compact-block tip announcement (cmpctblock/getblocktxn/"
                      "blocktxn), which reconstructs the whole block directly and races -perfwithholdheight's "
                      "own one-shot commitment-only accept for the SAME height (confirmed live: the race is "
                      "real, not hypothetical -- an earlier version of this test connected throughout and "
                      "compact-block relay won, silently skipping GETBODYRANGE entirely). A peer that is "
                      "genuinely behind catches up via headers + block-download instead, which -fetchbodyrange "
                      "routes through GETBODYRANGE with no competing path.")
        disconnect_nodes(fetcher, 1)

        self.log.info("building a real asset-creation tx and a real future-carrying tx, both unconfirmed")
        asset_txid = server.createasset({
            "name": ASSET_NAME,
            "updatable": True,
            "is_root": True,
            "is_unique": False,
            "decimalpoint": 2,
            "referenceHash": "",
            "maxMintCount": 10,
            "type": 0,
            "targetAddress": owner,
            "issueFrequency": 0,
            "amount": 10000,
            "ownerAddress": owner,
        })["txid"]
        future_txid = server.sendtoaddress(
            address=server.getnewaddress(),
            amount=100,
            future={"future_maturity": 3, "future_locktime": -1},
        )
        assert_equal(set(server.getrawmempool()), {asset_txid, future_txid})

        self.log.info("mining both into the withheld block while disconnected")
        server.generatetoaddress(1, owner)
        assert_equal(server.getblockcount(), WITHHELD_HEIGHT)

        self.log.info("reconnecting -- the fetcher is now genuinely behind by one block, forcing a real "
                      "GETBODYRANGE/BODYRANGE round trip, not the whole-block GETDATA fallback, to recover it")
        # A "mid-fetch, RPCs say bodies-not-held" assertion was tried here and
        # dropped: on localhost, a one-block GETBODYRANGE round trip completes
        # faster than this process can reliably observe the gap between
        # "header known" and "body known" without an artificial stall, making
        # that specific window a coin flip to catch, not a real signal.
        # F-227's own fix (validation.cpp's GetTransaction/TxIndex::FindTx
        # out-param) is type-agnostic -- it is driven purely by the block's
        # HaveBodies status, never by what the transaction itself contains --
        # so there is no asset/future-specific path for that behaviour to
        # diverge on; already covered generically, not re-tested here.
        with fetcher.assert_debug_log(["received: bodyrange"]), \
             server.assert_debug_log(["received: getbodyrange"]):
            connect_nodes(fetcher, 1)
            wait_until(lambda: fetcher.getblockcount() == WITHHELD_HEIGHT, timeout=30)

        assert_equal(fetcher.getbestblockhash(), server.getbestblockhash())
        self.log.info("confirmed: a real GETBODYRANGE/BODYRANGE round trip recovered a block "
                      "carrying real asset and future transactions")

        self.log.info("post-fetch: both special transactions materialised correctly on the fetcher")
        asset_tx = fetcher.getrawtransaction(asset_txid, 1)
        assert_equal(asset_tx["type"], TRANSACTION_NEW_ASSET)

        future_tx = fetcher.getrawtransaction(future_txid, 1)
        assert_equal(future_tx["type"], TRANSACTION_FUTURE)
        assert_equal(future_tx["futureTx"]["maturity"], 3)
        assert_equal(future_tx["futureTx"]["lockTime"], -1)

        assert_equal(fetcher.listassets(), server.listassets())
        fetcher_details = fetcher.getassetdetailsbyname(ASSET_NAME)
        assert_equal(fetcher_details, server.getassetdetailsbyname(ASSET_NAME))
        assert_equal(fetcher_details["Asset_name"], ASSET_NAME)
        assert_equal(fetcher_details["owner"], owner)
        assert_equal(fetcher_details["Circulating_supply"], 0)
        self.log.info("confirmed: the asset and future are indistinguishable on the fetcher from a "
                      "node that received this block the traditional way")


if __name__ == "__main__":
    FeatureDecoupledAssetsFuturesTest().main()
