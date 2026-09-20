// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TXDB_H
#define BITCOIN_TXDB_H

#include <bodystore.h>
#include <coins.h>
#include <dbwrapper.h>
#include <chain.h>
#include <indices/spent_index.h>
#include <indices/future_index.h>
#include <primitives/block.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

class CBlockIndex;

class CCoinsViewDBCursor;

class uint256;

//! -dbcache default (MiB)
static const int64_t nDefaultDbCache = 300;
//! -dbbatchsize default (bytes)
static const int64_t nDefaultDbBatchSize = 16 << 20;
//! max. -dbcache (MiB)
static const int64_t nMaxDbCache = sizeof(void *) > 4 ? 16384 : 1024;
//! min. -dbcache (MiB)
static const int64_t nMinDbCache = 4;
//! Max memory allocated to block tree DB specific cache, if no -txindex (MiB)
static const int64_t nMaxBlockDBCache = 2;
//! Max memory allocated to block tree DB specific cache, if -txindex (MiB)
// Unlike for the UTXO database, for the txindex scenario the leveldb cache make
// a meaningful difference: https://github.com/bitcoin/bitcoin/pull/8273#issuecomment-229601991
static const int64_t nMaxTxIndexCache = 1024;
//! Max memory allocated to coin DB specific cache (MiB)
static const int64_t nMaxCoinsDBCache = 8;

// Actually declared in validation.cpp; can't include because of circular dependency.
extern RecursiveMutex cs_main;

/** CCoinsView backed by the coin database (chainstate/) */
class CCoinsViewDB final : public CCoinsView {
protected:
    std::unique_ptr <CDBWrapper> m_db;
    fs::path m_ldb_path;
    bool m_is_memory;
public:
    /**
     * @param[in] ldb_path    Location in the filesystem where leveldb data will be stored.
     */
    explicit CCoinsViewDB(fs::path ldb_path, size_t nCacheSize, bool fMemory, bool fWipe);


    bool GetCoin(const COutPoint &outpoint, Coin &coin) const override;

    bool HaveCoin(const COutPoint &outpoint) const override;

    uint256 GetBestBlock() const override;

    std::vector <uint256> GetHeadBlocks() const override;

    bool BatchWrite(CCoinsMap &mapCoins, const uint256 &hashBlock) override;

    CCoinsViewCursor *Cursor() const override;

    //! Attempt to update from an older database format. Returns whether an error occurred.
    bool Upgrade();

    size_t EstimateSize() const override;

    //! Dynamically alter the underlying leveldb cache size.
    void ResizeCache(size_t new_cache_size)

    EXCLUSIVE_LOCKS_REQUIRED(cs_main);
};

/** Specialization of CCoinsViewCursor to iterate over a CCoinsViewDB */
class CCoinsViewDBCursor : public CCoinsViewCursor {
public:
    ~CCoinsViewDBCursor() {}

    bool GetKey(COutPoint &key) const override;

    bool GetValue(Coin &coin) const override;

    unsigned int GetValueSize() const override;

    bool Valid() const override;

    void Next() override;

private:
    CCoinsViewDBCursor(CDBIterator *pcursorIn, const uint256 &hashBlockIn) : CCoinsViewCursor(hashBlockIn),
                                                                             pcursor(pcursorIn) {}

    std::unique_ptr <CDBIterator> pcursor;
    std::pair<char, COutPoint> keyTmp;

    friend class CCoinsViewDB;
};

/** Access to the block database (blocks/index/) */
class CBlockTreeDB : public CDBWrapper {
public:
    explicit CBlockTreeDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);

    // 2.1.2/2.1.3 review found body-file info's persistence (originally its own
    // separate, independently-timed batch) genuinely coupled to this write once
    // CDiskBlockIndex started persisting nBodyFile/nBodyPos: an index entry
    // naming a body position can only be safely durable once the file-size
    // bookkeeping protecting that position from reuse is durable too, and two
    // separate batches can't guarantee that ordering across a crash. Decided
    // (owner, 2026-09-20, F-132): fold body-file info into THIS batch rather
    // than keep it separate and rely on caller-enforced ordering -- one flush,
    // no ordering between the two DATABASE writes to get wrong. (This closes
    // the database-vs-database race F-130 named; it does NOT by itself
    // guarantee the body FILE's own bytes are on disk before this batch makes
    // them durable to name -- see bodystore.h's file-level comment, F-133, for
    // the still-missing FlushBodyFile-equivalent 2.1.4 must add alongside the
    // real write path.) `bodyFileInfo`/`nLastBodyFile` are trailing and
    // defaulted so the one other call site (LoadBlockIndexDB's pre-1.3
    // bodiesmigrated migration, validation.cpp) needs no change: omitting them
    // (nLastBodyFile's default of -1) skips the body-file writes entirely,
    // leaving that call's on-disk effect exactly as before this decision --
    // `nLastBodyFile < 0` and a non-empty `bodyFileInfo` together would mean
    // the caller has a bug (data it wanted written but told us not to via the
    // sentinel), asserted rather than silently ignored.
    // Bodystore.h's GetDirtyBodyFileInfo is what a real caller (FlushStateToDisk)
    // gathers these from.
    bool WriteBatchSync(const std::vector <std::pair<int, const CBlockFileInfo *>> &fileInfo, int nLastFile,
                        const std::vector<const CBlockIndex *> &blockinfo,
                        const std::vector <std::pair<int, CBodyFileInfo>> &bodyFileInfo = {},
                        int nLastBodyFile = -1);

    bool ReadBlockFileInfo(int nFile, CBlockFileInfo &info);

    bool ReadLastBlockFile(int &nFile);

    bool ReadBodyFileInfo(int nFile, CBodyFileInfo &info);

    bool ReadLastBodyFile(int &nFile);

    bool WriteReindexing(bool fReindexing);

    void ReadReindexing(bool &fReindexing);

    bool ReadSpentIndex(CSpentIndexKey &key, CSpentIndexValue &value);

    bool UpdateSpentIndex(const std::vector <std::pair<CSpentIndexKey, CSpentIndexValue>> &vect);

    bool ReadFutureIndex(CFutureIndexKey &key, CFutureIndexValue &value);

    bool UpdateFutureIndex(const std::vector <std::pair<CFutureIndexKey, CFutureIndexValue>> &vect);

    bool UpdateAddressUnspentIndex(const std::vector <std::pair<CAddressUnspentKey, CAddressUnspentValue>> &vect);

    bool ReadAddressUnspentIndex(uint160 addressHash, int type,
                                 std::vector <std::pair<CAddressUnspentKey, CAddressUnspentValue>> &vect);

    bool WriteAddressIndex(const std::vector <std::pair<CAddressIndexKey, CAmount>> &vect);

    bool EraseAddressIndex(const std::vector <std::pair<CAddressIndexKey, CAmount>> &vect);

    bool ReadAddressIndex(uint160 addressHash, int type,
                          std::vector <std::pair<CAddressIndexKey, CAmount>> &addressIndex,
                          int start = 0, int end = 0);

    bool WriteTimestampIndex(const CTimestampIndexKey &timestampIndex);

    bool ReadTimestampIndex(const unsigned int &high, const unsigned int &low, std::vector <uint256> &vect);

    bool WriteFlag(const std::string &name, bool fValue);

    bool ReadFlag(const std::string &name, bool &fValue);

    bool LoadBlockIndexGuts(const Consensus::Params &consensusParams,
                            std::function<CBlockIndex *(const uint256 &)> insertBlockIndex);
};

#endif // BITCOIN_TXDB_H
