// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BODYRANGE_H
#define BITCOIN_BODYRANGE_H

#include <blockencodings.h>
#include <flatfile.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <vector>

/** 2.2.2 (F-143's accepted wire-format spec, five review rounds -- see
 *  docs/findings.md's F-143/F-144/F-145): the fetch protocol's request and
 *  response payloads. Hash-form addressing ONLY -- the original spec's
 *  height-addressing form was dropped by explicit owner decision during
 *  review, since every requester already holds the block hash it wants and
 *  2.2.1's own index (bodystore.h) resolves a hash to the same `FlatFilePos`
 *  a height lookup would, lock-free, regardless of chain position.
 *
 *  This header delivers the WIRE CONTRACT only -- the message-type names
 *  (`NetMsgType::GETBODYRANGE`/`BODYRANGE`, protocol.h/.cpp) and the
 *  request-validation classification below. It deliberately does NOT
 *  dispatch these messages anywhere (no `ProcessMessage` arm exists for
 *  them yet) and does NOT serve a response body -- both are 2.2.3's job,
 *  since serving requires resolving through 2.2.1's index and reading the
 *  body-store's own files, off `cs_main`, which needs a real network
 *  handler this header has no reason to depend on.
 *
 *  Index space: body index `i` is the index into the commitment block's own
 *  identifier list -- the SAME index space 2.2.1's own store already uses
 *  (`ReadBodyAt`, bodystore.h). Since the coinbase is always index 0 of that
 *  identifier list and is never stored in the body store, body index `i`
 *  names the same transaction as `vtx[i+1]` in a full `CBlock`. See
 *  `VtxIndexFromBodyIndex`/`BodyIndexFromVtxIndex` below -- named explicitly,
 *  matching F-115's own precedent, so nowhere re-derives this off-by-one by
 *  hand. */

/** The `getbodyrange` request payload. */
class CGetBodyRange {
public:
    uint256 hashBlock;
    uint32_t nStartIndex;  //!< first BODY index wanted (NOT a vtx index -- see "Index space" above)
    uint32_t nCount;       //!< how many, from nStartIndex; the server enforces its own byte-based
                           //!< ceiling and may serve fewer than requested (2.2.3's continuation contract)

    SERIALIZE_METHODS(CGetBodyRange, obj)
    {
        READWRITE(obj.hashBlock, obj.nStartIndex, obj.nCount);
    }
};

/** The `bodyrange` response payload. No `fFound` field, deliberately (F-143's
 *  v2-to-v3 revision): an honest requester never sends a request this struct
 *  could answer "found, but zero bodies" to (both request-validation BAN
 *  cases below make that impossible) -- a miss is simply `vBodies.empty()`,
 *  with no wire-representable disagreement state possible between two
 *  separate fields. */
class CBodyRange {
public:
    uint256 hashBlock;      //!< echoes the request -- confirms which block was actually served,
                            //!< and is what makes the body's own hash verifiable (§5.2)
    uint32_t nStartIndex;   //!< echoed from the request
    std::vector<CTransactionRef> vBodies;  //!< empty means a miss

    SERIALIZE_METHODS(CBodyRange, obj)
    {
        READWRITE(obj.hashBlock, obj.nStartIndex, Using<VectorFormatter<TransactionCompression>>(obj.vBodies));
    }
};

/** Body index `i` (2.2.1's own store-level index, `ReadBodyAt`) names the
 *  same transaction as `vtx[i+1]` in a full `CBlock` -- the coinbase is
 *  always `vtx[0]` and is never stored in the body store. */
static inline uint32_t VtxIndexFromBodyIndex(uint32_t bodyIndex) { return bodyIndex + 1; }

/** Inverse of the above. `vtxIndex` must be `>= 1` -- there is no body index
 *  for the coinbase itself, `vtx[0]`. */
static inline uint32_t BodyIndexFromVtxIndex(uint32_t vtxIndex) { return vtxIndex - 1; }

/** F-143's own request-validation classification, decided in this exact
 *  order (each tier only reachable once the tiers above it are ruled out):
 *
 *  - `BAN`: genuinely impossible for an honest requester to trigger --
 *    `nCount == 0`, or `nStartIndex + nCount` overflows `uint32_t`. Decided
 *    FIRST, with no `cs_main` and no I/O of any kind.
 *  - `MISS`: `hashBlock` resolves to nothing in the body-store index at all,
 *    OR it resolves but isn't currently serveable (a withheld block, F-144's
 *    own serveability flag) -- both read via `LookupServeableBodyPositionByHash`
 *    alone (bodystore.h: mutex-only, no `cs_main`, no file I/O). An unknown
 *    hash is exactly what an honest requester sends during ordinary
 *    header-propagation races (C1's own required parallel multi-source
 *    fetch), so this must never be a `BAN` -- `LookupBlockIndex`/`cs_main`
 *    must NEVER appear anywhere in this function.
 *  - `BAN`: `nStartIndex` at or past the resolved, SERVEABLE block's own
 *    body-record count -- this is the only tier with real I/O cost
 *    (`ReadBodyRecordCount` opens the body file), and is honest-proof only
 *    here, since an honest requester always knows a serveable block's own
 *    body count from its own commitment-block identifier list. A
 *    `ReadBodyRecordCount` failure (a corrupt on-disk record) is a `MISS`,
 *    never a `BAN` -- the requester cannot distinguish "corrupt" from
 *    "server-side problem" and did nothing wrong by asking.
 *  - `OK`: none of the above -- `posOut` is the resolved position, the range
 *    `[nStartIndex, nStartIndex+nCount)` may extend past the record's own
 *    count (2.2.3's own truncate-to-fit continuation contract handles that,
 *    not this function).
 *
 *  This function does no network I/O and takes no lock beyond the body
 *  store's own `cs_bodyIndex` (held only for the lookup itself) -- testable
 *  without a live peer, exactly as F-143's own deliverables list requires. */
enum class GetBodyRangeValidation {
    BAN,
    MISS,
    OK,
};

GetBodyRangeValidation ValidateGetBodyRange(const CGetBodyRange &request, FlatFilePos &posOut);

#endif // BITCOIN_BODYRANGE_H
