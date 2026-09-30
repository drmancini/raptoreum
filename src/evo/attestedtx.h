// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_ATTESTEDTX_H
#define BITCOIN_EVO_ATTESTEDTX_H

#include <primitives/transaction.h>

class CBlockIndex;

class CCoinsViewCache;

class CValidationState;

/** 5.4 (build-plan.md, "attested transaction type" -- transaction-decoupling.md
 *  section 17.6, tri's own buspool design): a transaction whose validity rule
 *  is a quorum threshold attestation plus a sanity check, not the full
 *  script interpreter.
 *
 *  5.4.1's own scope, precisely -- what this function checks and what it
 *  deliberately does not (yet):
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
 *    function does NOT need to touch it at all in 5.4.1: `CheckInputs`
 *    (validation.cpp, not declared in any header, so not callable from this
 *    file) already runs unconditionally for EVERY transaction in
 *    ATMP/ConnectBlock today, `TRANSACTION_ATTESTED` included, because
 *    5.4.1 makes no change to that control flow. The type's own throughput
 *    argument was never "skip this check" -- it is "do not pay for this
 *    check AGAIN at every relay hop once one honest check has already run
 *    and a quorum has attested to the result", which is 5.4.3's own job
 *    once 5.4.2's attestation-verification piece exists to justify the
 *    skip (matching 5.3/F-238's identical admission-only shape, just keyed
 *    on a passed CheckAttestedTx call rather than an islock). Until then,
 *    an attested transaction is checked exactly as strictly as an
 *    ordinary one, with one addition on top, not a substitute for anything.
 *  - Restricted to single-signature (P2PK/P2PKH) inputs only -- build-
 *    plan.md's own 5.4 row, interim decision (a). Anything else is
 *    REJECTED at the type level, not silently under-checked; broadening
 *    this is real, separate design work (which script shapes the
 *    shortened check can correctly validate was left open in section 17.6
 *    and is not decided by this file).
 *  - The quorum attestation signature itself is NOT checked here -- that
 *    is 5.4.2, which needs a payload format and an llmqType decision
 *    neither of which exist yet. A transaction that passes THIS function
 *    today has been checked exactly as strictly as an ordinary payment;
 *    nothing about it is trusted on a quorum's word until 5.4.2 lands. */
bool CheckAttestedTx(const CTransaction &tx, const CBlockIndex *pindexPrev, CValidationState &state,
                     const CCoinsViewCache &view, bool check_sigs);

#endif //BITCOIN_EVO_ATTESTEDTX_H
