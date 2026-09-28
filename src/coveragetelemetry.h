// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COVERAGETELEMETRY_H
#define BITCOIN_COVERAGETELEMETRY_H

#include <cstdint>

/** 4.2 (docs/build-plan.md's 4.2 row; docs/transaction-decoupling.md
 *  SS14.1/SS14.5/SS14.9): passive per-peer miss rates and per-range coverage
 *  for the body-range fetch protocol (bodyrange.h, net_processing.cpp's
 *  GETBODYRANGE/BODYRANGE handling) -- the data source SS14.5's activation
 *  criterion needs and, per a grep of this tree before this file existed,
 *  had none of. See net_processing.cpp's own doc comments at the two
 *  observation points (the BODYRANGE response handler) for exactly where
 *  this hooks in, and docs/findings.md's F-206 for the design writeup and
 *  the honest-loss-vs-deliberate-erasure distinction (SS14.9's own table)
 *  this file exists to preserve DATA for -- it does not classify anything
 *  itself; that is a later consumer's job (4.6, or an operator).
 *
 *  Scope correction (against this phase's own one-line brief, build-plan.md's
 *  4.2 row, which describes the fetch protocol as built around "GETBODYRANGE
 *  requests for ranges of blocks"): that is not what bodyrange.h's own
 *  CGetBodyRange is. Its nStartIndex/nCount range a single block's own body
 *  INDICES (one hashBlock per request) -- never a span of multiple blocks.
 *  The "range" SS14.9's honest-loss/deliberate-erasure table means is a
 *  different unit entirely: a contiguous span of block HEIGHTS ("oldest-
 *  first", "mid-history", "file-aligned" are all height-axis properties).
 *  This file introduces its OWN notion of a coverage range -- a fixed-size
 *  bucket of consecutive block heights -- and deliberately does not reuse
 *  bodyrange.h's CGetBodyRange::nStartIndex/nCount for it, since conflating
 *  the two would silently misname what is being measured. */

/** How many blocks make up one coverage-tracking bucket. A judgment call, not
 *  a re-derivation of anything measured: SS14.9's own shape table (file-
 *  aligned, mid-history, contiguous, ...) needs some grouping coarser than
 *  "one block" for a pattern to be visible in at all, and 1,000 is a round,
 *  easily-explained default with nothing else pulling it one way or the
 *  other. Deliberately NOT an attempt to align to body-file boundaries
 *  (bodystore.h's MAX_BODYFILE_SIZE, 128 MiB): a body file's own block count
 *  varies with real per-block body-byte occupancy, so no fixed block count
 *  tracks it exactly. A later consumer wanting true file-alignment must
 *  cross-reference this bucketing against the store's own recorded
 *  file/height mapping separately -- this phase only needs to preserve
 *  height as the observation's key, not pre-compute file alignment. */
static const int COVERAGE_RANGE_SIZE = 1000;

/** Maps a block height to its coverage-range bucket index (floor division).
 *  `nHeight` must be >= 0 (this codebase's genesis is height 0, and no
 *  caller has a legitimate negative height); `nRangeSize` must be > 0. */
int CoverageRangeIndex(int nHeight, int nRangeSize);

/** A plain hit/miss tally -- the shape both the per-peer and per-range-bucket
 *  telemetry below reduce to. A HIT is a validated GETBODYRANGE response
 *  that was not empty (bodyrange.h's own CBodyRange doc: no fFound field, a
 *  miss IS vBodies.empty()); a MISS is one that was. */
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

/** Per-coverage-range-bucket telemetry: the same hit/miss tally as above,
 *  plus timestamps a later consumer needs to tell a SHRINKING gap (SS14.9:
 *  a node still backfilling, honest) from a STABLE one (SS14.9: deliberate
 *  erasure) apart. This phase does not compute that trend itself (SS14.5:
 *  "a query and a counter, not a challenge economy") -- it exposes the raw
 *  timestamps for a caller polling repeatedly over time to diff. */
struct RangeCoverageStats {
    BodyRangeTally tally;
    int64_t nFirstObservedTime = 0;  //!< first observation ever recorded for this bucket
    int64_t nLastHitTime = 0;        //!< most recent HIT, 0 if none yet
    int64_t nLastMissTime = 0;       //!< most recent MISS, 0 if none yet
};

/** Records one observation against a coverage bucket. `nNow` is the caller's
 *  own current time (GetTimeMicros() at the real net_processing.cpp call
 *  site; parameterised here so this stays pure and testable without a wall
 *  clock, matching this project's own established split -- e.g.
 *  bodyrange.h's IsBodyRangeRequestStale). */
void RecordCoverageObservation(RangeCoverageStats &stats, bool fHit, int64_t nNow);

#endif // BITCOIN_COVERAGETELEMETRY_H
