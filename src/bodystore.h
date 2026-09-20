// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BODYSTORE_H
#define BITCOIN_BODYSTORE_H

#include <flatfile.h>
#include <primitives/transaction.h>

#include <vector>

/** 2.1 (transaction-decoupling.md §5/§5.1): a third flat-file series beside
 *  blk*.dat/rev*.dat, holding a block's non-coinbase transaction bodies --
 *  exactly the list MaterialiseBlock's own `bodies` parameter needs, since the
 *  coinbase is always carried whole inside the commitment block itself.
 *
 *  2.1.1 scope: the record format and the file series, fully self-contained
 *  and independently testable. FindBodyPos below is in-memory only -- it does
 *  not yet survive a restart (that needs the LevelDB bookkeeping FindBlockPos
 *  itself gets from vinfoBlockFile/pblocktree, a later 2.1 step), and nothing
 *  here is wired into AcceptBlock yet. */

/** Chunk size for the body-file series. Bodies are the bulk of a block's real
 *  bytes under decoupling -- the commitment block itself is small, coinbase
 *  plus one hash per remaining transaction -- so this chunks like blk*.dat
 *  (16 MiB), not rev*.dat (1 MiB). */
static const size_t BODYFILE_CHUNK_SIZE = 0x1000000; // 16 MiB

/** Roll over to a new body file once the current one would exceed this size --
 *  the FindBodyPos analogue of MAX_BLOCKFILE_SIZE. Matches it (128 MiB) rather
 *  than inventing a new number: nothing about body records changes what a
 *  reasonable single-file size is. */
static const unsigned int MAX_BODYFILE_SIZE = 0x8000000; // 128 MiB

/** The body store's own FlatFileSeq. Independent file numbering from
 *  BlockFileSeq()/UndoFileSeq() (build-plan.md 2.1's own scope note): a
 *  commitment block's bytes are small and roughly constant-size, while its
 *  bodies range from empty to the full resource-budget ceiling, so one
 *  blk*.dat file's worth of commitment blocks corresponds to a wildly
 *  varying amount of body data -- coupling the two series' rotation would
 *  create a many-to-one relationship with no benefit. */
FlatFileSeq BodyFileSeq();

/** Open a handle to the body file at the given position. */
FILE *OpenBodyFile(const FlatFilePos &pos, bool fReadOnly = false);

/** Find where to write nAddSize more bytes of body data, rolling over to a new
 *  file exactly as FindBlockPos does for blk*.dat once the current one would
 *  exceed BODYFILE_CHUNK_SIZE-driven allocation. In-memory only for now
 *  (2.1.1) -- see the file-level comment above. */
bool FindBodyPos(FlatFilePos &pos, unsigned int nAddSize);

/** Write one block's worth of bodies as a self-delimiting record at `pos`:
 *  [CompactSize count][count x CompactSize cumulative end-offset][tx
 *  bytes...]. The offset table is what makes ReadBodyAt (below) an O(1) seek
 *  instead of a sequential decode from the start -- transaction-decoupling.md
 *  §5.2's (height, index) addressing needs exactly this. */
bool WriteBodyRecord(const FlatFilePos &pos, const std::vector<CTransactionRef> &bodies);

/** Inverse of WriteBodyRecord -- every body, in order. */
bool ReadBodyRecord(const FlatFilePos &pos, std::vector<CTransactionRef> &bodiesOut);

/** How many transactions are in the record at `pos`, without deserializing
 *  any of them. */
bool ReadBodyRecordCount(const FlatFilePos &pos, unsigned int &countOut);

/** Read only the transaction at `index` from the record at `pos`, without
 *  deserializing any of the transactions before it. */
bool ReadBodyAt(const FlatFilePos &pos, unsigned int index, CTransactionRef &txOut);

#endif // BITCOIN_BODYSTORE_H
