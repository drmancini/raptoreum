// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coveragetelemetry.h>

#include <tinyformat.h>

#include <cassert>

const std::string CCoverageTelemetryCache::SERIALIZATION_VERSION_STRING = "CCoverageTelemetryCache-Version-1";

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

void RecordCoverageObservation(CoverageHeightMap &ledger, int nHeight, HeightCoverageStatus fObserved, int64_t nNow) {
    assert(fObserved != HeightCoverageStatus::NOT_OBSERVED);

    CoverageHeightRecord &rec = ledger[nHeight];
    if (rec.nFirstObservedTime == 0) {
        rec.nFirstObservedTime = nNow;
    }
    rec.nLastObservedTime = nNow;
    if (fObserved > rec.status) {
        rec.status = fObserved;
        rec.nLastChangeTime = nNow;
    }
}

void MergeCoverageTelemetrySnapshot(CoverageHeightMap &ledger, const CoverageHeightMap &snapshot) {
    for (const auto &kv: snapshot) {
        const int nHeight = kv.first;
        const CoverageHeightRecord &incoming = kv.second;

        CoverageHeightRecord &rec = ledger[nHeight];
        if (rec.nFirstObservedTime == 0 ||
            (incoming.nFirstObservedTime != 0 && incoming.nFirstObservedTime < rec.nFirstObservedTime)) {
            rec.nFirstObservedTime = incoming.nFirstObservedTime;
        }
        if (incoming.nLastObservedTime > rec.nLastObservedTime) {
            rec.nLastObservedTime = incoming.nLastObservedTime;
        }
        if (incoming.status > rec.status) {
            rec.status = incoming.status;
            // The incoming record's own nLastChangeTime is the historically
            // correct moment this status was reached, not "now" -- unlike
            // RecordCoverageObservation's own live call sites (which always
            // change status, if at all, at the caller's current time), a
            // merge is folding in a status that was already true at some
            // point in the past.
            rec.nLastChangeTime = incoming.nLastChangeTime;
        }
    }
}

CoverageRangeSummary SummarizeCoverageRange(const CoverageHeightMap &ledger, int nStartHeight,
                                             int nEndHeightInclusive) {
    CoverageRangeSummary out;
    auto it = ledger.lower_bound(nStartHeight);
    auto itEnd = ledger.upper_bound(nEndHeightInclusive);
    for (; it != itEnd; ++it) {
        const CoverageHeightRecord &rec = it->second;
        switch (rec.status) {
            case HeightCoverageStatus::FULL:
                out.nFull++;
                break;
            case HeightCoverageStatus::PARTIAL:
                out.nPartial++;
                break;
            case HeightCoverageStatus::MISS:
                out.nMissOnly++;
                break;
            case HeightCoverageStatus::NOT_OBSERVED:
                // Never actually stored (RecordCoverageObservation's own
                // assert) -- defensive only.
                break;
        }
        if (out.nFirstObservedTime == 0 || rec.nFirstObservedTime < out.nFirstObservedTime) {
            out.nFirstObservedTime = rec.nFirstObservedTime;
        }
        if (rec.nLastChangeTime > out.nLastChangeTime) {
            out.nLastChangeTime = rec.nLastChangeTime;
        }
    }
    return out;
}

void CCoverageTelemetryCache::Clear() {
    mapHeightStatus.clear();
}

void CCoverageTelemetryCache::CheckAndRemove() {
    // Deliberate no-op -- see this method's own doc comment, coveragetelemetry.h.
}

std::string CCoverageTelemetryCache::ToString() const {
    return strprintf("CCoverageTelemetryCache(heights=%u)", mapHeightStatus.size());
}
