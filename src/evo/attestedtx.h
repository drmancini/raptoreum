// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_ATTESTEDTX_H
#define BITCOIN_EVO_ATTESTEDTX_H

#include <bls/bls.h>
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
    static const uint16_t CURRENT_VERSION = 1;

    uint16_t nVersion{CURRENT_VERSION};
    CBLSSignature sig;

    SERIALIZE_METHODS(CAttestationPayload, obj)
    {
        READWRITE(obj.nVersion, obj.sig);
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
 *    and double-spend/UTXO correctness are NOT re-checked here: both are
 *    already unconditionally enforced by Consensus::CheckTxInputs, called
 *    from validation.cpp before CheckSpecialTx's own dispatch ever runs, for
 *    every transaction regardless of nType. Re-implementing the identical
 *    rule a second time here would risk the two copies silently diverging
 *    (this project's own established anti-pattern -- see F-95's legacy vs.
 *    accurate sigop-counter lesson for the same class of risk).
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
 *  - The quorum attestation signature (5.4.2, this function's own new
 *    piece as of this sub-step): `check_sigs` gates it, matching every
 *    sibling CheckXxxTx function's own convention. Verified by reusing
 *    CSigningManager::VerifyRecoveredSig (llmq/quorums_signing.h) directly
 *    -- the exact primitive ChainLocks already trusts for the identical
 *    kind of claim -- never by reimplementing BLS or quorum lookup. v1
 *    uses `Params().GetConsensus().llmqTypeChainLocks`, reusing an
 *    existing, already-live quorum type rather than standing up a new
 *    one (build-plan.md's own 5.4 row, open item: confirmed as a
 *    deliberate choice here, not yet put to tri for a real llmqType of
 *    its own). No live-quorum test is possible for this piece in this
 *    environment (F-216's own documented three-failed-DKG-attempts
 *    problem, the same limitation 5.3/F-238 already hit) -- verified by
 *    direct reading against VerifyRecoveredSig's own contract instead. */
bool CheckAttestedTx(const CTransaction &tx, const CBlockIndex *pindexPrev, CValidationState &state,
                     const CCoinsViewCache &view, bool check_sigs);

#endif //BITCOIN_EVO_ATTESTEDTX_H
