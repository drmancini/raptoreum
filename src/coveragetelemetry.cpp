// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coveragetelemetry.h>

#include <cassert>

int CoverageRangeIndex(int nHeight, int nRangeSize) {
    assert(nHeight >= 0);
    assert(nRangeSize > 0);
    return nHeight / nRangeSize;
}

void RecordBodyRangeTally(BodyRangeTally &tally, bool fHit) {
    if (fHit) {
        tally.nHits++;
    } else {
        tally.nMisses++;
    }
}

double BodyRangeMissRate(const BodyRangeTally &tally) {
    uint64_t nTotal = tally.nHits + tally.nMisses;
    if (nTotal == 0) {
        return -1.0;
    }
    return (double) tally.nMisses / (double) nTotal;
}

void RecordCoverageObservation(RangeCoverageStats &stats, bool fHit, int64_t nNow) {
    if (stats.tally.nHits == 0 && stats.tally.nMisses == 0) {
        stats.nFirstObservedTime = nNow;
    }
    RecordBodyRangeTally(stats.tally, fHit);
    if (fHit) {
        stats.nLastHitTime = nNow;
    } else {
        stats.nLastMissTime = nNow;
    }
}
