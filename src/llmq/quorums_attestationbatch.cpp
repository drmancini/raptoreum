// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/quorums_attestationbatch.h>

#include <chainparams.h>
#include <consensus/validation.h>
#include <evo/attestationbatch.h>
#include <evo/attestedtx.h>
#include <llmq/quorums.h>
#include <llmq/quorums_commitment.h>
#include <policy/policy.h>
#include <scheduler.h>
#include <script/interpreter.h>
#include <smartnode/smartnode-sync.h>
#include <timedata.h>
#include <util/system.h>
#include <validation.h>

namespace llmq {

CAttestationBatchHandler *attestationBatchHandler{nullptr};

int64_t MsUntilNextTickSlot(int64_t nowSecs, int64_t intervalMs) {
    const int64_t intervalSecs = intervalMs / 1000;
    const int64_t secsIntoCurrentSlot = nowSecs % intervalSecs;
    return ((intervalSecs - secsIntoCurrentSlot) % intervalSecs) * 1000;
}

CAttestationBatchHandler::CAttestationBatchHandler()
        : scheduler(std::make_unique<CScheduler>()),
          schedulerThread(std::make_unique<std::thread>(
                  [&] { TraceThread("atxb-schdlr", [&] { scheduler->serviceQueue(); }); })) {
}

CAttestationBatchHandler::~CAttestationBatchHandler() {
    // LOW-2 fix (Fable review, 2026-10-01): unregister BEFORE tearing down
    // the scheduler, and unconditionally -- not only inside Stop() -- so a
    // caller that destroys this object without ever calling Stop() first
    // cannot leave a dangling listener registration pointing at a half- or
    // fully-destroyed object for CSigningManager's own sig-shares worker
    // thread to call into later. Safe to call even if Stop() already did
    // this: UnregisterRecoveredSigsListener's own std::remove/erase pair
    // (quorums_signing.cpp) is a no-op on a listener that is not currently
    // registered.
    quorumSigningManager->UnregisterRecoveredSigsListener(this);
    scheduler->stop();
    schedulerThread->join();
}

void CAttestationBatchHandler::RunScheduledTick() {
    // MEDIUM-2 fix (Fable review, 2026-10-01): matches
    // CChainLocksHandler::TrySignChainTip's own identical guards exactly
    // (quorums_chainlocks.cpp, confirmed by reading it directly) -- kept
    // OUT of TrySignBatch itself, which is deliberately exposed as a
    // directly-callable public method so tests can exercise its own
    // queue/root logic without a live timer or a live smartnode (its own
    // doc comment, quorums_attestationbatch.h); gating TrySignBatch
    // itself would silently return early before ever touching the queue
    // in every one of those direct-call tests. AsyncSignIfMember itself
    // already refuses a non-smartnode internally (quorums_signing.cpp),
    // so this is a real node's own efficiency/IBD-safety guard, not a
    // correctness backstop TrySignBatch itself needs.
    if (!fSmartnodeMode) {
        return;
    }
    if (!smartnodeSync.IsBlockchainSynced()) {
        return;
    }
    TrySignBatch();
}

void CAttestationBatchHandler::Start() {
    quorumSigningManager->RegisterRecoveredSigsListener(this);

    // HIGH-2 fix (Fable review, 2026-10-01): scheduleEvery's own period is
    // relative to whenever Start() happens to be called on THIS node --
    // with no cross-node phase alignment, two members' own 5-second ticks
    // could straddle a completely different wall-clock window, each
    // snapshotting a different subset of "pending right now," defeating
    // the leaderless design's own core assumption (this class's own
    // header comment: threshold-many members signing the BYTE-IDENTICAL
    // message). Aligning the FIRST tick to the next GetAdjustedTime()
    // boundary divisible by the interval -- the same clock every node's
    // own GetAdjustedTime() already converges on via peer time sampling,
    // not a node-local wall clock -- means every node's own tick lands on
    // (within second-granularity rounding of) the same instant, not an
    // arbitrary per-node phase offset. Matches TrySignChainTip's own
    // established use of GetAdjustedTime() for time-slotted convergence
    // (quorums_chainlocks.cpp's own GetChainLockAttemptNumber) -- not a
    // technique foreign to this codebase.
    const int64_t msUntilNextSlot = MsUntilNextTickSlot(GetAdjustedTime(), TICK_INTERVAL_MS);
    scheduler->scheduleFromNow([&]() {
        RunScheduledTick();
        scheduler->scheduleEvery([&]() { RunScheduledTick(); }, TICK_INTERVAL_MS);
    }, msUntilNextSlot);
}

void CAttestationBatchHandler::Stop() {
    scheduler->stop();
    quorumSigningManager->UnregisterRecoveredSigsListener(this);
}

bool CAttestationBatchHandler::RequestAttestation(const CTransaction &tx, CValidationState &state,
                                                   const CCoinsViewCache &view) {
    if (!IsAttestedTx(tx)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-attested-tx-type");
    }
    if (!CheckAttestedTxInputShapes(tx, state, view)) {
        return false; // state filled in by CheckAttestedTxInputShapes
    }

    // The CRITICAL fix this method's own header comment (quorums_attestationbatch.h)
    // explains in full: CheckAttestedTxInputShapes above only checks the
    // PREVOUT's existence/script shape, never this transaction's own
    // scriptSig -- so without this loop, a request for a transaction
    // spending any single-sig UTXO with a completely empty scriptSig would
    // sail through unchallenged, all the way to a real quorum signature.
    // Checked ONCE, here, at request time -- never inside CheckAttestedTx
    // itself, which this project's own 5.4.3 work made deliberately skip
    // CheckInputs for every attested transaction on every validating node.
    PrecomputedTransactionData txdata(tx);
    for (size_t i = 0; i < tx.vin.size(); i++) {
        const Coin &coin = view.AccessCoin(tx.vin[i].prevout);
        CScriptCheck check(coin.out, tx, i, STANDARD_SCRIPT_VERIFY_FLAGS, /*cacheIn=*/false, &txdata);
        if (!check()) {
            std::string debugMsg = "attestation request input " + std::to_string(i) +
                                   " failed its signature check: " + ScriptErrorString(check.GetScriptError());
            return state.DoS(100, false, REJECT_INVALID, "bad-attested-tx-request-signature", false, debugMsg);
        }
    }

    const uint256 msgHash = ComputeAttestedMessageHash(tx);

    // CONFIRMED CRITICAL (own testing, 2026-10-01), fixed: this header's own
    // doc comment above promises RequestAttestation is idempotent -- "a
    // caller retrying its own already-queued request must not inflate the
    // batch with duplicates" -- but std::set::insert's own return value
    // answers a different question ("was this newly added") than the one
    // this function used it to answer ("should the caller treat this as
    // success"). A retry landing while the request is still sitting in
    // pendingRequests (the common case: a wallet polling/retrying before
    // the next tick), already hashed into an in-flight awaitingRecovery
    // batch, or already sitting in recoveredBatches (GetAttestation can
    // serve it right now) is the request having ALREADY succeeded, not a
    // new failure -- confirmed live: a correctly-signed transaction
    // submitted twice in a row threw RPC_VERIFY_REJECTED with an empty
    // CValidationState (no DoS call on this path, so FormatStateMessage
    // had nothing to report) on the second call alone, a functional-test
    // failure this project's own "a guard present is not a guard that
    // works" lesson would have caught even if this fix had not.
    LOCK(cs);
    if (pendingRequests.count(msgHash) > 0) {
        return true;
    }
    for (const auto &entry: awaitingRecovery) {
        const RecoveredBatch &batch = entry.second;
        if (std::find(batch.leaves.begin(), batch.leaves.end(), msgHash) != batch.leaves.end()) {
            return true;
        }
    }
    for (const auto &entry: recoveredBatches) {
        const RecoveredBatch &batch = entry.second;
        if (std::find(batch.leaves.begin(), batch.leaves.end(), msgHash) != batch.leaves.end()) {
            return true;
        }
    }

    pendingRequests.insert(msgHash);
    return true;
}

void CAttestationBatchHandler::SweepExpiredEntries() {
    AssertLockHeld(cs);
    const int64_t now = GetAdjustedTime();

    // HIGH-2/MEDIUM-1 fix (Fable review, 2026-10-01): a batch that has
    // not recovered a signature within a few ticks' worth of time is not
    // going to on its own -- the members whose own queues would need to
    // match already had their chance. Re-queue its leaves for a fresh
    // snapshot rather than leaving the stale entry (and the leaves it
    // swallowed) parked forever.
    for (auto it = awaitingRecovery.begin(); it != awaitingRecovery.end();) {
        if (now - it->second.timestamp >= AWAITING_RECOVERY_TIMEOUT_SECS) {
            pendingRequests.insert(it->second.leaves.begin(), it->second.leaves.end());
            it = awaitingRecovery.erase(it);
        } else {
            ++it;
        }
    }

    // MEDIUM-1 fix (Fable review, 2026-10-01): a recovered batch
    // otherwise never leaves this map for the lifetime of the process --
    // bounded retention instead (recoveredBatches' own doc comment, this
    // file's own header, explains why the specific window is a
    // placeholder, not a tuned value).
    for (auto it = recoveredBatches.begin(); it != recoveredBatches.end();) {
        if (now - it->second.timestamp >= RECOVERED_BATCH_RETENTION_SECS) {
            it = recoveredBatches.erase(it);
        } else {
            ++it;
        }
    }
}

uint256 CAttestationBatchHandler::TrySignBatch() {
    std::vector <uint256> leaves;
    {
        LOCK(cs);
        SweepExpiredEntries();
        // Mutation-tested honestly: removing this check does NOT change
        // TrySignBatch's own return value on an empty queue, since
        // ComputeMerkleRoot (consensus/merkle.cpp) already returns a null
        // uint256 for zero inputs -- confirmed by reading it, not
        // assumed. What this check still avoids, unobservable from the
        // return value alone and not mockable here, is a wasted
        // AsyncSignIfMember call asking a real quorum to sign over
        // nothing.
        if (pendingRequests.empty()) {
            return uint256();
        }
        leaves.assign(pendingRequests.begin(), pendingRequests.end());
        pendingRequests.clear();
    }

    // This method runs on this class's own scheduler thread, not the main
    // thread -- cs_main is required to read the chain tip safely, matching
    // CChainLocksHandler::TrySignChainTip's own identical guard
    // (quorums_chainlocks.cpp), confirmed by reading it directly before
    // relying on the pattern rather than assumed.
    int32_t signHeight;
    {
        LOCK(cs_main);
        const CBlockIndex *pindexTip = ::ChainActive().Tip();
        if (pindexTip == nullptr) {
            // No known chain tip to bind a signing height to (e.g. a
            // freshly started node before the genesis block is even
            // connected) -- requeue rather than drop; a later tick will
            // have a real tip.
            LOCK(cs);
            pendingRequests.insert(leaves.begin(), leaves.end());
            return uint256();
        }
        signHeight = pindexTip->nHeight;
    }

    const uint256 root = ComputeCanonicalBatchRoot(leaves);
    std::sort(leaves.begin(), leaves.end());

    {
        LOCK(cs);
        RecoveredBatch &awaiting = awaitingRecovery[root];
        awaiting.leaves = leaves;
        awaiting.signHeight = signHeight;
        awaiting.timestamp = GetAdjustedTime();
    }

    // F-248: its own `llmqTypeAttestedTx` field (evo/attestedtx.h's own
    // header doc comment), not ChainLocks' own -- 5.4.4.2 does not stand
    // up a second quorum type for the batched path either, it just points
    // at the same already-live one (LLMQ_400_85) v1 now uses.
    const Consensus::LLMQType llmqType = Params().GetConsensus().llmqTypeAttestedTx;
    const uint256 id = BuildAttestationBatchId(root);

    // HIGH-3 fix (Fable review, 2026-10-01): a signature for this exact id
    // could already have recovered, network-wide, microseconds before
    // this tick even ran (enough OTHER members' own ticks may have
    // converged and recovered moments earlier) -- CRecoveredSigsListener
    // only ever fires on the EDGE of recovery, never retroactively, so
    // this node's own HandleNewRecoveredSig would otherwise never see an
    // event for a root it only just started awaiting. Checking the
    // signing manager's own already-recovered store right after
    // registering interest closes that race, dispatched through
    // HandleNewRecoveredSig itself so both paths share one acceptance
    // rule (including its own HIGH-1 quorum-binding check).
    CRecoveredSig alreadyRecovered;
    if (quorumSigningManager->GetRecoveredSigForId(llmqType, id, alreadyRecovered)) {
        HandleNewRecoveredSig(alreadyRecovered);
    } else {
        // allowReSign=true (HIGH-2, Fable review 2026-10-01): a root that
        // recurs after SweepExpiredEntries re-queues its own leaves would
        // otherwise be refused outright by CSigningManager's own 7-day
        // revote guard (quorums_signing.h) the moment it exactly matches
        // an (id, msgHash) this node already voted on before -- that
        // guard exists for a different concern (repeatedly re-asking for
        // a DIFFERENT message under the same id), never triggered here.
        //
        // LOW-3 fix (Fable review, 2026-10-01): the return value signals
        // whether this node is even a member of the quorum selected to
        // sign (false also covers "not a smartnode," already provable
        // from fSmartnodeMode alone, but a false here while
        // fSmartnodeMode is true specifically means this smartnode is not
        // part of the selected quorum this round) -- worth a log line,
        // not worth acting on: TrySignBatch itself has no different
        // action to take either way, the batch is already queued for
        // recovery regardless of whether THIS node contributes a share.
        if (!quorumSigningManager->AsyncSignIfMember(llmqType, id, root, uint256(), /*allowReSign=*/true)) {
            LogPrint(BCLog::LLMQ, "CAttestationBatchHandler::%s -- not a member of the quorum selected for "
                                  "id=%s, root=%s\n", __func__, id.ToString(), root.ToString());
        }
    }

    return root;
}

bool CAttestationBatchHandler::GetAttestation(const uint256 &msgHash, CBLSSignature &retSig, int32_t &retSignHeight,
                                              CAttestationBatchProof &retProof) const {
    LOCK(cs);
    for (const auto &entry: recoveredBatches) {
        const RecoveredBatch &batch = entry.second;
        if (std::find(batch.leaves.begin(), batch.leaves.end(), msgHash) == batch.leaves.end()) {
            continue;
        }
        retSig = batch.sig;
        retSignHeight = batch.signHeight;
        retProof = BuildAttestationBatchProof(batch.leaves, msgHash);
        return true;
    }
    return false;
}

void CAttestationBatchHandler::HandleNewRecoveredSig(const CRecoveredSig &recoveredSig) {
    if (recoveredSig.getLlmqType() != Params().GetConsensus().llmqTypeAttestedTx) {
        return;
    }
    const uint256 &root = recoveredSig.getMsgHash();
    // Confirms this recovered signature is actually an attestation-batch
    // signature, not some other (id, msgHash) pair that happens to share
    // the same llmqType -- recomputing the expected id from the candidate
    // root and comparing against the signature's own id, rather than only
    // checking root membership in awaitingRecovery below, so a hash
    // collision between an unrelated object's msgHash and one of this
    // node's own tracked roots could never be mistaken for a real match
    // (the same domain-separation discipline BuildAttestationId/
    // BuildAttestationBatchId already follow, evo/attestedtx.h).
    if (BuildAttestationBatchId(root) != recoveredSig.getId()) {
        return;
    }

    LOCK(cs);
    auto it = awaitingRecovery.find(root);
    // Not just a logic check: mutation-tested live, removing this let
    // `it->second` dereference end() on a root this node never asked to
    // be signed (e.g. another node's own, unrelated batch, now that the
    // id/llmqType filters above have both been bypassed too) -- undefined
    // behaviour that manifested as a genuine infinite loop/hang, not a
    // clean crash, confirmed by killing the actual hung process rather
    // than assumed.
    if (it == awaitingRecovery.end()) {
        return;
    }

    // HIGH-1 fix (Fable review, 2026-10-01): AsyncSignIfMember (called
    // from TrySignBatch) selects its signing quorum from whatever the
    // chain tip happens to be AT THAT EXACT CALL (quorums_signing.cpp,
    // confirmed by reading it, not assumed) -- not from `signHeight`,
    // which this class recorded moments earlier under a SEPARATE cs_main
    // lock. A block connecting in that narrow window makes the ACTUAL
    // signing quorum differ from the one `signHeight` alone would make a
    // later verifier (CheckAttestedTx) re-derive, shipping an attestation
    // that can never pass its own verification. Re-deriving the EXPECTED
    // quorum from the recorded signHeight and comparing against this
    // signature's own getQuorumHash() catches that mismatch before it is
    // ever stored. See this method's own header doc comment
    // (quorums_attestationbatch.h) for why the "quorum exists but does
    // not match" branch cannot be tested directly here (F-216) while the
    // "no quorum determinable at all" branch (quorum == nullptr) both
    // can be and is, live, via mutation testing.
    CQuorumCPtr quorum = CSigningManager::SelectQuorumForSigning(recoveredSig.getLlmqType(), root,
                                                                  it->second.signHeight);
    if (quorum == nullptr || quorum->qc->quorumHash != recoveredSig.getQuorumHash()) {
        return;
    }

    RecoveredBatch batch = it->second;
    batch.sig = recoveredSig.sig.Get();
    batch.timestamp = GetAdjustedTime();
    recoveredBatches[root] = batch;
    awaitingRecovery.erase(it);
}

} // namespace llmq
