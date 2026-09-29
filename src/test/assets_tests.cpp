// Copyright (c) 2018-2019 The Dash Core developers
// Copyright (c) 2020-2023 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/test_raptoreum.h>

#include <base58.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <index/txindex.h>
#include <keystore.h>
#include <messagesigner.h>
#include <netbase.h>
#include <policy/policy.h>
#include <script/interpreter.h>
#include <script/sign.h>
#include <script/standard.h>
#include <spork.h>
#include <txmempool.h>
#include <validation.h>

#include <assets/assets.h>
#include <assets/assetsdb.h>
#include <assets/assetstype.h>
#include <core_io.h>
#include <evo/providertx.h>
#include <evo/specialtx.h>

#include <boost/test/unit_test.hpp>

using SimpleUTXOMap = std::map<COutPoint, std::pair<int, CAmount>>;

static SimpleUTXOMap BuildSimpleUtxoMap(const std::vector<CTransactionRef>& txs)
{
    SimpleUTXOMap utxos;
    for (size_t i = 0; i < txs.size(); i++) {
        auto& tx = txs[i];
        for (size_t j = 0; j < tx->vout.size(); j++) {
            if (tx->vout[j].nValue > 0)
                utxos.emplace(COutPoint(tx->GetHash(), j), std::make_pair((int)i + 1, tx->vout[j].nValue));
        }
    }
    return utxos;
}

static std::vector<COutPoint> SelectUTXOs(SimpleUTXOMap& utoxs, CAmount amount, CAmount& changeRet)
{
    changeRet = 0;

    std::vector<COutPoint> selectedUtxos;
    CAmount selectedAmount = 0;
    while (!utoxs.empty()) {
        bool found = false;
        for (auto it = utoxs.begin(); it != utoxs.end(); ++it) {
            if (::ChainActive().Height() - it->second.first < 101) {
                continue;
            }

            found = true;
            selectedAmount += it->second.second;
            selectedUtxos.emplace_back(it->first);
            utoxs.erase(it);
            break;
        }
        BOOST_ASSERT(found);
        if (selectedAmount >= amount) {
            changeRet = selectedAmount - amount;
            break;
        }
    }

    return selectedUtxos;
}

static void FundTransaction(CMutableTransaction& tx, SimpleUTXOMap& utoxs, const CScript& scriptPayout, CAmount amount, const CKey& coinbaseKey)
{
    CAmount change;
    auto inputs = SelectUTXOs(utoxs, amount, change);
    for (size_t i = 0; i < inputs.size(); i++) {
        tx.vin.emplace_back(CTxIn(inputs[i]));
    }
    if (change != 0) {
        tx.vout.emplace_back(CTxOut(change, scriptPayout));
    }
}

static bool SignTransaction(const CTxMemPool& mempool, CMutableTransaction& tx, const CKey& coinbaseKey)
{
    CBasicKeyStore tempKeystore;
    tempKeystore.AddKeyPubKey(coinbaseKey, coinbaseKey.GetPubKey());

    for (size_t i = 0; i < tx.vin.size(); i++) {
        uint256 hashBlock;
        CTransactionRef txFrom = GetTransaction(/* block_index */ nullptr, &mempool, tx.vin[i].prevout.hash,
            Params().GetConsensus(), hashBlock);
        BOOST_CHECK_MESSAGE(txFrom, "SignTransaction: GetTransaction");
        if (!txFrom)
            return false;
        bool ret = SignSignature(tempKeystore, *txFrom, tx, i, SIGHASH_ALL);
        BOOST_CHECK_MESSAGE(ret, "SignTransaction: SignSignature");
        if (!ret)
           return false;
    }
    return true;
}

static CMutableTransaction
CreateNewAssetTx(const CTxMemPool& mempool, SimpleUTXOMap& utxos, const CKey& coinbaseKey, std::string name, bool updatable, bool is_unique, uint8_t type, uint8_t decimalPoint, CAmount amount, uint16_t maxMintCount = 10)
{
    CKeyID ownerKey = coinbaseKey.GetPubKey().GetID();
    CNewAssetTx newAsset;

    newAsset.name = name;
    newAsset.isRoot = true;
    newAsset.updatable = updatable;
    newAsset.isUnique = is_unique;
    newAsset.decimalPoint = decimalPoint;
    newAsset.referenceHash = "";
    newAsset.type = type;
    newAsset.maxMintCount = maxMintCount;
    newAsset.fee = getAssetsFees();
    newAsset.targetAddress = ownerKey;
    newAsset.ownerAddress = ownerKey;
    newAsset.amount = amount * COIN;
    //newAsset.collateralAddress = CKey();

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_NEW_ASSET;
    FundTransaction(tx, utxos, GetScriptForDestination(coinbaseKey.GetPubKey().GetID()), newAsset.fee * COIN + 1 * COIN,
        coinbaseKey);
    newAsset.inputsHash = CalcTxInputsHash(tx);
    SetTxPayload(tx, newAsset);
    BOOST_ASSERT(SignTransaction(mempool, tx, coinbaseKey));

    return tx;
}

static CMutableTransaction
CreateUpdateAssetTx(const CTxMemPool& mempool, SimpleUTXOMap& utxos, const CKey& coinbaseKey, const CKey& newowner, std::string assetId, bool updatable, uint8_t type, CAmount amount)
{
    CKeyID ownerKey = newowner.GetPubKey().GetID();
    CUpdateAssetTx upAsset;

    CAssetMetaData asset;
    passetsCache->GetAssetMetaData(assetId, asset);

    upAsset.assetId = assetId;
    upAsset.updatable = updatable;
    upAsset.referenceHash = "";
    upAsset.fee = getAssetsFees();
    upAsset.type = type;
    upAsset.targetAddress = asset.targetAddress;
    upAsset.issueFrequency = asset.issueFrequency;
    upAsset.maxMintCount = asset.maxMintCount;
    upAsset.amount = amount * COIN;
    upAsset.ownerAddress = ownerKey;
    upAsset.collateralAddress = asset.collateralAddress;

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_UPDATE_ASSET;
    FundTransaction(tx, utxos, GetScriptForDestination(coinbaseKey.GetPubKey().GetID()), upAsset.fee * COIN + 1 * COIN,
        coinbaseKey);
    upAsset.inputsHash = CalcTxInputsHash(tx);

    std::string m = upAsset.MakeSignString(passetsCache.get());
    // lets prove we own the asset
    BOOST_ASSERT(CMessageSigner::SignMessage(m, upAsset.vchSig, coinbaseKey));

    SetTxPayload(tx, upAsset);
    BOOST_ASSERT(SignTransaction(mempool, tx, coinbaseKey));

    return tx;
}

static CMutableTransaction
CreateMintAssetTx(const CTxMemPool& mempool, SimpleUTXOMap& utxos, const CKey& coinbaseKey, std::string assetId)
{
    CKeyID ownerKey = coinbaseKey.GetPubKey().GetID();
    CMintAssetTx mint;

    CAssetMetaData asset;
    BOOST_ASSERT(passetsCache->GetAssetMetaData(assetId, asset));

    mint.assetId = assetId;
    mint.fee = getAssetsFees();

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_MINT_ASSET;


    if (asset.isUnique) {
        CScript scriptPubKey = GetScriptForDestination(asset.targetAddress);
        CAssetTransfer assetTransfer(asset.assetId, asset.amount, 0);
        assetTransfer.BuildAssetTransaction(scriptPubKey);
        CTxOut out(0, scriptPubKey);
        tx.vout.push_back(out);
    } else {
        CScript scriptPubKey = GetScriptForDestination(asset.targetAddress);
        CAssetTransfer assetTransfer(assetId, asset.amount);
        assetTransfer.BuildAssetTransaction(scriptPubKey);
        CTxOut out(0, scriptPubKey);
        tx.vout.push_back(out);
    }

    FundTransaction(tx, utxos, GetScriptForDestination(coinbaseKey.GetPubKey().GetID()), mint.fee * COIN + 1 * COIN,
        coinbaseKey);
    mint.inputsHash = CalcTxInputsHash(tx);

    std::string m = mint.MakeSignString(passetsCache.get());
    // lets prove we own the asset
    BOOST_ASSERT(CMessageSigner::SignMessage(m, mint.vchSig, coinbaseKey));

    SetTxPayload(tx, mint);
    BOOST_ASSERT(SignTransaction(mempool, tx, coinbaseKey));

    return tx;
}


static CScript GenerateRandomAddress()
{
    CKey key;
    key.MakeNewKey(false);
    return GetScriptForDestination(key.GetPubKey().GetID());
}

BOOST_AUTO_TEST_SUITE(assets_creation_tests)

BOOST_FIXTURE_TEST_CASE(assets_creation, TestChainDIP3BeforeActivationSetup)
{
    CKey sporkKey;
    sporkKey.MakeNewKey(false);
    sporkManager.SetSporkAddress(EncodeDestination(sporkKey.GetPubKey().GetID()));
    sporkManager.SetPrivKey(EncodeSecret(sporkKey));
    sporkManager.UpdateSpork(SPORK_22_SPECIAL_TX_FEE, 2560, *m_node.connman);

    auto utxos = BuildSimpleUtxoMap(m_coinbase_txns);

    auto tx = CreateNewAssetTx(*m_node.mempool, utxos, coinbaseKey, "TEST_ASSET", true, false, 0, 8, 1000);
    std::vector<CMutableTransaction> txns = {tx};

    int nHeight = ::ChainActive().Height();

    // Mining a block with a asset create transaction
    auto block = std::make_shared<CBlock>(CreateBlock(txns, coinbaseKey));
    EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

    BOOST_ASSERT(::ChainActive().Height() == nHeight + 1);
    BOOST_ASSERT(block->GetHash() == ::ChainActive().Tip()->GetBlockHash());

    //invalid asset name
    tx = CreateNewAssetTx(*m_node.mempool, utxos, coinbaseKey, "*Test_Asset*", true, false, 0, 8, 1000);
    txns = {tx};
    block = std::make_shared<CBlock>(CreateBlock(txns, coinbaseKey));
    //block should be rejected
    EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

    BOOST_ASSERT(::ChainActive().Height() == nHeight + 1);

    //invalid distribution type
    tx = CreateNewAssetTx(*m_node.mempool, utxos, coinbaseKey, "TEST_ASSET", true, false, 5, 8, 1000);
    txns = {tx};
    block = std::make_shared<CBlock>(CreateBlock(txns, coinbaseKey));
    //block should be rejected
    EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

    BOOST_ASSERT(::ChainActive().Height() == nHeight + 1);

    //invalid decimalPoint
    tx = CreateNewAssetTx(*m_node.mempool, utxos, coinbaseKey, "TEST_ASSET", true, false, 0, 9, 1000);
    txns = {tx};
    block = std::make_shared<CBlock>(CreateBlock(txns, coinbaseKey));
    //block should be rejected
    EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

    BOOST_ASSERT(::ChainActive().Height() == nHeight + 1);
}

BOOST_FIXTURE_TEST_CASE(assets_update, TestChainDIP3BeforeActivationSetup)
{
    CKey sporkKey;
    sporkKey.MakeNewKey(false);
    sporkManager.SetSporkAddress(EncodeDestination(sporkKey.GetPubKey().GetID()));
    sporkManager.SetPrivKey(EncodeSecret(sporkKey));
    sporkManager.UpdateSpork(SPORK_22_SPECIAL_TX_FEE,
        2560, *m_node.connman);

    auto utxos = BuildSimpleUtxoMap(m_coinbase_txns);

    auto tx = CreateNewAssetTx(*m_node.mempool, utxos, coinbaseKey, "TEST_ASSET", true, false, 0, 8, 1000);
    std::vector<CMutableTransaction> txns = {tx};

    int nHeight = ::ChainActive().Height();

    // Mining a block with a asset create transaction
    {
        auto block = std::make_shared<CBlock>(CreateBlock(txns, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

        BOOST_ASSERT(::ChainActive().Height() == nHeight + 1);
        BOOST_ASSERT(block->GetHash() == ::ChainActive().Tip()->GetBlockHash());
    }

    CAssetMetaData asset;
    BOOST_ASSERT(passetsCache->GetAssetMetaData(tx.GetHash().ToString(), asset));

    //change asset owner
    CKey key;
    key.MakeNewKey(false);
    std::string assetId = tx.GetHash().ToString();
    tx = CreateUpdateAssetTx(*m_node.mempool, utxos, coinbaseKey, key, assetId, true, 0, 1000);
    {
        auto block = std::make_shared<CBlock>(CreateBlock({tx}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

        BOOST_ASSERT(::ChainActive().Height() == nHeight + 2);
        BOOST_ASSERT(block->GetHash() == ::ChainActive().Tip()->GetBlockHash());
    }

    BOOST_ASSERT(passetsCache->GetAssetMetaData(assetId, asset));
    BOOST_ASSERT(asset.ownerAddress == key.GetPubKey().GetID());

    //any atemp to update with the coinbaseKey should fail
    tx = CreateUpdateAssetTx(*m_node.mempool, utxos, coinbaseKey, coinbaseKey, assetId, true, 0, 10000);
    {
        auto block = std::make_shared<CBlock>(CreateBlock({tx}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

        BOOST_ASSERT(::ChainActive().Height() == nHeight + 2);
        BOOST_ASSERT(block->GetHash() != ::ChainActive().Tip()->GetBlockHash());
    }
}

BOOST_FIXTURE_TEST_CASE(assets_mint, TestChainDIP3BeforeActivationSetup)
{
    CKey sporkKey;
    sporkKey.MakeNewKey(false);
    sporkManager.SetSporkAddress(EncodeDestination(sporkKey.GetPubKey().GetID()));
    sporkManager.SetPrivKey(EncodeSecret(sporkKey));
    sporkManager.UpdateSpork(SPORK_22_SPECIAL_TX_FEE, 2560, *m_node.connman);

    auto utxos = BuildSimpleUtxoMap(m_coinbase_txns);

    auto tx = CreateNewAssetTx(*m_node.mempool, utxos, coinbaseKey, "TEST_ASSET", true, false, 0, 8, 1000);
    std::vector<CMutableTransaction> txns = {tx};

    int nHeight = ::ChainActive().Height();

    // Mining a block with a asset create transaction
    {
        auto block = std::make_shared<CBlock>(CreateBlock(txns, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

        BOOST_ASSERT(::ChainActive().Height() == nHeight + 1);
        BOOST_ASSERT(block->GetHash() == ::ChainActive().Tip()->GetBlockHash());
    }

    std::string assetId = tx.GetHash().ToString();
    tx = CreateMintAssetTx(*m_node.mempool, utxos, coinbaseKey, assetId);

    {
        auto block = std::make_shared<CBlock>(CreateBlock({tx}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

        BOOST_ASSERT(::ChainActive().Height() == nHeight + 2);
        BOOST_ASSERT(block->GetHash() == ::ChainActive().Tip()->GetBlockHash());
    }

    CAssetMetaData asset;
    BOOST_ASSERT(passetsCache->GetAssetMetaData(assetId, asset));

    BOOST_ASSERT(asset.circulatingSupply == 1000);

    // Allow TX index to catch up with the block index.
    g_txindex->BlockUntilSyncedToCurrentChain();

    //transfer asset
    CMutableTransaction tx2;

    CKey key;
    key.MakeNewKey(false);
    CScript scriptPubKey = GetScriptForDestination(key.GetPubKey().GetID());
    CAssetTransfer assetTransfer(assetId, 100 * COIN);
    assetTransfer.BuildAssetTransaction(scriptPubKey);
    CTxOut out(0, scriptPubKey);
    tx2.vout.push_back(out);

    //change
    CScript scriptPubKey2 = GetScriptForDestination(coinbaseKey.GetPubKey().GetID());
    CAssetTransfer assetTransfer2(assetId, 900 * COIN);
    assetTransfer2.BuildAssetTransaction(scriptPubKey2);
    CTxOut out2(0, scriptPubKey2);
    tx2.vout.push_back(out2);
    tx2.vin.push_back(CTxIn(COutPoint(tx.GetHash(), 0)));

    FundTransaction(tx2, utxos, GetScriptForDestination(coinbaseKey.GetPubKey().GetID()), 1 * COIN, coinbaseKey);
    BOOST_ASSERT(SignTransaction(*m_node.mempool, tx2, coinbaseKey));

    {
        auto block = std::make_shared<CBlock>(CreateBlock({tx2}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

        BOOST_ASSERT(::ChainActive().Height() == nHeight + 3);
        BOOST_ASSERT(block->GetHash() == ::ChainActive().Tip()->GetBlockHash());
    }
}

BOOST_FIXTURE_TEST_CASE(assets_invalid_cases, TestChainDIP3BeforeActivationSetup)
{
    CKey sporkKey;
    sporkKey.MakeNewKey(false);
    sporkManager.SetSporkAddress(EncodeDestination(sporkKey.GetPubKey().GetID()));
    sporkManager.SetPrivKey(EncodeSecret(sporkKey));
    sporkManager.UpdateSpork(SPORK_22_SPECIAL_TX_FEE, 2560, *m_node.connman);

    auto utxos = BuildSimpleUtxoMap(m_coinbase_txns);

    //create a asset
    auto tx = CreateNewAssetTx(*m_node.mempool, utxos, coinbaseKey, "TEST_ASSET", false, false, 0, 2, 100);
    std::vector<CMutableTransaction> txns = {tx};

    int nHeight = ::ChainActive().Height();

    // Mining a block with a asset create transaction
    {
        auto block = std::make_shared<CBlock>(CreateBlock(txns, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

        BOOST_ASSERT(::ChainActive().Height() == nHeight + 1);
        BOOST_ASSERT(block->GetHash() == ::ChainActive().Tip()->GetBlockHash());
    }

    std::string assetId = tx.GetHash().ToString();

    tx = CreateMintAssetTx(*m_node.mempool, utxos, coinbaseKey, assetId);

    {
        auto block = std::make_shared<CBlock>(CreateBlock({tx}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

        BOOST_ASSERT(::ChainActive().Height() == nHeight + 2);
        BOOST_ASSERT(block->GetHash() == ::ChainActive().Tip()->GetBlockHash());
    }

    CAssetMetaData asset;
    BOOST_ASSERT(passetsCache->GetAssetMetaData(assetId, asset));
    BOOST_ASSERT(asset.circulatingSupply == 100);

    // Allow TX index to catch up with the block index.
    g_txindex->BlockUntilSyncedToCurrentChain();

    {
        //bad amount, decimalPoint = 2
        CMutableTransaction tx2;

        CKey key;
        key.MakeNewKey(false);
        CScript scriptPubKey = GetScriptForDestination(key.GetPubKey().GetID());
        CAssetTransfer assetTransfer(assetId, 12.1234 * COIN);
        assetTransfer.BuildAssetTransaction(scriptPubKey);
        CTxOut out(0, scriptPubKey);
        tx2.vout.push_back(out);
        //change
        CScript scriptPubKey2 = GetScriptForDestination(coinbaseKey.GetPubKey().GetID());
        CAssetTransfer assetTransfer2(assetId, 87.8766 * COIN);
        assetTransfer2.BuildAssetTransaction(scriptPubKey2);
        CTxOut out2(0, scriptPubKey2);
        tx2.vout.push_back(out2);

        tx2.vin.push_back(CTxIn(COutPoint(tx.GetHash(), 0)));

        FundTransaction(tx2, utxos, GetScriptForDestination(coinbaseKey.GetPubKey().GetID()), 1 * COIN, coinbaseKey);
        BOOST_ASSERT(SignTransaction(*m_node.mempool, tx2, coinbaseKey));

        auto block = std::make_shared<CBlock>(CreateBlock({tx2}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

        BOOST_ASSERT(::ChainActive().Height() == nHeight + 2);
        BOOST_ASSERT(block->GetHash() != ::ChainActive().Tip()->GetBlockHash());
    }

    {
        //input-output mismatch
        CMutableTransaction tx2;

        CKey key;
        key.MakeNewKey(false);
        CScript scriptPubKey = GetScriptForDestination(key.GetPubKey().GetID());
        CAssetTransfer assetTransfer(assetId, 12.12 * COIN);
        assetTransfer.BuildAssetTransaction(scriptPubKey);
        CTxOut out(0, scriptPubKey);
        tx2.vout.push_back(out);
        //change
        CScript scriptPubKey2 = GetScriptForDestination(coinbaseKey.GetPubKey().GetID());
        CAssetTransfer assetTransfer2(assetId, 88.88 * COIN);
        assetTransfer2.BuildAssetTransaction(scriptPubKey2);
        CTxOut out2(0, scriptPubKey2);
        tx2.vout.push_back(out2);

        tx2.vin.push_back(CTxIn(COutPoint(tx.GetHash(), 0)));

        FundTransaction(tx2, utxos, GetScriptForDestination(coinbaseKey.GetPubKey().GetID()), 1 * COIN, coinbaseKey);
        BOOST_ASSERT(SignTransaction(*m_node.mempool, tx2, coinbaseKey));

        auto block = std::make_shared<CBlock>(CreateBlock({tx2}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

        BOOST_ASSERT(::ChainActive().Height() == nHeight + 2);
        BOOST_ASSERT(block->GetHash() != ::ChainActive().Tip()->GetBlockHash());
    }

    {
        //bad native asset amount
        CMutableTransaction tx2;

        CKey key;
        key.MakeNewKey(false);
        CScript scriptPubKey = GetScriptForDestination(key.GetPubKey().GetID());
        CAssetTransfer assetTransfer(assetId, 12.12 * COIN);
        assetTransfer.BuildAssetTransaction(scriptPubKey);
        CTxOut out(1 * COIN, scriptPubKey);
        tx2.vout.push_back(out);
        //change
        CScript scriptPubKey2 = GetScriptForDestination(coinbaseKey.GetPubKey().GetID());
        CAssetTransfer assetTransfer2(assetId, 87.88 * COIN);
        assetTransfer2.BuildAssetTransaction(scriptPubKey2);
        CTxOut out2(0, scriptPubKey2);
        tx2.vout.push_back(out2);

        tx2.vin.push_back(CTxIn(COutPoint(tx.GetHash(), 0)));

        FundTransaction(tx2, utxos, GetScriptForDestination(coinbaseKey.GetPubKey().GetID()), 1 * COIN, coinbaseKey);
        BOOST_ASSERT(SignTransaction(*m_node.mempool, tx2, coinbaseKey));

        auto block = std::make_shared<CBlock>(CreateBlock({tx2}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

        BOOST_ASSERT(::ChainActive().Height() == nHeight + 2);
        BOOST_ASSERT(block->GetHash() != ::ChainActive().Tip()->GetBlockHash());
    }

    //create a unique asset
    tx = CreateNewAssetTx(*m_node.mempool, utxos, coinbaseKey, "UNIQUE_ASSET", false, true, 0, 0, 10);
    txns = {tx};

    nHeight = ::ChainActive().Height();

    // Mining a block with a asset create transaction
    {
        auto block = std::make_shared<CBlock>(CreateBlock(txns, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

        BOOST_ASSERT(::ChainActive().Height() == nHeight + 1);
        BOOST_ASSERT(block->GetHash() == ::ChainActive().Tip()->GetBlockHash());
    }

    assetId = tx.GetHash().ToString();
    tx = CreateMintAssetTx(*m_node.mempool, utxos, coinbaseKey, assetId);

    {
        auto block = std::make_shared<CBlock>(CreateBlock({tx}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

        BOOST_ASSERT(::ChainActive().Height() == nHeight + 2);
        BOOST_ASSERT(block->GetHash() == ::ChainActive().Tip()->GetBlockHash());
    }

    BOOST_ASSERT(passetsCache->GetAssetMetaData(assetId, asset));

    BOOST_ASSERT(asset.circulatingSupply == 10);
    // Allow TX index to catch up with the block index.
    g_txindex->BlockUntilSyncedToCurrentChain();

    {
        //mismatch uniqueid
        CMutableTransaction tx2;

        CKey key;
        key.MakeNewKey(false);
        CScript scriptPubKey = GetScriptForDestination(key.GetPubKey().GetID());
        CAssetTransfer assetTransfer(assetId, 2 * COIN, 0);
        assetTransfer.BuildAssetTransaction(scriptPubKey);
        CTxOut out(0, scriptPubKey);
        tx2.vout.push_back(out);
        CAssetTransfer assetTransfer2(assetId, 8 * COIN, 0);
        assetTransfer.BuildAssetTransaction(scriptPubKey);
        CTxOut out2(0, scriptPubKey);
        tx2.vout.push_back(out2);

        tx2.vin.push_back(CTxIn(COutPoint(tx.GetHash(), 0)));

        FundTransaction(tx2, utxos, GetScriptForDestination(coinbaseKey.GetPubKey().GetID()), 1 * COIN, coinbaseKey);
        BOOST_ASSERT(SignTransaction(*m_node.mempool, tx2, coinbaseKey));

        auto block = std::make_shared<CBlock>(CreateBlock({tx2}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

        BOOST_ASSERT(::ChainActive().Height() == nHeight + 2);
        BOOST_ASSERT(block->GetHash() != ::ChainActive().Tip()->GetBlockHash());
    }

    {
        //amount mismatch
        CMutableTransaction tx2;

        CKey key;
        key.MakeNewKey(false);
        CScript scriptPubKey = GetScriptForDestination(key.GetPubKey().GetID());
        CAssetTransfer assetTransfer(assetId, 1 * COIN, 0); //uniqueId=0
        assetTransfer.BuildAssetTransaction(scriptPubKey);
        CTxOut out(0, scriptPubKey);
        tx2.vout.push_back(out);

        CScript scriptPubKey2 = GetScriptForDestination(key.GetPubKey().GetID());
        CAssetTransfer assetTransfer2(assetId, 1 * COIN, 1); ////uniqueId=1
        assetTransfer.BuildAssetTransaction(scriptPubKey2);
        CTxOut out2(0, scriptPubKey2);
        tx2.vout.push_back(out2);

        tx2.vin.push_back(CTxIn(COutPoint(tx.GetHash(), 0))); //uniqueId=0


        FundTransaction(tx2, utxos, GetScriptForDestination(coinbaseKey.GetPubKey().GetID()), 1 * COIN, coinbaseKey);
        BOOST_ASSERT(SignTransaction(*m_node.mempool, tx2, coinbaseKey));

        auto block = std::make_shared<CBlock>(CreateBlock({tx2}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

        BOOST_ASSERT(::ChainActive().Height() == nHeight + 2);
        BOOST_ASSERT(block->GetHash() != ::ChainActive().Tip()->GetBlockHash());
    }

    {
        //native asset amount != 0
        CMutableTransaction tx2;

        CKey key;
        key.MakeNewKey(false);
        CScript scriptPubKey = GetScriptForDestination(key.GetPubKey().GetID());
        CAssetTransfer assetTransfer(assetId, 10 * COIN, 0);
        assetTransfer.BuildAssetTransaction(scriptPubKey);
        CTxOut out(1, scriptPubKey);
        tx2.vout.push_back(out);

        tx2.vin.push_back(CTxIn(COutPoint(tx.GetHash(), 0)));

        FundTransaction(tx2, utxos, GetScriptForDestination(coinbaseKey.GetPubKey().GetID()), 1 * COIN, coinbaseKey);
        BOOST_ASSERT(SignTransaction(*m_node.mempool, tx2, coinbaseKey));

        auto block = std::make_shared<CBlock>(CreateBlock({tx2}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

        BOOST_ASSERT(::ChainActive().Height() == nHeight + 2);
        BOOST_ASSERT(block->GetHash() != ::ChainActive().Tip()->GetBlockHash());
    }
}

// F-223 (3.4, bug 1 / B8): same root cause as the mint-cap case below --
// ProcessSpecialTxsInBlock validates every special tx in a block in ONE pass
// (evo/specialtx.cpp), against a CAssetsCache that is mutated only
// afterwards, in ConnectBlock's own separate per-tx loop (UpdateCoins/
// AddAssets, validation.cpp). So two NEW_ASSET txs for the SAME name in ONE
// block both individually pass CheckNewAssetTx's own bad-assets-dup-name
// check against the identical pre-block snapshot. F-216 found this exactly
// (docs/findings.md, "same-block dup-name") via
// feature_characterise_assets.py's characterise_double_issuance_in_block;
// this pins the same scenario as a real regression test, in C++, against
// this worktree's own build.
BOOST_FIXTURE_TEST_CASE(assets_same_block_duplicate_name_rejected, TestChainDIP3BeforeActivationSetup)
{
    CKey sporkKey;
    sporkKey.MakeNewKey(false);
    sporkManager.SetSporkAddress(EncodeDestination(sporkKey.GetPubKey().GetID()));
    sporkManager.SetPrivKey(EncodeSecret(sporkKey));
    sporkManager.UpdateSpork(SPORK_22_SPECIAL_TX_FEE, 2560, *m_node.connman);

    auto utxos = BuildSimpleUtxoMap(m_coinbase_txns);

    auto tx1 = CreateNewAssetTx(*m_node.mempool, utxos, coinbaseKey, "DUPEINBLOCK", true, false, 0, 8, 1000);
    auto tx2 = CreateNewAssetTx(*m_node.mempool, utxos, coinbaseKey, "DUPEINBLOCK", true, false, 0, 8, 1000);

    int nHeight = ::ChainActive().Height();
    auto block = std::make_shared<CBlock>(CreateBlock({tx1, tx2}, coinbaseKey));
    EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

    // Before the fix: both individually pass, the block connects, and the
    // second registration is silently dropped (InsertAsset's own duplicate
    // guard returns false, unchecked by its caller). After the fix: the
    // second tx sees the first tx's own just-registered name and the block
    // is rejected outright.
    BOOST_CHECK_EQUAL(::ChainActive().Height(), nHeight);
    BOOST_CHECK(block->GetHash() != ::ChainActive().Tip()->GetBlockHash());
}

// F-223 (3.4, bug 1 / B8): the mint-cap half of the same root cause.
// checkAssetMintAmount's own cap check (asset.mintCount >= asset.maxMintCount,
// evo/providertx.cpp) reads the SAME pre-block CAssetsCache snapshot for
// every mint tx in the block, so N independently-signed mints against a cap
// of N-1 all individually pass. F-216 found this exactly (docs/findings.md,
// "mint: cap can be exceeded within one block") via
// feature_characterise_assets.py's characterise_double_mint_exceeds_cap_in_block.
BOOST_FIXTURE_TEST_CASE(assets_same_block_mint_exceeds_cap_rejected, TestChainDIP3BeforeActivationSetup)
{
    CKey sporkKey;
    sporkKey.MakeNewKey(false);
    sporkManager.SetSporkAddress(EncodeDestination(sporkKey.GetPubKey().GetID()));
    sporkManager.SetPrivKey(EncodeSecret(sporkKey));
    sporkManager.UpdateSpork(SPORK_22_SPECIAL_TX_FEE, 2560, *m_node.connman);

    auto utxos = BuildSimpleUtxoMap(m_coinbase_txns);

    // maxMintCount = 1: a single mint is allowed, a second must not be.
    auto tx = CreateNewAssetTx(*m_node.mempool, utxos, coinbaseKey, "CAPPEDINBLOCK", true, false, 0, 0, 10, 1);
    std::string assetId = tx.GetHash().ToString();
    {
        auto block = std::make_shared<CBlock>(CreateBlock({tx}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);
    }

    auto mintA = CreateMintAssetTx(*m_node.mempool, utxos, coinbaseKey, assetId);
    auto mintB = CreateMintAssetTx(*m_node.mempool, utxos, coinbaseKey, assetId);

    int nHeight = ::ChainActive().Height();
    auto block = std::make_shared<CBlock>(CreateBlock({mintA, mintB}, coinbaseKey));
    EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

    // Before the fix: both mints individually pass (mintCount=0 for both
    // checks), the block connects, and mintCount ends up at 2, one past the
    // cap of 1. After the fix: the second mint sees the first mint's own
    // effect and the block is rejected outright.
    BOOST_CHECK_EQUAL(::ChainActive().Height(), nHeight);
    BOOST_CHECK(block->GetHash() != ::ChainActive().Tip()->GetBlockHash());
}

// F-223 (3.4, bug 2 / B9): CAssetsCache::UndoMintAsset (assets/assets.cpp)
// matches its undo record by assetId alone, scanning the block's ENTIRE
// vUndoData vector with no break -- so it always ends up applying whichever
// matching record is LAST in that vector, regardless of which tx is actually
// being undone. A block with two mints on the same asset: disconnecting the
// EARLIER one re-applies the LATER one's own undo record instead of its own,
// leaving the earlier mint's effect still applied after "undoing" it.
BOOST_FIXTURE_TEST_CASE(assets_mint_undo_keyed_by_tx_index, TestChainDIP3BeforeActivationSetup)
{
    CKey sporkKey;
    sporkKey.MakeNewKey(false);
    sporkManager.SetSporkAddress(EncodeDestination(sporkKey.GetPubKey().GetID()));
    sporkManager.SetPrivKey(EncodeSecret(sporkKey));
    sporkManager.UpdateSpork(SPORK_22_SPECIAL_TX_FEE, 2560, *m_node.connman);

    auto utxos = BuildSimpleUtxoMap(m_coinbase_txns);

    // maxMintCount=10 (the default): three total mints stay well clear of
    // the cap, isolating this test from bug 1/B8 above.
    auto tx = CreateNewAssetTx(*m_node.mempool, utxos, coinbaseKey, "UNDOTEST", true, false, 0, 0, 10);
    std::string assetId = tx.GetHash().ToString();
    {
        auto block = std::make_shared<CBlock>(CreateBlock({tx}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);
    }

    // Block 1: a single mint. mintCount 0 -> 1, circulatingSupply 0 -> 10.
    tx = CreateMintAssetTx(*m_node.mempool, utxos, coinbaseKey, assetId);
    {
        auto block = std::make_shared<CBlock>(CreateBlock({tx}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);
    }

    CAssetMetaData asset;
    BOOST_REQUIRE(passetsCache->GetAssetMetaData(assetId, asset));
    BOOST_REQUIRE_EQUAL(asset.mintCount, 1);
    BOOST_REQUIRE_EQUAL(asset.circulatingSupply, 10);

    // Block 2: TWO mints on the SAME asset, in ONE block. mintCount 1 -> 3,
    // circulatingSupply 10 -> 30.
    //
    // CMintAssetTx::MakeSignString (evo/providertx.cpp) bakes the asset's
    // CURRENT circulatingSupply into the signed message -- a deliberate
    // anti-conflict mechanism (matching mintasset's own RPC-level
    // existsAssetTxConflict guard, which refuses a second concurrent mint on
    // the same asset). With bug 1/B8 fixed above, the second mint in a block
    // is now correctly checked against the first mint's own in-block effect,
    // so it must be signed against that SAME expected post-first-mint state
    // to pass -- exactly what a wallet correctly coordinating two sequential
    // mints into one block would need to do. Simulate that here: build mint2
    // normally (against the real, current passetsCache), then temporarily
    // apply its known effect to passetsCache before building+signing mint3,
    // reverting before mining -- so mint3 is signed the same way a genuinely
    // sequential wallet flow would sign it, and this test is exercising the
    // undo/tx-index bug, not the (separate, already-fixed) intra-block
    // visibility one.
    auto mint2 = CreateMintAssetTx(*m_node.mempool, utxos, coinbaseKey, assetId);
    CMutableTransaction mint3;
    {
        CDatabaseAssetData &entry = passetsCache->mapAsset[assetId];
        CAssetMetaData saved = entry.asset;
        entry.asset.circulatingSupply += 10; // mint2's own effect (UpdateAsset's own amount/COIN math)
        entry.asset.mintCount += 1;
        mint3 = CreateMintAssetTx(*m_node.mempool, utxos, coinbaseKey, assetId);
        entry.asset = saved;
    }
    CBlockIndex *pindexBlock2;
    {
        auto block = std::make_shared<CBlock>(CreateBlock({mint2, mint3}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);
        pindexBlock2 = ::ChainActive().Tip();
        BOOST_REQUIRE_EQUAL(pindexBlock2->GetBlockHash().ToString(), block->GetHash().ToString());
    }

    BOOST_REQUIRE(passetsCache->GetAssetMetaData(assetId, asset));
    BOOST_REQUIRE_EQUAL(asset.mintCount, 3);
    BOOST_REQUIRE_EQUAL(asset.circulatingSupply, 30);

    // Disconnect block 2 (invalidate the tip). A correct undo restores
    // EXACTLY block 1's post-state: mintCount=1, circulatingSupply=10.
    {
        LOCK(cs_main);
        CValidationState state;
        BOOST_REQUIRE(InvalidateBlock(state, Params(), pindexBlock2));
    }

    BOOST_REQUIRE(passetsCache->GetAssetMetaData(assetId, asset));
    BOOST_CHECK_EQUAL(asset.mintCount, 1);
    BOOST_CHECK_EQUAL(asset.circulatingSupply, 10);
}

// F-223 (3.4, bug 3): CheckNewAssetTx/CheckUpdateAssetTx's own distribution-type
// bound is `assetTx.type < 0 && assetTx.type > 3` (evo/providertx.cpp). type is
// uint8_t, so `type < 0` is always false, and with `&&` the whole condition is
// always false regardless -- no type value is ever rejected here. F-216 already
// found this dead code (docs/findings.md); this pins it directly rather than via
// the collateralAddress check that happens to mask it whenever collateralAddress
// is left null (assets_creation's own "invalid distribution type" case above
// passes today for that reason, not because this bound fires -- confirmed by
// giving collateralAddress a real, non-null value below so that mask cannot
// fire, isolating the bound this test actually means to pin).
BOOST_FIXTURE_TEST_CASE(assets_distribution_type_out_of_range_rejected, TestChainDIP3BeforeActivationSetup)
{
    CKey sporkKey;
    sporkKey.MakeNewKey(false);
    sporkManager.SetSporkAddress(EncodeDestination(sporkKey.GetPubKey().GetID()));
    sporkManager.SetPrivKey(EncodeSecret(sporkKey));
    sporkManager.UpdateSpork(SPORK_22_SPECIAL_TX_FEE, 2560, *m_node.connman);

    auto utxos = BuildSimpleUtxoMap(m_coinbase_txns);

    CKey collateralKey;
    collateralKey.MakeNewKey(false);

    CKeyID ownerKey = coinbaseKey.GetPubKey().GetID();
    CNewAssetTx newAsset;
    newAsset.name = "OUTOFRANGETYPE";
    newAsset.isRoot = true;
    newAsset.updatable = true;
    newAsset.isUnique = false;
    newAsset.decimalPoint = 8;
    newAsset.referenceHash = "";
    newAsset.type = 99; // out of range: only 0-3 (manual/coinbase/address/schedule) are defined
    newAsset.maxMintCount = 10;
    newAsset.fee = getAssetsFees();
    newAsset.targetAddress = ownerKey;
    newAsset.ownerAddress = ownerKey;
    newAsset.amount = 1000 * COIN;
    // Real, non-null collateralAddress: with type != 0, a null collateralAddress
    // is independently rejected (bad-assets-collateralAddress) -- exactly the
    // masking F-216 found. Setting a real one isolates the distribution-type
    // bound itself.
    newAsset.collateralAddress = collateralKey.GetPubKey().GetID();

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_NEW_ASSET;
    FundTransaction(tx, utxos, GetScriptForDestination(coinbaseKey.GetPubKey().GetID()), newAsset.fee * COIN + 1 * COIN,
        coinbaseKey);
    newAsset.inputsHash = CalcTxInputsHash(tx);
    SetTxPayload(tx, newAsset);
    BOOST_ASSERT(SignTransaction(*m_node.mempool, tx, coinbaseKey));

    int nHeight = ::ChainActive().Height();
    std::vector<CMutableTransaction> txns = {tx};
    auto block = std::make_shared<CBlock>(CreateBlock(txns, coinbaseKey));
    EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);

    // Before the fix: type=99 with a real collateralAddress sails through every
    // check (the bound is dead code) and the block connects. After the fix:
    // rejected bad-assets-distibution-type, height unchanged.
    BOOST_CHECK_EQUAL(::ChainActive().Height(), nHeight);
    BOOST_CHECK(block->GetHash() != ::ChainActive().Tip()->GetBlockHash());
}

BOOST_FIXTURE_TEST_CASE(assets_new_asset_undo_visible_before_flush, TestChainDIP3BeforeActivationSetup)
{
    // F-224 (found while investigating 3.4b item 2's correctness trap):
    // CheckIfAssetExists (assets.cpp) checks only the LOCAL cache's own
    // mapAsset, then falls straight to passetsdb -- unlike GetAssetMetaData,
    // it never consults the GLOBAL passetsCache->mapAsset in between.
    // DisconnectTip constructs a FRESH, EMPTY CAssetsCache per call
    // (validation.cpp: `CAssetsCache assetCache;`), and DisconnectBlock's
    // own NEW_ASSET-undo branch (validation.cpp:2052) calls
    // CheckIfAssetExists on exactly that empty cache before calling
    // RemoveAsset -- so disconnecting a block whose asset creation has not
    // yet been durably written to passetsdb (DumpCacheToDatabase runs on
    // FlushStateToDisk's own periodic/critical schedule, not on every
    // block -- and does not run here, since this test's tiny coin cache
    // never reaches CRITICAL) silently no-ops the undo: RemoveAsset is
    // never called, and the asset stays resident (and reported as
    // existing) in passetsCache after its own creating block was
    // disconnected.
    CKey sporkKey;
    sporkKey.MakeNewKey(false);
    sporkManager.SetSporkAddress(EncodeDestination(sporkKey.GetPubKey().GetID()));
    sporkManager.SetPrivKey(EncodeSecret(sporkKey));
    sporkManager.UpdateSpork(SPORK_22_SPECIAL_TX_FEE, 2560, *m_node.connman);

    auto utxos = BuildSimpleUtxoMap(m_coinbase_txns);
    auto tx = CreateNewAssetTx(*m_node.mempool, utxos, coinbaseKey, "UNDOASSETTEST", true, false, 0, 8, 1000);
    std::string assetId = tx.GetHash().ToString();

    CBlockIndex *pindexBlock;
    {
        auto block = std::make_shared<CBlock>(CreateBlock({tx}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);
        pindexBlock = ::ChainActive().Tip();
        BOOST_REQUIRE_EQUAL(pindexBlock->GetBlockHash().ToString(), block->GetHash().ToString());
    }

    BOOST_REQUIRE(passetsCache->CheckIfAssetExists(assetId));

    // Disconnect the block that created it -- the asset must cease to exist.
    {
        LOCK(cs_main);
        CValidationState state;
        BOOST_REQUIRE(InvalidateBlock(state, Params(), pindexBlock));
    }

    BOOST_CHECK_MESSAGE(!passetsCache->CheckIfAssetExists(assetId),
                         "asset " << assetId << " still reported as existing after its creating block was disconnected");
}

BOOST_FIXTURE_TEST_CASE(asset_cache_flush_bounds_global_cache_size, RegTestingSetup)
{
    // F-224 (build-plan 3.4b item 1). Before the fix, LoadAssets (assetsdb.cpp)
    // capped passetsCache->mapAsset/mapAssetId at MAX_CACHE_ASSETS_SIZE only at
    // startup; CAssetsCache::Flush() (assets.cpp) then merged every
    // subsequently-touched asset into the global map with no eviction, ever.
    // Drive the cache well past the cap directly through InsertAsset+Flush
    // (real block mining would make testing thousands of distinct assets
    // impractically slow) and confirm LoadAssets' own startup invariant --
    // size <= cap -- continues to hold after runtime growth too.
    const size_t cap = MAX_CACHE_ASSETS_SIZE;
    const size_t total = cap + 500;

    for (size_t i = 0; i < total; i++) {
        // Default-constructed (empty), not copy-constructed from
        // passetsCache -- matches ConnectTip/DisconnectTip's own real
        // per-block pattern (`CAssetsCache assetCache;`, validation.cpp). A
        // copy-constructed local would re-touch EVERY entry it started with
        // on every Flush() call (Flush() iterates the whole local mapAsset,
        // not just the delta), swamping any real recency signal.
        CAssetsCache local;
        CNewAssetTx newAssetTx;
        newAssetTx.name = "ASSET" + std::to_string(i);
        newAssetTx.isRoot = true;
        newAssetTx.decimalPoint = 0;
        std::string assetId = "assetid" + std::to_string(i);
        BOOST_REQUIRE(local.InsertAsset(newAssetTx, assetId, 1));
        BOOST_REQUIRE(local.Flush());

        // Simulate FlushStateToDisk's own periodic full flush (validation.cpp,
        // fDoFullFlush), which is what actually writes dirty entries to
        // passetsdb -- without this every entry stays "dirty" and
        // EvictOverflowAssets correctly refuses to touch any of them (see the
        // dedicated dirty-protection test below), so the bound would never
        // engage and this test would not distinguish the fix from the bug.
        if (i % 100 == 99) {
            BOOST_REQUIRE(passetsCache->DumpCacheToDatabase());
        }
    }
    BOOST_REQUIRE(passetsCache->DumpCacheToDatabase());

    BOOST_CHECK_EQUAL(passetsCache->mapAsset.size(), cap);
    BOOST_CHECK_EQUAL(passetsCache->mapAssetId.size(), cap);

    // Safety-by-construction: an evicted asset must still be found via
    // GetAssetMetaData's own LevelDB fallback (assets.cpp) -- eviction from
    // the in-memory map must never mean the data is gone, only that it has to
    // be refetched.
    CAssetMetaData evictedAsset;
    BOOST_CHECK(passetsCache->GetAssetMetaData("assetid0", evictedAsset));
    BOOST_CHECK_EQUAL(evictedAsset.name, "ASSET0");
}

BOOST_FIXTURE_TEST_CASE(asset_cache_eviction_never_drops_unflushed_assets, RegTestingSetup)
{
    // F-224 correctness gate -- the archive doc's own "correctness trap"
    // (docs/archive/asset-cache-drag.md) named this for item 2, but it binds
    // item 1's eviction just as hard: an asset confirmed on-chain but not yet
    // durably written to passetsdb (DumpCacheToDatabase runs on
    // FlushStateToDisk's own periodic/critical schedule -- up to
    // DATABASE_FLUSH_INTERVAL = 24h, validation.h:127 -- not on every block,
    // confirmed by reading FlushStateToDisk's fDoFullFlush conditions) must
    // never be evicted: doing so would make it invisible to every reader
    // (GetAssetMetaData's own DB fallback would miss it too) until the next
    // flush -- a real duplicate-name/duplicate-mint hole, not a slowdown. Use
    // a small explicit cap so "dirty count exceeds the cap" is reachable
    // without thousands of insertions.
    const size_t smallCap = 5;

    // Durable pool: insert, flush, and dump to DB so these become eviction-
    // eligible.
    for (size_t i = 0; i < smallCap; i++) {
        // Default-constructed (empty), not copy-constructed from
        // passetsCache -- matches ConnectTip/DisconnectTip's own real
        // per-block pattern (`CAssetsCache assetCache;`, validation.cpp). A
        // copy-constructed local would re-touch EVERY entry it started with
        // on every Flush() call (Flush() iterates the whole local mapAsset,
        // not just the delta), swamping any real recency signal.
        CAssetsCache local;
        CNewAssetTx newAssetTx;
        newAssetTx.name = "DURABLE" + std::to_string(i);
        newAssetTx.isRoot = true;
        std::string assetId = "durableid" + std::to_string(i);
        BOOST_REQUIRE(local.InsertAsset(newAssetTx, assetId, 1));
        BOOST_REQUIRE(local.Flush());
    }
    BOOST_REQUIRE(passetsCache->DumpCacheToDatabase());
    BOOST_CHECK_EQUAL(passetsCache->mapAsset.size(), smallCap);

    // Dirty batch: more than smallCap new assets, inserted+flushed but NEVER
    // dumped to passetsdb -- exactly the "recently confirmed, not yet
    // flushed" state the correctness trap describes.
    const size_t dirtyCount = smallCap * 3;
    std::vector<std::string> dirtyIds;
    for (size_t i = 0; i < dirtyCount; i++) {
        // Default-constructed (empty), not copy-constructed from
        // passetsCache -- matches ConnectTip/DisconnectTip's own real
        // per-block pattern (`CAssetsCache assetCache;`, validation.cpp). A
        // copy-constructed local would re-touch EVERY entry it started with
        // on every Flush() call (Flush() iterates the whole local mapAsset,
        // not just the delta), swamping any real recency signal.
        CAssetsCache local;
        CNewAssetTx newAssetTx;
        newAssetTx.name = "DIRTY" + std::to_string(i);
        newAssetTx.isRoot = true;
        std::string assetId = "dirtyid" + std::to_string(i);
        BOOST_REQUIRE(local.InsertAsset(newAssetTx, assetId, 1));
        BOOST_REQUIRE(local.Flush());
        // Flush() always evicts at the production default (2500); exercise
        // the mechanism at this test's smaller cap directly instead of
        // waiting for 2500+ real insertions.
        passetsCache->EvictOverflowAssets(smallCap);
        passetsCache->EvictOverflowAssetIds(smallCap);
        dirtyIds.push_back(assetId);
    }

    // Every dirty asset must still be directly resident -- there is nowhere
    // else for it to live, since it was never durably written. Both maps:
    // EvictOverflowAssetIds has its own, separate dirty check (via the
    // corresponding mapAsset entry) from EvictOverflowAssets.
    for (const auto &id : dirtyIds) {
        BOOST_CHECK_MESSAGE(passetsCache->mapAsset.count(id) == 1,
                             "dirty asset " << id << " was evicted with no durable copy");
    }
    for (size_t i = 0; i < dirtyCount; i++) {
        std::string name = "DIRTY" + std::to_string(i);
        BOOST_CHECK_MESSAGE(passetsCache->mapAssetId.count(name) == 1,
                             "dirty asset name " << name << " was evicted from mapAssetId with no durable copy");
    }
    // The cap is exceeded here -- correctly: temporary overflow is the honest
    // outcome when nothing safe remains to evict, not silent data loss.
    BOOST_CHECK_EQUAL(passetsCache->mapAsset.size(), dirtyCount);
    BOOST_CHECK_EQUAL(passetsCache->mapAssetId.size(), dirtyCount);

    // The durable pool, having nothing protecting it, was fully evicted to
    // make room first.
    for (size_t i = 0; i < smallCap; i++) {
        BOOST_CHECK_EQUAL(passetsCache->mapAsset.count("durableid" + std::to_string(i)), 0u);
        BOOST_CHECK_EQUAL(passetsCache->mapAssetId.count("DURABLE" + std::to_string(i)), 0u);
    }
}

BOOST_FIXTURE_TEST_CASE(asset_cache_eviction_is_recency_ordered, RegTestingSetup)
{
    // Distinguishes real LRU eviction from a weaker policy (e.g. evicting
    // whatever iterates first in mapAsset -- for a std::map keyed by a string
    // ID that's sorted-key order, unrelated to actual use) -- touch an
    // early-inserted asset again right before the cache overflows and confirm
    // eviction picks a genuinely untouched one instead.
    const size_t smallCap = 5;
    std::vector<std::string> ids;
    for (size_t i = 0; i < smallCap; i++) {
        // Default-constructed (empty), not copy-constructed from
        // passetsCache -- matches ConnectTip/DisconnectTip's own real
        // per-block pattern (`CAssetsCache assetCache;`, validation.cpp). A
        // copy-constructed local would re-touch EVERY entry it started with
        // on every Flush() call (Flush() iterates the whole local mapAsset,
        // not just the delta), swamping any real recency signal.
        CAssetsCache local;
        CNewAssetTx newAssetTx;
        newAssetTx.name = "A" + std::to_string(i);
        newAssetTx.isRoot = true;
        std::string assetId = "id" + std::to_string(i);
        BOOST_REQUIRE(local.InsertAsset(newAssetTx, assetId, 1));
        BOOST_REQUIRE(local.Flush());
        ids.push_back(assetId);
    }
    BOOST_REQUIRE(passetsCache->DumpCacheToDatabase()); // make all smallCap evictable
    BOOST_CHECK_EQUAL(passetsCache->mapAsset.size(), smallCap);

    // Re-touch the OLDEST entry (ids[0]) via the real read path, making it
    // most-recently-used just before overflow.
    CAssetMetaData tmp;
    BOOST_REQUIRE(passetsCache->GetAssetMetaData(ids.front(), tmp));

    // One more asset overflows the cache by exactly one. A naive "evict the
    // oldest inserted" policy would pick ids[0]; since it was just touched,
    // ids[1] (the next-oldest, untouched since its own insertion) must be
    // the one evicted instead.
    CAssetsCache local; // see the comment on the loop's own local cache above
    CNewAssetTx newAssetTx;
    newAssetTx.name = "NEW";
    newAssetTx.isRoot = true;
    BOOST_REQUIRE(local.InsertAsset(newAssetTx, "newid", 1));
    BOOST_REQUIRE(local.Flush());
    passetsCache->EvictOverflowAssets(smallCap);

    BOOST_CHECK_EQUAL(passetsCache->mapAsset.count(ids[0]), 1u); // touched -- survives
    BOOST_CHECK_EQUAL(passetsCache->mapAsset.count(ids[1]), 0u); // untouched, oldest remaining -- evicted
    BOOST_CHECK_EQUAL(passetsCache->mapAsset.count("newid"), 1u); // just inserted -- survives
}

BOOST_FIXTURE_TEST_CASE(atmp_rejects_dup_name_against_unflushed_asset, TestChainDIP3BeforeActivationSetup)
{
    // F-224 (build-plan 3.4b item 2): the archive doc's own named
    // correctness trap (docs/archive/asset-cache-drag.md), reproduced
    // directly. validation.cpp's ATMP guard now constructs an EMPTY
    // CAssetsCache for asset-typed transactions instead of a full copy of
    // passetsCache. A first NEW_ASSET tx is mined into a block (confirmed,
    // resident only in passetsCache's own in-memory state -- this test's
    // tiny coin cache never reaches CRITICAL, so DumpCacheToDatabase never
    // runs and the asset is never durably written to passetsdb). A second,
    // same-name NEW_ASSET tx is then submitted directly to ATMP, never
    // mined. If the empty ATMP cache could only see passetsdb (missing the
    // global-cache middle tier this row's own investigation added to
    // CheckIfAssetExists/GetAssetId), the duplicate would sail through ATMP
    // -- exactly the "let a duplicate-name asset through" regression the
    // archive doc warned a naive fix could cause.
    CKey sporkKey;
    sporkKey.MakeNewKey(false);
    sporkManager.SetSporkAddress(EncodeDestination(sporkKey.GetPubKey().GetID()));
    sporkManager.SetPrivKey(EncodeSecret(sporkKey));
    sporkManager.UpdateSpork(SPORK_22_SPECIAL_TX_FEE, 2560, *m_node.connman);

    auto utxos = BuildSimpleUtxoMap(m_coinbase_txns);

    auto tx1 = CreateNewAssetTx(*m_node.mempool, utxos, coinbaseKey, "ATMPDUPTEST", true, false, 0, 8, 1000);
    {
        auto block = std::make_shared<CBlock>(CreateBlock({tx1}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);
        BOOST_REQUIRE_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), block->GetHash().ToString());
    }
    // Confirmed but never durably written -- exactly the window the
    // archive doc's trap describes.
    BOOST_REQUIRE(passetsCache->CheckIfAssetExists(tx1.GetHash().ToString()));

    // Same name, a fresh tx (different txid/assetId) -- never mined, only
    // offered to ATMP directly.
    auto tx2 = CreateNewAssetTx(*m_node.mempool, utxos, coinbaseKey, "ATMPDUPTEST", true, false, 0, 8, 1000);

    CValidationState state;
    {
        LOCK(cs_main);
        BOOST_CHECK(!AcceptToMemoryPool(*m_node.mempool, state, MakeTransactionRef(tx2),
                                         nullptr /* pfMissingInputs */, true /* bypass_limits */,
                                         0 /* nAbsurdFee */));
    }
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-assets-dup-name");
    BOOST_CHECK(!m_node.mempool->exists(tx2.GetHash()));
}

BOOST_FIXTURE_TEST_CASE(atmp_allows_reregistration_after_reorg_undoes_creation, TestChainDIP3BeforeActivationSetup)
{
    // F-224: the OTHER half of CheckIfAssetExists's new global middle tier
    // -- the GLOBAL NewAssetsToRemove check, not just the GLOBAL mapAsset
    // check. RemoveAsset (assets.cpp) marks an asset removed only in the
    // dirty NewAssetsToRemove set; it never erases the entry from mapAsset
    // itself (confirmed by reading RemoveAsset and DumpCacheToDatabase in
    // full -- DumpCacheToDatabase erases the passetsdb row but likewise
    // never touches mapAsset). So after a reorg properly undoes a NEW_ASSET
    // creation (assets_new_asset_undo_visible_before_flush above), a STALE
    // "exists" entry for that assetId still sits in passetsCache->mapAsset
    // forever. Without also checking passetsCache->NewAssetsToRemove here,
    // an empty ATMP cache's new global middle tier would find that stale
    // entry and wrongly reject a legitimate re-registration of the same
    // name as a duplicate -- a real behaviour regression relative to
    // today's full-copy ATMP cache, which already carries
    // passetsCache->NewAssetsToRemove via its copy constructor and so
    // already masks this correctly.
    CKey sporkKey;
    sporkKey.MakeNewKey(false);
    sporkManager.SetSporkAddress(EncodeDestination(sporkKey.GetPubKey().GetID()));
    sporkManager.SetPrivKey(EncodeSecret(sporkKey));
    sporkManager.UpdateSpork(SPORK_22_SPECIAL_TX_FEE, 2560, *m_node.connman);

    auto utxos = BuildSimpleUtxoMap(m_coinbase_txns);

    auto tx1 = CreateNewAssetTx(*m_node.mempool, utxos, coinbaseKey, "REORGREUSETEST", true, false, 0, 8, 1000);
    CBlockIndex *pindexBlock;
    {
        auto block = std::make_shared<CBlock>(CreateBlock({tx1}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);
        pindexBlock = ::ChainActive().Tip();
        BOOST_REQUIRE_EQUAL(pindexBlock->GetBlockHash().ToString(), block->GetHash().ToString());
    }
    BOOST_REQUIRE(passetsCache->CheckIfAssetExists(tx1.GetHash().ToString()));

    // Undo the creation via a reorg -- properly, now that the fix makes the
    // undo itself reachable (assets_new_asset_undo_visible_before_flush).
    {
        LOCK(cs_main);
        CValidationState state;
        BOOST_REQUIRE(InvalidateBlock(state, Params(), pindexBlock));
    }
    // InvalidateBlock's own disconnectpool re-admits tx1 itself back into
    // the mempool (standard reorg handling) -- still claiming the name via
    // CTxMemPool's OWN separate mapAssetsToHash index (txmempool.cpp), which
    // is a real and correct reason for a second same-name tx to conflict,
    // but a DIFFERENT mechanism than the one this test means to isolate
    // (confirmed empirically: tx2 below was rejected "asset-dup" via
    // existsAssetTxConflict, never reaching CheckNewAssetTx/
    // CheckIfAssetExists at all -- and CTxMemPool::clear()/_clear() turned
    // out not to clear mapAssetsToHash/mapAssetsIdToHash either, a separate,
    // pre-existing mempool-index bug out of this row's scope, noted but not
    // fixed here). removeRecursive is the normal per-tx removal path and
    // does erase the index (txmempool.cpp), so use that instead of clear().
    BOOST_REQUIRE(m_node.mempool->exists(tx1.GetHash()));
    m_node.mempool->removeRecursive(tx1, MemPoolRemovalReason::MANUAL);
    BOOST_REQUIRE(!m_node.mempool->exists(tx1.GetHash()));
    BOOST_REQUIRE(!passetsCache->CheckIfAssetExists(tx1.GetHash().ToString()));
    // The stale entry is still directly resident -- RemoveAsset/
    // DumpCacheToDatabase never erase mapAsset itself, only the dirty
    // marker and the DB row. This is what the masking check above must see
    // past.
    BOOST_REQUIRE_EQUAL(passetsCache->mapAsset.count(tx1.GetHash().ToString()), 1u);

    // A fresh NEW_ASSET tx reusing the SAME name must be accepted by ATMP --
    // the original was genuinely undone.
    auto tx2 = CreateNewAssetTx(*m_node.mempool, utxos, coinbaseKey, "REORGREUSETEST", true, false, 0, 8, 1000);
    CValidationState state2;
    {
        LOCK(cs_main);
        BOOST_CHECK(AcceptToMemoryPool(*m_node.mempool, state2, MakeTransactionRef(tx2),
                                        nullptr /* pfMissingInputs */, true /* bypass_limits */,
                                        0 /* nAbsurdFee */));
    }
    BOOST_CHECK(m_node.mempool->exists(tx2.GetHash()));
}

BOOST_FIXTURE_TEST_CASE(block_reconnect_after_flush_does_not_see_stale_dup_name, TestChainDIP3BeforeActivationSetup)
{
    // F-224: found running feature_assets_rules.py's own test_reorg against
    // this row's build, not from source reading alone. RemoveAsset (via
    // CheckIfAssetExists's own fix above, this same row) only marks a
    // removal in NewAssetsToRemove/NewAssetsToAdd; DumpCacheToDatabase is
    // the ONE place that removal becomes durable, and its own
    // ClearDirtyCache() (assets.cpp) wipes NewAssetsToRemove immediately
    // after -- so once a full flush has happened, the masking check
    // CheckIfAssetExists's global middle tier relies on no longer protects
    // anything, and a stale mapAsset/mapAssetId entry (left behind because
    // DumpCacheToDatabase used to erase only the passetsdb row, never the
    // in-memory maps) becomes visible again as a false "still exists".
    // Reproduced live: invalidateblock, then a `listassets` RPC call (whose
    // own GetListAssets forces exactly this flush, assetsdb.cpp
    // ForceFlushStateToDisk), then reconsiderblock -- ConnectBlock failed
    // with bad-assets-dup-name, rejecting a legitimate reconnect of a block
    // that had already been fully valid. This test drives the same
    // sequence directly. Fixed by having DumpCacheToDatabase erase the
    // in-memory mapAsset/mapAssetId entries (and their LRU records) in the
    // same pass it erases the passetsdb row, so the cache and the DB go
    // stale together instead of the cache lagging forever.
    CKey sporkKey;
    sporkKey.MakeNewKey(false);
    sporkManager.SetSporkAddress(EncodeDestination(sporkKey.GetPubKey().GetID()));
    sporkManager.SetPrivKey(EncodeSecret(sporkKey));
    sporkManager.UpdateSpork(SPORK_22_SPECIAL_TX_FEE, 2560, *m_node.connman);

    auto utxos = BuildSimpleUtxoMap(m_coinbase_txns);

    auto tx1 = CreateNewAssetTx(*m_node.mempool, utxos, coinbaseKey, "RECONNECTTEST", true, false, 0, 8, 1000);
    CBlockIndex *pindexBlock;
    {
        auto block = std::make_shared<CBlock>(CreateBlock({tx1}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);
        pindexBlock = ::ChainActive().Tip();
        BOOST_REQUIRE_EQUAL(pindexBlock->GetBlockHash().ToString(), block->GetHash().ToString());
    }
    BOOST_REQUIRE(passetsCache->CheckIfAssetExists(tx1.GetHash().ToString()));

    {
        LOCK(cs_main);
        CValidationState state;
        BOOST_REQUIRE(InvalidateBlock(state, Params(), pindexBlock));
    }
    BOOST_REQUIRE(!passetsCache->CheckIfAssetExists(tx1.GetHash().ToString()));

    // The trigger: a full flush, exactly like listassets's own
    // ForceFlushStateToDisk (assetsdb.cpp:GetListAssets). Durably erases the
    // DB row AND clears NewAssetsToRemove's masking protection in the same
    // call.
    BOOST_REQUIRE(passetsCache->DumpCacheToDatabase());

    // Reconnect the SAME block -- ResetBlockFailureFlags + ActivateBestChain
    // is exactly what the reconsiderblock RPC does (rpc/blockchain.cpp).
    {
        LOCK(cs_main);
        ResetBlockFailureFlags(pindexBlock);
    }
    CValidationState state2;
    BOOST_REQUIRE(ActivateBestChain(state2, Params()));
    BOOST_CHECK_MESSAGE(state2.IsValid(), "ActivateBestChain: " << state2.GetRejectReason());
    BOOST_CHECK_EQUAL(::ChainActive().Tip()->GetBlockHash().ToString(), pindexBlock->GetBlockHash().ToString());
    BOOST_CHECK(passetsCache->CheckIfAssetExists(tx1.GetHash().ToString()));
}

// 4.3.1 (build-plan.md's 4.3 row, docs/findings.md's F-225): BuildAssetIndexFromCoins
// must reconstruct the address-balance secondary index purely from the CURRENT coin
// set, with no reliance on having ever run with -assetindex enabled. fAssetIndex
// defaults to DEFAULT_ASSETINDEX (false, validation.h) and nothing in this test suite
// ever flips it, so every block mined below genuinely never populates
// mapAssetAddressAmount/passetsdb's balance rows via the normal AddAssetBlance/
// RemoveAddressBalance path -- this is a real "-assetindex was never on" starting
// condition, not a simulated one.
BOOST_FIXTURE_TEST_CASE(build_asset_index_from_coins_reconstructs_current_balances, TestChainDIP3BeforeActivationSetup)
{
    BOOST_REQUIRE(!fAssetIndex);

    auto utxos = BuildSimpleUtxoMap(m_coinbase_txns);

    auto tx = CreateNewAssetTx(*m_node.mempool, utxos, coinbaseKey, "COINSCAN_ASSET", true, false, 0, 8, 1000);
    std::string assetId = tx.GetHash().ToString();
    {
        auto block = std::make_shared<CBlock>(CreateBlock({tx}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);
        BOOST_REQUIRE(block->GetHash() == ::ChainActive().Tip()->GetBlockHash());
    }

    tx = CreateMintAssetTx(*m_node.mempool, utxos, coinbaseKey, assetId);
    {
        auto block = std::make_shared<CBlock>(CreateBlock({tx}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);
        BOOST_REQUIRE(block->GetHash() == ::ChainActive().Tip()->GetBlockHash());
    }
    // Matching assets_mint's own established pattern (above): the mint tx is
    // no longer in the mempool once mined, so SignTransaction's own
    // GetTransaction lookup for the transfer's input needs the txindex to
    // have caught up first -- without this, isolated single-test runs happen
    // to be fast enough to race past it, but the full suite's cumulative
    // load does not, and the lookup fails intermittently.
    g_txindex->BlockUntilSyncedToCurrentChain();

    // Split the full 1000-unit minted supply across two distinct addresses in
    // one transfer, neither of which is coinbaseKey's own address.
    CKey keyB, keyC;
    keyB.MakeNewKey(false);
    keyC.MakeNewKey(false);
    CScript scriptB = GetScriptForDestination(keyB.GetPubKey().GetID());
    CScript scriptC = GetScriptForDestination(keyC.GetPubKey().GetID());

    CMutableTransaction transferTx;
    CAssetTransfer transferB(assetId, 400 * COIN);
    transferB.BuildAssetTransaction(scriptB);
    transferTx.vout.push_back(CTxOut(0, scriptB));
    CAssetTransfer transferC(assetId, 600 * COIN);
    transferC.BuildAssetTransaction(scriptC);
    transferTx.vout.push_back(CTxOut(0, scriptC));
    transferTx.vin.push_back(CTxIn(COutPoint(tx.GetHash(), 0)));
    FundTransaction(transferTx, utxos, GetScriptForDestination(coinbaseKey.GetPubKey().GetID()), 1 * COIN, coinbaseKey);
    BOOST_REQUIRE(SignTransaction(*m_node.mempool, transferTx, coinbaseKey));
    {
        auto block = std::make_shared<CBlock>(CreateBlock({transferTx}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);
        BOOST_REQUIRE(block->GetHash() == ::ChainActive().Tip()->GetBlockHash());
    }

    std::string addressB = EncodeDestination(keyB.GetPubKey().GetID());
    std::string addressC = EncodeDestination(keyC.GetPubKey().GetID());

    // Confirm the "index never built" starting condition for real, not assumed.
    CAmount128 preAmount;
    BOOST_CHECK(!passetsdb->ReadAssetAddressAmount(assetId, addressB, preAmount));
    BOOST_CHECK(!passetsdb->ReadAssetAddressAmount(assetId, addressC, preAmount));
    BOOST_CHECK(passetsCache->mapAssetAddressAmount.find(std::make_pair(assetId, addressB)) ==
                passetsCache->mapAssetAddressAmount.end());

    // Asset EXISTENCE/metadata, by contrast, is already there unconditionally
    // (never gated on fAssetIndex) -- confirming this rebuild has no reason to
    // touch it.
    CAssetMetaData meta;
    BOOST_REQUIRE(passetsCache->GetAssetMetaData(assetId, meta));

    BOOST_REQUIRE(BuildAssetIndexFromCoins(::ChainstateActive().CoinsDB()));

    CAmount128 amountB, amountC;
    BOOST_REQUIRE(passetsdb->ReadAssetAddressAmount(assetId, addressB, amountB));
    BOOST_REQUIRE(passetsdb->ReadAssetAddressAmount(assetId, addressC, amountC));
    BOOST_CHECK_EQUAL(amountB.str(), CAmount128(400 * COIN).str());
    BOOST_CHECK_EQUAL(amountC.str(), CAmount128(600 * COIN).str());

    CAmount128 addrAmountB, addrAmountC;
    BOOST_REQUIRE(passetsdb->ReadAssetAddressAssetAmount(addressB, assetId, addrAmountB));
    BOOST_REQUIRE(passetsdb->ReadAssetAddressAssetAmount(addressC, assetId, addrAmountC));
    BOOST_CHECK_EQUAL(addrAmountB.str(), CAmount128(400 * COIN).str());
    BOOST_CHECK_EQUAL(addrAmountC.str(), CAmount128(600 * COIN).str());

    BOOST_CHECK_EQUAL(passetsCache->mapAssetAddressAmount.at(std::make_pair(assetId, addressB)).str(),
                       CAmount128(400 * COIN).str());
    BOOST_CHECK_EQUAL(passetsCache->mapAssetAddressAmount.at(std::make_pair(assetId, addressC)).str(),
                       CAmount128(600 * COIN).str());

    // Asset existence/metadata is unchanged by the rebuild.
    CAssetMetaData metaAfter;
    BOOST_REQUIRE(passetsCache->GetAssetMetaData(assetId, metaAfter));
    BOOST_CHECK_EQUAL(metaAfter.name, meta.name);
    BOOST_CHECK_EQUAL(metaAfter.circulatingSupply, meta.circulatingSupply);

    // Rerunning the scan with no coin-set change is idempotent -- writing the
    // same balances twice must not double them (a plain LevelDB Put overwrites
    // regardless, so this alone would not catch a missing
    // EraseAssetAddressAmounts -- see the moved-away case just below for that).
    BOOST_REQUIRE(BuildAssetIndexFromCoins(::ChainstateActive().CoinsDB()));
    CAmount128 amountBAgain;
    BOOST_REQUIRE(passetsdb->ReadAssetAddressAmount(assetId, addressB, amountBAgain));
    BOOST_CHECK_EQUAL(amountBAgain.str(), CAmount128(400 * COIN).str());

    // Now move addressB's entire holding away to a fresh address D. A rebuild
    // must leave addressB with NO stale entry -- this is what actually
    // exercises EraseAssetAddressAmounts: a plain overwrite of the still-live
    // (assetId, addressD) key would never touch the old, now-stale
    // (assetId, addressB) key on its own.
    g_txindex->BlockUntilSyncedToCurrentChain();
    CKey keyD;
    keyD.MakeNewKey(false);
    CScript scriptD = GetScriptForDestination(keyD.GetPubKey().GetID());
    std::string addressD = EncodeDestination(keyD.GetPubKey().GetID());

    CMutableTransaction moveAwayTx;
    CAssetTransfer transferD(assetId, 400 * COIN);
    transferD.BuildAssetTransaction(scriptD);
    moveAwayTx.vout.push_back(CTxOut(0, scriptD));
    moveAwayTx.vin.push_back(CTxIn(COutPoint(transferTx.GetHash(), 0)));
    FundTransaction(moveAwayTx, utxos, GetScriptForDestination(coinbaseKey.GetPubKey().GetID()), 1 * COIN, coinbaseKey);
    // moveAwayTx spends from two different owners (transferTx's output 0,
    // owned by keyB, plus a fee input funded from coinbaseKey's own coins),
    // so SignTransaction's own single-key keystore can't cover it -- sign
    // with a keystore holding both.
    {
        CBasicKeyStore tempKeystore;
        tempKeystore.AddKeyPubKey(coinbaseKey, coinbaseKey.GetPubKey());
        tempKeystore.AddKeyPubKey(keyB, keyB.GetPubKey());
        for (size_t i = 0; i < moveAwayTx.vin.size(); i++) {
            uint256 hashBlock;
            CTransactionRef txFrom = GetTransaction(/* block_index */ nullptr, m_node.mempool,
                moveAwayTx.vin[i].prevout.hash, Params().GetConsensus(), hashBlock);
            BOOST_REQUIRE(txFrom);
            BOOST_REQUIRE(SignSignature(tempKeystore, *txFrom, moveAwayTx, i, SIGHASH_ALL));
        }
    }
    {
        auto block = std::make_shared<CBlock>(CreateBlock({moveAwayTx}, coinbaseKey));
        EnsureChainman(m_node).ProcessNewBlock(Params(), block, true, nullptr);
        BOOST_REQUIRE(block->GetHash() == ::ChainActive().Tip()->GetBlockHash());
    }

    BOOST_REQUIRE(BuildAssetIndexFromCoins(::ChainstateActive().CoinsDB()));

    CAmount128 amountD;
    BOOST_REQUIRE(passetsdb->ReadAssetAddressAmount(assetId, addressD, amountD));
    BOOST_CHECK_EQUAL(amountD.str(), CAmount128(400 * COIN).str());

    CAmount128 staleAmountB;
    BOOST_CHECK(!passetsdb->ReadAssetAddressAmount(assetId, addressB, staleAmountB));
    BOOST_CHECK(!passetsdb->ReadAssetAddressAssetAmount(addressB, assetId, staleAmountB));
    BOOST_CHECK(passetsCache->mapAssetAddressAmount.find(std::make_pair(assetId, addressB)) ==
                passetsCache->mapAssetAddressAmount.end());
}

BOOST_AUTO_TEST_CASE(validate_amount_decimal_point) {
    // Normal range (decimalPoint 0..8): the divisor is 10^(8 - decimalPoint) and an
    // amount is valid iff it is a whole multiple of that divisor.
    BOOST_CHECK(validateAmount(100000000, 0));  // exactly 1 unit with 0 decimals
    BOOST_CHECK(!validateAmount(1, 0));         // not a multiple of 1e8
    BOOST_CHECK(validateAmount(10000, 4));      // multiple of 1e4
    BOOST_CHECK(!validateAmount(10001, 4));     // not a multiple of 1e4
    BOOST_CHECK(validateAmount(1, 8));          // divisor is 1, anything passes
    BOOST_CHECK(validateAmount(0, 4));          // zero is always valid

    // Out-of-range decimalPoint (> 8) must be rejected, not crash. Before the fix the
    // divisor 10^(8 - decimalPoint) underflowed to 0 and the modulo raised SIGFPE.
    BOOST_CHECK(!validateAmount(100000000, 9));
    BOOST_CHECK(!validateAmount(0, 100));
    BOOST_CHECK(!validateAmount(12345, 65535)); // max uint16_t, previously div-by-zero
}

BOOST_AUTO_TEST_SUITE_END()
