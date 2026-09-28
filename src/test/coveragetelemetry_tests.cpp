// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// 4.2 (docs/build-plan.md's 4.2 row; F-206); F-212 (rework, docs/findings.md):
// the pure, testable logic behind passive per-peer miss-rate and per-height
// cross-peer coverage telemetry -- bucket indexing, tally recording,
// miss-rate computation, per-height deduped observation recording
// (F-212's own gap 1/gap 2 fix), range rollups, persistence-snapshot
// merging, and (de)serialization (F-212's own gap 3 fix). The
// net_processing.cpp call sites that feed real GETBODYRANGE/BODYRANGE
// traffic into this have no dedicated unit coverage here, matching this
// project's own established convention for message-processing glue
// (bodyrange_tests.cpp's own precedent, GETBODYRANGE/BODYRANGE
// ProcessMessage dispatch) -- every byte of actual decision logic lives here
// instead, where it is fully testable without CNode/CConnman scaffolding.
// The completion-hook end-to-end wiring (gap 2's own net_processing.cpp
// call site) IS driven through a real handler pass, in
// bodyrange_hit_recording_tests.cpp, matching that file's own established
// exception to the same convention (its own doc comment explains why).

#include <clientversion.h>
#include <coveragetelemetry.h>
#include <streams.h>
#include <test/test_raptoreum.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(coveragetelemetry_tests, BasicTestingSetup)

// -- CoverageRangeIndex --------------------------------------------------

BOOST_AUTO_TEST_CASE(range_index_of_height_zero_is_bucket_zero) {
    BOOST_CHECK_EQUAL(CoverageRangeIndex(0, 1000), 0);
}

BOOST_AUTO_TEST_CASE(range_index_stays_in_bucket_zero_up_to_the_boundary) {
    BOOST_CHECK_EQUAL(CoverageRangeIndex(999, 1000), 0);
}

BOOST_AUTO_TEST_CASE(range_index_rolls_over_exactly_at_the_boundary) {
    // Boundary: height == nRangeSize must land in bucket 1, not bucket 0 --
    // proves this is `/`, not an off-by-one `(height-1)/size` or similar.
    BOOST_CHECK_EQUAL(CoverageRangeIndex(1000, 1000), 1);
}

BOOST_AUTO_TEST_CASE(range_index_of_a_high_block_uses_the_right_bucket) {
    BOOST_CHECK_EQUAL(CoverageRangeIndex(2500, 1000), 2);
    BOOST_CHECK_EQUAL(CoverageRangeIndex(1999, 1000), 1);
    BOOST_CHECK_EQUAL(CoverageRangeIndex(2000, 1000), 2);
}

BOOST_AUTO_TEST_CASE(range_index_respects_a_different_range_size) {
    // Mutation-found-gap guard: a test that only ever passes 1000 would
    // survive a mutant that hardcodes 1000 instead of using nRangeSize.
    BOOST_CHECK_EQUAL(CoverageRangeIndex(250, 100), 2);
    BOOST_CHECK_EQUAL(CoverageRangeIndex(99, 100), 0);
    BOOST_CHECK_EQUAL(CoverageRangeIndex(100, 100), 1);
}

// -- BodyRangeTally / RecordBodyRangeTally / BodyRangeMissRate -----------

BOOST_AUTO_TEST_CASE(fresh_tally_is_all_zero) {
    BodyRangeTally tally;
    BOOST_CHECK_EQUAL(tally.nHits, 0U);
    BOOST_CHECK_EQUAL(tally.nMisses, 0U);
}

BOOST_AUTO_TEST_CASE(recording_a_hit_increments_only_hits) {
    BodyRangeTally tally;
    RecordBodyRangeTally(tally, /*fHit=*/true);
    BOOST_CHECK_EQUAL(tally.nHits, 1U);
    BOOST_CHECK_EQUAL(tally.nMisses, 0U);
}

BOOST_AUTO_TEST_CASE(recording_a_miss_increments_only_misses) {
    BodyRangeTally tally;
    RecordBodyRangeTally(tally, /*fHit=*/false);
    BOOST_CHECK_EQUAL(tally.nHits, 0U);
    BOOST_CHECK_EQUAL(tally.nMisses, 1U);
}

BOOST_AUTO_TEST_CASE(tally_accumulates_across_many_observations) {
    BodyRangeTally tally;
    for (int i = 0; i < 7; i++) RecordBodyRangeTally(tally, true);
    for (int i = 0; i < 3; i++) RecordBodyRangeTally(tally, false);
    BOOST_CHECK_EQUAL(tally.nHits, 7U);
    BOOST_CHECK_EQUAL(tally.nMisses, 3U);
}

BOOST_AUTO_TEST_CASE(miss_rate_is_sentinel_with_no_observations) {
    BodyRangeTally tally;
    BOOST_CHECK_EQUAL(BodyRangeMissRate(tally), -1.0);
}

BOOST_AUTO_TEST_CASE(miss_rate_is_zero_with_only_hits) {
    BodyRangeTally tally;
    RecordBodyRangeTally(tally, true);
    RecordBodyRangeTally(tally, true);
    BOOST_CHECK_EQUAL(BodyRangeMissRate(tally), 0.0);
}

BOOST_AUTO_TEST_CASE(miss_rate_is_one_with_only_misses) {
    BodyRangeTally tally;
    RecordBodyRangeTally(tally, false);
    BOOST_CHECK_EQUAL(BodyRangeMissRate(tally), 1.0);
}

BOOST_AUTO_TEST_CASE(miss_rate_is_the_correct_fraction_for_a_mix) {
    BodyRangeTally tally;
    tally.nHits = 3;
    tally.nMisses = 1;
    BOOST_CHECK_CLOSE(BodyRangeMissRate(tally), 0.25, 0.0001);
}

// -- RecordCoverageObservation (F-212 rework) -----------------------------
//
// F-212 replaces the old bucket-level, event-accumulating
// RecordCoverageObservation(RangeCoverageStats&, bool, int64_t) with a
// per-HEIGHT, deduped, monotonic-upgrade-only model -- see
// coveragetelemetry.h's own top-of-file doc comment for the full mapping
// from F-209's three flagged gaps to what changed here.

BOOST_AUTO_TEST_CASE(fresh_ledger_has_no_entry_for_an_unobserved_height) {
    CoverageHeightMap ledger;
    BOOST_CHECK(ledger.find(100) == ledger.end());
}

BOOST_AUTO_TEST_CASE(one_observation_creates_exactly_one_entry) {
    CoverageHeightMap ledger;
    RecordCoverageObservation(ledger, 100, HeightCoverageStatus::MISS, 1000);
    BOOST_CHECK_EQUAL(ledger.size(), 1U);
    BOOST_REQUIRE(ledger.find(100) != ledger.end());
    BOOST_CHECK(ledger.at(100).status == HeightCoverageStatus::MISS);
}

BOOST_AUTO_TEST_CASE(first_observed_time_is_set_once_and_never_moves) {
    CoverageHeightMap ledger;
    RecordCoverageObservation(ledger, 100, HeightCoverageStatus::MISS, 1000);
    RecordCoverageObservation(ledger, 100, HeightCoverageStatus::PARTIAL, 2000);
    BOOST_CHECK_EQUAL(ledger.at(100).nFirstObservedTime, 1000);
}

BOOST_AUTO_TEST_CASE(last_observed_time_updates_on_every_call_even_without_an_upgrade) {
    CoverageHeightMap ledger;
    RecordCoverageObservation(ledger, 100, HeightCoverageStatus::FULL, 1000);
    // A later, WEAKER observation (e.g. a differently-behaving peer) must
    // not change status, but must still move nLastObservedTime -- this is
    // the "is data still flowing about this height" signal, independent of
    // the "best-known confidence" signal.
    RecordCoverageObservation(ledger, 100, HeightCoverageStatus::MISS, 5000);
    BOOST_CHECK(ledger.at(100).status == HeightCoverageStatus::FULL);
    BOOST_CHECK_EQUAL(ledger.at(100).nLastObservedTime, 5000);
}

// The core dedup guarantee (gap 1): F-209's own example -- 17 misses then 1
// hit for the SAME height must reduce to one record, not 18 tallied events.
BOOST_AUTO_TEST_CASE(seventeen_misses_then_one_hit_dedupe_to_a_single_partial_record) {
    CoverageHeightMap ledger;
    for (int i = 0; i < 17; i++) {
        RecordCoverageObservation(ledger, 500, HeightCoverageStatus::MISS, 1000 + i);
    }
    RecordCoverageObservation(ledger, 500, HeightCoverageStatus::PARTIAL, 2000);

    BOOST_CHECK_EQUAL(ledger.size(), 1U);
    BOOST_CHECK(ledger.at(500).status == HeightCoverageStatus::PARTIAL);
}

// Monotonic upgrade: status only ever moves UP the confidence ladder.
BOOST_AUTO_TEST_CASE(status_upgrades_miss_to_partial_to_full_in_order) {
    CoverageHeightMap ledger;
    RecordCoverageObservation(ledger, 1, HeightCoverageStatus::MISS, 100);
    BOOST_CHECK(ledger.at(1).status == HeightCoverageStatus::MISS);

    RecordCoverageObservation(ledger, 1, HeightCoverageStatus::PARTIAL, 200);
    BOOST_CHECK(ledger.at(1).status == HeightCoverageStatus::PARTIAL);

    RecordCoverageObservation(ledger, 1, HeightCoverageStatus::FULL, 300);
    BOOST_CHECK(ledger.at(1).status == HeightCoverageStatus::FULL);
}

// Monotonic: a later, WEAKER observation never downgrades status -- once
// FULL, a height stays FULL forever (coveragetelemetry.h's own doc on why).
BOOST_AUTO_TEST_CASE(full_status_is_sticky_against_a_later_miss) {
    CoverageHeightMap ledger;
    RecordCoverageObservation(ledger, 1, HeightCoverageStatus::FULL, 100);
    RecordCoverageObservation(ledger, 1, HeightCoverageStatus::MISS, 200);
    BOOST_CHECK(ledger.at(1).status == HeightCoverageStatus::FULL);
}

BOOST_AUTO_TEST_CASE(full_status_is_sticky_against_a_later_partial) {
    CoverageHeightMap ledger;
    RecordCoverageObservation(ledger, 1, HeightCoverageStatus::FULL, 100);
    RecordCoverageObservation(ledger, 1, HeightCoverageStatus::PARTIAL, 200);
    BOOST_CHECK(ledger.at(1).status == HeightCoverageStatus::FULL);
}

// nLastChangeTime tracks only real upgrades, never a same-or-weaker
// re-observation -- this is what lets a later consumer diff "coverage
// confidence last improved" against "we last heard anything at all"
// (nLastObservedTime), matching SS14.9's shrinking-vs-stable distinction.
BOOST_AUTO_TEST_CASE(last_change_time_only_moves_on_a_real_upgrade) {
    CoverageHeightMap ledger;
    RecordCoverageObservation(ledger, 1, HeightCoverageStatus::PARTIAL, 100);
    BOOST_CHECK_EQUAL(ledger.at(1).nLastChangeTime, 100);

    // A repeat at the SAME level must not move nLastChangeTime.
    RecordCoverageObservation(ledger, 1, HeightCoverageStatus::PARTIAL, 200);
    BOOST_CHECK_EQUAL(ledger.at(1).nLastChangeTime, 100);

    // A genuine upgrade does.
    RecordCoverageObservation(ledger, 1, HeightCoverageStatus::FULL, 300);
    BOOST_CHECK_EQUAL(ledger.at(1).nLastChangeTime, 300);
}

// Distinct heights never interfere with each other's dedup state.
BOOST_AUTO_TEST_CASE(different_heights_are_tracked_independently) {
    CoverageHeightMap ledger;
    RecordCoverageObservation(ledger, 1, HeightCoverageStatus::FULL, 100);
    RecordCoverageObservation(ledger, 2, HeightCoverageStatus::MISS, 100);
    BOOST_CHECK_EQUAL(ledger.size(), 2U);
    BOOST_CHECK(ledger.at(1).status == HeightCoverageStatus::FULL);
    BOOST_CHECK(ledger.at(2).status == HeightCoverageStatus::MISS);
}

// -- SummarizeCoverageRange -------------------------------------------------

BOOST_AUTO_TEST_CASE(summary_of_an_empty_ledger_is_all_zero) {
    CoverageHeightMap ledger;
    CoverageRangeSummary sum = SummarizeCoverageRange(ledger, 0, 999);
    BOOST_CHECK_EQUAL(sum.nFull, 0U);
    BOOST_CHECK_EQUAL(sum.nPartial, 0U);
    BOOST_CHECK_EQUAL(sum.nMissOnly, 0U);
}

// Gap 1's own worked example: a 300-block contiguous erasure and 300
// scattered corrupt blocks land in the SAME bucket-level miss count under a
// rollup alone -- proving the rollup by itself still cannot distinguish
// them (that is exactly why GetBodyRangeCoverageHeights/
// SummarizeCoverageRange's own per-height sibling data must stay
// queryable; this test pins the rollup's own limit, not a bug).
BOOST_AUTO_TEST_CASE(rollup_counts_are_identical_for_contiguous_vs_scattered_misses) {
    CoverageHeightMap contiguous;
    for (int h = 100; h < 400; h++) {
        RecordCoverageObservation(contiguous, h, HeightCoverageStatus::MISS, 1000);
    }
    CoverageHeightMap scattered;
    for (int h = 100; h < 1000; h += 3) {
        // 300 heights spread across the same 1000-wide span.
        if (scattered.size() >= 300) break;
        RecordCoverageObservation(scattered, h, HeightCoverageStatus::MISS, 1000);
    }
    BOOST_REQUIRE_EQUAL(scattered.size(), 300U);

    CoverageRangeSummary sumContiguous = SummarizeCoverageRange(contiguous, 0, 999);
    CoverageRangeSummary sumScattered = SummarizeCoverageRange(scattered, 0, 999);
    BOOST_CHECK_EQUAL(sumContiguous.nMissOnly, sumScattered.nMissOnly);
}

BOOST_AUTO_TEST_CASE(summary_counts_each_status_level_separately) {
    CoverageHeightMap ledger;
    RecordCoverageObservation(ledger, 10, HeightCoverageStatus::MISS, 1000);
    RecordCoverageObservation(ledger, 20, HeightCoverageStatus::PARTIAL, 1000);
    RecordCoverageObservation(ledger, 30, HeightCoverageStatus::FULL, 1000);
    RecordCoverageObservation(ledger, 40, HeightCoverageStatus::FULL, 1000);

    CoverageRangeSummary sum = SummarizeCoverageRange(ledger, 0, 999);
    BOOST_CHECK_EQUAL(sum.nMissOnly, 1U);
    BOOST_CHECK_EQUAL(sum.nPartial, 1U);
    BOOST_CHECK_EQUAL(sum.nFull, 2U);
}

BOOST_AUTO_TEST_CASE(summary_excludes_heights_outside_the_requested_range) {
    CoverageHeightMap ledger;
    RecordCoverageObservation(ledger, 999, HeightCoverageStatus::FULL, 1000);
    RecordCoverageObservation(ledger, 1000, HeightCoverageStatus::FULL, 1000);

    CoverageRangeSummary sum = SummarizeCoverageRange(ledger, 0, 999);
    BOOST_CHECK_EQUAL(sum.nFull, 1U);
}

BOOST_AUTO_TEST_CASE(summary_first_observed_is_the_earliest_in_range) {
    CoverageHeightMap ledger;
    RecordCoverageObservation(ledger, 10, HeightCoverageStatus::MISS, 5000);
    RecordCoverageObservation(ledger, 20, HeightCoverageStatus::MISS, 1000);
    CoverageRangeSummary sum = SummarizeCoverageRange(ledger, 0, 999);
    BOOST_CHECK_EQUAL(sum.nFirstObservedTime, 1000);
}

BOOST_AUTO_TEST_CASE(summary_last_change_is_the_latest_in_range) {
    CoverageHeightMap ledger;
    RecordCoverageObservation(ledger, 10, HeightCoverageStatus::MISS, 1000);
    RecordCoverageObservation(ledger, 20, HeightCoverageStatus::PARTIAL, 9000);
    CoverageRangeSummary sum = SummarizeCoverageRange(ledger, 0, 999);
    BOOST_CHECK_EQUAL(sum.nLastChangeTime, 9000);
}

// -- MergeCoverageTelemetrySnapshot (gap 3) ---------------------------------

BOOST_AUTO_TEST_CASE(merging_into_an_empty_ledger_copies_every_record) {
    CoverageHeightMap snapshot;
    RecordCoverageObservation(snapshot, 1, HeightCoverageStatus::FULL, 100);
    RecordCoverageObservation(snapshot, 2, HeightCoverageStatus::MISS, 200);

    CoverageHeightMap ledger;
    MergeCoverageTelemetrySnapshot(ledger, snapshot);

    BOOST_CHECK_EQUAL(ledger.size(), 2U);
    BOOST_CHECK(ledger.at(1).status == HeightCoverageStatus::FULL);
    BOOST_CHECK(ledger.at(2).status == HeightCoverageStatus::MISS);
}

// The merge must apply the SAME monotonic-upgrade rule as a live
// observation -- a persisted FULL must not be regressed by a live ledger
// that (implausibly, but the rule must hold regardless) only ever saw MISS.
BOOST_AUTO_TEST_CASE(merge_never_downgrades_an_existing_live_record) {
    CoverageHeightMap ledger;
    RecordCoverageObservation(ledger, 1, HeightCoverageStatus::FULL, 100);

    CoverageHeightMap snapshot;
    RecordCoverageObservation(snapshot, 1, HeightCoverageStatus::MISS, 500);

    MergeCoverageTelemetrySnapshot(ledger, snapshot);
    BOOST_CHECK(ledger.at(1).status == HeightCoverageStatus::FULL);
}

// The merge must upgrade a weaker live record with a stronger persisted one
// -- the actual startup scenario (gap 3): the live ledger starts empty
// (NOT_OBSERVED, no entry) or with only what has been seen since a restart,
// and the persisted snapshot carries whatever was learned before.
BOOST_AUTO_TEST_CASE(merge_upgrades_a_weaker_live_record_with_a_stronger_persisted_one) {
    CoverageHeightMap ledger;
    RecordCoverageObservation(ledger, 1, HeightCoverageStatus::MISS, 100);

    CoverageHeightMap snapshot;
    RecordCoverageObservation(snapshot, 1, HeightCoverageStatus::FULL, 500);

    MergeCoverageTelemetrySnapshot(ledger, snapshot);
    BOOST_CHECK(ledger.at(1).status == HeightCoverageStatus::FULL);
    // The persisted record's own historical change time is preserved, not
    // replaced with "now" -- a merge is folding in a status that was
    // already true at some point in the past, not observing it live.
    BOOST_CHECK_EQUAL(ledger.at(1).nLastChangeTime, 500);
}

BOOST_AUTO_TEST_CASE(merge_preserves_the_earliest_first_observed_time) {
    CoverageHeightMap ledger;
    RecordCoverageObservation(ledger, 1, HeightCoverageStatus::MISS, 9000);

    CoverageHeightMap snapshot;
    RecordCoverageObservation(snapshot, 1, HeightCoverageStatus::FULL, 100);

    MergeCoverageTelemetrySnapshot(ledger, snapshot);
    BOOST_CHECK_EQUAL(ledger.at(1).nFirstObservedTime, 100);
}

// -- CoverageHeightRecord / CCoverageTelemetryCache (de)serialization ------
// (gap 3's own on-disk format -- round-tripped through CDataStream exactly
// as CFlatDB<T> would, without touching the filesystem.)

BOOST_AUTO_TEST_CASE(coverage_height_record_round_trips_through_serialization) {
    CoverageHeightRecord rec;
    rec.status = HeightCoverageStatus::FULL;
    rec.nFirstObservedTime = 111;
    rec.nLastObservedTime = 222;
    rec.nLastChangeTime = 333;

    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << rec;

    CoverageHeightRecord rec2;
    ss >> rec2;

    BOOST_CHECK(rec2.status == HeightCoverageStatus::FULL);
    BOOST_CHECK_EQUAL(rec2.nFirstObservedTime, 111);
    BOOST_CHECK_EQUAL(rec2.nLastObservedTime, 222);
    BOOST_CHECK_EQUAL(rec2.nLastChangeTime, 333);
}

// Mutation-found-gap guard: a mutant that always serialises/deserialises
// HeightCoverageStatus::FULL regardless of the real value would still pass
// the test above -- this pins a DIFFERENT status level through the same
// round trip so such a mutant cannot hide behind one fixed value.
BOOST_AUTO_TEST_CASE(coverage_height_record_round_trip_preserves_a_non_full_status) {
    CoverageHeightRecord rec;
    rec.status = HeightCoverageStatus::PARTIAL;

    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << rec;
    CoverageHeightRecord rec2;
    ss >> rec2;

    BOOST_CHECK(rec2.status == HeightCoverageStatus::PARTIAL);
}

BOOST_AUTO_TEST_CASE(coverage_telemetry_cache_round_trips_through_serialization) {
    CCoverageTelemetryCache cache;
    RecordCoverageObservation(cache.mapHeightStatus, 1, HeightCoverageStatus::FULL, 100);
    RecordCoverageObservation(cache.mapHeightStatus, 2, HeightCoverageStatus::MISS, 200);

    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << cache;

    CCoverageTelemetryCache cache2;
    ss >> cache2;

    BOOST_CHECK_EQUAL(cache2.mapHeightStatus.size(), 2U);
    BOOST_CHECK(cache2.mapHeightStatus.at(1).status == HeightCoverageStatus::FULL);
    BOOST_CHECK(cache2.mapHeightStatus.at(2).status == HeightCoverageStatus::MISS);
}

// A version-string mismatch (an unrecognised/future format) must be treated
// as "nothing usable" -- matching CSporkManager::Unserialize's own
// established precedent (spork.cpp) -- rather than deserializing garbage
// into the map or throwing.
BOOST_AUTO_TEST_CASE(coverage_telemetry_cache_ignores_an_unrecognised_version_string) {
    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << std::string("SomeOtherFormat-Version-99");
    // No map payload follows -- Unserialize must return before trying to
    // read one, once the version string fails to match.

    CCoverageTelemetryCache cache;
    RecordCoverageObservation(cache.mapHeightStatus, 1, HeightCoverageStatus::FULL, 100);
    ss >> cache;

    BOOST_CHECK(cache.mapHeightStatus.empty());
}

BOOST_AUTO_TEST_CASE(coverage_telemetry_cache_clear_empties_the_map) {
    CCoverageTelemetryCache cache;
    RecordCoverageObservation(cache.mapHeightStatus, 1, HeightCoverageStatus::FULL, 100);
    cache.Clear();
    BOOST_CHECK(cache.mapHeightStatus.empty());
}

BOOST_AUTO_TEST_SUITE_END()
