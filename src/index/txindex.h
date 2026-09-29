// Copyright (c) 2017-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INDEX_TXINDEX_H
#define BITCOIN_INDEX_TXINDEX_H

#include <chain.h>
#include <index/base.h>
#include <txdb.h>

/**
 * TxIndex is used to look up transactions included in the blockchain by hash.
 * The index is written to a LevelDB database and records the filesystem
 * location of each transaction by transaction hash.
 */
class TxIndex final : public BaseIndex {
protected:
    class DB;

private:
    const std::unique_ptr <DB> m_db;

protected:
    /// Override base class init to migrate from old database.
    bool Init() override;

    bool WriteBlock(const CBlock &block, const CBlockIndex *pindex) override;

    BaseIndex::DB &GetDB() const override;

    const char *GetName() const override { return "txindex"; }

public:
    /// Constructs the index, which becomes available to be queried.
    explicit TxIndex(size_t n_cache_size, bool f_memory = false, bool f_wipe = false);

    // Destructor is declared because this class contains a unique_ptr to an incomplete type.
    virtual ~TxIndex() override;

    /// Look up a transaction by hash.
    ///
    /// @param[in]   tx_hash  The hash of the transaction to be returned.
    /// @param[out]  block_hash  The hash of the block the transaction is found in.
    /// @param[out]  tx  The transaction itself.
    /// @param[out]  bodies_not_held  4.3.3 (build-plan.md's 4.3 row,
    ///              docs/findings.md's F-225): set true, when provided, if
    ///              this call failed specifically because the containing
    ///              block's bodies are not held by this node (see the
    ///              existing HaveBodies check below) rather than any other
    ///              reason (no index entry, I/O error). Left untouched
    ///              (false) on every other path.
    /// @return  true if transaction is found, false otherwise
    bool FindTx(const uint256 &tx_hash, uint256 &block_hash, CTransactionRef &tx,
               bool *bodies_not_held = nullptr) const;

    bool HasTx(const uint256 &tx_hash) const;
};

/// The global transaction index, used in GetTransaction. May be null.
extern std::unique_ptr <TxIndex> g_txindex;

#endif // BITCOIN_INDEX_TXINDEX_H
