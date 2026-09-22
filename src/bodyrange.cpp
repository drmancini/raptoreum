// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bodyrange.h>

#include <bodystore.h>

#include <limits>

GetBodyRangeValidation ValidateGetBodyRange(const CGetBodyRange &request, FlatFilePos &posOut) {
    if (request.nCount == 0) {
        return GetBodyRangeValidation::BAN;
    }
    if (request.nStartIndex > std::numeric_limits<uint32_t>::max() - request.nCount) {
        return GetBodyRangeValidation::BAN;
    }

    if (!LookupServeableBodyPositionByHash(request.hashBlock, posOut)) {
        return GetBodyRangeValidation::MISS;
    }

    unsigned int count = 0;
    if (!ReadBodyRecordCount(posOut, count)) {
        return GetBodyRangeValidation::MISS;
    }
    if (request.nStartIndex >= count) {
        return GetBodyRangeValidation::BAN;
    }

    return GetBodyRangeValidation::OK;
}

void BuildBodyRangeResponse(const CGetBodyRange &request, const FlatFilePos &pos,
                             uint64_t nByteCeiling, CBodyRange &respOut) {
    respOut.hashBlock = request.hashBlock;
    respOut.nStartIndex = request.nStartIndex;
    // F-150 (2.2.3a's own Fable review, HIGH): this used to call ReadBodyAt
    // in a loop, which re-opens the record and re-reads its ENTIRE offset
    // table on every single call -- O(request.nCount x the record's own
    // real body count), measured at ~12s of real CPU for one network
    // request against a 100,000-body record. ReadBodyRange (bodystore.h)
    // opens the record and reads its header exactly once, then streams --
    // O(record size) total, regardless of how many bodies are requested.
    ReadBodyRange(pos, request.nStartIndex, request.nCount, nByteCeiling, respOut.vBodies);
}
