// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_QUORUMS_ATTESTATIONBATCH_H
#define BITCOIN_LLMQ_QUORUMS_ATTESTATIONBATCH_H

#include <bls/bls.h>
#include <evo/attestationbatch.h>
#include <llmq/quorums_signing.h>
#include <sync.h>
#include <uint256.h>

#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>

class CScheduler;

class CTransaction;

class CValidationState;

class CCoinsViewCache;

namespace llmq {

/** 5.4.4.2 (build-plan.md, F-245): the quorum-member side of batch signing
 *  -- collects pending attestation requests, and on a fixed interval tick,
 *  asks the quorum to collaboratively sign a Merkle root over whatever is
 *  locally queued. Mirrors CChainLocksHandler's own shape exactly (its own
 *  CScheduler + dedicated thread, Start/Stop registering as a
 *  CRecoveredSigsListener, quorums_chainlocks.h/.cpp) -- confirmed by
 *  reading that class directly before building this one: the signing-
 *  SESSION machinery (AsyncSignIfMember/CSigSharesManager/
 *  HandleNewRecoveredSig) is 100% reused, unchanged. The one genuinely new
 *  design here is WHAT gets signed and WHEN, not HOW a quorum collaborates
 *  to produce a signature over it.
 *
 *  The leaderless convergence design (the owner's own choice, 2026-10-01,
 *  AskUserQuestion, over putting this to trí first or discussing further):
 *  unlike ChainLocks' own message (the best chain tip, already-agreed
 *  consensus state every node computes identically from blockchain state
 *  alone), a batch of pending attestation requests is mempool-adjacent --
 *  different quorum members' own local queues could genuinely differ at
 *  any instant, since nothing forces them to converge the way chain state
 *  does. BLS threshold signing does not need ALL members to agree, only
 *  THRESHOLD-many independently signing the byte-identical message -- so
 *  this relies on ordinary P2P propagation delay being short relative to
 *  the tick interval (the same kind of assumption ChainLocks' own 5-second
 *  cadence already makes, just probabilistic here rather than consensus-
 *  guaranteed by block structure): on most ticks, enough members' queues
 *  will have converged by request-propagation alone for a threshold subset
 *  to agree. A tick where they have not simply produces no valid
 *  aggregate signature -- trí's own confirmed "accepted rare-loss cost"
 *  (transaction-decoupling.md section 17.6), not a failure this class
 *  needs to detect or report; an un-recovered request's own hash simply
 *  remains queued (or gets re-requested) for a later tick's fresh
 *  snapshot.
 *
 *  Deliberately NOT built here (see build-plan.md's own 5.4.4.2 row):
 *  the actual wire message a remote wallet uses to reach RequestAttestation
 *  (and the one it uses to retrieve a recovered GetAttestation result) --
 *  this class exposes the calls ANY transport would route into, but the
 *  transport itself, and the init.cpp/net_processing.cpp wiring to start
 *  this handler on a real node at all, are this row's own next sub-step.
 *
 *  5.4.4.1-OPEN row's own item (b), F-244's txid-malleability MEDIUM, is
 *  now closed (F-247): BuildAttestationBatchProof/GetAttestation below
 *  carry a CAttestationBatchProof (evo/attestationbatch.h), not a
 *  CPartialMerkleTree -- the new type has no padding bits, flag bytes, or
 *  any other encoding freedom left for a re-encoding to exploit, by
 *  construction, not by an added check. */
/** Pure phase-alignment math for Start()'s own first scheduled tick
 *  (HIGH-2, Fable review 2026-10-01) -- exposed, and kept free of
 *  CAttestationBatchHandler entirely, so it can be tested directly
 *  without a real scheduler, wall clock, or class instance at all,
 *  matching this project's own established preference for extracting
 *  scheduled logic's own pure decisions into standalone functions
 *  (CChainLocksHandler's own DecideChainLockSignAction,
 *  quorums_chainlocks.h, is the precedent this follows). Returns how
 *  many milliseconds from `nowSecs` until the next boundary that is an
 *  exact multiple of `intervalMs` -- 0 if `nowSecs` already lands
 *  exactly on one. */
int64_t MsUntilNextTickSlot(int64_t nowSecs, int64_t intervalMs);

class CAttestationBatchHandler : public CRecoveredSigsListener {
public:
    CAttestationBatchHandler();

    ~CAttestationBatchHandler();

    void Start();

    void Stop();

    /** Validates `tx` as a candidate attested transaction -- type, the
     *  shared structural checks (CheckAttestedTxInputShapes,
     *  evo/attestedtx.h), and, CRITICALLY, a genuine per-input signature
     *  check (CScriptCheck, validation.h -- the exact primitive CheckInputs
     *  itself uses, reused rather than reimplemented or exposing
     *  CheckInputs, which is file-local to validation.cpp) -- and only on
     *  full success, queues its ComputeAttestedMessageHash (evo/attestedtx.h;
     *  the transaction itself need not have its attestation payload yet,
     *  only everything else final, which is why a wallet can request this
     *  BEFORE a quorum has attested, closing the bootstrapping problem a
     *  mempool-relay-triggered design would otherwise have: CheckAttestedTx
     *  is TRANSACTION_ATTESTED's own ONLY validity rule, with no "accepted
     *  by ordinary means first, attested as an optional add-on later" path
     *  the way an islock's own host transaction has, confirmed by reading
     *  CInstantSendManager::TransactionAddedToMempool, quorums_instantsend.cpp
     *  -- its own trigger for signing an islock at all, firing only AFTER
     *  ordinary ATMP already accepted the host transaction) for inclusion
     *  in the next tick's batch. Idempotent -- a caller retrying its own
     *  already-queued request must not inflate the batch with duplicates.
     *
     *  Fable review (2026-10-01), CONFIRMED CRITICAL, fixed: this used to
     *  take a bare uint256 msgHash with NO validation at all -- the caller
     *  simply asserted "this is worth attesting," unchecked. CheckAttestedTx
     *  itself never references txin.scriptSig anywhere (confirmed by direct
     *  re-read of its current structural loop before relying on this, not
     *  assumed) -- it only checks the PREVOUT's existence/script shape, by
     *  design, since 5.4.1 restricted attested inputs to single-sig scripts
     *  it did not yet need to verify. Combined with 5.4.3's own CheckInputs
     *  skip (fires for any IsAttestedTx-true transaction, with no "checked
     *  at least once somewhere else first" fallback the way islock has),
     *  this meant NOTHING in the request-to-verification chain ever checked
     *  that the requester actually owned the key for a single one of the
     *  inputs being attested -- once a live quorum signed a batch
     *  containing an attacker-supplied hash, CheckAttestedTx would accept
     *  the resulting transaction with a completely empty scriptSig. Fixed
     *  by moving a real signature check here, to request time, the ONE
     *  place it can run without re-imposing cost on every validating node
     *  per relay hop (CheckAttestedTxInputShapes' own doc comment,
     *  evo/attestedtx.h, explains why it cannot live inside CheckAttestedTx
     *  itself) -- restoring trí's own original design intent
     *  (transaction-decoupling.md section 17.6's "input sign" finding): one
     *  honest check, paid for once, not re-paid for at every hop once a
     *  quorum has attested to the result. */
    bool RequestAttestation(const CTransaction &tx, CValidationState &state, const CCoinsViewCache &view);

    /** Sweeps expired entries (SweepExpiredEntries), snapshots and clears
     *  the pending queue, computes the canonical root
     *  (ComputeCanonicalBatchRoot, evo/attestationbatch.h) over whatever
     *  was queued, and asks the quorum to sign it (AsyncSignIfMember,
     *  allowReSign=true -- see this method's own .cpp comment for why) --
     *  unless the queue was empty, in which case there is nothing to sign
     *  and this is a no-op (trí's own "no minimum batch size" means a
     *  small queue is never refused, but an EMPTY one still has nothing
     *  to sign over).
     *
     *  Deliberately NOT gated on fSmartnodeMode/IsBlockchainSynced here
     *  (unlike CChainLocksHandler::TrySignChainTip's own identical
     *  guards, MEDIUM-2, Fable review 2026-10-01) -- those live in
     *  RunScheduledTick instead, the method the scheduler actually calls,
     *  so this method stays exposed as public and directly callable, not
     *  reachable only via the scheduler's own timer or only on a real
     *  smartnode: this project's own established preference for testing
     *  scheduled logic by calling it directly (and the real timer, once
     *  5.4.4.2's own next sub-step wires Start() into a running node,
     *  reaches this same method through RunScheduledTick). Returns the
     *  root it asked for, or a null uint256 if the queue was empty.
     *
     *  Fable review (2026-10-01), CONFIRMED HIGH, fixed: a signature for
     *  this exact batch could already have recovered, network-wide,
     *  microseconds before this call even ran -- enough OTHER members'
     *  own ticks may have converged and recovered moments earlier, and
     *  CRecoveredSigsListener only ever fires on the EDGE of recovery,
     *  never retroactively, so this node's own HandleNewRecoveredSig
     *  would otherwise never see it. Checked here, immediately after this
     *  node's own awaitingRecovery entry is registered, via the signing
     *  manager's own already-recovered store (GetRecoveredSigForId) --
     *  dispatched through HandleNewRecoveredSig itself so both paths
     *  share one acceptance rule, including its own HIGH-1 quorum-binding
     *  check. */
    uint256 TrySignBatch();

    /** Once `msgHash`'s own batch has recovered a signature, fills in
     *  everything a CAttestationPayload v2 (evo/attestedtx.h) needs to be
     *  independently verifiable: the signature itself, the height it was
     *  signed at, and this one transaction's own Merkle inclusion proof
     *  (BuildAttestationBatchProof, evo/attestationbatch.h). Returns false
     *  if no recovered batch covers this hash -- not yet (this tick's own
     *  queue has not converged and recovered a signature), or never (the
     *  request was never queued, its own recovered batch has since aged
     *  out of recoveredBatches' own retention window, or HandleNewRecoveredSig's
     *  own HIGH-1 quorum-binding check rejected the recovery -- see that
     *  method's own doc comment).
     *
     *  No live quorum is constructible in this test environment (F-216's
     *  own documented limitation), so this method's own "found a match"
     *  branch -- four lines, no branching logic of its own beyond the
     *  BuildAttestationBatchProof call, which IS tested directly,
     *  test/attestationbatch_tests.cpp -- is verified by direct reading
     *  rather than exercised end-to-end through this class; every test
     *  that reaches this method here does so after a recovery this
     *  class's own guards correctly rejected, proving the "not found"
     *  branch instead. */
    bool GetAttestation(const uint256 &msgHash, CBLSSignature &retSig, int32_t &retSignHeight,
                        CAttestationBatchProof &retProof) const;

    /** Fable review (2026-10-01), CONFIRMED HIGH, fixed: accepting ANY
     *  well-formed recovered signature that matched this node's own
     *  awaitingRecovery entry used to be enough -- but AsyncSignIfMember
     *  (called from TrySignBatch) selects its signing quorum from
     *  whatever the chain tip happens to be AT THAT EXACT CALL
     *  (quorums_signing.cpp, confirmed by reading its implementation, not
     *  assumed), not from the `signHeight` this class recorded moments
     *  earlier under a SEPARATE cs_main lock -- so a block connecting in
     *  that narrow window can make the ACTUAL signing quorum differ from
     *  the one `signHeight` alone would make a later verifier
     *  (CheckAttestedTx) re-derive, shipping an attestation that can
     *  never pass its own verification. Now checked by re-deriving the
     *  EXPECTED quorum from the recorded signHeight
     *  (CSigningManager::SelectQuorumForSigning) and comparing against
     *  the recovered signature's own getQuorumHash() -- a mismatch (or no
     *  determinable quorum at all) means this recovery cannot be trusted
     *  to match the height this batch claims, and is dropped rather than
     *  stored.
     *
     *  No live quorum is constructible in this test environment to
     *  exercise the "quorum exists but does not match" branch
     *  specifically (F-216's own documented limitation, hit identically
     *  by every other quorum-lookup in this feature) -- verified by
     *  direct reading of SelectQuorumForSigning's own contract instead;
     *  the "no quorum determinable at all" branch (quorum == nullptr) is
     *  the one this environment CAN and does exercise, and is
     *  mutation-tested live. */
    void HandleNewRecoveredSig(const CRecoveredSig &recoveredSig) override;

private:
    // 5 seconds, matching CChainLocksHandler::Start's own cadence exactly
    // (quorums_chainlocks.cpp). A real interval-length/propagation-margin
    // analysis for the leaderless convergence design (this class's own
    // header comment) is 5.4.4.2's own still-open tuning question, not
    // decided here.
    static constexpr int64_t TICK_INTERVAL_MS = 5 * 1000;
    // How long an awaitingRecovery entry is given to converge before its
    // own leaves are re-queued for a fresh snapshot (HIGH-2, Fable review
    // 2026-10-01) -- three ticks' worth, a placeholder giving propagation
    // a few rounds rather than just one, not a tuned value.
    static constexpr int64_t AWAITING_RECOVERY_TIMEOUT_SECS = 3 * (TICK_INTERVAL_MS / 1000);
    // How long a recovered batch stays available for GetAttestation once
    // signed (MEDIUM-1, Fable review 2026-10-01) -- ten minutes, a
    // reasonable placeholder, not a tuned value; same as islock/chainlock
    // cleanup intervals needed their own tuning, genuinely open.
    static constexpr int64_t RECOVERED_BATCH_RETENTION_SECS = 10 * 60;

    std::unique_ptr <CScheduler> scheduler;
    std::unique_ptr <std::thread> schedulerThread;

    mutable RecursiveMutex cs;
    std::set <uint256> pendingRequests GUARDED_BY(cs);

    struct RecoveredBatch {
        std::vector <uint256> leaves; // canonically sorted, matches signHeight/sig's own signed root
        int32_t signHeight;
        CBLSSignature sig;
        int64_t timestamp; // GetAdjustedTime() when this entry was created -- SweepExpiredEntries' own bound
    };

    // Populated by TrySignBatch the moment it asks a quorum to sign a
    // root, keyed by that same root -- the leaves/signHeight a signature
    // will need to be matched with once (if) HandleNewRecoveredSig ever
    // sees it recovered. No signature yet; moved into recoveredBatches
    // below once one arrives.
    //
    // Fable review (2026-10-01), CONFIRMED HIGH/MEDIUM, fixed: an entry
    // that never recovers (not enough other members' own queues converged
    // on this exact root) used to sit here for the lifetime of the
    // process, growing unboundedly and permanently swallowing its own
    // leaves -- SweepExpiredEntries (called every tick, from
    // TrySignBatch) now re-queues a stale entry's leaves for a fresh
    // snapshot and drops the entry itself once
    // AWAITING_RECOVERY_TIMEOUT_SECS has passed with no recovery.
    std::map <uint256, RecoveredBatch> awaitingRecovery GUARDED_BY(cs);

    // Keyed by the batch root (== the recovered signature's own msgHash),
    // so GetAttestation can find which still-held batch (if any) covers a
    // given leaf.
    //
    // Fable review (2026-10-01), CONFIRMED MEDIUM, fixed: a recovered
    // batch used to stay here permanently, growing unboundedly over the
    // node's own uptime -- SweepExpiredEntries now prunes an entry once
    // RECOVERED_BATCH_RETENTION_SECS has passed since it recovered.
    std::map <uint256, RecoveredBatch> recoveredBatches GUARDED_BY(cs);

    // MEDIUM-2 fix (Fable review, 2026-10-01): the ACTUAL scheduled-timer
    // body. TrySignBatch itself is deliberately left ungated (see its own
    // doc comment) so tests can call it directly without a live
    // smartnode or a synced chain; this wrapper, which only the scheduler
    // (Start, below) ever calls, carries the fSmartnodeMode/
    // IsBlockchainSynced guards CChainLocksHandler::TrySignChainTip's own
    // identical ones (quorums_chainlocks.cpp) are matched against.
    void RunScheduledTick();

    // HIGH-2/MEDIUM-1 fix (Fable review, 2026-10-01): called at the start
    // of every TrySignBatch tick, under cs -- re-queues leaves from an
    // awaitingRecovery entry that has timed out without converging, and
    // prunes a recoveredBatches entry past its own retention window. See
    // awaitingRecovery/recoveredBatches' own doc comments above for why
    // each bound exists.
    void SweepExpiredEntries() EXCLUSIVE_LOCKS_REQUIRED(cs);
};

extern CAttestationBatchHandler *attestationBatchHandler;

} // namespace llmq

#endif //BITCOIN_LLMQ_QUORUMS_ATTESTATIONBATCH_H
