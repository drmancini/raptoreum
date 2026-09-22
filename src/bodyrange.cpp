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
