// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BODYRANGE_H
#define BITCOIN_BODYRANGE_H

#include <blockencodings.h>
#include <flatfile.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <vector>

/** 2.2.2 (F-143's accepted wire-format spec, five review rounds -- see
 *  docs/findings.md's F-143/F-144/F-145): the fetch protocol's request and
 *  response payloads. Hash-form addressing ONLY -- the original spec's
 *  height-addressing form was dropped by explicit owner decision during
 *  review, since every requester already holds the block hash it wants and
 *  2.2.1's own index (bodystore.h) resolves a hash to the same `FlatFilePos`
 *  a height lookup would, lock-free, regardless of chain position.
 *
 *  This header delivers the WIRE CONTRACT only -- the message-type names
 *  (`NetMsgType::GETBODYRANGE`/`BODYRANGE`, protocol.h/.cpp) and the
 *  request-validation classification below. It deliberately does NOT
 *  dispatch these messages anywhere (no `ProcessMessage` arm exists for
 *  them yet) and does NOT serve a response body -- both are 2.2.3's job,
 *  since serving requires resolving through 2.2.1's index and reading the
 *  body-store's own files, off `cs_main`, which needs a real network
 *  handler this header has no reason to depend on.
 *
 *  Index space: body index `i` is the index into the commitment block's own
 *  identifier list -- the SAME index space 2.2.1's own store already uses
 *  (`ReadBodyAt`, bodystore.h). Since the coinbase is always index 0 of that
 *  identifier list and is never stored in the body store, body index `i`
 *  names the same transaction as `vtx[i+1]` in a full `CBlock`. See
 *  `VtxIndexFromBodyIndex`/`BodyIndexFromVtxIndex` below -- named explicitly,
 *  matching F-115's own precedent, so nowhere re-derives this off-by-one by
 *  hand. */

/** The `getbodyrange` request payload. */
class CGetBodyRange {
public:
    uint256 hashBlock;
    uint32_t nStartIndex;  //!< first BODY index wanted (NOT a vtx index -- see "Index space" above)
    uint32_t nCount;       //!< how many, from nStartIndex; the server enforces its own byte-based
                           //!< ceiling and may serve fewer than requested (2.2.3's continuation contract)

    SERIALIZE_METHODS(CGetBodyRange, obj)
    {
        READWRITE(obj.hashBlock, obj.nStartIndex, obj.nCount);
    }
};

/** The `bodyrange` response payload. No `fFound` field, deliberately (F-143's
 *  v2-to-v3 revision): an honest requester never sends a request this struct
 *  could answer "found, but zero bodies" to (both request-validation BAN
 *  cases below make that impossible) -- a miss is simply `vBodies.empty()`,
 *  with no wire-representable disagreement state possible between two
 *  separate fields. */
class CBodyRange {
public:
    uint256 hashBlock;      //!< echoes the request -- confirms which block was actually served,
                            //!< and is what makes the body's own hash verifiable (§5.2)
    uint32_t nStartIndex;   //!< echoed from the request
    std::vector<CTransactionRef> vBodies;  //!< empty means a miss

    SERIALIZE_METHODS(CBodyRange, obj)
    {
        READWRITE(obj.hashBlock, obj.nStartIndex, Using<VectorFormatter<TransactionCompression>>(obj.vBodies));
    }
};

/** Body index `i` (2.2.1's own store-level index, `ReadBodyAt`) names the
 *  same transaction as `vtx[i+1]` in a full `CBlock` -- the coinbase is
 *  always `vtx[0]` and is never stored in the body store. */
static inline uint32_t VtxIndexFromBodyIndex(uint32_t bodyIndex) { return bodyIndex + 1; }

/** Inverse of the above. `vtxIndex` must be `>= 1` -- there is no body index
 *  for the coinbase itself, `vtx[0]`. */
static inline uint32_t BodyIndexFromVtxIndex(uint32_t vtxIndex) { return vtxIndex - 1; }

/** F-143's own request-validation classification, decided in this exact
 *  order (each tier only reachable once the tiers above it are ruled out):
 *
 *  - `BAN`: genuinely impossible for an honest requester to trigger --
 *    `nCount == 0`, or `nStartIndex + nCount` overflows `uint32_t`. Decided
 *    FIRST, with no `cs_main` and no I/O of any kind.
 *  - `MISS`: `hashBlock` resolves to nothing in the body-store index at all,
 *    OR it resolves but isn't currently serveable (a withheld block, F-144's
 *    own serveability flag) -- both read via `LookupServeableBodyPositionByHash`
 *    alone (bodystore.h: mutex-only, no `cs_main`, no file I/O). An unknown
 *    hash is exactly what an honest requester sends during ordinary
 *    header-propagation races (C1's own required parallel multi-source
 *    fetch), so this must never be a `BAN` -- `LookupBlockIndex`/`cs_main`
 *    must NEVER appear anywhere in this function.
 *  - `BAN`: `nStartIndex` at or past the resolved, SERVEABLE block's own
 *    body-record count -- this is the only tier with real I/O cost
 *    (`ReadBodyRecordCount` opens the body file), and is honest-proof only
 *    here, since an honest requester always knows a serveable block's own
 *    body count from its own commitment-block identifier list. A
 *    `ReadBodyRecordCount` failure (a corrupt on-disk record) is a `MISS`,
 *    never a `BAN` -- the requester cannot distinguish "corrupt" from
 *    "server-side problem" and did nothing wrong by asking.
 *  - `OK`: none of the above -- `posOut` is the resolved position, the range
 *    `[nStartIndex, nStartIndex+nCount)` may extend past the record's own
 *    count (2.2.3's own truncate-to-fit continuation contract handles that,
 *    not this function).
 *
 *  This function does no network I/O and takes no lock beyond the body
 *  store's own `cs_bodyIndex` (held only for the lookup itself) -- testable
 *  without a live peer, exactly as F-143's own deliverables list requires. */
enum class GetBodyRangeValidation {
    BAN,
    MISS,
    OK,
};

GetBodyRangeValidation ValidateGetBodyRange(const CGetBodyRange &request, FlatFilePos &posOut);

/** 2.2.3 (serving handler): builds the response for an OK-classified
 *  request. The caller must already hold a `GetBodyRangeValidation::OK` from
 *  `ValidateGetBodyRange` for this exact `request`/`pos` pair -- this
 *  function trusts that and does no re-validation, including of the overflow
 *  invariant `ValidateGetBodyRange`'s own BAN tier already enforced
 *  (`nStartIndex + nCount` fits in `uint32_t`).
 *
 *  A thin wrapper over `bodystore.h`'s own `ReadBodyRange` -- see that
 *  function's doc comment for the exact stop conditions (nCount reached, the
 *  byte ceiling reached with the first body always force-included
 *  regardless of its own size, or the record's real end reached). All three
 *  stop conditions produce identical wire behaviour: `respOut.vBodies` holds
 *  whatever was read so far, possibly fewer than `request.nCount`, possibly
 *  empty. There is no separate wire signal for "there is more" -- a client
 *  infers continuation from getting back fewer bodies than it asked for and
 *  re-requests starting after what it received (F-147's own deferred item:
 *  truncate, never leave a hole).
 *
 *  F-150 (2.2.3a's own Fable review, HIGH): an earlier version of this
 *  function called `ReadBodyAt` in a loop, which re-opens the record and
 *  re-reads its ENTIRE offset table on every single call -- an O(nCount x
 *  the record's own real body count) cost, measured at ~12s of real CPU on
 *  the message-handler thread for one network request against a
 *  100,000-body record. `ReadBodyRange` opens the record and reads its
 *  header exactly once, then streams -- O(record size) total.
 *
 *  Off `cs_main` and every chain-state lock -- reads only through the
 *  caller-supplied `FlatFilePos`, exactly like `ValidateGetBodyRange` itself.
 */
void BuildBodyRangeResponse(const CGetBodyRange &request, const FlatFilePos &pos,
                             uint64_t nByteCeiling, CBodyRange &respOut);

/** 2.2.4 (the fetching CLIENT's own counterpart to ValidateGetBodyRange,
 *  build-plan.md's 2.2 row): checks a BODYRANGE response's WIRE SHAPE
 *  against the CGetBodyRange that solicited it, before any attempt to
 *  MaterialiseBlock the bodies it carries. Purely a shape check -- it does
 *  NOT know whether this exact request was ever actually sent to the peer
 *  that answered (the caller's own in-flight bookkeeping's job, since that
 *  needs a NodeId this function has no reason to depend on) and does NOT
 *  validate the bodies' own content against the block's committed
 *  identifiers (MaterialiseBlock's job, once this passes).
 *
 *  Fails (returns false) if:
 *   - `response.hashBlock` does not echo `request.hashBlock` -- an honest
 *     server always echoes what it actually served (bodyrange.h's own
 *     CBodyRange::hashBlock doc, F-143's "response-must-echo" rule); a
 *     mismatch here is never innocent for THIS specific request/response
 *     pairing, whatever else might explain a stale or reordered message;
 *   - `response.nStartIndex` does not echo `request.nStartIndex` -- there
 *     is no wire reason it should ever legitimately differ;
 *   - `response.vBodies.size()` exceeds `request.nCount` -- over-delivery.
 *     `BuildBodyRangeResponse`'s own truncate-only continuation contract
 *     can only ever return FEWER bodies than asked for, never more, so
 *     this can only mean the response was not built by that function (or
 *     wasn't for this request);
 *   - any entry in `response.vBodies` is a null `CTransactionRef` -- a
 *     genuine mid-range read failure on the SERVING side truncates rather
 *     than leaving a hole (bodyrange.h's own `BuildBodyRangeResponse` doc,
 *     F-147's deferred requirement) -- enforced here, on the reading side,
 *     since nothing about how a response is BUILT changes for 2.2.4.
 *
 *  An EMPTY `vBodies` (an honest miss, per `CBodyRange`'s own doc) always
 *  passes this shape check: it echoes hashBlock/nStartIndex like any other
 *  honest response and zero bodies is never over-delivery. Telling an
 *  honest miss apart from a withholding announcer is the CALLER's own job
 *  (2.2.4's announcer-ring classification), not this function's. */
bool ValidateBodyRangeResponse(const CGetBodyRange &request, const CBodyRange &response);

/** F-158 (independent second-model review of F-155/F-156/F-157, CONFIRMED
 *  HIGH): `ValidateBodyRangeResponse` above deliberately does NOT validate a
 *  chunk's body content against the block's committed identifiers -- that
 *  was left to `MaterialiseBlock`, run once at completion over the WHOLE
 *  accumulated `mapBodyRangePartial` buffer. That buffer is keyed only by
 *  block hash, with no record of which peer contributed which chunk (a
 *  block whose full body needs more than one GETBODYRANGE round trip can be
 *  completed by a DIFFERENT peer than the one who supplied an earlier
 *  chunk -- single-source only means one request in flight at a time per
 *  hash, not one peer for a hash's entire lifetime). Consequence: an
 *  attacker answers an early chunk with bad data then disconnects (or
 *  simply lets its own in-flight slot get reaped) -- FinalizeNode/the
 *  reaper both deliberately keep `mapBodyRangePartial` on the theory that
 *  bodies already received are "already shape-validated" and so still
 *  good, but shape validation (`ValidateBodyRangeResponse`) was never
 *  CONTENT validation. An honest peer later completes the range;
 *  `MaterialiseBlock`'s own hash check fails on the COMBINED data; the
 *  completion-time `Misbehaving(pfrom, 100, ...)` call bans the honest
 *  completing peer for the departed attacker's bad chunk.
 *
 *  Fixed by validating each chunk's own hashes against the correct SLICE of
 *  `commitments.vCommitments` (`nStartIndex .. nStartIndex+chunkBodies.size()`)
 *  the moment it arrives, before it is ever appended to the cross-peer
 *  accumulation buffer -- the same per-tx hash check `MaterialiseBlock`
 *  (primitives/block.cpp) already does over a block's FULL body list,
 *  applied incrementally per chunk instead of once at the end. A failure
 *  here is unambiguously the CURRENTLY-ANSWERING peer's fault (this exact
 *  chunk, from this exact response, checked before touching any
 *  previously-accumulated data from a possibly-different peer) -- the
 *  caller can misbehave that peer directly and, since earlier chunks were
 *  already validated at their OWN arrival time, keep the still-good prefix
 *  and simply not append the bad chunk, rather than discarding everything
 *  accumulated so far. Once every chunk has passed this check on arrival,
 *  `MaterialiseBlock`'s own final check at completion is provably
 *  redundant (same predicate, already applied to every element) --
 *  matching F-157's own precedent of a defense-in-depth check the current
 *  call pattern can no longer actually trip. */
bool ValidateBodyRangeChunkHashes(const CCommitmentBlock &commitments, uint32_t nStartIndex,
                                  const std::vector<CTransactionRef> &chunkBodies);

/** 2.2.4 (build-plan.md's 2.2 row): the default aggregate cap on in-flight
 *  GETBODYRANGE requests across ALL peers at once (net_processing.cpp's own
 *  fetching client) -- not per-peer, a separate, still-unbuilt concern.
 *  Shared between net_processing.cpp (the cap's own enforcement) and
 *  init.cpp (the -maxbodyrangeinflight help text default), matching
 *  DEFAULT_CHECKBLOCKS's own cross-file placement (validation.h). Single-
 *  source fetching has no reason to chase many bodies at once, so the
 *  default is conservative. */
static const unsigned int DEFAULT_MAX_BODYRANGE_INFLIGHT = 16;

/** 2.2.4: the aggregate concurrent-chase cap's own eligibility check for a
 *  single (peer, block) candidate -- pure decision logic, deliberately
 *  taking every input as a plain value rather than reaching into
 *  CNodeState/CAnnouncerRing/g_body_retry_state itself, so it is testable
 *  without any net_processing.cpp scaffolding (this project's own
 *  established split, matching ValidateGetBodyRange/BuildBodyRangeResponse).
 *
 *  `fWasAnnounced`: the candidate peer's own CAnnouncerRing::WasAnnounced
 *  verdict for this block -- 2.2.4 is single-source only (no multi-source
 *  scoring, a later phase, build-plan.md's own scope note), so a peer that
 *  never announced this hash is never a candidate, however otherwise idle.
 *  `nNow`/`nNextAttempt`: the re-keyed g_body_retry_state entry for this
 *  exact (peer, hash) pair -- reuses net_processing.cpp's EXISTING backoff
 *  arithmetic unchanged (only what the map is keyed on changed, per
 *  build-plan.md's 2.2.4 row); this function does not compute a backoff
 *  itself, it only asks whether the existing `nNextAttempt` has passed.
 *  `nInFlight`/`nMaxInFlight`: the aggregate cap, checked LAST since it is a
 *  global resource limit unrelated to this specific peer/hash pair's own
 *  eligibility -- a peer that is otherwise perfectly eligible still does
 *  not get a request once the cap is saturated. */
bool ShouldRequestBodyRange(bool fWasAnnounced, int64_t nNow, int64_t nNextAttempt,
                             unsigned int nInFlight, unsigned int nMaxInFlight);

/** B3's per-block exponential backoff (docs/transaction-decoupling.md SS14.8:
 *  "must not key off accumulated work"), re-keyed to (peer, hash) rather
 *  than a bare hash (net_processing.cpp's own re-keyed g_body_retry_state --
 *  build-plan.md's 2.2.4 row: "re-keying the existing g_body_retry_state
 *  backoff to body-ranges, not new backoff logic"). This is the SAME
 *  arithmetic 1.3.6/H-2's original whole-block call site used inline
 *  (`BODY_RETRY_BASE_MICROS << std::min(nAttempts - 1, 5U)`, clamped to
 *  `nMaxMicros`) -- reused verbatim, not reinvented, factored out here as a
 *  named, independently testable function rather than copy-pasted inline a
 *  second time at 2.2.4's own single-source GETBODYRANGE call site
 *  (SendMessages), which is this function's only real caller today: the
 *  original whole-block site no longer backs off at all (its own comment,
 *  net_processing.cpp -- that failure mode moved entirely to the
 *  GETBODYRANGE path once a commitment-only block's missing body became
 *  THIS mechanism's job instead). `nAttempts` must be >= 1 -- the caller
 *  increments its own counter before calling, matching the original inline
 *  call site's own order. */
int64_t NextBodyRetryBackoffMicros(unsigned int nAttempts, int64_t nBaseMicros, int64_t nMaxMicros);

/** 2.2.4: is an in-flight GETBODYRANGE request against a peer stale enough
 *  to reap -- a peer that stays CONNECTED but never answers at all is not
 *  caught by any existing mechanism: `PeerLogicValidation::FinalizeNode`
 *  only frees a `mapBodyRangeInFlight` slot on disconnect, and the existing
 *  whole-block stalling/download timeouts (`BLOCK_STALLING_TIMEOUT`,
 *  `BLOCK_DOWNLOAD_TIMEOUT_*`) are driven by `state.nBlocksInFlight` /
 *  `vBlocksInFlight`, which a GETBODYRANGE request never touches
 *  (`mapBodyRangeInFlight` is a wholly separate accounting structure, only
 *  ever populated/cleared by 2.2.4's own code). Left unreaped, a single
 *  silently-unresponsive-but-connected peer permanently narrows the
 *  aggregate cap (`nBodyRangeInFlight`) by one slot for as long as it stays
 *  connected -- across enough such peers, exhausts it entirely.
 *
 *  Pure, taking every input as a plain value (this project's own established
 *  split, matching `ShouldRequestBodyRange`) -- `nStaleAfterMicros` is the
 *  caller's own choice of how long is too long; net_processing.cpp's own
 *  caller reuses `BODY_RETRY_MAX_MICROS` (the existing backoff ceiling)
 *  rather than a new constant: "longer than the longest gap this mechanism
 *  would ever wait between its OWN retries" is already an established
 *  notion of "too long" here, not an invented one -- matching build-plan.md
 *  2.2.4's own "not new backoff logic" instruction in spirit, even though
 *  this isn't backoff arithmetic itself. */
bool IsBodyRangeRequestStale(int64_t nRequestTime, int64_t nNow, int64_t nStaleAfterMicros);

/** F-157 (Fable review of F-155/F-156): `FindNextBlocksToDownload`'s own
 *  staller-detection blames an unrelated peer (`waitingfor`) for THIS
 *  peer's lack of progress once its whole-block candidate list (`vBlocks`)
 *  comes back empty at the download window's end -- but 2.2.4 added a
 *  SECOND, independent source of real progress for the exact same peer
 *  (`vBodyBlocks`, the single-source GETBODYRANGE candidates), which the
 *  original check never consulted: a peer with genuine outstanding
 *  body-range work queued was still treated as evidence of an unrelated
 *  peer stalling, purely because the WHOLE-BLOCK list happened to be empty.
 *  Pure, taking both candidate counts as plain values (this project's own
 *  established split) rather than reaching into `FindNextBlocksToDownload`'s
 *  own local `vBlocks`/`vBodyBlocks` -- true only when NEITHER fetch path
 *  found anything for this peer to do. */
bool HasOutstandingBlockDownloadWork(size_t nWholeBlockCandidates, size_t nBodyRangeCandidates);

/** F-159 (second independent review of F-158, CONFIRMED MEDIUM): whether a
 *  chunk response is positioned to append cleanly onto a cross-peer
 *  accumulation buffer's own CURRENT size -- distinct from
 *  ValidateBodyRangeResponse's own nStartIndex check, which only confirms
 *  the response echoes what THIS SPECIFIC REQUEST asked for
 *  (mapBodyRangeInFlight's own recorded value), never whether the
 *  accumulation buffer itself still matches that expectation. If the
 *  buffer was reset between this request being issued and this response
 *  arriving (the abandonment reaper racing a still-outstanding request is
 *  one such path, though a dedicated guard there closes that specific
 *  case too) -- an honestly-answered, hash-valid chunk appended at
 *  `end()` regardless would silently land at the wrong logical offset,
 *  corrupting the assembly for a LATER, entirely innocent peer to be
 *  banned over. Pure, taking both sizes as plain values (this project's
 *  own established split) so the guard is testable and mutation-provable
 *  independent of any net_processing.cpp scaffolding. */
bool IsBodyRangeChunkAligned(size_t nAccumulatedSoFar, uint32_t nResponseStartIndex);

/** F-185 (4.1.4, build-plan.md's 4.1.4 row): whole-block download disconnects
 *  a peer that stalls or never resolves an in-flight fetch
 *  (BLOCK_STALLING_TIMEOUT, BLOCK_DOWNLOAD_TIMEOUT_BASE/_PER_PEER,
 *  net_processing.cpp) -- GETBODYRANGE's own retry state
 *  (BodyRetryState::nAttempts, net_processing.cpp) shares no fields with
 *  that mechanism (confirmed by grep), so a peer that persistently stalls
 *  or never resolves a body-range fetch was never actually disconnected,
 *  only internally backed off. This is the trigger check, pure (this
 *  project's own established split, matching ShouldRequestBodyRange/
 *  IsBodyRangeRequestStale/HasOutstandingBlockDownloadWork/
 *  IsBodyRangeChunkAligned above) -- see net_processing.cpp's own
 *  BODY_RANGE_DISCONNECT_ATTEMPTS doc comment for why 18 is the chosen
 *  threshold (a deliberate multiple of where NextBodyRetryBackoffMicros's
 *  own backoff shape saturates, not an arbitrary number). */
bool ShouldDisconnectForBodyRangeAttempts(unsigned int nAttempts, unsigned int nDisconnectThreshold);

#endif // BITCOIN_BODYRANGE_H
