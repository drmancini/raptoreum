#!/usr/bin/env python3
# Copyright (c) 2026 The Raptoreum developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The last item on the creative-scenario backlog: an asset MINT split across
two separate commitment-only blocks -- block N creates the asset, block N+1
mints more of it, and a fetcher never sees either block's real bytes except
through two independent GETBODYRANGE/BODYRANGE round trips.

feature_decoupled_assets_futures.py already proved assets and futures
materialise correctly inside ONE decoupled block. This is genuinely
different, not a re-run of that: `mintasset` (rpc/rpcassets.cpp:521) reads
`passetsCache->GetAssetMetaData` -- the SAME in-memory cache `ConnectBlock`'s
own special-tx dispatch (assets.cpp:AddAssets) updates, and nothing else --
so a mint transaction is load-bearing on its creation transaction already
being CONNECTED, not merely known-to-exist. The question worth a real test
is whether that cross-block dependency survives when BOTH blocks arrive as
commitment-only and get recovered independently, height-ordered, rather than
assuming ConnectTip's own strict height ordering (unrelated to decoupling,
pre-existing) is enough reason to skip building it.

`-perfwithholdheight` is deliberately passed TWICE, paired with
`-perfwithholdcount=2` -- not 1, not unset. Tried unset first and hit
feature_bodyrange_e2e.py's own already-documented F-218 gotcha for real:
`g_perf_withhold_count` is ONE counter SHARED across every matching height,
and `ReadBlockFromDisk`'s own non-decrementing peek (`PerfWithholdStillActive`,
validation.cpp) reports "still withheld" for as long as that counter stays
nonzero -- including for the READ-BACK `ConnectTip` performs immediately
after a REAL GETBODYRANGE fetch completes, which has nothing to do with the
harness and must not be treated as still-simulated. Left unset (the default,
"withholds forever"), that read-back fails FOREVER for both heights -- a
fatal AbortNode ("Failed to read block"), confirmed live before settling on
the fix: `PerfWithholdBodies` decrements the shared counter exactly once per
matching height's own header-accept (`old > 0`, returns whether THIS call's
own pre-decrement value was positive), and both headers are accepted during
ordinary headers-first sync, well before either GETBODYRANGE fetch even
starts -- so count=2 lets both decrements land first (2->1->0), leaving the
counter at 0 for both heights' later read-backs, exactly as count=1 does for
feature_bodyrange_e2e.py's own single-height case.

Built on feature_decoupled_assets_futures.py's own harness (reused directly,
not re-derived): same CHAIN_HEIGHT=830 bar (feature_assets.py's own subsidy-
maturity/fee-affordability requirement), same disconnect-before-mining fix
for the compact-block-relay race.
"""
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, connect_nodes, disconnect_nodes, wait_until

DKG_INTERVAL = 30         # llmq_test, quorums_parameters.h
MINING_WINDOW = (10, 18)  # dkgMiningWindowStart .. End
CHAIN_HEIGHT = 830        # feature_assets.py's own bar: RIP1 active, coins matured, fee affordable
CREATE_HEIGHT = CHAIN_HEIGHT + 1   # 831 -- the asset's own creation
MINT_HEIGHT = CHAIN_HEIGHT + 2     # 832 -- mints more of the SAME asset, in a SEPARATE block

SPORK22_FEE_100 = 25600   # second byte = fee in whole RTM (feature_assets.py's own constant)
ASSET_NAME = "MINTSPLIT"
MINT_AMOUNT = 500         # feature_assets.py's own convention: per-mint issuance in whole units

TRANSACTION_NEW_ASSET = 8   # src/primitives/transaction.h
TRANSACTION_MINT_ASSET = 10


class FeatureDecoupledAssetMintSplitTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            [
                # fetcher: never sees either real body except via GETBODYRANGE
                "-fetchbodyrange=1",
                "-perfwithholdheight=%d" % CREATE_HEIGHT,
                "-perfwithholdheight=%d" % MINT_HEIGHT,
                "-perfwithholdcount=2",
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
        for h in (CREATE_HEIGHT, MINT_HEIGHT):
            stage = h % DKG_INTERVAL
            assert not (MINING_WINDOW[0] <= stage <= MINING_WINDOW[1]), \
                "height %d must sit outside the DKG mining window" % h

        self.log.info("connecting the fetcher and letting it IBD-sync the primed chain normally")
        connect_nodes(fetcher, 1)
        wait_until(lambda: fetcher.getblockcount() == CHAIN_HEIGHT, timeout=30)
        assert_equal(fetcher.getbestblockhash(), server.getbestblockhash())

        self.log.info("opening the special-tx fee spork on the server, waiting for both nodes to see it")
        server.spork("SPORK_22_SPECIAL_TX_FEE", SPORK22_FEE_100)
        wait_until(lambda: all(n.spork("show")["SPORK_22_SPECIAL_TX_FEE"] == SPORK22_FEE_100
                               for n in self.nodes), timeout=30)

        self.log.info("disconnecting before mining -- an already-synced, still-connected peer would "
                      "recover both withheld blocks via ordinary compact-block relay instead of "
                      "GETBODYRANGE, the same race feature_decoupled_assets_futures.py already found live")
        disconnect_nodes(fetcher, 1)

        self.log.info("block N: creating the asset -- mintasset (the NEXT block) needs this one "
                      "actually CONNECTED first, not merely broadcast, since it reads passetsCache "
                      "(assets.cpp), which ConnectBlock's own special-tx dispatch is the only writer of")
        create_txid = server.createasset({
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
            "amount": MINT_AMOUNT,
            "ownerAddress": owner,
        })["txid"]
        server.generatetoaddress(1, owner)
        assert_equal(server.getblockcount(), CREATE_HEIGHT)

        self.log.info("block N+1: minting more of the SAME asset, in a SEPARATE commitment-only block")
        mint_txid = server.mintasset(create_txid)["txid"]
        server.generatetoaddress(1, owner)
        assert_equal(server.getblockcount(), MINT_HEIGHT)

        server_details_before_fetch = server.getassetdetailsbyname(ASSET_NAME)
        assert_equal(server_details_before_fetch["MintCount"], 1)
        assert_equal(server_details_before_fetch["Circulating_supply"], MINT_AMOUNT)

        self.log.info("reconnecting -- the fetcher is genuinely behind by two blocks, forcing two "
                      "independent GETBODYRANGE/BODYRANGE round trips (one per block) rather than the "
                      "whole-block GETDATA fallback")
        with fetcher.assert_debug_log(["received: bodyrange"]), \
             server.assert_debug_log(["received: getbodyrange"]):
            connect_nodes(fetcher, 1)
            wait_until(lambda: fetcher.getblockcount() == MINT_HEIGHT, timeout=30)

        assert_equal(fetcher.getbestblockhash(), server.getbestblockhash())
        self.log.info("confirmed: two real GETBODYRANGE/BODYRANGE round trips recovered both halves "
                      "of a cross-block asset dependency")

        self.log.info("post-fetch: both special transactions materialised correctly on the fetcher")
        create_tx = fetcher.getrawtransaction(create_txid, 1)
        assert_equal(create_tx["type"], TRANSACTION_NEW_ASSET)
        mint_tx = fetcher.getrawtransaction(mint_txid, 1)
        assert_equal(mint_tx["type"], TRANSACTION_MINT_ASSET)

        self.log.info("the decisive check: the fetcher's asset state reflects the SECOND block's own "
                      "effect (MintCount=1, Circulating_supply=%d), not just the first block's creation "
                      "(which alone would show MintCount=0, Circulating_supply=0)", MINT_AMOUNT)
        assert_equal(fetcher.listassets(), server.listassets())
        fetcher_details = fetcher.getassetdetailsbyname(ASSET_NAME)
        assert_equal(fetcher_details, server_details_before_fetch)
        assert_equal(fetcher_details["MintCount"], 1)
        assert_equal(fetcher_details["Circulating_supply"], MINT_AMOUNT)
        self.log.info("confirmed: the cross-block asset-mint dependency survives two independent "
                      "commitment-only recoveries, indistinguishable from a node that received both "
                      "blocks the traditional way")


if __name__ == "__main__":
    FeatureDecoupledAssetMintSplitTest().main()
