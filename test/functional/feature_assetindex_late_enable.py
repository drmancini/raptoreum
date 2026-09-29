#!/usr/bin/env python3
# Copyright (c) 2026 The Raptoreum developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""4.3.1 (build-plan.md's 4.3 row, docs/findings.md's F-225): -assetindex can
be turned on after the fact, without -reindex, on a node that never ran with
it -- BuildAssetIndexFromCoins (assets.cpp) rebuilds the address-balance
secondary index from a scan of the CURRENT coin set, wired into init.cpp's
own assetindex-state-change handling.

This is the real, end-to-end version of what assets_tests.cpp's own
build_asset_index_from_coins_reconstructs_current_balances exercises directly
against the production function -- here it goes through actual process
restarts and real RPC calls, the same way an operator would use it.

Confirms three things a direct unit-level call cannot: (1) a node that never
had -assetindex reports the "not functional" refusal exactly as documented,
(2) restarting with -assetindex and no -reindex actually returns correct,
non-empty balances afterward, not just "doesn't crash", and (3) asset
EXISTENCE/metadata (getassetdetailsbyid) was already correct even before the
restart, confirming that half was never gated in the first place.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

SPORK22_FEE_100 = 25600
ASSET_FEE = 100
CHAIN_HEIGHT = 830
MINTED = 10000
COIN = 100000000


class AssetIndexLateEnableTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # No -assetindex here -- that is the whole point of this test.
        self.extra_args = [["-sporkkey=cVpnZj4dZvRXmBf7Jze1GjpLQb25iKP92GDXUsKdUJTXhXRo2RFA"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def mine(self, blocks=1):
        self.nodes[0].generate(blocks)

    def run_test(self):
        node = self.nodes[0]
        self.owner = node.getnewaddress()
        self.recipient = node.getnewaddress()

        self.log.info("Building a chain with Round Voting active and coins to spend")
        while node.getblockcount() < CHAIN_HEIGHT:
            node.generatetoaddress(min(100, CHAIN_HEIGHT - node.getblockcount()), self.owner)

        node.spork("SPORK_22_SPECIAL_TX_FEE", SPORK22_FEE_100)

        self.log.info("Creating, minting and splitting an asset with -assetindex OFF the whole time")
        created = node.createasset({
            "name": "LATEINDEX",
            "updatable": True,
            "is_root": True,
            "is_unique": False,
            "decimalpoint": 2,
            "referenceHash": "",
            "maxMintCount": 10,
            "type": 0,
            "targetAddress": self.owner,
            "issueFrequency": 0,
            "amount": MINTED,
            "ownerAddress": self.owner,
        })
        asset_id = created["txid"]
        self.mine()

        node.mintasset(asset_id)
        self.mine()

        # asset_change_address explicit: otherwise the wallet's own asset
        # change lands on a FRESH internal address, not self.owner, and the
        # per-address balance check below would (correctly) see self.owner
        # holding nothing -- a test-construction mistake, not a production
        # bug, caught by first running this and finding an empty result.
        node.sendasset(asset_id, 25, self.recipient, "", self.owner)
        self.mine()

        self.log.info("Asset existence/metadata is already correct with -assetindex OFF -- never gated on it")
        details = node.getassetdetailsbyid(asset_id)
        assert_equal(details["Asset_name"], "LATEINDEX")
        assert_equal(details["Circulating_supply"], MINTED)
        assert_equal(details["owner"], self.owner)

        self.log.info("The balance RPCs refuse, exactly as documented, while -assetindex is off")
        refusal = node.listassetbalancesbyaddress(self.owner)
        assert "not functional unless -assetindex is enabled" in refusal
        refusal2 = node.listaddressesbyasset("LATEINDEX")
        assert "not functional unless -assetindex is enabled" in refusal2

        self.log.info("Restarting WITH -assetindex, no -reindex -- this is the row's own claim under test")
        self.stop_node(0)
        self.start_node(0, extra_args=[
            "-sporkkey=cVpnZj4dZvRXmBf7Jze1GjpLQb25iKP92GDXUsKdUJTXhXRo2RFA",
            "-assetindex",
        ])
        node = self.nodes[0]

        self.log.info("The balance index is correct immediately, reconstructed from the coin set alone")
        owner_balance = node.listassetbalancesbyaddress(self.owner)
        assert_equal(owner_balance["LATEINDEX"], str((MINTED - 25) * COIN))
        recipient_balance = node.listassetbalancesbyaddress(self.recipient)
        assert_equal(recipient_balance["LATEINDEX"], str(25 * COIN))

        holders = node.listaddressesbyasset("LATEINDEX")
        assert_equal(holders[self.owner], str((MINTED - 25) * COIN))
        assert_equal(holders[self.recipient], str(25 * COIN))
        assert_equal(sum(int(v) for v in holders.values()), MINTED * COIN)

        self.log.info("Asset existence/metadata is unchanged by the rebuild")
        details_after = node.getassetdetailsbyid(asset_id)
        assert_equal(details_after, details)

        self.log.info("Turning -assetindex back OFF needs nothing special either")
        self.stop_node(0)
        self.start_node(0, extra_args=[
            "-sporkkey=cVpnZj4dZvRXmBf7Jze1GjpLQb25iKP92GDXUsKdUJTXhXRo2RFA",
        ])
        node = self.nodes[0]
        assert_equal(node.getassetdetailsbyid(asset_id)["Circulating_supply"], MINTED)
        refusal3 = node.listassetbalancesbyaddress(self.owner)
        assert "not functional unless -assetindex is enabled" in refusal3


if __name__ == '__main__':
    AssetIndexLateEnableTest().main()
