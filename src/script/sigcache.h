// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SCRIPT_SIGCACHE_H
#define BITCOIN_SCRIPT_SIGCACHE_H

#include <script/interpreter.h>

#include <vector>

// DoS prevention: limit cache size to 256MB (over 8,000,000 entries on
// 64-bit systems). Due to how we count cache size, actual memory usage is
// slightly more.
//
// 4.5.1 (F-199): raised from upstream's 32 MB. This budget splits in half
// between signatureCache and scriptExecutionCache (script/sigcache.cpp,
// validation.cpp); scriptExecutionCache holds one ~32-byte CuckooCache entry
// per TRANSACTION (validation.cpp:1616-1623's keying, confirmed by F-197's
// own re-derivation). At 32 MB total the 16 MB scriptExecutionCache half
// held 524,288 entries -- only ~2.1 of this branch's own 250,000-tx
// design-point blocks (transaction-decoupling.md:478-480's "2.1 blocks at
// 8 MB" figure). 256 MB (8x) gives a 128 MB half, 4,194,304 entries, ~16.8
// design-point blocks -- comfortable margin for cuckoocache.h's epoch-based
// eviction (a 3-epoch scheme targeting ~90% load, recency-preferring but not
// exact FIFO/LRU, so it needs safety margin rather than sizing flush against
// the bare block count) without spending meaningfully more than 1.6% of
// MAX_MAX_SIG_CACHE_SIZE's own headroom below.
static const unsigned int DEFAULT_MAX_SIG_CACHE_SIZE = 256;
// Maximum sig cache size allowed
static const int64_t MAX_MAX_SIG_CACHE_SIZE = 16384;

class CPubKey;

/**
 * We're hashing a nonce into the entries themselves, so we don't need extra
 * blinding in the set hash computation.
 *
 * This may exhibit platform endian dependent behavior but because these are
 * nonced hashes (random) and this state is only ever used locally it is safe.
 * All that matters is local consistency.
 */
class SignatureCacheHasher {
public:
    template<uint8_t hash_select>
    uint32_t operator()(const uint256 &key) const {
        static_assert(hash_select < 8, "SignatureCacheHasher only has 8 hashes available.");
        uint32_t u;
        std::memcpy(&u, key.begin() + 4 * hash_select, 4);
        return u;
    }
};

class CachingTransactionSignatureChecker : public TransactionSignatureChecker {
private:
    bool store;

public:
    CachingTransactionSignatureChecker(const CTransaction *txToIn, unsigned int nInIn, const CAmount &amount,
                                       PrecomputedTransactionData &txdataIn, bool storeIn = true)
            : TransactionSignatureChecker(txToIn, nInIn, amount, txdataIn), store(storeIn) {}

    bool VerifySignature(const std::vector<unsigned char> &vchSig, const CPubKey &vchPubKey,
                         const uint256 &sighash) const override;
};

/** Test-only: skip ECDSA verification entirely (-perfskipsigs).
 *
 * Measures an upper bound on what any scheme that removes per-input signature
 * checking from AcceptToMemoryPool could save. Never set outside a performance
 * rig: with this on the node accepts transactions with invalid signatures.
 */
extern bool g_perf_skip_sigs;

void InitSignatureCache();

#endif // BITCOIN_SCRIPT_SIGCACHE_H
