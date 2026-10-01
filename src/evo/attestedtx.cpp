// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/attestedtx.h>

#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/validation.h>
#include <evo/specialtx.h>
#include <hash.h>
#include <llmq/quorums_signing.h>
#include <script/standard.h>
#include <streams.h>
#include <sync.h>

#include <algorithm>
#include <deque>
#include <set>

/** build-plan.md's own 5.4 row, interim decision (a): restricted to
 *  single-signature (P2PK/P2PKH) inputs only until non-standard-script
 *  handling is actually designed -- rejecting anything else at the type
 *  level rather than silently under-checking it. `Solver` is the existing,
 *  already-tested script classifier (script/standard.h/.cpp) reused here,
 *  not reimplemented. */
static bool IsRestrictedSingleSigScript(const CScript &scriptPubKey) {
    std::vector <std::vector<unsigned char>> solutions;
    txnouttype type = Solver(scriptPubKey, solutions);
    return type == TX_PUBKEY || type == TX_PUBKEYHASH;
}

/** 5.4.2: mirrors CLSIG_REQUESTID_PREFIX's own established shape exactly
 *  (llmq/quorums_chainlocks.cpp, "clsig", domain-separating a chainlock's
 *  own signing session from anything else that might share a quorum type)
 *  -- a different prefix here means an attestation signature can never be
 *  replayed as, or confused with, a chainlock or islock signature over the
 *  coincidentally-identical hash of some other object, even though this
 *  type currently reuses ChainLocks' own llmqType (see this file's own
 *  header doc comment for why that reuse, not a new quorum type, is v1's
 *  own interim choice). */
static const std::string ATTESTATION_REQUESTID_PREFIX = "atx";

/** Fable review (2026-10-01), LOW, fixed: a v2 batch of exactly one
 *  transaction has a Merkle root equal to that transaction's own message
 *  hash (CPartialMerkleTree's own height-0 case: the root IS the leaf) --
 *  without a separate prefix here, BuildAttestationId(root) would collide
 *  with BuildAttestationId(msgHash) for that one transaction, aliasing a
 *  v1 id and a v2 batch id onto the same lookup key. Domain-separated the
 *  same way "atx" already separates attestation ids from clsig/islock/
 *  inlock ids. */
static const std::string ATTESTATION_BATCH_REQUESTID_PREFIX = "atxb";

/** Fable review (2026-09-30), CONFIRMED HIGH, fixed here: the attested
 *  message cannot be tx.GetHash() itself. CTransaction::Serialize writes
 *  vExtraPayload unconditionally whenever nType != TRANSACTION_NORMAL
 *  (primitives/transaction.h) with no SER_GETHASH exclusion at that layer,
 *  and vExtraPayload IS this type's own CAttestationPayload -- which
 *  carries the very signature being verified. tx.GetHash() therefore
 *  depends on payload.sig, and a signer cannot choose a signature whose
 *  value determines the message it is supposed to be a signature over.
 *  No valid attestation could ever have been constructed against the
 *  unfixed version; this is why CProRegTx/CProUpServTx (evo/providertx.h)
 *  exclude their own signature field under SER_GETHASH -- but that guard
 *  only matters when the PAYLOAD OBJECT is itself the thing being
 *  SerializeHash'd. Here the payload is pre-flattened into opaque
 *  vExtraPayload bytes before the OUTER transaction is hashed, so a guard
 *  inside CAttestationPayload's own SERIALIZE_METHODS would never be
 *  consulted -- the fix has to strip vExtraPayload from a COPY of the
 *  transaction before hashing, at the outer layer, not add a guard the
 *  outer layer never triggers. */
uint256 ComputeAttestedMessageHash(const CTransaction &tx) {
    CMutableTransaction stripped(tx);
    stripped.vExtraPayload.clear();
    return ::SerializeHash(CTransaction(stripped));
}

/** id/msgHash are both derived from the payload-stripped transaction hash
 *  above, never carried on the wire (CAttestationPayload's own doc
 *  comment explains why nSignHeight, the one field that IS carried, is
 *  carried) -- id additionally mixes in the domain-separation prefix
 *  above so a signature produced for this purpose cannot be replayed as
 *  one of a different kind sharing the same quorum. */
static uint256 BuildAttestationId(const uint256 &strippedTxHash) {
    return ::SerializeHash(std::make_pair(ATTESTATION_REQUESTID_PREFIX, strippedTxHash));
}

uint256 BuildAttestationBatchId(const uint256 &batchRoot) {
    return ::SerializeHash(std::make_pair(ATTESTATION_BATCH_REQUESTID_PREFIX, batchRoot));
}

/** Fable review (2026-10-01), CONFIRMED CRITICAL, fixed: a per-NODE cache
 *  of (batchRoot, nSignHeight, sig) triples this node has already verified
 *  via a real VerifyRecoveredSig call -- trí's own throughput argument
 *  (verify a batch's signature once, not once per transaction sharing it)
 *  realized correctly, per validating node, rather than the unsound
 *  "verify once anywhere, trust everywhere" design this file originally
 *  shipped with (looking the signature up in a shared store instead of
 *  carrying it -- see CAttestationPayload's own `sig` doc comment,
 *  evo/attestedtx.h, for why that store is not a general network-
 *  propagation layer). Keyed on the full triple, not just the root: a
 *  transaction carrying a forged signature over a genuinely-batched root
 *  must still fail its OWN verification, never riding on an earlier,
 *  real entry's cache hit for the same root. Bounded FIFO and process-
 *  lifetime only (not persisted) -- 5.4.4.2 has not yet decided how large
 *  a real batch-signing interval's own working set gets, and this is
 *  reachable from multiple threads (parallel script checking,
 *  g_parallel_script_checks), hence the lock. */
static const size_t MAX_VERIFIED_BATCH_CACHE_ENTRIES = 10000;
static RecursiveMutex cs_verifiedBatchCache;
static std::set <uint256> g_verifiedBatchCache GUARDED_BY(cs_verifiedBatchCache);
static std::deque <uint256> g_verifiedBatchCacheOrder GUARDED_BY(cs_verifiedBatchCache);

static uint256 ComputeBatchCacheKey(const uint256 &batchRoot, int32_t nSignHeight, const CBLSSignature &sig) {
    CHashWriter ss(SER_GETHASH, 0);
    ss << batchRoot << nSignHeight << sig;
    return ss.GetHash();
}

static bool IsKnownVerifiedBatch(const uint256 &cacheKey) {
    LOCK(cs_verifiedBatchCache);
    return g_verifiedBatchCache.count(cacheKey) != 0;
}

static void RememberVerifiedBatch(const uint256 &cacheKey) {
    LOCK(cs_verifiedBatchCache);
    if (g_verifiedBatchCache.insert(cacheKey).second) {
        g_verifiedBatchCacheOrder.push_back(cacheKey);
        if (g_verifiedBatchCacheOrder.size() > MAX_VERIFIED_BATCH_CACHE_ENTRIES) {
            g_verifiedBatchCache.erase(g_verifiedBatchCacheOrder.front());
            g_verifiedBatchCacheOrder.pop_front();
        }
    }
}

static bool IsPlausibleAttestationSignHeight(int32_t nSignHeight, const CBlockIndex *pindexPrev) {
    if (nSignHeight < 0 || pindexPrev == nullptr) {
        return false;
    }
    if (nSignHeight > pindexPrev->nHeight) {
        return false;
    }
    return pindexPrev->nHeight - nSignHeight <= MAX_ATTESTATION_SIGN_HEIGHT_AGE;
}

/** 5.4.4.1 (build-plan.md, F-243): the quorum-free half of v2 verification,
 *  factored out so it can be held to a direct test without needing a real
 *  recovered signature to exist anywhere (CAttestationPayload's own `sig`
 *  doc comment explains why that piece cannot be tested live in this
 *  environment, same as v1's VerifyRecoveredSig call never could be).
 *  `proof` is expected to match EXACTLY one leaf -- an attested transaction's
 *  own payload proves only itself, never a set of other transactions too --
 *  and that one leaf must equal `expectedLeaf` (this transaction's own
 *  ComputeAttestedMessageHash), or the proof is for some OTHER transaction
 *  entirely, carried (maliciously or by a packaging mistake) onto this one's
 *  payload. CPartialMerkleTree::ExtractMatches (merkleblock.h) itself
 *  returns a null root on any structurally malformed proof -- confirmed by
 *  reading its own implementation before relying on that contract, not
 *  assumed. The `matchedHashes.size() != 1` check is not just a logic
 *  nicety: it guards a genuine out-of-bounds `matchedHashes[0]` read on a
 *  zero-match proof. Fable review (2026-10-01), CONFIRMED MEDIUM, fixed:
 *  this file's own first attempt at proving that live mutation-tested the
 *  WRONG scenario -- a two-match proof (both matches equal to
 *  `expectedLeaf`) still crashed when the check was removed, but via a
 *  different, unrelated mechanism (falling through into
 *  CSigningManager::VerifyRecoveredSig, which needs ChainstateActive() and
 *  therefore a chainstate-manager-initializing fixture, not
 *  BasicTestingSetup -- the same class of fixture-dependent fragility
 *  F-241's own mutant 2 already found once) -- not the empty-vector read
 *  this comment originally, wrongly, credited it to. The genuine
 *  zero-match case is covered by its own dedicated test now
 *  (attested_tx_rejects_a_v2_payload_matching_zero_leaves,
 *  test/attestedtx_tests.cpp), under TestingSetup so a removed check fails
 *  by assertion, not by crash, making the mutant's own failure mode
 *  unambiguous. */
static bool ExtractAttestedBatchRoot(const CPartialMerkleTree &proof, const uint256 &expectedLeaf, uint256 &retRoot) {
    std::vector <uint256> matchedHashes;
    std::vector<unsigned int> matchedIndices;
    CPartialMerkleTree proofCopy(proof);
    uint256 root = proofCopy.ExtractMatches(matchedHashes, matchedIndices);
    if (root.IsNull() || matchedHashes.size() != 1) {
        return false;
    }
    if (matchedHashes[0] != expectedLeaf) {
        return false;
    }
    retRoot = root;
    return true;
}

/** 5.4.4.2 (build-plan.md, F-245), factored out of CheckAttestedTx: the
 *  structural, payload-independent checks -- type, coinbase, non-empty,
 *  per-input confirmed/script-shape. Shared with
 *  CAttestationBatchHandler::RequestAttestation (llmq/quorums_attestationbatch.cpp)
 *  so a request for a transaction that could never pass CheckAttestedTx
 *  anyway is rejected before ever wasting a quorum's own signing effort --
 *  one copy, not two that could silently diverge (this project's own
 *  established anti-pattern, F-95's own sigop-counter lesson for the same
 *  class of risk). Deliberately does NOT include the per-input SIGNATURE
 *  check (CScriptCheck) -- that is RequestAttestation's own, separate job,
 *  run once at request time, never inside this function: CheckAttestedTx
 *  itself runs unconditionally on every validating node regardless of the
 *  5.4.3 CheckInputs-skip, so putting a real script check HERE would pay
 *  the full cost on every node every time, defeating the skip's entire
 *  purpose. See CAttestationBatchHandler::RequestAttestation's own doc
 *  comment for the CRITICAL this split fixes (Fable review, 2026-10-01):
 *  without a script check anywhere in the request-to-verification chain,
 *  anyone who could get a hash into a signed batch could spend any
 *  single-sig UTXO with no valid signature at all. */
bool CheckAttestedTxInputShapes(const CTransaction &tx, CValidationState &state, const CCoinsViewCache &view) {
    if (tx.IsCoinBase()) {
        // Consensus::CheckTxInputs' own value-conservation/double-spend
        // checks (this file's own doc comment explains why they are not
        // duplicated here) have nothing to apply to a coinbase, and the
        // script-shape restriction below is meaningless for an input with
        // no real prevout -- CheckSpecialTx never reaches nType-specific
        // dispatch for a coinbase transaction today (TRANSACTION_COINBASE
        // is its own, separate type), so this is unreachable in practice;
        // refused explicitly rather than assumed unreachable forever.
        return state.DoS(100, false, REJECT_INVALID, "bad-attested-tx-coinbase");
    }

    if (tx.vin.empty()) {
        return state.DoS(100, false, REJECT_INVALID, "bad-attested-tx-no-inputs");
    }

    // Fable review (2026-09-30), CONFIRMED MEDIUM, fixed: this loop's own
    // Coin lookup is NOT redundant with Consensus::CheckTxInputs the way
    // the comment below used to claim -- ATMP passes CoinsTip() to
    // CheckSpecialTx (validation.cpp), deliberately mempool-blind,
    // while CheckTxInputs itself ran against the mempool-backed view; in
    // ConnectBlock, ProcessSpecialTxsInBlock runs BEFORE the per-tx loop's
    // own CheckTxInputs/UpdateCoins, not after. Spending an unconfirmed
    // parent -- an attested transaction chained off another one still in
    // the mempool -- genuinely reaches this branch; it is not a "something
    // else already broke" signal. Treated as a real, if narrow, consensus
    // restriction (attested inputs must already be confirmed) rather than
    // an impossible case, matching this file's own single-signature
    // restriction in kind: a deliberate v1 narrowing, named as one.
    for (const CTxIn &txin: tx.vin) {
        const Coin &coin = view.AccessCoin(txin.prevout);
        if (coin.IsSpent()) {
            return state.DoS(10, false, REJECT_INVALID, "bad-attested-tx-unconfirmed-input", false,
                             "TRANSACTION_ATTESTED inputs must already be confirmed (5.4.1)");
        }
        if (!IsRestrictedSingleSigScript(coin.out.scriptPubKey)) {
            // Fable review (2026-09-30), LOW, fixed: this rule is enforced
            // inside ConnectBlock (consensus), not mempool policy --
            // REJECT_NONSTANDARD is policy vocabulary (BIP61 messages, log
            // triage) and misleading here.
            return state.DoS(10, false, REJECT_INVALID, "bad-attested-tx-nonstandard-input", false,
                             "TRANSACTION_ATTESTED is restricted to single-signature inputs (5.4.1)");
        }
    }
    return true;
}

bool CheckAttestedTx(const CTransaction &tx, const CBlockIndex *pindexPrev, CValidationState &state,
                     const CCoinsViewCache &view, bool check_sigs) {
    if (!IsAttestedTx(tx)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-attested-tx-type");
    }

    if (!CheckAttestedTxInputShapes(tx, state, view)) {
        return false; // state filled in by CheckAttestedTxInputShapes
    }

    // Fable review (2026-09-30), CONFIRMED MEDIUM, fixed: payload presence
    // and version are structural rules, not signature verification --
    // every sibling CheckXxxTx function (CheckProRegTx et al.) parses and
    // validates its own payload unconditionally and gates ONLY the
    // signature check on check_sigs. The previous version of this function
    // returned true here before ever calling GetTxPayload, so a
    // completely payload-less attested transaction could pass under
    // assumevalid/RollforwardBlock's check_sigs=false while a fully-
    // validating node would reject it -- moved below the structural
    // checks to match the real sibling convention this file's own
    // comment already claimed to follow.
    CAttestationPayload payload;
    if (!GetTxPayload(tx, payload)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-attested-tx-payload");
    }
    if (payload.nVersion == 0 || payload.nVersion > CAttestationPayload::CURRENT_VERSION) {
        return state.DoS(100, false, REJECT_INVALID, "bad-attested-tx-payload-version");
    }
    // Fable review (2026-10-01), CONFIRMED MEDIUM, PARTIALLY addressed: a
    // third party can rewrite this transaction's own txid (vExtraPayload
    // is part of what CTransaction::GetHash() covers for any non-normal
    // nType) without touching anything the attestation covers, by
    // flipping an unused padding bit in CPartialMerkleTree's own flag-byte
    // encoding, or by claiming nTransactions = n+1 for an odd-leaf-count
    // proof -- both decode to the identical root and matched leaf
    // (merkleblock.cpp's own tolerance of this ambiguity, the
    // CVE-2012-2459 shape), confirmed live by the review's own PoC: three
    // distinct txids from the same logical attestation. This check (re-
    // serializing the successfully-parsed payload and requiring an exact
    // byte match, now GetTxPayload has already required full consumption
    // of tx.vExtraPayload, specialtx.h's own `ds.empty()` check) is a
    // real, general defence, but does NOT close the specific padding-bit
    // vector -- confirmed by direct testing, not assumed: CPartialMerkleTree's
    // own read path (BytesToBits, merkleblock.cpp) expands every byte into
    // vBits wholesale, including padding, and ExtractMatches's own
    // consistency check only compares BYTE COUNTS (ExtractMatches's own
    // `(nBitsUsed+7)/8 != (vBits.size()+7)/8`), never padding bit VALUES --
    // so a flipped padding bit is faithfully preserved through vBits and
    // re-emitted identically on the next write, round-tripping this check
    // undetected (an attempt at a regression test for exactly this, during
    // this same review response, could not be made to fail -- removed
    // rather than shipped as a false-positive "proof"). Nor does it close
    // the nTransactions ambiguity (both n and n+1 round-trip losslessly,
    // since nTransactions is itself an explicit, faithfully-preserved
    // field). Both remaining gaps need the same deeper fix: committing the
    // real batch size and a canonical bit-level encoding into what the
    // quorum actually signs, which is 5.4.4.2's own job (what the quorum
    // signs), not this verification-side file's -- or exposing/canonicalizing
    // CPartialMerkleTree's own vBits directly, a change to that existing,
    // shared class this sub-step does not make.
    {
        CDataStream ds(SER_NETWORK, PROTOCOL_VERSION);
        ds << payload;
        // memcmp, not std::equal: CDataStream's own element type is signed
        // `char` (CSerializeData, support/allocators/zeroafterfree.h) while
        // vExtraPayload is std::vector<unsigned char> -- std::equal across
        // the two promotes each element to int before comparing, so any
        // byte >= 0x80 (routine in real hash/signature data) sign-extends
        // differently on each side and never compares equal, producing
        // false rejections on every payload regardless of malleability
        // (confirmed live: all 5 existing tests failed here before this
        // fix). memcmp compares raw bytes, matching CBLSWrapper::
        // CheckMalleable's own established pattern for the identical kind
        // of check (bls/bls.h).
        if (ds.size() != tx.vExtraPayload.size() ||
            memcmp(ds.data(), tx.vExtraPayload.data(), ds.size()) != 0) {
            return state.DoS(100, false, REJECT_INVALID, "bad-attested-tx-payload-nonstandard-encoding");
        }
    }
    if (!IsPlausibleAttestationSignHeight(payload.nSignHeight, pindexPrev)) {
        return state.DoS(10, false, REJECT_INVALID, "bad-attested-tx-payload-height");
    }

    if (!check_sigs) {
        // Matches every sibling CheckXxxTx function's own convention
        // (CheckProRegTx et al.): callers that have already verified
        // signatures elsewhere (or deliberately do not need to, e.g. a
        // -reindex replay of already-connected history) may skip doing it
        // again here -- now correctly only skipping the signature check
        // itself, not the structural payload checks above.
        return true;
    }

    const uint256 msgHash = ComputeAttestedMessageHash(tx);
    // v1's own interim choice (this file's own header doc comment):
    // ChainLocks' existing, already-live llmqType, not a new one stood up
    // for this purpose -- confirmed a real ::Consensus field by direct
    // read of chainparams.cpp before relying on it, matching every other
    // call site that already trusts it (quorums_chainlocks.cpp). Applies to
    // both versions below -- 5.4.4.1 does not stand up a second quorum type
    // for the batched path either.
    const Consensus::LLMQType llmqType = Params().GetConsensus().llmqTypeChainLocks;

    if (payload.nVersion == 2) {
        // Fable review (2026-10-01), CONFIRMED CRITICAL, fixed: this branch
        // originally looked an already-recovered signature up in
        // quorumSigningManager's own store by id alone (BuildAttestationId
        // reused unchanged from v1, over the batch root instead of a
        // per-tx hash) instead of verifying anything itself -- confirmed
        // live, by the review's own PoC, that this makes block validity
        // depend on local, non-consensus, 7-day-expiring state almost no
        // node ever populates (CAttestationPayload's own `sig` doc comment,
        // evo/attestedtx.h, has the full finding). Fixed: v2 now verifies
        // the SAME way v1 does, over the extracted root instead of this
        // one transaction's own hash, with its own domain-separated id
        // (BuildAttestationBatchId, not BuildAttestationId -- the "atxb"
        // prefix closes a separate LOW finding from the same review: a
        // one-leaf batch's root equals its member's own message hash, so
        // reusing v1's id would have aliased a v1 attestation and a v2
        // batch attestation for that one transaction onto the same key).
        uint256 batchRoot;
        if (!ExtractAttestedBatchRoot(payload.batchProof, msgHash, batchRoot)) {
            return state.DoS(100, false, REJECT_INVALID, "bad-attested-tx-batch-proof");
        }
        const uint256 id = BuildAttestationBatchId(batchRoot);
        const uint256 cacheKey = ComputeBatchCacheKey(batchRoot, payload.nSignHeight, payload.sig);
        if (!IsKnownVerifiedBatch(cacheKey)) {
            if (!llmq::CSigningManager::VerifyRecoveredSig(llmqType, payload.nSignHeight, id, batchRoot,
                                                            payload.sig)) {
                return state.DoS(100, false, REJECT_INVALID, "bad-attested-tx-batch-attestation");
            }
            RememberVerifiedBatch(cacheKey);
        }
        return true;
    }

    const uint256 id = BuildAttestationId(msgHash);
    if (!llmq::CSigningManager::VerifyRecoveredSig(llmqType, payload.nSignHeight, id, msgHash, payload.sig)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-attested-tx-attestation");
    }

    return true;
}
