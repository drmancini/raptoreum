// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ANNOUNCERRING_H
#define BITCOIN_ANNOUNCERRING_H

#include <saltedhasher.h>
#include <uint256.h>

#include <algorithm>
#include <cstdint>
#include <limits>
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
 *  only records and answers queries. **This set is closed as of 2.2.3b/
 *  F-153, not closed permanently** -- a Fable review of the first version
 *  found a sixth real path (CMPCTBLOCK announcing a block whose parent we
 *  don't have yet, fixed the same way as the HEADERS unconnecting case),
 *  and any future announcement mechanism (e.g. a commitment-only relay
 *  message, once one exists) is a new insertion point this comment cannot
 *  already know about.
 *
 *  This ring is per-CNodeState and is destroyed with it on disconnect
 *  (FinalizeNode, net_processing.cpp) -- a peer that announces, disconnects,
 *  and reconnects starts with an empty ring. This is inherent to the
 *  accepted per-peer design (F-143) and is the same shape as the already-
 *  accepted residual limitation that a withholder who simply goes silent,
 *  rather than answering, is not closed by this mechanism either.
 *
 *  This type does its own I/O-free, lock-free bookkeeping and is fully
 *  testable without CNode/CConnman/cs_main scaffolding, matching this
 *  project's own established split (ValidateGetBodyRange/
 *  BuildBodyRangeResponse/ReadBodyRange the same way) between pure, tested
 *  logic and the untested net_processing.cpp glue that wires it up. */
class CAnnouncerRing {
public:
    /** Record that this peer announced `hash` at `height`. Evicts every
     *  entry more than `depth` blocks behind the ring's own EFFECTIVE
     *  height first (see EffectiveHeight below) -- an eviction is a memory-
     *  hygiene measure only, not the correctness boundary (WasAnnounced
     *  re-checks freshness itself rather than trusting that eviction has
     *  run recently). An entry that is ALREADY stale relative to the cutoff
     *  at the moment it would be recorded is not inserted at all -- there
     *  is no reason to add an entry only to evict it on the very next
     *  call. */
    void Record(int height, const uint256 &hash, int currentTipHeight, int depth);

    /** Was `hash` ever recorded by this peer, and is that record still
     *  within `depth` blocks of the ring's own effective height? Re-derives
     *  freshness from the stored height at query time -- correct regardless
     *  of whether Record has run recently enough to have evicted a since-
     *  gone-stale entry itself. */
    bool WasAnnounced(const uint256 &hash, int currentTipHeight, int depth) const;

    /** Current entry count -- test-only visibility into the eviction
     *  policy's own memory-bounding behaviour. */
    size_t size() const { return m_heightByHash.size(); }

private:
    /** F-153 (a Fable review of F-152, HIGH): using `currentTipHeight` alone
     *  as the cutoff basis was wrong -- `currentTipHeight` is the CONNECTED
     *  chain's own tip, which during headers-first sync lags far behind the
     *  headers actually being accepted (and so recorded) for a peer racing
     *  ahead. Every one of those far-ahead entries is "above the tip" and
     *  so never evicted under the original scheme, and Record's own
     *  eviction scan is O(ring size) on every call -- together, measured
     *  directly: 200,000 such Record calls against real mainnet-scale
     *  heights took ~692s. `EffectiveHeight` instead tracks the highest
     *  height this ring has EVER recorded and uses whichever is higher,
     *  that or `currentTipHeight` -- so the ring stays a bounded window
     *  around wherever the peer's own header stream has actually reached,
     *  correctly degrading back to plain `currentTipHeight` once real sync
     *  catches up and the two stay close together (the steady-state,
     *  post-IBD case every existing test already covers). */
    int EffectiveHeight(int currentTipHeight) const {
        return std::max(currentTipHeight, m_maxRecordedHeight);
    }

    std::unordered_map<uint256, int, StaticSaltedHasher> m_heightByHash;
    int m_maxRecordedHeight = std::numeric_limits<int>::min();
};

#endif // BITCOIN_ANNOUNCERRING_H
