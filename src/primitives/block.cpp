// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2020-2023 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/merkle.h>
#include <hash.h>
#include <primitives/block.h>
#include <primitives/powcache.h>
#include <sync.h>
#include <uint256.h>
#include <util/system.h>
#include <util/time.h>
#include <util/strencodings.h>
#include <logging.h>

#define BEGIN(a) ((char*)&(a))
#define END(a) ((char*)&((&(a))[1]))

uint256 CBlockHeader::GetHash() const {
    return SerializeHash(*this);
}

uint256 CBlockHeader::ComputeHash() const {
    return HashGR(BEGIN(nVersion), END(nNonce), hashPrevBlock);
}

uint256 CBlockHeader::GetPOWHash(bool readCache) const {
    LOCK(cs_pow);
    CPowCache &cache(CPowCache::Instance());

    uint256 headerHash = GetHash();
    uint256 powHash;
    bool found = false;

    if (readCache) {
        found = cache.get(headerHash, powHash);
    }

    if (!found || cache.IsValidate()) {
        uint256 powHash2 = ComputeHash();
        if (found && powHash2 != powHash) {
            LogPrintf("PowCache failure: headerHash: %s, from cache: %s, computed: %s, correcting\n",
                      headerHash.ToString(), powHash.ToString(), powHash2.ToString());
        }
        powHash = powHash2;
        cache.erase(headerHash); // If it exists, replace it
        cache.insert(headerHash, powHash2);
    }
    return powHash;
}

std::string CBlock::ToString() const {
    std::stringstream s;
    s << strprintf(
            "CBlock(hash=%s, ver=0x%08x, hashPrevBlock=%s, hashMerkleRoot=%s, nTime=%u, nBits=%08x, nNonce=%u, vtx=%u)\n",
            GetHash().ToString(),
            nVersion,
            hashPrevBlock.ToString(),
            hashMerkleRoot.ToString(),
            nTime, nBits, nNonce,
            vtx.size());
    for (const auto &tx: vtx) {
        s << "  " << tx->ToString() << "\n";
    }
    return s.str();
}

std::vector <uint256> CCommitmentBlock::Identifiers() const {
    std::vector <uint256> ids;
    if (!coinbase) {
        return ids;
    }
    ids.reserve(1 + vCommitments.size());
    ids.push_back(coinbase->GetHash());
    ids.insert(ids.end(), vCommitments.begin(), vCommitments.end());
    return ids;
}

uint256 CCommitmentBlock::ComputeMerkleRoot(bool *mutated) const {
    return ::ComputeMerkleRoot(Identifiers(), mutated);
}

bool CCommitmentBlock::HasDuplicateIdentifiers() const {
    // Deliberately a set and not the merkle tree's `mutated` flag. That flag
    // compares hashes[pos] with hashes[pos+1] for even pos only, so a repeat at an
    // odd boundary is never looked at -- it survives every check the tree performs.
    const std::vector <uint256> ids = Identifiers();
    std::set <uint256> seen(ids.begin(), ids.end());
    return seen.size() != ids.size();
}

std::string CCommitmentBlock::ToString() const {
    std::stringstream s;
    s << strprintf("CCommitmentBlock(hash=%s, hashMerkleRoot=%s, commits=%u)\n",
                   GetHash().ToString(), hashMerkleRoot.ToString(), CommittedCount());
    if (coinbase) {
        s << "  coinbase " << coinbase->ToString() << "\n";
    }
    for (const auto &id: vCommitments) {
        s << "  " << id.ToString() << "\n";
    }
    return s.str();
}

CCommitmentBlock CommitmentsFromBlock(const CBlock &block) {
    CCommitmentBlock c;
    *(static_cast<CBlockHeader *>(&c)) = block.GetBlockHeader();
    if (block.vtx.empty()) {
        return c;
    }
    c.coinbase = block.vtx[0];
    c.vCommitments.reserve(block.vtx.size() - 1);
    for (size_t i = 1; i < block.vtx.size(); i++) {
        c.vCommitments.push_back(block.vtx[i]->GetHash());
    }
    return c;
}

bool MaterialiseBlock(const CCommitmentBlock &commitments,
                      const std::vector <CTransactionRef> &bodies,
                      CBlock &blockOut) {
    if (!commitments.coinbase || bodies.size() != commitments.vCommitments.size()) {
        return false;
    }
    // A fresh object, so fChecked is false. CheckBlock returns early on fChecked,
    // and a block that carried it over from the commitment-only pass would skip the
    // very checks the bodies were fetched for.
    blockOut = CBlock();
    *(static_cast<CBlockHeader *>(&blockOut)) = *(static_cast<const CBlockHeader *>(&commitments));
    blockOut.vtx.reserve(1 + bodies.size());
    blockOut.vtx.push_back(commitments.coinbase);
    for (size_t i = 0; i < bodies.size(); i++) {
        if (!bodies[i] || bodies[i]->GetHash() != commitments.vCommitments[i]) {
            blockOut.SetNull();
            return false;
        }
        blockOut.vtx.push_back(bodies[i]);
    }
    // C4 (Fable review, F-121, 2026-09-19): the assert this replaced
    // (!blockOut.fChecked) was tautological -- blockOut is a freshly
    // constructed CBlock() a few lines above, whose constructor already sets
    // fChecked = false, and nothing between there and here touches it.
    return true;
}
