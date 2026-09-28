// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COVERAGETELEMETRY_H
#define BITCOIN_COVERAGETELEMETRY_H

#include <serialize.h>

#include <cstdint>
#include <map>
#include <string>

/** 4.2 (docs/build-plan.md's 4.2 row; docs/transaction-decoupling.md
 *  SS14.1/SS14.5/SS14.9): passive per-peer miss rates and per-range coverage
 *  for the body-range fetch protocol (bodyrange.h, net_processing.cpp's
 *  GETBODYRANGE/BODYRANGE handling) -- the data source SS14.5's activation
 *  criterion needs. See docs/findings.md's F-206 for the original design,
 *  F-208/F-209 for the build and its security fix, and F-212 for this file's
 *  own rework (below) of the data MODEL itself, which F-209's independent
 *  review found could not actually support the honest-loss-vs-deliberate-
 *  erasure distinction SS14.9 describes, for three reasons -- this file does
 *  not classify anything itself; that stays a later consumer's job (4.6, or
 *  an operator), matching SS14.5's own "a query and a counter, not a
 *  challenge economy" framing.
 *
 *  F-212 (this rework) closes three gaps F-209 found in F-208's original
 *  data model, all in the CROSS-PEER view (net_processing.cpp's
 *  mapBodyRangeCoverage) -- the PER-PEER reliability tally just below
 *  (BodyRangeTally/RecordBodyRangeTally/BodyRangeMissRate,
 *  CNodeState::bodyRangeTally) is untouched: every round trip with a given
 *  peer genuinely IS a fact about that peer's own answering reliability, so
 *  accumulating it per-event was never the bug -- only the cross-peer,
 *  per-range view conflated distinct blocks' worth of evidence.
 *
 *  1. Per-request, not per-block, granularity. The old mapBodyRangeCoverage
 *     was keyed by CoverageRangeIndex(height) and simply incremented a
 *     hit/miss COUNTER on every observation -- so a block retried 18 times
 *     (F-185's stall threshold) before finally succeeding contributed 17
 *     misses and 1 hit to its bucket, when the real-world fact is "this one
 *     block is fully covered." Fixed by keying the cross-peer view by
 *     INDIVIDUAL HEIGHT (CoverageHeightMap below) and reducing every
 *     observation for a height to ONE deduped record
 *     (RecordCoverageObservation's own monotonic-upgrade rule) rather than
 *     an ever-growing counter -- a height's status reflects the STRONGEST
 *     evidence ever seen for it, never how many times it was asked about.
 *     COVERAGE_RANGE_SIZE-wide buckets still exist (SummarizeCoverageRange)
 *     but are now a ROLLUP computed by scanning the per-height ledger, not
 *     the ledger's own storage granularity -- which is also what makes a
 *     300-block contiguous erasure distinguishable from 300 scattered
 *     corrupt blocks (SS14.9's own example): the per-height detail is still
 *     there to query (net_processing.h's GetBodyRangeCoverageHeights) even
 *     though a bucket rollup alone could not tell them apart either.
 *
 *  2. Partial-body erasure was invisible. The old code recorded a hit the
 *     moment the FIRST chunk (nStartIndex == 0) of a range validated, with
 *     no record of whether the block's FULL body was ever actually
 *     reconstructed -- a peer serving chunk 0 and silently withholding the
 *     rest of a large block still read as a clean hit. Fixed with a THIRD
 *     status level (PARTIAL, below FULL) plus a new recording hook at
 *     net_processing.cpp's own existing body-range completion point (where
 *     acc.vBodies.size() reaches BodyRangeWantedCount(pindex) and
 *     ProcessFetchedBodyRange succeeds) -- the exact place this codebase
 *     already knows, for its own reasons (assembling the block), whether a
 *     body was ever fully reconstructed. This reuses that existing
 *     completion signal rather than re-deriving it: no new per-chunk-
 *     position tracking was added anywhere, only a new call into
 *     RecordCoverageObservation at a call site that already existed.
 *
 *  3. Tip-only accumulation, no persistence. The old mapBodyRangeCoverage
 *     was in-memory only, reset on every restart, and only ever grew from
 *     THIS node's own outbound fetches -- so a synced node barely
 *     accumulated history-wide evidence at all, exactly the opposite of
 *     what SS14.5's activation criterion needs. Fixed with
 *     CCoverageTelemetryCache below, a thin serialization wrapper matching
 *     this codebase's own established CFlatDB<T> convention
 *     (flat-database.h; sporks.dat/mncache.dat/governance.dat/
 *     netfulfilled.dat/powcache.dat all use the same whole-object dump/load
 *     pattern) rather than a new file format -- see net_processing.h's
 *     GetCoverageTelemetrySnapshot/LoadCoverageTelemetrySnapshot for how
 *     init.cpp wires this to real load-at-startup/dump-at-shutdown-and-
 *     periodically timing. Flagged, not solved here: a live, long-uptime
 *     mainnet-scale node could eventually accumulate one record per block
 *     height ever observed (worst case, a fully-covered multi-million-block
 *     chain), with no eviction/rollup policy -- matching F-208's own
 *     already-flagged, still-unbuilt bucket-eviction concern, now at
 *     per-height rather than per-bucket granularity. A real deployment
 *     wanting a bound on this needs a retention/rollup policy this phase
 *     does not attempt, since nothing yet consumes this data in a way that
 *     would tell us what that policy should look like (matching SS14.5's
 *     own "a query and a counter" scope discipline: build what the
 *     consumer needs, not what might someday be convenient). */

/** How many blocks make up one coverage-tracking bucket, for
 *  SummarizeCoverageRange's own rollup view -- unchanged from F-206's
 *  original judgment call (not a re-derivation of anything measured): a
 *  round, easily-explained default, deliberately not an attempt to align to
 *  body-file boundaries (bodystore.h's MAX_BODYFILE_SIZE) since a body
 *  file's own block count varies with real per-block body-byte occupancy. */
static const int COVERAGE_RANGE_SIZE = 1000;

/** Maps a block height to its coverage-range bucket index (floor division).
 *  `nHeight` must be >= 0 (this codebase's genesis is height 0, and no
 *  caller has a legitimate negative height); `nRangeSize` must be > 0. */
int CoverageRangeIndex(int nHeight, int nRangeSize);

/** A plain hit/miss tally -- the PER-PEER reliability metric
 *  (CNodeState::bodyRangeTally, net_processing.cpp), untouched by F-212's
 *  rework (see this file's own doc comment above for why: this is
 *  genuinely per-event, not per-block, data -- every round trip with a
 *  given peer really is a separate fact about that peer). A HIT is a
 *  validated GETBODYRANGE response that was not empty (bodyrange.h's own
 *  CBodyRange doc: no fFound field, a miss IS vBodies.empty()); a MISS is
 *  one that was. */
struct BodyRangeTally {
    uint64_t nHits = 0;
    uint64_t nMisses = 0;
};

/** Records one observed, already-validated GETBODYRANGE round trip. */
void RecordBodyRangeTally(BodyRangeTally &tally, bool fHit);

/** The observed miss rate, or -1.0 if there have been no observations yet.
 *  A caller must not read "0 misses out of 0 observations" as "0% miss
 *  rate" -- that is indistinguishable from "never asked", and SS14.9's own
 *  distinction (honest loss vs. deliberate erasure) depends on knowing
 *  which one it is. */
double BodyRangeMissRate(const BodyRangeTally &tally);

/** F-212: a single block height's own best-known coverage evidence, in the
 *  CROSS-PEER view -- the confidence ladder gap 1 and gap 2 both reduce to.
 *  Ordered by increasing strength of evidence that this height's body is
 *  genuinely available SOMEWHERE on the network, never by recency:
 *
 *   NOT_OBSERVED -- no BODYRANGE round trip has ever named this height
 *                   (the default; never actually stored -- an entry only
 *                   exists in CoverageHeightMap once something has been
 *                   observed).
 *   MISS         -- at least one first-chunk request came back empty, and
 *                   nothing has ever come back positive.
 *   PARTIAL      -- at least one peer's first chunk (nStartIndex == 0)
 *                   validated (gap 1's own "F-209's hit-after-hash-
 *                   validation" security fix is preserved -- this can only
 *                   be reached after ValidateBodyRangeChunkHashes succeeds),
 *                   but this height's FULL body has never been confirmed
 *                   reconstructed. This is gap 2's own signature: a range
 *                   stuck at PARTIAL, never advancing to FULL, is exactly
 *                   what a peer silently withholding everything past chunk
 *                   0 looks like.
 *   FULL         -- this height's complete body was confirmed reconstructed
 *                   (net_processing.cpp's ProcessFetchedBodyRange returned
 *                   true) at least once. The strongest signal this data
 *                   model has, and sticky: once reached, a later miss (a
 *                   different peer no longer serving it, say) does not
 *                   downgrade it -- the fact "someone genuinely served the
 *                   whole thing, once" remains true forever, matching
 *                   SS14.9's own "the target stays enumerable forever"
 *                   framing. Whether it is STILL served now is a live-
 *                   availability question this phase does not attempt (see
 *                   this file's own top-of-file doc comment on scope). */
enum class HeightCoverageStatus : uint8_t {
    NOT_OBSERVED = 0,
    MISS = 1,
    PARTIAL = 2,
    FULL = 3,
};

/** One height's own deduped coverage record. `nFirstObservedTime` is set
 *  once, on the first-ever observation for this height, of any kind.
 *  `nLastObservedTime` updates on EVERY observation, hit or miss, whether or
 *  not it changes `status` -- this is the "is data still flowing about this
 *  height at all" signal. `nLastChangeTime` updates only when `status`
 *  actually moves up the ladder -- this is the "when did our confidence in
 *  this height last improve" signal, and is what a later consumer diffs
 *  against SS14.9's own shape table (a STABLE gap has an old
 *  nLastChangeTime and a recent nLastObservedTime; a SHRINKING one has
 *  both moving together). */
struct CoverageHeightRecord {
    HeightCoverageStatus status = HeightCoverageStatus::NOT_OBSERVED;
    int64_t nFirstObservedTime = 0;
    int64_t nLastObservedTime = 0;
    int64_t nLastChangeTime = 0;

    SERIALIZE_METHODS(CoverageHeightRecord, obj)
    {
        uint8_t statusByte = 0;
        SER_WRITE(obj, statusByte = (uint8_t) obj.status);
        READWRITE(statusByte, obj.nFirstObservedTime, obj.nLastObservedTime, obj.nLastChangeTime);
        SER_READ(obj, obj.status = (HeightCoverageStatus) statusByte);
    }
};

/** The cross-peer coverage ledger's own storage: sparse (only heights that
 *  have ever been observed have an entry at all), keyed by absolute block
 *  height. net_processing.cpp's mapBodyRangeCoverage is exactly this type,
 *  GUARDED_BY(cs_main); see net_processing.h for the read/persistence
 *  accessors built on top of it. */
typedef std::map<int, CoverageHeightRecord> CoverageHeightMap;

/** F-212: records one observation for a single height against the cross-
 *  peer ledger -- gap 1's own dedup point. `fObserved` must not be
 *  NOT_OBSERVED (an observation always reports something). Applies the
 *  monotonic-upgrade rule (this struct's own doc, HeightCoverageStatus
 *  above): `status` only ever moves UP the confidence ladder, and a
 *  repeated or weaker observation never regresses it -- this is what
 *  reduces "17 misses then 1 hit" to a single FULL/PARTIAL record instead
 *  of 18 separate tallied events. */
void RecordCoverageObservation(CoverageHeightMap &ledger, int nHeight, HeightCoverageStatus fObserved, int64_t nNow);

/** F-212 (gap 3): folds every record in `snapshot` into `ledger`, applying
 *  the SAME monotonic-upgrade rule RecordCoverageObservation uses for a
 *  single live observation -- reused rather than re-implemented, so
 *  persisted history (loaded at startup) and live observations (recorded
 *  as BODYRANGE traffic arrives) are combined by exactly one copy of the
 *  ordering rule, never two that could silently drift apart. The intended
 *  caller is net_processing.h's LoadCoverageTelemetrySnapshot, once, at
 *  startup, folding a freshly-loaded CCoverageTelemetryCache into the
 *  (empty, at that point) live ledger -- but the merge itself makes no
 *  assumption that `ledger` starts empty, and is safe to call at any time. */
void MergeCoverageTelemetrySnapshot(CoverageHeightMap &ledger, const CoverageHeightMap &snapshot);

/** F-212: a rolled-up view of a contiguous height range, computed on demand
 *  by scanning the per-height ledger (net_processing.h's
 *  GetBodyRangeCoverageStats, the RPC's own bucket view) -- NOT maintained
 *  incrementally, unlike the old per-bucket RangeCoverageStats it replaces,
 *  since the underlying ledger is now keyed by height and a bucket is just
 *  one way of looking at a slice of it. `nFirstObservedTime` is the
 *  earliest of any height in range; `nLastChangeTime` is the latest of any
 *  height in range. */
struct CoverageRangeSummary {
    uint64_t nMissOnly = 0;   //!< distinct heights whose best-known status is MISS
    uint64_t nPartial = 0;    //!< distinct heights whose best-known status is PARTIAL
    uint64_t nFull = 0;       //!< distinct heights whose best-known status is FULL
    int64_t nFirstObservedTime = 0;
    int64_t nLastChangeTime = 0;
};

/** Rolls up `ledger`'s entries in `[nStartHeight, nEndHeightInclusive]` into
 *  one CoverageRangeSummary. A range with no observed heights at all
 *  returns a default-constructed (all-zero) summary. */
CoverageRangeSummary SummarizeCoverageRange(const CoverageHeightMap &ledger, int nStartHeight,
                                             int nEndHeightInclusive);

/** F-212 (gap 3): the on-disk cache wrapper for coveragetelemetry.dat,
 *  matching this codebase's own CFlatDB<T> convention (flat-database.h)
 *  rather than a new file format -- the same whole-object, checksummed,
 *  magic-message-tagged dump every other small non-consensus per-node cache
 *  in this tree already uses (spork.h's CSporkManager is the closest
 *  precedent this class's own Serialize/Unserialize/Clear/CheckAndRemove/
 *  ToString shape is modelled on). Plain and lock-free: never touched under
 *  cs_main itself -- net_processing.h's GetCoverageTelemetrySnapshot/
 *  LoadCoverageTelemetrySnapshot are what copy the live, guarded ledger
 *  into and out of an instance of this at startup/shutdown (init.cpp). */
class CCoverageTelemetryCache {
public:
    CoverageHeightMap mapHeightStatus;

    template<typename Stream>
    void Serialize(Stream &s) const {
        s << SERIALIZATION_VERSION_STRING << mapHeightStatus;
    }

    template<typename Stream>
    void Unserialize(Stream &s) {
        std::string strVersion;
        s >> strVersion;
        if (strVersion != SERIALIZATION_VERSION_STRING) {
            // An unrecognised format is treated the same as "nothing
            // usable here" -- matching CSporkManager::Unserialize's own
            // precedent (spork.cpp) of silently starting empty rather than
            // throwing, since this is a re-accumulable cache, not durable
            // state a node cannot safely run without (this file's own
            // top-of-file doc comment: "a query and a counter", not a
            // security property).
            mapHeightStatus.clear();
            return;
        }
        s >> mapHeightStatus;
    }

    /** CFlatDB<T>'s own required interface (flat-database.h) -- fulfils it
     *  the same way CSporkManager::Clear does: resets in-memory state only,
     *  never touches the file itself. */
    void Clear();

    /** CFlatDB<T>'s own required interface. There is no signature-style
     *  validity check that applies to telemetry data the way it does to a
     *  spork message (CSporkManager::CheckAndRemove's own doc) -- a
     *  deserialized CoverageHeightRecord is either well-formed (the format
     *  check in Unserialize above already guards that) or the whole load
     *  was already rejected, so this is a deliberate no-op, not an
     *  oversight. */
    void CheckAndRemove();

    std::string ToString() const;

private:
    static const std::string SERIALIZATION_VERSION_STRING;
};

#endif // BITCOIN_COVERAGETELEMETRY_H
