// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// 4.2 (docs/build-plan.md's 4.2 row; F-206): the pure, testable logic behind
// passive per-peer miss-rate and per-range coverage telemetry -- bucket
// indexing, tally recording, miss-rate computation, and coverage-bucket
// observation recording. The net_processing.cpp call sites that feed real
// GETBODYRANGE/BODYRANGE traffic into this have no dedicated unit coverage,
// matching this project's own established convention for message-processing
// glue (bodyrange_tests.cpp's own precedent, GETBODYRANGE/BODYRANGE
// ProcessMessage dispatch) -- every byte of actual decision logic lives here
// instead, where it is fully testable without CNode/CConnman scaffolding.

#include <coveragetelemetry.h>
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

// -- RecordCoverageObservation --------------------------------------------

BOOST_AUTO_TEST_CASE(coverage_records_first_observed_time_only_on_the_first_call) {
    RangeCoverageStats stats;
    RecordCoverageObservation(stats, /*fHit=*/true, /*nNow=*/1000);
    BOOST_CHECK_EQUAL(stats.nFirstObservedTime, 1000);

    // A second, later observation must not move nFirstObservedTime.
    RecordCoverageObservation(stats, /*fHit=*/true, /*nNow=*/2000);
    BOOST_CHECK_EQUAL(stats.nFirstObservedTime, 1000);
}

// Mutation-found-gap guard: a guard condition mutated from
// "(nHits == 0 && nMisses == 0)" to "nHits == 0" alone would still pass
// every hit-first test above, since a hit-first sequence never has
// nMisses > 0 at the check point -- only a MISS-first sequence exercises
// the nMisses half of the guard.
BOOST_AUTO_TEST_CASE(coverage_records_first_observed_time_when_first_call_is_a_miss) {
    RangeCoverageStats stats;
    RecordCoverageObservation(stats, /*fHit=*/false, /*nNow=*/4000);
    BOOST_CHECK_EQUAL(stats.nFirstObservedTime, 4000);

    // A later HIT must not move it.
    RecordCoverageObservation(stats, /*fHit=*/true, /*nNow=*/9000);
    BOOST_CHECK_EQUAL(stats.nFirstObservedTime, 4000);
}

BOOST_AUTO_TEST_CASE(coverage_hit_updates_tally_and_last_hit_time_only) {
    RangeCoverageStats stats;
    RecordCoverageObservation(stats, /*fHit=*/true, /*nNow=*/5000);
    BOOST_CHECK_EQUAL(stats.tally.nHits, 1U);
    BOOST_CHECK_EQUAL(stats.tally.nMisses, 0U);
    BOOST_CHECK_EQUAL(stats.nLastHitTime, 5000);
    BOOST_CHECK_EQUAL(stats.nLastMissTime, 0);
}

BOOST_AUTO_TEST_CASE(coverage_miss_updates_tally_and_last_miss_time_only) {
    RangeCoverageStats stats;
    RecordCoverageObservation(stats, /*fHit=*/false, /*nNow=*/6000);
    BOOST_CHECK_EQUAL(stats.tally.nHits, 0U);
    BOOST_CHECK_EQUAL(stats.tally.nMisses, 1U);
    BOOST_CHECK_EQUAL(stats.nLastMissTime, 6000);
    BOOST_CHECK_EQUAL(stats.nLastHitTime, 0);
}

BOOST_AUTO_TEST_CASE(coverage_last_hit_and_miss_times_track_independently) {
    RangeCoverageStats stats;
    RecordCoverageObservation(stats, /*fHit=*/true, /*nNow=*/100);
    RecordCoverageObservation(stats, /*fHit=*/false, /*nNow=*/200);
    RecordCoverageObservation(stats, /*fHit=*/true, /*nNow=*/300);

    BOOST_CHECK_EQUAL(stats.tally.nHits, 2U);
    BOOST_CHECK_EQUAL(stats.tally.nMisses, 1U);
    BOOST_CHECK_EQUAL(stats.nFirstObservedTime, 100);
    BOOST_CHECK_EQUAL(stats.nLastHitTime, 300);
    BOOST_CHECK_EQUAL(stats.nLastMissTime, 200);
}

// The exact regression this shape exists to make visible (SS14.9): a
// bucket whose hits stopped arriving a while ago but whose misses keep
// arriving right up to "now" is a STABLE hole -- distinguishable, using
// only this struct's own fields, from one whose last hit is recent (still
// being served, at least intermittently).
BOOST_AUTO_TEST_CASE(coverage_shape_distinguishes_a_stale_hit_from_a_fresh_miss) {
    RangeCoverageStats stats;
    RecordCoverageObservation(stats, /*fHit=*/true, /*nNow=*/1000);
    RecordCoverageObservation(stats, /*fHit=*/false, /*nNow=*/500000);
    RecordCoverageObservation(stats, /*fHit=*/false, /*nNow=*/900000);

    BOOST_CHECK_EQUAL(stats.nLastHitTime, 1000);
    BOOST_CHECK_EQUAL(stats.nLastMissTime, 900000);
    BOOST_CHECK(stats.nLastMissTime > stats.nLastHitTime);
}

BOOST_AUTO_TEST_SUITE_END()
