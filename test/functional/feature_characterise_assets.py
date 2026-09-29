#!/usr/bin/env python3
"""Pin what the node accepts and rejects for asset operations, and at which stage.

Build-plan item 0.2's full-scope reopening (F-216). feature_assets.py and
feature_assets_rules.py already exercise the asset lifecycle broadly, but every
create/update/mint they drive goes through the WALLET RPC (createasset,
updateasset, mintasset), which has its own pre-checks and throws before a
transaction is ever built. That is a different, narrower layer than consensus:
a param createasset refuses to turn into a transaction is never exercised
against CheckNewAssetTx at all. This file's job is the accept/reject boundary
those two files do not reach: a hand-built, correctly-signed transaction with
exactly one structural fault, submitted straight to the node's mempool or a
block, recording the consensus layer's own verdict -- the same method
feature_characterise_accept.py uses for ordinary payments.

The rows classify per docs/transaction-decoupling.md section 2.4: a bare
commitment block (header, coinbase, identifier list, no transaction bodies)
either can or cannot perform the check. Every asset rule needs the tx body at
minimum (CheckSpecialTx dispatches only for a non-NORMAL type, which needs the
tx to know at all), and most also need the asset registry (CAssetsCache) or
the UTXO view -- so every row here is body-dependent; the interesting question
per row is what ELSE it needs beyond the body.

Run against an UNMODIFIED tree (F-46b, F-58): master, never perf/throughput-rig.

Setup mirrors feature_assets.py exactly (its own docstring explains why):
Round Voting (the asset soft fork) activates by height 300 but is not active
from genesis, and every asset special-tx refuses to enter the mempool unless
SPORK_22_SPECIAL_TX_FEE carries a non-zero fee -- both gates a naive test would
silently sail past (an inactive Round Voting makes CheckSpecialTx's dispatch
itself unreachable for these types).

**A trap this file's own first draft fell into, corrected here, not routed
around**: checkSpecialTxFee (consensus/tx_verify.cpp) is called from
Consensus::CheckTxInputs, which AcceptToMemoryPoolWorker invokes at
validation.cpp:858 -- BEFORE CheckSpecialTx's own dispatch at validation.cpp:953.
So the SPORK_22 fee check runs ahead of every structural rule this file means
to characterise, for every hand-built NEW_ASSET/UPDATE_ASSET/MINT_ASSET
transaction, not merely for the RPC convenience path. A payload whose `fee`
field does not equal getAssetsFees() (src/assets/assets.cpp:33-38, the spork
value's second byte -- 100 here) is rejected bad-txns-wrong-future-fee-or-
not-enable before CheckNewAssetTx/CheckUpdateAssetTx/CheckMintAssetTx is ever
reached, masking whatever rule the row means to pin -- exactly the class of
harness trap feature_characterise_accept.py's own reseal()/reconnect_p2p()
helpers exist to avoid for its own mutations. Every builder below sets
payload.fee = ASSET_FEE by default for this reason; the one row that wants
the fee check itself overrides it deliberately.
"""
import base64
import struct
from decimal import Decimal

from test_framework.authproxy import JSONRPCException
from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import (
    CTransaction,
    CTxOut,
    FromHex,
    ToHex,
    hash256,
    ser_string,
    ser_uint256,
    uint256_from_str,
)
from test_framework.mininode import P2PDataStore, mininode_lock, network_thread_start, network_thread_join
from test_framework.script import CScript
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, wait_until

TRANSACTION_NEW_ASSET = 8
TRANSACTION_UPDATE_ASSET = 9
TRANSACTION_MINT_ASSET = 10

# Second byte is the fee in whole RTM; matches feature_assets.py exactly.
SPORK22_FEE_100 = 25600
# Round Voting is active by 300, but the launch subsidy pays only 4 RTM a
# block until 720 and every asset transaction costs 100 -- feature_assets.py's
# own comment. Build past both, same target height, so the two files' chains
# behave identically.
CHAIN_HEIGHT = 830
COIN = 100000000
NULL20 = b"\x00" * 20
# Whole-RTM fee SPORK22_FEE_100 requires (its second byte, feature_assets.py's
# own ASSET_FEE). checkSpecialTxFee (consensus/tx_verify.cpp) rejects any
# special tx whose own payload fee disagrees with the spork -- every payload
# below must carry this exact value or every row fails on the fee check
# before it ever reaches the rule the row means to test. Found empirically:
# every issuance row hit bad-txns-wrong-future-fee-or-not-enable first, until
# each payload's fee field was set to match.
ASSET_FEE = 100

# (dkgInterval, dkgMiningWindowStart, dkgMiningWindowEnd) for every LLMQ type
# CRegTestParams actually enables (chainparams.cpp: LLMQ_50_60, LLMQ_400_60,
# LLMQ_400_85, LLMQ_100_67 -- llmq/quorums_parameters.h's llmq50_60/400_60/
# 400_85/100_67_testnet). NOT test_framework.blocktools.REGTEST_LLMQS: that
# dict (types 100/101) only applies to a RaptoreumTestFramework-based test
# that has run set_raptoreum_test_params(), which swaps in small, fast test
# quorums via -llmqtestparams -- a plain BitcoinTestFramework subclass (this
# file, matching every other file in this 0.2 series) never gets that
# override, so the REAL enabled types are these four. Passing node= to
# create_block and trusting its blocktools helper to supply the right
# commitments produced bad-qc-missing here on the first run -- it was
# building commitments for the wrong four types entirely.
REGTEST_REAL_LLMQ_WINDOWS = (
    (30, 10, 18),    # LLMQ_50_60
    (360, 20, 28),   # LLMQ_400_60
    (720, 20, 48),   # LLMQ_400_85
    (24, 10, 18),    # LLMQ_100_67 (testnet-scale params, used on regtest too)
)


def inputs_hash(vin):
    """CalcTxInputsHash (evo/specialtx.cpp): double-SHA256 over each vin's
    serialized prevout, in order. Only the vin set binds it -- unaffected by
    mutating any other payload field, which is what lets a single mutation
    below stay a single fault."""
    data = b"".join(txin.prevout.serialize() for txin in vin)
    return uint256_from_str(hash256(data))


class CNewAssetTx:
    """Python mirror of src/evo/providertx.h's CNewAssetTx, root-asset shape
    only (isRoot always True here -- root issuance needs no owner-of-root
    signature, per CheckNewAssetTx's own isRoot guard, which is what lets
    every row below be a single hand-built, correctly-signed transaction with
    no message-signing machinery required)."""

    def __init__(self):
        self.nVersion = 1
        self.name = b"TESTASSET"
        self.isRoot = True
        self.updatable = True
        self.isUnique = False
        self.maxMintCount = 10
        self.decimalPoint = 2
        self.referenceHash = b""
        self.fee = ASSET_FEE  # matches SPORK22_FEE_100's required fee exactly
        self.type = 0
        self.targetAddress = NULL20
        self.issueFrequency = 0
        self.amount = 0
        self.ownerAddress = NULL20
        self.collateralAddress = NULL20
        self.exChainType = 0
        self.externalPayoutScript = b""
        self.externalTxid = 0
        self.externalConfirmations = 0
        self.inputsHash = 0

    def serialize(self):
        r = b""
        r += struct.pack("<H", self.nVersion)
        r += ser_string(self.name)
        r += struct.pack("<??", self.updatable, self.isUnique)
        r += struct.pack("<H", self.maxMintCount)
        r += struct.pack("<B", self.decimalPoint)
        r += ser_string(self.referenceHash)
        r += struct.pack("<H", self.fee)
        r += struct.pack("<B", self.type)
        r += self.targetAddress
        r += struct.pack("<B", self.issueFrequency)
        r += struct.pack("<q", self.amount)
        r += self.ownerAddress
        r += self.collateralAddress
        r += struct.pack("<?", self.isRoot)
        # isRoot True: rootId/vchSig are skipped entirely (providertx.h:288-295)
        r += struct.pack("<H", self.exChainType)
        r += ser_string(self.externalPayoutScript)
        r += ser_uint256(self.externalTxid)
        r += struct.pack("<H", self.externalConfirmations)
        r += ser_uint256(self.inputsHash)
        return r


class CUpdateAssetTx:
    """Python mirror of CUpdateAssetTx. vchSig is left empty throughout: every
    row this file drives rejects BEFORE the owner-signature check runs
    (asset-exists and updatable both sit earlier in CheckUpdateAssetTx than
    the signature verification -- providertx.cpp's own ordering), so no
    message-signing machinery is needed here either."""

    def __init__(self):
        self.nVersion = 1
        self.assetId = b""
        self.updatable = True
        self.referenceHash = b""
        self.fee = ASSET_FEE
        self.type = 0
        self.targetAddress = NULL20
        self.issueFrequency = 0
        self.maxMintCount = 10
        self.amount = 0
        self.ownerAddress = NULL20
        self.collateralAddress = NULL20
        self.vchSig = b""
        self.exChainType = 0
        self.externalPayoutScript = b""
        self.externalTxid = 0
        self.externalConfirmations = 0
        self.inputsHash = 0

    def serialize(self):
        r = b""
        r += struct.pack("<H", self.nVersion)
        r += ser_string(self.assetId)
        r += struct.pack("<?", self.updatable)
        r += ser_string(self.referenceHash)
        r += struct.pack("<H", self.fee)
        r += struct.pack("<B", self.type)
        r += self.targetAddress
        r += struct.pack("<B", self.issueFrequency)
        r += struct.pack("<H", self.maxMintCount)
        r += struct.pack("<q", self.amount)
        r += self.ownerAddress
        r += self.collateralAddress
        r += struct.pack("<H", self.exChainType)
        r += ser_string(self.externalPayoutScript)
        r += ser_uint256(self.externalTxid)
        r += struct.pack("<H", self.externalConfirmations)
        r += ser_uint256(self.inputsHash)
        r += ser_string(self.vchSig)
        return r


class CharacteriseAssetsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # -maxtxfee: DEFAULT_TRANSACTION_MAXFEE (wallet.h) is COIN/10 = 0.1 RTM.
        # checkSpecialTxFee (consensus/tx_verify.cpp) subtracts the FULL 100 RTM
        # special fee from the transaction's own miner fee, so fundrawtransaction
        # has to be able to pay ~100 RTM as an actual fee, not just a high
        # feeRate on a small output -- the wallet's own safety cap silently
        # capped every fee at 0.1 RTM until this was raised, which is why the
        # first version of this file saw every row fail identically on
        # bad-txns-fee-too-low ("fee (-99.90)") rather than the rule under test.
        self.extra_args = [[
            "-sporkkey=cVpnZj4dZvRXmBf7Jze1GjpLQb25iKP92GDXUsKdUJTXhXRo2RFA",
            "-maxtxfee=2000",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        self.observed = {}
        self.accepted_ok = set()  # rows where ACCEPTED is the expected, interesting result

        self.log.info("priming to height %d: Round Voting active, subsidy matured", CHAIN_HEIGHT)
        self.owner = node.getnewaddress()
        while node.getblockcount() < CHAIN_HEIGHT:
            node.generatetoaddress(min(100, CHAIN_HEIGHT - node.getblockcount()), self.owner)
        assert_equal(node.getblockchaininfo()["rip1_softforks"]["Round Voting"]["status"], "active")

        self.log.info("opening the fee spork (SPORK_22), as feature_assets.py does")
        node.spork("SPORK_22_SPECIAL_TX_FEE", SPORK22_FEE_100)
        wait_until(lambda: node.spork("show")["SPORK_22_SPECIAL_TX_FEE"] == SPORK22_FEE_100, timeout=30)

        self.bootstrap_p2p()

        # --- issuance: CheckNewAssetTx, mempool-standalone -------------------
        self.check("issuance: decimalPoint > 8", lambda: self.issue(decimalPoint=9))
        self.check("issuance: amount not a multiple of the decimalPoint divisor",
                    lambda: self.issue(decimalPoint=2, amount=1))
        self.check("issuance: ownerAddress null", lambda: self.issue(ownerAddress=NULL20))
        self.check("issuance: targetAddress null", lambda: self.issue(targetAddress=NULL20))
        self.check("issuance: type != 0 needs collateralAddress",
                    lambda: self.issue(type=1, collateralAddress=NULL20))
        self.check("issuance: referenceHash > 128 chars",
                    lambda: self.issue(referenceHash=b"a" * 129))
        self.check("issuance: reserved root name RTM", lambda: self.issue(name=b"RTM"))
        self.check("issuance: inputsHash tampered", lambda: self.issue(_bad_inputs_hash=True))
        # F-223 (build-plan 3.4, bug 3): was `type < 0 && type > 3` -- always
        # false for an unsigned type field with `&&`, so no type value was
        # ever rejected here (F-216 found this dead bound first). Fixed to
        # `type > 3` (evo/providertx.cpp) -- out-of-range now rejected
        # bad-assets-distibution-type, no longer in accepted_ok.
        self.check("issuance: out-of-range type is now rejected (F-223 fix)",
                    lambda: self.issue(type=99, collateralAddress=b"\x01" * 20))

        self.log.info("=" * 72)
        self.log.info("issuance rows characterised; a real confirmed asset for the")
        self.log.info("registry-dependent rows below")
        self.log.info("=" * 72)

        real_asset_id = self.create_real_asset("REALASSET")

        self.check("issuance: duplicate name against a confirmed asset",
                    lambda: self.issue(name=b"REALASSET"))

        # --- update: CheckUpdateAssetTx, mempool-standalone -------------------
        self.check("update: assetId does not exist",
                    lambda: self.update(assetId=("0" * 64).encode()))

        locked_asset_id = self.create_real_asset("LOCKEDASSET", updatable=False)
        self.check("update: asset.updatable is false",
                    lambda: self.update(assetId=locked_asset_id.encode()))

        # --- mint: CheckMintAssetTx, mempool-standalone -----------------------
        self.check("mint: assetId does not exist",
                    lambda: self.mint(assetId=("1" * 64).encode()))

        # --- transfer: no special-tx type, an ordinary tx with an asset script
        self.log.info("minting REALASSET on this wallet, to build a transfer from it")
        node.mintasset(real_asset_id)
        node.generate(1)
        self.asset_for_transfer = real_asset_id
        self.characterise_bad_transfer_value()

        self.log.info("=" * 72)
        self.log.info("characterised rejection reasons (docs/transaction-decoupling.md 2.4)")
        for name, got in self.observed.items():
            self.log.info("  %-62s %s" % (name, got))
        self.log.info("=" * 72)

        # --- block-only surprises: bypass mempool dedup entirely --------------
        # Every fresh_tx() call above locked its own chosen input (needed to
        # stop successive calls picking the same coin -- fresh_tx's own
        # docstring) and none of those transactions were ever broadcast or
        # mined, so nothing depends on those locks anymore; freeing them all
        # here avoids exhausting the >300 RTM UTXO pool two block-level tests
        # further down (found empirically -- ValueError: max() arg is an
        # empty sequence once ~15 prior rows had each locked one away).
        self.nodes[0].lockunspent(True)
        self.characterise_double_issuance_in_block()
        self.characterise_double_mint_exceeds_cap_in_block()

        accepted = [n for n, g in self.observed.items()
                    if g.startswith("ACCEPTED") and n not in self.accepted_ok]
        broken = [n for n, g in self.observed.items() if g.startswith("HARNESS")]
        assert not broken, "harness could not express: %s" % ", ".join(broken)
        assert not accepted, (
            "these rules are NOT enforced at accept time and were not expected "
            "to be accepted: %s" % ", ".join(accepted))

    # ---- harness ------------------------------------------------------------

    def bootstrap_p2p(self):
        self.nodes[0].add_p2p_connection(P2PDataStore())
        network_thread_start()
        self.nodes[0].p2p.wait_for_getheaders(timeout=10)

    def reconnect_p2p(self):
        """A block that fails to connect can be DoS-scoring, and the node
        disconnects the offering peer -- feature_characterise_accept.py's own
        reconnect_p2p docstring names this exactly. mine_block_with is called
        more than once in this file (double-issuance, then double-mint), so
        without reconnecting between calls the second one dies with
        'Not connected', a harness failure that looks nothing like the rule
        under test -- found empirically."""
        self.nodes[0].disconnect_p2ps()
        network_thread_join()
        self.bootstrap_p2p()

    def clear_dkg_window(self):
        """Mine forward (the node's own miner, which supplies whatever real
        commitments regtest's four actually-enabled LLMQ types need) until the
        NEXT height clears every one of their DKG mining windows at once --
        feature_characterise_tiebreak.py's own next_height_is_clear, extended
        from one window to all four real ones (REGTEST_REAL_LLMQ_WINDOWS)."""
        node = self.nodes[0]

        def next_height_is_clear():
            h = node.getblockcount() + 1
            return all(not (start <= h % interval <= end)
                       for interval, start, end in REGTEST_REAL_LLMQ_WINDOWS)

        while not next_height_is_clear():
            node.generatetoaddress(1, self.owner)

    def check(self, name, build):
        try:
            hexTx = build()
            got = self.try_send(hexTx)
        except Exception as e:
            self.observed[name] = "HARNESS ERROR: %s" % e
            self.log.info("  %-62s -> %s", name, self.observed[name])
            return
        self.observed[name] = got
        self.log.info("  %-62s -> %s", name, self.observed[name])

    def try_send(self, hexTx):
        try:
            # maxfeerate=0: sendrawtransaction's OWN absurd-fee sanity clamp
            # (DEFAULT_MAX_RAW_TX_FEE, rpc/rawtransaction.cpp) is unrelated to
            # -maxtxfee and to checkSpecialTxFee -- it fires independently on
            # a genuinely-required ~100 RTM special-tx fee, and disabling it
            # here is the harness accepting a fee it built on purpose, not
            # the node relaxing a real consensus or policy rule.
            self.nodes[0].sendrawtransaction(hexTx, 0)
            return "ACCEPTED (no rejection)"
        except JSONRPCException as e:
            return e.error["message"]

    def fresh_tx(self):
        """An unsigned CTransaction spending exactly ONE explicitly-chosen
        real UTXO, with a change output. The fee has to clear two bars, not
        one: the ordinary relay fee, AND checkSpecialTxFee's ASSET_FEE=100 RTM
        special fee (subtracted from the tx's own miner fee, consensus/
        tx_verify.cpp), which sits BEFORE CheckSpecialTx in
        AcceptToMemoryPoolWorker and masks whatever rule a mutation is meant
        to reach if underfunded -- found empirically. FEE=300 clears both
        with comfortable margin.

        Picking the input explicitly (rather than fundrawtransaction's own
        coin selection) is deliberate, not just a style choice: this test
        calls fresh_tx() dozens of times, and coin selection across many
        small change-shaped UTXOs left over from the wallet's own history
        occasionally needs so many inputs to clear FEE that
        fundrawtransaction's own retry budget is exhausted ("Exceeded max
        tries") -- found empirically running the double-mint surprise
        candidate. The node's own coinbase rewards are large enough (get_
        block_subsidy: 5000 RTM past height 720) that picking the single
        largest unlocked UTXO avoids this category of failure entirely."""
        node = self.nodes[0]
        FEE = Decimal("300")
        utxo = max((u for u in node.listunspent() if u["amount"] > FEE),
                   key=lambda u: u["amount"])
        # lockunspent: none of these transactions are ever broadcast (the
        # whole point is to submit them together in a hand-built block, or
        # not at all), so the wallet has no other way to know an input is
        # already spoken for -- without this, two calls in the same test can
        # select the SAME coin, and a block combining both then fails
        # bad-txns-inputs-missingorspent for an unrelated reason. Found
        # empirically building the double-mint surprise candidate (six
        # independent mint() calls in a row, no block mined between them).
        node.lockunspent(False, [{"txid": utxo["txid"], "vout": utxo["vout"]}])
        dest = node.getnewaddress()
        raw = node.createrawtransaction(
            [{"txid": utxo["txid"], "vout": utxo["vout"]}],
            {dest: utxo["amount"] - FEE})
        return FromHex(CTransaction(), raw)

    def sign(self, tx):
        signed = self.nodes[0].signrawtransactionwithwallet(ToHex(tx))
        assert signed["complete"], signed
        return signed["hex"]

    def keyid(self, address):
        """The 20-byte hash160 backing a P2PKH address, read from its own
        scriptPubKey rather than a hand-rolled base58 decoder."""
        info = self.nodes[0].getaddressinfo(address)
        spk = bytes.fromhex(info["scriptPubKey"])
        assert spk[:3] == bytes([0x76, 0xa9, 0x14]), "not a P2PKH scriptPubKey: %s" % info["scriptPubKey"]
        return spk[3:23]

    def asset_transfer_script(self, address, asset_id, raw_amount):
        """A P2PKH-plus-asset-marker scriptPubKey, matching CScript::
        IsAssetScript's own layout exactly (script/script.cpp:275-292) and
        CAssetTransfer's wire format (assets/assetstype.h:17-39, isUnique
        always False here): [25-byte P2PKH][OP_ASSET_ID=0xbc][push opcode for
        the data that follows][b"rtm" + assetId + isUnique(1B) + nAmount(i64)]
        [OP_DROP=0x75]. The marker is LOWERCASE -- IsAssetScript's own
        RTM_R/RTM_T/RTM_M constants (script.h) are 0x72/0x74/0x6d, i.e.
        ASCII 'r'/'t'/'m', despite the "//R"-style comments beside them
        reading as uppercase; found empirically (every hand-built asset
        output silently failed IsAssetScript(), which checkAssetMintAmount
        reads as zero matching outputs -> bad-mint-assets-amount, not the
        cap check the row was actually testing). The data here is >75 bytes
        (a 64-hex-char assetId alone is 65 bytes once compact-size-prefixed),
        so it needs OP_PUSHDATA1 (0x4c) plus an explicit length byte rather
        than a direct small-push opcode -- confirmed against IsAssetScript's
        own two branches, which check for the marker at index 27 (short
        push) or 28 (this one)."""
        p2pkh = bytes([0x76, 0xa9, 0x14]) + self.keyid(address) + bytes([0x88, 0xac])
        payload = b"rtm" + ser_string(asset_id.encode()) + struct.pack("<?", False) + struct.pack("<q", raw_amount)
        assert len(payload) > 75, "expected the OP_PUSHDATA1 branch; a shorter assetId needs the direct-push one"
        push = bytes([0x4c, len(payload)]) + payload
        return p2pkh + bytes([0xbc]) + push + bytes([0x75])

    # ---- issuance -------------------------------------------------------

    def issue(self, **overrides):
        """A structurally-valid root NEW_ASSET tx, with exactly the given
        fields overridden. Correctly signed: CheckSpecialTx (mempool,
        validation.cpp:953) runs BEFORE script verification, so a rejected
        row's reason is always the special-tx rule, never a signature
        failure -- but an ACCEPTED row (the dead-bound check) has to be
        genuinely valid all the way through, which is why every row is
        signed, not just the ones expected to fail."""
        bad_inputs_hash = overrides.pop("_bad_inputs_hash", False)
        tx = self.fresh_tx()
        tx.nVersion = 3
        tx.nType = TRANSACTION_NEW_ASSET

        payload = CNewAssetTx()
        payload.ownerAddress = self.keyid(self.owner)
        payload.targetAddress = self.keyid(self.owner)
        for k, v in overrides.items():
            setattr(payload, k, v)
        payload.inputsHash = inputs_hash(tx.vin)
        if bad_inputs_hash:
            payload.inputsHash = (payload.inputsHash + 1) % (2 ** 256)
        tx.vExtraPayload = payload.serialize()
        return self.sign(tx)

    def create_real_asset(self, name, updatable=True, amount=100):
        """A genuinely confirmed root asset, via the RPC feature_assets.py
        itself uses -- for the rows that need a real registry entry to
        collide with or reference, not another hand-built fault.

        createasset's own RPC layer rejects amount<=0 outright
        (rpc/rpcassets.cpp: `CAmount a = amount.get_int64() * COIN; if (a <= 0
        || a > MAX_MONEY) throw ...`), so a real asset always needs a positive
        declared distribution even when the test only cares about its
        existence, not its supply."""
        node = self.nodes[0]
        # createasset returns an object (txid, Name, Isunique, ...), not a
        # bare txid -- ["txid"] below, matching feature_assets.py:225.
        txid = node.createasset({
            "name": name,
            "updatable": updatable,
            "is_root": True,
            "isunique": False,
            "maxMintCount": 5,
            "decimalpoint": 2,
            "referenceHash": "",
            "type": 0,
            "targetAddress": self.owner,
            "issueFrequency": 0,
            "amount": amount,
            "ownerAddress": self.owner,
        })["txid"]
        assert txid in node.getrawmempool()
        node.generate(1)
        details = node.getassetdetailsbyname(name)
        return details["Asset_id"]

    # ---- update / mint ----------------------------------------------------

    def update(self, **overrides):
        tx = self.fresh_tx()
        tx.nVersion = 3
        tx.nType = TRANSACTION_UPDATE_ASSET
        payload = CUpdateAssetTx()
        payload.ownerAddress = self.keyid(self.owner)
        payload.targetAddress = self.keyid(self.owner)
        for k, v in overrides.items():
            setattr(payload, k, v)
        payload.inputsHash = inputs_hash(tx.vin)
        tx.vExtraPayload = payload.serialize()
        return self.sign(tx)

    def mint(self, sign_for_name=None, **overrides):
        """A MINT_ASSET tx. CMintAssetTx: nVersion, assetId, fee, inputsHash,
        vchSig (providertx.h:416-435) -- the mint AMOUNT lives in the
        transaction's own asset-transfer outputs, not the payload, so a bare
        payload with no outputs is enough to reach any rule checked before
        CheckMintAssetTx's owner-signature verification (providertx.cpp:372),
        e.g. an unknown assetId.

        Rules checked AFTER that line (the mint ceiling, checkAssetMintAmount)
        need vchSig to actually verify, which needs a REAL owner signature --
        pass sign_for_name (the confirmed asset's own name) to compute one the
        same way CMintAssetTx::MakeSignString does (providertx.cpp:807-821):
        name|ownerAddress|targetAddress|circulatingSupply|SerializeHash(the
        payload minus vchSig, since SERIALIZE_METHODS excludes it under
        SER_GETHASH).ToString(), signed via signmessage -- this codebase's
        CMessageSigner is the same scheme Bitcoin Core's signmessage/
        verifymessage RPCs use, so the node's own wallet can produce it
        without reimplementing ECDSA signing here. sign_for_name also adds a
        matching asset-transfer output (checkAssetMintAmount, providertx.cpp:
        304-342, needs the outputs to sum to EXACTLY asset.amount, or its own
        bad-mint-assets-amount fires ahead of whatever rule sign_for_name was
        for)."""
        tx = self.fresh_tx()
        tx.nVersion = 3
        tx.nType = TRANSACTION_MINT_ASSET
        assetId = overrides.get("assetId", b"0" * 64)
        prefix = (struct.pack("<H", 1) + ser_string(assetId) + struct.pack("<H", ASSET_FEE)
                  + ser_uint256(inputs_hash(tx.vin)))
        if sign_for_name:
            node = self.nodes[0]
            details = node.getassetdetailsbyname(sign_for_name)
            raw_amount = int(round(details["Distribution"]["Amount"] * COIN))
            tx.vout.append(CTxOut(0, CScript(self.asset_transfer_script(
                self.owner, assetId.decode(), raw_amount))))
            msg_hash_hex = hash256(prefix)[::-1].hex()
            message = "%s|%s|%s|%d|%s" % (
                details["Asset_name"], details["owner"],
                details["Distribution"]["TargetAddress"],
                details["Circulating_supply"], msg_hash_hex)
            sig_b64 = node.signmessage(details["owner"], message)
            vchSig = base64.b64decode(sig_b64)
        else:
            vchSig = b""
        tx.vExtraPayload = prefix + ser_string(vchSig)
        return self.sign(tx)

    # ---- transfer -----------------------------------------------------------

    def characterise_bad_transfer_value(self):
        """A real, wallet-signed sendasset transaction, with its own
        asset-carrying output's RTM value bumped off zero after signing.
        checkOutput (consensus/tx_verify.cpp) runs in Consensus::CheckTxInputs,
        which -- like CheckSpecialTx -- sits before script verification in
        AcceptToMemoryPoolWorker, so mutating a signed transaction's output
        still reaches the structural rule first, exactly as
        feature_characterise_accept.py's mutate_tx_negative_value does for an
        ordinary payment.

        Submitted via a hand-built block (mine_block_with), not
        sendrawtransaction: the ORIGINAL sendasset transaction is already
        sitting unconfirmed in the mempool (sendasset has to broadcast for
        real to get a genuinely wallet-signed transaction to mutate), so the
        mutated copy spends the same inputs as a transaction the mempool
        already holds -- txn-mempool-conflict fires first and never reaches
        checkOutput at all if sent the same way as every other row here.
        Found empirically. ConnectBlock runs Consensus::CheckTxInputs against
        the chain's own UTXO set, not the mempool, so a block built directly
        around the mutated copy reaches the real rule -- PROVIDED the
        original never gets mined for real first. mine_block_with's own
        clear_dkg_window() mines with the node's real miner, which sweeps up
        anything sitting in the mempool -- including the original
        sendasset transaction, confirming it for real and spending the very
        input the mutated copy needs, which then fails
        bad-txns-inputs-missingorspent instead (found empirically, the same
        way). Clearing the window BEFORE broadcasting sidesteps this:
        mine_block_with's own call becomes a no-op once already clear."""
        node = self.nodes[0]
        self.clear_dkg_window()
        recipient = node.getnewaddress()
        # sendasset returns {"txid": ...} (rpc/rpcassets.cpp:748-750), same
        # shape as createasset -- not a bare string.
        txid = node.sendasset(self.asset_for_transfer, 1, recipient)["txid"]
        raw = node.getrawtransaction(txid, 0)
        tx = FromHex(CTransaction(), raw)
        for vout in tx.vout:
            script = bytes(vout.scriptPubKey)
            if len(script) > 25 and script[-1] == 0x75:  # OP_DROP: an asset-tagged output
                assert vout.nValue == 0, "expected the asset output to carry no RTM value"
                vout.nValue = 1000
        tx.rehash()

        connected, reason = self.mine_block_with([tx])
        got = "ACCEPTED (no rejection)" if connected else reason
        self.observed["transfer: asset-carrying output value must be 0"] = got
        self.log.info("  %-62s -> %s", "transfer: asset-carrying output value must be 0", got)
        tx.rehash()
        return ToHex(tx)

    # ---- block-level: bypass mempool dedup ---------------------------------

    def mine_block_with(self, extra_txs):
        """A hand-built block: node-mined coinbase plus the given already-
        valid transactions, submitted via p2p exactly like
        feature_characterise_accept.py's submit_p2p. Two independently-valid
        mempool transactions can each individually pass CheckSpecialTx against
        the SAME pre-block CAssetsCache snapshot (ProcessSpecialTxsInBlock
        validates every tx against one cache taken at block start; the cache
        is mutated only afterwards, in UpdateCoins) -- a combination mempool
        ATMP's own asset-dup/txn-mempool-conflict guards would normally
        prevent by rejecting the second transaction on arrival. Direct block
        submission is the only way to reach this."""
        node = self.nodes[0]
        self.reconnect_p2p()
        # A hand-built coinbase carries no quorum commitments at all, so this
        # block must land OUTSIDE every real DKG mining window (F-63) -- a
        # block built inside one is rejected bad-qc-missing regardless of what
        # else it carries. This masked both surprise candidates below on the
        # first two runs: both came back "connected: False" for what looked
        # like a within-block business-rule rejection and was actually just
        # this (the first attempted fix, passing node= to create_block to
        # supply commitments instead of avoiding the window, made it worse --
        # test_framework.blocktools.REGTEST_LLMQS's types 100/101 only exist
        # under a RaptoreumTestFramework test's -llmqtestparams override,
        # never active here, so it built commitments for the wrong types).
        self.clear_dkg_window()
        tip = int(node.getbestblockhash(), 16)
        height = node.getblockcount() + 1
        t = node.getblock(node.getbestblockhash())["time"] + 1
        block = create_block(tip, create_coinbase(height), t)
        block.vtx += extra_txs
        block.hashMerkleRoot = block.calc_merkle_root()
        block.solve()
        with mininode_lock:
            node.p2p.reject_reason_received = None
        # success=False here is a probe, not an expectation (mirrors
        # feature_characterise_accept.py's submit_p2p): a raised
        # AssertionError means the assertion "tip did NOT advance" itself
        # failed, i.e. the tip DID advance and the block was accepted.
        try:
            node.p2p.send_blocks_and_test([block], node, success=False, request_block=True, timeout=10)
        except AssertionError:
            pass
        connected = node.getbestblockhash() == block.hash
        with mininode_lock:
            reason = node.p2p.reject_reason_received
        reason = reason.decode() if isinstance(reason, bytes) else reason
        return connected, reason

    def characterise_double_issuance_in_block(self):
        """F-223 (build-plan 3.4, bug 1/B8): was a pure characterisation --
        both individually passed CheckSpecialTx against the SAME pre-block
        CAssetsCache snapshot (ProcessSpecialTxsInBlock validated the whole
        block in one pass before ConnectBlock's own separate per-tx mutation
        pass ever ran), the block connected, and the second name registration
        was silently dropped (CAssetsCache::InsertAsset's own duplicate guard
        returns false, unchecked by its caller) rather than rejected
        outright. Fixed via a scratch CAssetsCache in ProcessSpecialTxsInBlock
        (evo/specialtx.cpp), mutated immediately after each asset tx's own
        check succeeds, so the second tx in the block now sees the first
        tx's own just-registered name. Now asserted, not merely observed."""
        node = self.nodes[0]
        name = b"DUPEINBLOCK"
        txs = []
        for _ in range(2):
            hexTx = self.issue(name=name, ownerAddress=self.keyid(node.getnewaddress()),
                                targetAddress=self.keyid(node.getnewaddress()))
            txs.append(FromHex(CTransaction(), hexTx))
        connected, reason = self.mine_block_with(txs)
        self.log.info("=" * 72)
        self.log.info("two NEW_ASSET txs, same name, one block")
        self.log.info("  block with both connected: %s (reject reason if not: %s)", connected, reason)
        self.log.info("=" * 72)
        assert not connected, (
            "F-223 regression: a block with two same-name NEW_ASSET txs connected -- "
            "the second registration should now be rejected, not silently dropped")
        self.observed["same-block dup-name (two NEW_ASSET txs, one name, one block)"] = (
            "REJECTED: %s (F-223 fix confirmed)" % reason)
        self.double_issuance_connected = connected

    def characterise_double_mint_exceeds_cap_in_block(self):
        """F-223 (build-plan 3.4, bug 1/B8): was a pure characterisation --
        mintasset's own RPC-level guard (existsAssetTxConflict) refuses a
        second concurrent mint on the same assetId ('Asset mint or update tx
        exist on mempool'), so this routes around it exactly like
        characterise_double_issuance_in_block does: build (cap+1)
        independently hand-signed MINT_ASSET transactions (never sent to the
        mempool individually) and submit them together in one hand-built
        block. Before the fix, ProcessSpecialTxsInBlock's single per-block
        pass checked each individually against the SAME pre-block
        CAssetsCache/UTXO snapshot and all connected, ending mintCount one
        past the cap. Fixed via the same scratch-cache change as the
        dup-name row above (evo/specialtx.cpp) -- the second mint in the
        block now sees the first mint's own effect. Now asserted."""
        node = self.nodes[0]
        # Every fresh_tx() call so far in this test locked its own inputs
        # (needed to stop TWO calls in a row from picking the same coin, see
        # fresh_tx's own docstring) and none of those transactions were ever
        # broadcast or mined, so nothing actually depends on those locks
        # anymore -- freeing them all avoids "Insufficient funds" here, found
        # empirically once enough earlier rows had run.
        node.lockunspent(True)
        asset_id = self.create_real_asset("CAPPEDASSET")
        details_before = node.getassetdetailsbyname("CAPPEDASSET")
        cap = details_before["maxMintCount"]

        txs = []
        for i in range(cap + 1):
            hexTx = self.mint(assetId=asset_id.encode(), sign_for_name="CAPPEDASSET")
            txs.append(FromHex(CTransaction(), hexTx))
        connected, reason = self.mine_block_with(txs)

        self.log.info("=" * 72)
        self.log.info("%d hand-signed MINT_ASSET txs (cap=%d), one block", len(txs), cap)
        self.log.info("  block with all %d connected: %s (reject reason if not: %s)",
                      len(txs), connected, reason)
        self.log.info("=" * 72)
        assert not connected, (
            "F-223 regression: a block with %d mints against a cap of %d connected -- "
            "the cap should now be enforced within a single block, not just across blocks"
            % (len(txs), cap))
        self.observed["mint: cap can be exceeded within one block"] = (
            "block rejected (%s): F-223 fix confirmed, same-block over-cap minting IS caught" % reason)


if __name__ == "__main__":
    CharacteriseAssetsTest().main()
