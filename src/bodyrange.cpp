// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bodyrange.h>

#include <bodystore.h>
#include <version.h>

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
    respOut.vBodies.clear();

    uint64_t nRunningBytes = 0;
    for (uint32_t i = 0; i < request.nCount; i++) {
        CTransactionRef tx;
        if (!ReadBodyAt(pos, request.nStartIndex + i, tx)) {
            break;
        }
        uint64_t nTxBytes = GetSerializeSize(*tx, SER_NETWORK, PROTOCOL_VERSION);
        if (!respOut.vBodies.empty() && nRunningBytes + nTxBytes > nByteCeiling) {
            break;
        }
        respOut.vBodies.push_back(tx);
        nRunningBytes += nTxBytes;
    }
}
