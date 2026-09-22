// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// 2.2.3b (F-143's accepted fetch-protocol spec): the announcer ring's own
// pure record-and-query logic. See announcerring.h for the design and why
// reaching the right insertion points (net_processing.cpp, untested glue,
// not covered here) took F-143's own spec review five rounds.

#include <announcerring.h>
#include <test/test_raptoreum.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(announcerring_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(was_announced_is_false_for_a_hash_never_recorded) {
    CAnnouncerRing ring;
    BOOST_CHECK(!ring.WasAnnounced(uint256S("0x1"), /*currentTipHeight=*/100, /*depth=*/10));
}

BOOST_AUTO_TEST_CASE(was_announced_is_true_right_after_recording) {
    CAnnouncerRing ring;
    uint256 hash = uint256S("0x1");
    ring.Record(/*height=*/95, hash, /*currentTipHeight=*/100, /*depth=*/10);
    BOOST_CHECK(ring.WasAnnounced(hash, 100, 10));
}

BOOST_AUTO_TEST_CASE(was_announced_is_false_once_the_recorded_height_falls_outside_depth) {
    CAnnouncerRing ring;
    uint256 hash = uint256S("0x1");
    ring.Record(/*height=*/90, hash, /*currentTipHeight=*/100, /*depth=*/10);
    // height 90 is exactly 10 behind tip 100 -- still within depth 10 (>=,
    // not >, matching a closed [tip-depth, tip] window).
    BOOST_CHECK(ring.WasAnnounced(hash, 100, 10));
    // The tip has now moved on -- the same recorded entry is queried again,
    // without a fresh Record call, and must correctly report stale. This is
    // the property that distinguishes "WasAnnounced re-derives freshness
    // itself" from "WasAnnounced trusts Record's own last eviction pass".
    BOOST_CHECK(!ring.WasAnnounced(hash, 101, 10));
}

BOOST_AUTO_TEST_CASE(record_does_not_insert_an_already_stale_entry) {
    CAnnouncerRing ring;
    uint256 hash = uint256S("0x1");
    // height 50 is already more than depth 10 behind tip 100 at the moment
    // of recording -- there is no reason to insert an entry only to evict
    // it on the very next call.
    ring.Record(/*height=*/50, hash, /*currentTipHeight=*/100, /*depth=*/10);
    BOOST_CHECK(!ring.WasAnnounced(hash, 100, 10));
    BOOST_CHECK_EQUAL(ring.size(), 0U);
}

BOOST_AUTO_TEST_CASE(record_evicts_stale_entries_from_other_hashes_on_each_call) {
    CAnnouncerRing ring;
    uint256 oldHash = uint256S("0x1");
    uint256 newHash = uint256S("0x2");

    ring.Record(/*height=*/50, oldHash, /*currentTipHeight=*/55, /*depth=*/10);
    BOOST_CHECK_EQUAL(ring.size(), 1U);

    // Recording a second, unrelated hash once the tip has moved far enough
    // that oldHash's own entry (height 50) is now outside [tip-depth, tip]
    // must evict it -- this is the memory-bounding property, distinct from
    // (and not required for) WasAnnounced's own correctness, which was
    // already proven above to re-derive freshness independently of this.
    ring.Record(/*height=*/200, newHash, /*currentTipHeight=*/200, /*depth=*/10);
    BOOST_CHECK_EQUAL(ring.size(), 1U);
    BOOST_CHECK(ring.WasAnnounced(newHash, 200, 10));
    BOOST_CHECK(!ring.WasAnnounced(oldHash, 200, 10));
}

BOOST_AUTO_TEST_CASE(record_keeps_multiple_distinct_hashes_at_the_same_height) {
    // A peer can legitimately announce two different blocks at the same
    // height (competing forks, before either is known to win) -- a design
    // keyed purely by height would lose one to the other.
    CAnnouncerRing ring;
    uint256 hashA = uint256S("0xa");
    uint256 hashB = uint256S("0xb");
    ring.Record(/*height=*/50, hashA, /*currentTipHeight=*/50, /*depth=*/10);
    ring.Record(/*height=*/50, hashB, /*currentTipHeight=*/50, /*depth=*/10);
    BOOST_CHECK(ring.WasAnnounced(hashA, 50, 10));
    BOOST_CHECK(ring.WasAnnounced(hashB, 50, 10));
    BOOST_CHECK_EQUAL(ring.size(), 2U);
}

BOOST_AUTO_TEST_CASE(record_keeps_a_future_height_regardless_of_how_far_ahead) {
    // A peer announcing a block taller than our own current tip (we simply
    // haven't caught up yet) must never be treated as "too old" -- the
    // eviction condition only excludes heights BEHIND the tip.
    CAnnouncerRing ring;
    uint256 hash = uint256S("0x1");
    ring.Record(/*height=*/1000, hash, /*currentTipHeight=*/50, /*depth=*/10);
    BOOST_CHECK(ring.WasAnnounced(hash, 50, 10));
}

BOOST_AUTO_TEST_CASE(record_overwrites_the_height_of_a_hash_recorded_twice) {
    CAnnouncerRing ring;
    uint256 hash = uint256S("0x1");
    ring.Record(/*height=*/50, hash, /*currentTipHeight=*/50, /*depth=*/10);
    ring.Record(/*height=*/60, hash, /*currentTipHeight=*/60, /*depth=*/10);
    BOOST_CHECK_EQUAL(ring.size(), 1U);
    // Still fresh at the newer height/tip pairing.
    BOOST_CHECK(ring.WasAnnounced(hash, 60, 10));
}

BOOST_AUTO_TEST_SUITE_END()
