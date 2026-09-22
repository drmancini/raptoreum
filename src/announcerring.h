// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ANNOUNCERRING_H
#define BITCOIN_ANNOUNCERRING_H

#include <saltedhasher.h>
#include <uint256.h>

#include <cstdint>
#include <unordered_map>

/** 2.2.3b (F-143's accepted fetch-protocol spec, the "announcer ring"): a
 *  per-peer record of which blocks that peer has announced to us, height-
 *  bounded rather than count-bounded (F-143's own round-3 finding: a
 *  count-bounded ring is free to grind once every-announcer-not-just-first
 *  is tracked -- flooding cheap announcements evicts real ones). This is
 *  the FETCHER's own bookkeeping: when a body fetch later fails against a
 *  peer that announced the block we asked for, this is what lets that be
 *  told apart from a peer who was never on the hook for it. Consuming this
 *  (actually deciding misbehaviour from a failed fetch) is 2.2.4's job, not
 *  this type's -- this is purely the record-and-query mechanism.
 *
 *  Reaching the CORRECT set of insertion points took F-143's own spec
 *  review five rounds (v1: a global "last announcer wins" map, wrong --
 *  loses every earlier announcer; v2: reusing the existing
 *  pindexBestKnownBlock/UpdateBlockAvailability mechanism, wrong -- a peer's
 *  own withheld-block obligation is erased for free by relaying any later
 *  honest block; v3: first ring version, but count-bounded and only
 *  recorded a HEADERS batch's trailing header; v4: still missed three real
 *  announcement paths -- INV, an unconnecting-headers deferred resolution,
 *  and a partially-rejected HEADERS batch's own accepted prefix; v5: the
 *  fix for those still only covered ONE of two independent code paths that
 *  resolve a previously-unknown hash). The insertion points this type's own
 *  caller (net_processing.cpp) must use are the two independent resolution
 *  branches of UpdateBlockAvailability/ProcessBlockAvailability (which
 *  between them already cover INV and the unconnecting-headers deferred
 *  resolution, since both of those already route through
 *  UpdateBlockAvailability) plus a per-header loop over a HEADERS batch's
 *  own genuinely-accepted prefix, on BOTH the success path and the
 *  partial-failure path (confirmed directly against
 *  ChainstateManager::ProcessNewBlockHeaders, validation.cpp: its own
 *  out-param is populated incrementally, per header, even on a caller that
 *  later returns false) -- this type itself is agnostic to all of that; it
 *  only records and answers queries.
 *
 *  This type does its own I/O-free, lock-free bookkeeping and is fully
 *  testable without CNode/CConnman/cs_main scaffolding, matching this
 *  project's own established split (ValidateGetBodyRange/
 *  BuildBodyRangeResponse/ReadBodyRange the same way) between pure, tested
 *  logic and the untested net_processing.cpp glue that wires it up. */
class CAnnouncerRing {
public:
    /** Record that this peer announced `hash` at `height`. Evicts every
     *  entry more than `depth` blocks behind `currentTipHeight` first, so
     *  the ring's own memory stays bounded regardless of how long a peer
     *  stays connected -- this eviction is a memory-hygiene measure only,
     *  not the correctness boundary (see WasAnnounced, which re-checks
     *  freshness itself rather than trusting that eviction has run
     *  recently). An entry that is ALREADY stale relative to the cutoff at
     *  the moment it would be recorded is not inserted at all -- there is
     *  no reason to add an entry only to evict it on the very next call. */
    void Record(int height, const uint256 &hash, int currentTipHeight, int depth);

    /** Was `hash` ever recorded by this peer, and is that record still
     *  within `depth` blocks of `currentTipHeight`? Re-derives freshness
     *  from the stored height at query time -- correct regardless of
     *  whether Record has run recently enough to have evicted a since-gone-
     *  stale entry itself. */
    bool WasAnnounced(const uint256 &hash, int currentTipHeight, int depth) const;

    /** Current entry count -- test-only visibility into the eviction
     *  policy's own memory-bounding behaviour. */
    size_t size() const { return m_heightByHash.size(); }

private:
    std::unordered_map<uint256, int, StaticSaltedHasher> m_heightByHash;
};

#endif // BITCOIN_ANNOUNCERRING_H
