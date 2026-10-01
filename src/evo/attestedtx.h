// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_ATTESTEDTX_H
#define BITCOIN_EVO_ATTESTEDTX_H

#include <bls/bls.h>
#include <merkleblock.h>
#include <primitives/transaction.h>
#include <serialize.h>

class CBlockIndex;

class CCoinsViewCache;

class CValidationState;

/** 5.4.2 (build-plan.md): the quorum attestation itself, carried in
 *  TRANSACTION_ATTESTED's own vExtraPayload (GetTxPayload, evo/specialtx.h,
 *  the same mechanism every sibling special-tx type already uses).
 *
 *  Deliberately does NOT carry `id`, `msgHash` or `quorumHash` -- all three
 *  are derived, not trusted from the wire, so a payload cannot claim to
 *  attest a different transaction than the one it is actually attached to.
 *  `id`/`msgHash` come from the transaction's own hash (BuildAttestationId/
 *  attestedtx.cpp); `quorumHash` is never a VerifyRecoveredSig parameter at
 *  all -- CSigningManager::VerifyRecoveredSig (llmq/quorums_signing.h)
 *  selects the quorum itself, deterministically, from `id` and the signing
 *  height (confirmed by direct read of its implementation before relying
 *  on it, not assumed: `SelectQuorumForSigning(llmqType, id, signedAtHeight,
 *  signOffset)`). */
class CAttestationPayload {
public:
    // 5.4.4.1 (build-plan.md, F-243): v1 (a direct per-tx quorum signature)
    // and v2 (a Merkle proof of inclusion under a batch root some quorum
    // signature already covers) are BOTH valid, current versions, not a
    // deprecated-vs-current pair -- trí's own cost model (build-plan.md's
    // 5.2 row, F-11/F-22) puts the batching break-even at roughly 10-20
    // transactions per signature, so below that a direct v1 signature is
    // still the cheaper choice. Bumping CURRENT_VERSION to 2 does not
    // invalidate existing v1 payloads: the version bound below
    // (CheckAttestedTx) is `0 < nVersion <= CURRENT_VERSION`, an inclusive
    // range, not an exact match.
    static const uint16_t CURRENT_VERSION = 2;

    // Deliberately NOT `{CURRENT_VERSION}`: 5.4.4.2 (the batch-formation
    // machinery a v2 payload depends on to ever be verifiable in
    // production) does not exist yet, so a caller that default-constructs
    // this object without deliberately choosing v2 must still get a
    // payload something can actually verify today -- CURRENT_VERSION is
    // the version-check's own UPPER BOUND, not a safe default to build
    // from blindly, and the two are allowed to differ on purpose.
    uint16_t nVersion{1};
    // Fable review (2026-09-30): carried explicitly rather than inferred by
    // the verifier from pindexPrev -- a verifier-inferred height silently
    // drifts from whatever height the signer actually used the moment the
    // signing quorum rotates between signing and verifying, which is not
    // hypothetical (mainnet's real llmqTypeChainLocks rotates every 360
    // blocks, llmq/quorums_parameters.h). Bounding this against pindexPrev
    // at verify time (attestedtx.cpp) is still an interim choice, not
    // islock's own fuller cycleHash/dkgInterval scheme (quorums_instantsend.cpp).
    // Applies identically to v1 and v2 -- v2's own sig (below) is over the
    // batch, but SelectQuorumForSigning still needs a height to pick the
    // quorum that was expected to have signed it, same as v1.
    int32_t nSignHeight{-1};
    // The quorum signature. v1: directly over ComputeAttestedMessageHash(tx)
    // (attestedtx.cpp). v2: over the batch's own Merkle root (batchProof's
    // own ExtractMatches, not this transaction's own hash) -- ALWAYS
    // present and ALWAYS what CheckAttestedTx verifies via
    // VerifyRecoveredSig, for both versions identically.
    //
    // Fable review (2026-10-01), CONFIRMED CRITICAL, fixed: v2 originally
    // carried NO signature at all here, relying on CheckAttestedTx looking
    // one up in llmq::quorumSigningManager's own recovered-sig store by the
    // batch root alone. That store is NOT a general network-propagation
    // layer -- confirmed by reading net.cpp's own QSENDRECSIGS gating
    // (CConnman::RelayRecoveredSig/CMNAuth, both: "SPV and regular full
    // nodes should not send this message") and CChainLocksHandler::
    // ProcessNewChainLock (quorums_chainlocks.cpp), which re-verifies a
    // CLSIG's own carried signature directly rather than consulting that
    // store at all. A regular full node, or any smartnode outside the
    // signing quorum's own relay set, would never hold the entry the old
    // v2 path looked up -- making block validity depend on local,
    // 7-day-expiring, non-consensus state instead of a cryptographic fact,
    // confirmed live by the review's own PoC (the identical transaction's
    // verdict flipped on nothing but local DB contents). Fixed by carrying
    // the signature directly, same as v1, verified the same way -- see
    // CheckAttestedTx's own comment for the per-node verification cache
    // that keeps this throughput-neutral (trí's own argument was "verify
    // once, not once per transaction sharing a batch" -- now true per
    // validating NODE, which is what a consensus rule can actually
    // guarantee, rather than the unsound "verify once, trust everywhere"
    // the previous version of this field relied on).
    CBLSSignature sig;
    // 5.4.4.1 (build-plan.md, F-243): v2 only (default-constructed, unused,
    // for v1). Proves THIS transaction's own ComputeAttestedMessageHash(tx)
    // is one leaf of the batch `sig` above is actually a signature over --
    // CPartialMerkleTree (merkleblock.h, the existing SPV single/multi-leaf
    // inclusion-proof primitive, reused rather than hand-rolling a new
    // branch/sibling-hash format) constructed with exactly one matched leaf
    // (this transaction's own hash) out of the batch's full leaf list.
    // HOW a batch's signature actually gets requested, collected, and
    // recovered in the first place -- the hard, genuinely novel "N quorum
    // members must agree on identical batch membership with no leader"
    // problem build-plan.md's own 5.4.4.2 row names as still open -- is
    // deliberately NOT built here. This sub-step is the payload shape and
    // the verification side, exercisable and testable on their own once a
    // real batch signature exists, however it got produced.
    CPartialMerkleTree batchProof;

    SERIALIZE_METHODS(CAttestationPayload, obj)
    {
        READWRITE(obj.nVersion, obj.nSignHeight, obj.sig);
        if (obj.nVersion == 2) {
            READWRITE(obj.batchProof);
        } else {
            // Fable review (2026-10-01), LOW, fixed: an object reused
            // across more than one (de)serialization must not retain a
            // PREVIOUS use's batchProof -- CheckAttestedTx always
            // deserializes into a fresh local (attestedtx.cpp), so this had
            // no live exposure today, but a future caller that reuses one
            // object across multiple GetTxPayload calls would otherwise
            // silently inherit stale proof data on a v1 read.
            SER_READ(obj, obj.batchProof = CPartialMerkleTree());
        }
    }
};

/** 5.4 (build-plan.md, "attested transaction type" -- transaction-decoupling.md
 *  section 17.6, tri's own buspool design): a transaction whose validity rule
 *  is a quorum threshold attestation plus a sanity check, not the full
 *  script interpreter.
 *
 *  What this function checks, and what it deliberately does not (yet):
 *
 *  - Input/output value conservation (the inflation check, tri's own term)
 *    is NOT re-checked here: Consensus::CheckTxInputs (validation.cpp)
 *    already enforces it for every transaction regardless of nType.
 *    Re-implementing the identical rule a second time here would risk the
 *    two copies silently diverging (this project's own established
 *    anti-pattern -- see F-95's legacy vs. accurate sigop-counter lesson
 *    for the same class of risk). Fable review (2026-09-30), CONFIRMED
 *    MEDIUM, fixed: double-spend/UTXO correctness is NOT already covered
 *    the way an earlier version of this comment claimed -- ATMP passes
 *    CoinsTip() to CheckSpecialTx (deliberately mempool-blind), and
 *    ConnectBlock's own ProcessSpecialTxsInBlock runs BEFORE the per-tx
 *    loop's own CheckTxInputs/UpdateCoins, not after -- so this
 *    function's own Coin-existence loop below is load-bearing, not
 *    redundant, and spending an unconfirmed parent genuinely reaches it
 *    (see that loop's own comment for the fix: treated as a real, named
 *    v1 restriction, not an impossible case).
 *  - "Input sign" (section 17.6, confirmed to mean an independent,
 *    separate-from-the-quorum per-input signature check) is real, and this
 *    function does NOT need to touch it at all: `CheckInputs`
 *    (validation.cpp, not declared in any header, so not callable from this
 *    file) already runs unconditionally for EVERY transaction in
 *    ATMP/ConnectBlock today, `TRANSACTION_ATTESTED` included, because
 *    neither 5.4.1 nor 5.4.2 change that control flow. The type's own
 *    throughput argument was never "skip this check" -- it is "do not pay
 *    for this check AGAIN at every relay hop once one honest check has
 *    already run and a quorum has attested to the result", which is
 *    5.4.3's own job, now that 5.4.2 below gives it something real to key
 *    the skip on (matching 5.3/F-238's identical admission-only shape,
 *    just keyed on a passed CheckAttestedTx call rather than an islock).
 *    Until 5.4.3 wires the skip in, an attested transaction is checked
 *    exactly as strictly as an ordinary one, with two additions on top,
 *    not a substitute for anything.
 *  - Restricted to single-signature (P2PK/P2PKH) inputs only -- build-
 *    plan.md's own 5.4 row, interim decision (a). Anything else is
 *    REJECTED at the type level, not silently under-checked; broadening
 *    this is real, separate design work (which script shapes the
 *    shortened check can correctly validate was left open in section 17.6
 *    and is not decided by this file).
 *  - The quorum attestation itself: `check_sigs` gates only the signature
 *    verification, not the payload's own structural checks above it
 *    (Fable review, CONFIRMED MEDIUM, fixed -- see CheckAttestedTx's own
 *    comment at the `check_sigs` branch). Both versions verify via
 *    CSigningManager::VerifyRecoveredSig (llmq/quorums_signing.h) directly
 *    -- the exact primitive ChainLocks already trusts for the identical
 *    kind of claim -- never by reimplementing BLS or quorum lookup. v1
 *    verifies directly over ComputeAttestedMessageHash(tx), never
 *    tx.GetHash() (Fable review, CONFIRMED HIGH, fixed -- see that
 *    function's own comment, attestedtx.cpp, for why the obvious version
 *    is circular and unbuildable). v2 (5.4.4.1, F-243/F-244: a Merkle
 *    proof under a batch root, the signature carried alongside it)
 *    verifies the SAME way, over the extracted root instead of this one
 *    transaction's own hash -- see CAttestationPayload's own `sig`/
 *    `batchProof` doc comments for the full design, the CRITICAL this
 *    round's own Fable review found and fixed (the signature was
 *    originally NOT carried, looked up instead in a store that is not a
 *    general network-propagation layer), and what is deliberately not
 *    built yet (5.4.4.2: how a batch's signature is actually produced in
 *    the first place). A per-node verification cache (attestedtx.cpp,
 *    keyed on the exact (root, height, signature) triple so a forged
 *    signature over a real root can never ride on a genuine one's cache
 *    hit) keeps repeated transactions sharing one batch cheap without
 *    needing the unsound shared-store design -- trí's own throughput
 *    argument, now true per validating node, which is what a consensus
 *    rule can actually guarantee. Both versions use
 *    `Params().GetConsensus().llmqTypeChainLocks`, reusing an existing,
 *    already-live quorum type rather than standing up a new one
 *    (build-plan.md's own 5.4 row, open item: confirmed as a deliberate
 *    choice here, not yet put to tri for a real llmqType of its own). No
 *    live-quorum test is possible for either version's own final
 *    acceptance step in this environment (F-216's own documented
 *    three-failed-DKG-attempts problem, the same limitation 5.3/F-238
 *    already hit) -- verified by direct reading against
 *    VerifyRecoveredSig's own contract instead, for both versions. */
bool CheckAttestedTx(const CTransaction &tx, const CBlockIndex *pindexPrev, CValidationState &state,
                     const CCoinsViewCache &view, bool check_sigs);

/** Fable review (2026-10-01), CONFIRMED CRITICAL, fixed: whether a
 *  transaction IS an attested transaction must be judged identically
 *  everywhere. CheckSpecialTx (evo/specialtx.cpp) only ever dispatches to
 *  CheckAttestedTx when tx.nVersion == 3 exactly -- its own `nVersion != 3`
 *  guard returns true, unchecked, for any other version, treating the
 *  transaction as an ordinary payment regardless of nType. This function's
 *  own type check, and validation.cpp's 5.4.3 whitelist/CheckInputs-skip
 *  conditions, originally checked ONLY `nType == TRANSACTION_ATTESTED`, with
 *  no nVersion term -- so a transaction with nVersion=4 (or anything other
 *  than 3) and nType=11 passed the whitelist and took the CheckInputs skip
 *  in both ATMP and ConnectBlock while CheckSpecialTx silently never ran
 *  CheckAttestedTx at all, meaning NEITHER check ran. Confirmed live via the
 *  review's own executable PoC: an unsigned spend of an arbitrary coin
 *  connects into the chain once EUpdate::ATTESTED_TX is active. Every one of
 *  those three sites (and this function's own type check, for the same
 *  reason, even though CheckSpecialTx's own guard means it is not reachable
 *  with the wrong version via that one path today) now goes through this
 *  single, shared predicate instead of re-deriving it. */
inline bool IsAttestedTx(const CTransaction &tx) {
    return tx.nVersion == 3 && tx.nType == TRANSACTION_ATTESTED;
}

/** Exposed (not file-local `static`) purely so attestedtx_tests.cpp can hold
 *  this HIGH-severity fix (Fable review, 2026-09-30) to a genuine mutation
 *  test: no live quorum is obtainable in this test environment (F-216's own
 *  documented three-failed-DKG-attempts problem), so every reachable
 *  CheckAttestedTx failure path here fails at the quorum-lookup step, before
 *  the message hash's actual VALUE is ever consulted -- a revert back to
 *  `tx.GetHash()` would NOT be caught by any test that only calls
 *  CheckAttestedTx. This function's one load-bearing property (independence
 *  from `payload.sig`) is pure and quorum-free, so it is tested directly
 *  instead. */
uint256 ComputeAttestedMessageHash(const CTransaction &tx);

#endif //BITCOIN_EVO_ATTESTEDTX_H
