// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2020-2023 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PRIMITIVES_BLOCK_H
#define BITCOIN_PRIMITIVES_BLOCK_H

#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>
#include <unordered_lru_cache.h>


/** Nodes collect new transactions into a block, hash them into a hash tree,
 * and scan through nonce values to make the block's hash satisfy proof-of-work
 * requirements.  When they solve the proof-of-work, they broadcast the block
 * to everyone and the block is added to the block chain.  The first transaction
 * in the block is a special one that creates a new coin owned by the creator
 * of the block.
 */
class CBlockHeader {
public:
    // header
    int32_t nVersion;

    uint256 hashPrevBlock;
    uint256 hashMerkleRoot;
    uint32_t nTime;
    uint32_t nBits;
    uint32_t nNonce;

    CBlockHeader() {
        SetNull();
    }

    SERIALIZE_METHODS(CBlockHeader, obj
    ) { READWRITE(obj.nVersion, obj.hashPrevBlock, obj.hashMerkleRoot, obj.nTime, obj.nBits, obj.nNonce); }

    void SetNull() {
        nVersion = 0;
        hashPrevBlock.SetNull();
        hashMerkleRoot.SetNull();
        nTime = 0;
        nBits = 0;
        nNonce = 0;
    }

    bool IsNull() const {
        return (nBits == 0);
    }

    /// Compute the Header Hash from the block
    uint256 GetHash() const;

    /// Compute the POW hash using GhostRider algorithm
    uint256 ComputeHash() const;

    /// Caching lookup/computation of POW hash using GhostRider algorithm
    uint256 GetPOWHash(bool readCache = true) const;

    int64_t GetBlockTime() const {
        return (int64_t) nTime;
    }
};


class CBlock : public CBlockHeader {
public:
    // network and disk
    std::vector <CTransactionRef> vtx;

    mutable CTxOut txoutFounder; // founder payment
    // memory only
    mutable bool fChecked;

    CBlock() {
        SetNull();
    }

    CBlock(const CBlockHeader &header) {
        SetNull();
        *(static_cast<CBlockHeader *>(this)) = header;
    }

    SERIALIZE_METHODS(CBlock, obj
    )
    {
        READWRITEAS(CBlockHeader, obj);
        READWRITE(obj.vtx);
    }

    void SetNull() {
        CBlockHeader::SetNull();
        vtx.clear();
        fChecked = false;
        txoutFounder = CTxOut();
    }

    CBlockHeader GetBlockHeader() const {
        CBlockHeader block;
        block.nVersion = nVersion;
        block.hashPrevBlock = hashPrevBlock;
        block.hashMerkleRoot = hashMerkleRoot;
        block.nTime = nTime;
        block.nBits = nBits;
        block.nNonce = nNonce;
        return block;
    }

    std::string ToString() const;
};


/**
 * A block as it travels under transaction decoupling: the header, the coinbase in
 * full, and one 32-byte identifier for each remaining transaction, in block order.
 *
 * The coinbase is carried whole because a block cannot be judged without it -- the
 * founder payment, the CbTx type and the height all live there -- and because
 * carrying it means the merkle root binds it automatically. If instead the list
 * held an identifier for the coinbase too, a separate rule would be needed to tie
 * the carried transaction to that identifier; this way there is nothing to tie.
 *
 * What this form CAN certify on its own is the commitment-checkable half of
 * BLOCK_VALID_TRANSACTIONS: proof of work, the merkle root, size, the coinbase in
 * every respect. What it cannot is anything about the transactions it only names.
 */
class CCommitmentBlock : public CBlockHeader {
public:
    CTransactionRef coinbase;
    std::vector <uint256> vCommitments;   //!< identifiers for vtx[1..], in block order

    CCommitmentBlock() { SetNull(); }

    SERIALIZE_METHODS(CCommitmentBlock, obj
    )
    {
        READWRITEAS(CBlockHeader, obj);
        READWRITE(obj.coinbase);
        READWRITE(obj.vCommitments);
    }

    void SetNull() {
        CBlockHeader::SetNull();
        coinbase.reset();
        vCommitments.clear();
    }

    bool IsNull() const { return coinbase == nullptr; }

    //! How many transactions the block commits to, coinbase included.
    size_t CommittedCount() const { return coinbase ? 1 + vCommitments.size() : 0; }

    //! The merkle leaves, in block order: the coinbase's own hash, then the identifiers.
    std::vector <uint256> Identifiers() const;

    //! Recompute the merkle root from the identifiers alone. Equals
    //! BlockMerkleRoot() of the block this was built from.
    uint256 ComputeMerkleRoot(bool *mutated = nullptr) const;

    //! Do the identifiers repeat? The merkle tree cannot answer this -- it compares
    //! hashes only at even positions, so a duplicate at an odd boundary passes --
    //! and the answer matters because a block naming the same transaction twice is
    //! not a block anyone can fill.
    bool HasDuplicateIdentifiers() const;

    std::string ToString() const;
};

/** Build the commitment form of a full block. */
CCommitmentBlock CommitmentsFromBlock(const CBlock &block);

/**
 * Rebuild a full block from its commitments and the bodies it names.
 *
 * `bodies` must be the transactions for vCommitments, in the same order. Returns
 * false if any identifier does not match, which is the only thing a node has to go
 * on when a peer answers a body request.
 *
 * The result is a FRESH CBlock, so fChecked is false by construction. That matters:
 * CheckBlock returns early on fChecked, and a materialised block that inherited the
 * flag from its commitment-only pass would never have its bodies checked at all.
 */
bool MaterialiseBlock(const CCommitmentBlock &commitments,
                      const std::vector <CTransactionRef> &bodies,
                      CBlock &blockOut);

/** Describes a place in the block chain to another node such that if the
 * other node doesn't have the same branch, it can find a recent common trunk.
 * The further back it is, the further before the fork it may be.
 */
struct CBlockLocator {
    std::vector <uint256> vHave;

    CBlockLocator() {}

    explicit CBlockLocator(const std::vector <uint256> &vHaveIn) : vHave(vHaveIn) {}

    SERIALIZE_METHODS(CBlockLocator, obj
    )
    {
        int nVersion = s.GetVersion();
        if (!(s.GetType() & SER_GETHASH))
            READWRITE(nVersion);
        READWRITE(obj.vHave);
    }

    void SetNull() {
        vHave.clear();
    }

    bool IsNull() const {
        return vHave.empty();
    }
};

#endif // BITCOIN_PRIMITIVES_BLOCK_H
