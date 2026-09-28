// Copyright (c) 2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BLOCKENCODINGS_H
#define BITCOIN_BLOCKENCODINGS_H

#include <consensus/consensus.h>
#include <primitives/block.h>

class CTxMemPool;

// Transaction compression schemes for compact block relay
// can be introduced by writing an actual formatter here.
using TransactionCompression = DefaultFormatter;

class DifferenceFormatter {
    uint64_t m_shift = 0;

public:
    template<typename Stream, typename I>
    void Ser(Stream &s, I v) {
        if (v < m_shift || v >= std::numeric_limits<uint64_t>::max())
            throw std::ios_base::failure("differential value overflow");
        WriteCompactSize(s, v - m_shift);
        m_shift = uint64_t(v) + 1;
    }

    template<typename Stream, typename I>
    void Unser(Stream &s, I &v) {
        uint64_t n = ReadCompactSize(s);
        m_shift += n;
        if (m_shift < n || m_shift >= std::numeric_limits<uint64_t>::max() || m_shift < std::numeric_limits<I>::min() ||
            m_shift > std::numeric_limits<I>::max())
            throw std::ios_base::failure("differential value overflow");
        v = I(m_shift++);
    }
};

class BlockTransactionsRequest {
public:
    // A BlockTransactionsRequest message
    uint256 blockhash;
    // F-200 (4.5.2 Part A): widened from uint16_t (max 65,535) -- this
    // branch's own MAX_DIP0001_BLOCK_SIZE (8,000,000, consensus/consensus.h)
    // needs up to 250,000 identifiers, which overflows a uint16_t outright.
    // Wire-format-neutral: DifferenceFormatter serializes via
    // WriteCompactSize/ReadCompactSize (variable-length CompactSize, not a
    // fixed-width field), so this only relaxes the C++-side bound, exactly
    // like PrefilledTransaction::index below.
    std::vector <uint32_t> indexes;

    SERIALIZE_METHODS(BlockTransactionsRequest, obj
    )
    {
        READWRITE(obj.blockhash, Using < VectorFormatter < DifferenceFormatter >> (obj.indexes));
    }
};

class BlockTransactions {
public:
    // A BlockTransactions message
    uint256 blockhash;
    std::vector <CTransactionRef> txn;

    BlockTransactions() {}

    explicit BlockTransactions(const BlockTransactionsRequest &req) :
            blockhash(req.blockhash), txn(req.indexes.size()) {}

    SERIALIZE_METHODS(BlockTransactions, obj
    )
    {
        READWRITE(obj.blockhash, Using < VectorFormatter < TransactionCompression >> (obj.txn));
    }
};

/** F-201 (4.5.2 Part B): whether a BLOCKTXN response summing to
 *  nTotalSerializedSize bytes should be declined in favour of a full-block
 *  fallback, given a size ceiling. SendBlockTransactions (net_processing.cpp)
 *  currently builds every requested transaction into ONE BlockTransactions
 *  object with zero size checking -- at design throughput (~373 B/tx
 *  average, F-30), as few as ~8,000 requested transactions already exceeds
 *  MAX_PROTOCOL_MESSAGE_LENGTH (3 MB, net.h) and gets this node disconnected
 *  by the peer it's replying to (net.cpp's oversized-message check). Kept as
 *  a pure predicate, separate from SendBlockTransactions's CNode/CConnman
 *  side effects, so the decision itself is unit-testable without a network
 *  harness -- same shape as bodyrange.h's ShouldDisconnectForBodyRangeAttempts. */
bool ShouldDeclineBlockTransactionsForSize(uint64_t nTotalSerializedSize, uint64_t nSizeCeiling);

// Dumb serialization/storage-helper for CBlockHeaderAndShortTxIDs and PartiallyDownloadedBlock
struct PrefilledTransaction {
    // Used as an offset since last prefilled tx in CBlockHeaderAndShortTxIDs,
    // as a proper transaction-in-block-index in PartiallyDownloadedBlock.
    // F-200 (4.5.2 Part A): widened from uint16_t, see BlockTransactionsRequest::indexes
    // above for why. Serialized via COMPACTSIZE (CompactSizeFormatter), an
    // already variable-length wire encoding -- confirmed by direct read of
    // serialize.h's CompactSizeFormatter::Ser/Unser -- so widening this
    // field's own C++ type does not change what goes on the wire for any
    // value that already fit in a uint16_t; it only relaxes the acceptable
    // upper bound.
    uint32_t index;
    CTransactionRef tx;

    SERIALIZE_METHODS(PrefilledTransaction, obj) {
        READWRITE(COMPACTSIZE(obj.index), Using<TransactionCompression>(obj.tx));
    }
};

typedef enum ReadStatus_t {
    READ_STATUS_OK,
    READ_STATUS_INVALID, // Invalid object, peer is sending bogus crap
    READ_STATUS_FAILED, // Failed to process object
    READ_STATUS_CHECKBLOCK_FAILED, // Used only by FillBlock to indicate a
    // failure in CheckBlock.
} ReadStatus;

class CBlockHeaderAndShortTxIDs {
private:
    mutable uint64_t shorttxidk0, shorttxidk1;
    uint64_t nonce;

    void FillShortTxIDSelector() const;

    friend class PartiallyDownloadedBlock;

protected:
    std::vector <uint64_t> shorttxids;
    std::vector <PrefilledTransaction> prefilledtxn;

public:
    static constexpr int SHORTTXIDS_LENGTH = 6;
    CBlockHeader header;

    // Dummy for deserialization
    CBlockHeaderAndShortTxIDs() {}

    CBlockHeaderAndShortTxIDs(const CBlock &block);

    uint64_t GetShortID(const uint256 &txhash) const;

    size_t BlockTxCount() const { return shorttxids.size() + prefilledtxn.size(); }

    SERIALIZE_METHODS(CBlockHeaderAndShortTxIDs, obj
    )
    {
        READWRITE(obj.header, obj.nonce, Using < VectorFormatter < CustomUintFormatter <
                                         SHORTTXIDS_LENGTH>>>(obj.shorttxids), obj.prefilledtxn);
        if (ser_action.ForRead()) {
            // F-200 (4.5.2 Part A): bound to the design's own already-decided
            // consensus cap (COMMITMENT_BUDGET_MAX_INPUTS, D-19, 1.2/F-98-F-99),
            // not to the widened index type's own incidental max. A bare
            // "does it fit in uint32_t" check here would silently readmit the
            // exact "fits because the TYPE happens to be big enough" hazard
            // this widening is supposed to close (F-197 item 4), just moved to
            // a threshold nobody chose on purpose.
            //
            // F-202 (4.5.2 fix, review): +1, not bare COMMITMENT_BUDGET_MAX_
            // INPUTS -- that constant bounds aggregate NON-coinbase input
            // count (consensus/tx_verify.h's own GetBlockInputCount, D-19's
            // doc comment: "the coinbase's own dummy input is excluded"),
            // not total transaction count, but BlockTxCount() counts every
            // transaction, coinbase included. The real maximum is
            // COMMITMENT_BUDGET_MAX_INPUTS one-input, non-coinbase
            // transactions (the input-minimizing, tx-count-maximizing shape)
            // plus exactly one coinbase = COMMITMENT_BUDGET_MAX_INPUTS + 1
            // total transactions. The bare (no +1) bound rejected exactly
            // that maximal block, one short of the true limit, disagreeing
            // by one with PartiallyDownloadedBlock::InitData's own
            // candidateindex bound just below (blockencodings.cpp), which
            // already correctly permits a 0-based index up to
            // COMMITMENT_BUDGET_MAX_INPUTS -- the (COMMITMENT_BUDGET_MAX_
            // INPUTS + 1)-th transaction.
            if (obj.BlockTxCount() > COMMITMENT_BUDGET_MAX_INPUTS + 1) {
                throw std::ios_base::failure("indexes overflowed the commitment input budget");
            }
            obj.FillShortTxIDSelector();
        }
    }
};

class PartiallyDownloadedBlock {
protected:
    std::vector <CTransactionRef> txn_available;
    size_t prefilled_count = 0, mempool_count = 0, extra_count = 0;
    const CTxMemPool *pool;
public:
    CBlockHeader header;

    explicit PartiallyDownloadedBlock(CTxMemPool *poolIn) : pool(poolIn) {}

    // extra_txn is a list of extra transactions to look at, in <hash, reference> form
    ReadStatus InitData(const CBlockHeaderAndShortTxIDs &cmpctblock,
                        const std::vector <std::pair<uint256, CTransactionRef>> &extra_txn);

    bool IsTxAvailable(size_t index) const;

    ReadStatus FillBlock(CBlock &block, const std::vector <CTransactionRef> &vtx_missing);
};

#endif // BITCOIN_BLOCKENCODINGS_H
