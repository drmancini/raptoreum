// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BODYSTORE_H
#define BITCOIN_BODYSTORE_H

#include <consensus/consensus.h>
#include <flatfile.h>
#include <primitives/transaction.h>
#include <serialize.h>

#include <string>
#include <vector>

/** 2.1 (transaction-decoupling.md §5/§5.1): a fourth flat-file series beside
 *  blk*.dat/rev*.dat, holding a block's non-coinbase transaction bodies --
 *  exactly the list MaterialiseBlock's own `bodies` parameter needs, since the
 *  coinbase is always carried whole inside the commitment block itself.
 *  Index 0 here is vtx[1] (the first non-coinbase transaction), matching
 *  CCommitmentBlock::vCommitments' own indexing -- the coinbase is never
 *  stored here.
 *
 *  2.1.1 scope: the record format and the file series, fully self-contained
 *  and independently testable.
 *  2.1.2 scope: persisting the file bookkeeping via pblocktree (LoadBodyFileInfo/
 *  FlushBodyFileInfo below), on CBlockFileInfo's own pattern, so FindBodyPos can
 *  survive a restart -- callable and correct, but not yet auto-invoked at real
 *  startup/shutdown (that's wired in alongside 2.1.4, when AcceptBlock actually
 *  calls into this module and a real restart has something to lose).
 *  Still ahead: 2.1.3 (CBlockIndex nBodyFile/nBodyPos + a migration), 2.1.4
 *  (wire the write into AcceptBlock). */

/** Persistent per-body-file bookkeeping -- the FindBodyPos analogue of
 *  CBlockFileInfo (chain.h), persisted the same way via pblocktree. Simpler
 *  than CBlockFileInfo: no undo/height/time tracking, since nothing here is a
 *  pruning or reindex decision yet (2.1's own "can delete later, but won't at
 *  first" scope, §8.4). */
class CBodyFileInfo {
public:
    unsigned int nSize = 0; //!< number of used bytes of this body file

    SERIALIZE_METHODS(CBodyFileInfo, obj) {
        READWRITE(VARINT(obj.nSize));
    }

    std::string ToString() const;
};

/** Chunk size for the body-file series. Bodies are the bulk of a block's real
 *  bytes under decoupling -- the commitment block itself is small, coinbase
 *  plus one hash per remaining transaction -- so this chunks like blk*.dat
 *  (16 MiB), not rev*.dat (1 MiB). */
static const size_t BODYFILE_CHUNK_SIZE = 0x1000000; // 16 MiB

/** Roll over to a new body file once the current one would reach this size --
 *  the FindBodyPos analogue of MAX_BLOCKFILE_SIZE. Matches it (128 MiB) rather
 *  than inventing a new number: nothing about body records changes what a
 *  reasonable single-file size is. A single record can never span two files
 *  (WriteBodyRecord/ReadBodyRecord open one file and read or write it
 *  sequentially), so this is also a ceiling on one block's worth of bodies --
 *  checked against the real budget below, not just asserted. */
static const unsigned int MAX_BODYFILE_SIZE = 0x8000000; // 128 MiB

// A body record's header overhead (CompactSize count + one 4-byte offset per
// transaction) has to fit in the same file as the bodies themselves. At
// COMMITMENT_BUDGET_MAX_INPUTS transactions (worst case one input each, the
// smallest legal transaction), that's ~4.6 MB of offsets on top of
// COMMITMENT_BUDGET_BODY_BYTES -- comfortably under MAX_BODYFILE_SIZE, but
// checked at compile time so a future budget raise fails loudly instead of
// making FindBodyPos loop forever (2.1.1 review finding #2).
static_assert((uint64_t) COMMITMENT_BUDGET_BODY_BYTES +
              (uint64_t) COMMITMENT_BUDGET_MAX_INPUTS * 4 + 16 < MAX_BODYFILE_SIZE,
              "a full body record must fit in one body file with room to spare");

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

/** Exact number of bytes WriteBodyRecord will write for `bodies` -- what a
 *  caller must pass as FindBodyPos's nAddSize. Computed the same way
 *  WriteBodyRecord itself does (a CompactSize count, one 4-byte offset per
 *  body, then each body's own bytes), so the two can never disagree. */
uint64_t GetBodyRecordSerializedSize(const std::vector<CTransactionRef> &bodies);

/** Find where to write nAddSize more bytes of body data, rolling over to a new
 *  file exactly as FindBlockPos does for blk*.dat once the current one would
 *  reach MAX_BODYFILE_SIZE -- including truncating and fsyncing the finished
 *  file (FindBlockPos's own FlushBlockFile(finalize=true) call), so a rolled
 *  file doesn't keep its full chunk-sized preallocation forever. Returns false
 *  without allocating anything if nAddSize alone could never fit in one file.
 *  In-memory only for now (2.1.1) -- see the file-level comment above. */
bool FindBodyPos(FlatFilePos &pos, unsigned int nAddSize);

/** Write one block's worth of bodies as a self-delimiting record at `pos`:
 *  [CompactSize count][count x fixed 4-byte cumulative end-offset][tx
 *  bytes...]. Offsets are fixed-width, not CompactSize -- serialize.h's
 *  ReadCompactSize refuses anything over MAX_SIZE (32 MiB), well under
 *  COMMITMENT_BUDGET_BODY_BYTES (110 MB), which made every record over 32 MiB
 *  of bodies unreadable (2.1.1 review finding #1). The offset table is what
 *  makes ReadBodyAt (below) a direct seek instead of a sequential decode from
 *  the start -- transaction-decoupling.md §5.2's (height, index) addressing
 *  needs exactly this. `pos` must come from FindBodyPos with nAddSize equal to
 *  GetBodyRecordSerializedSize(bodies), or this will overwrite whatever comes
 *  after it in the file. */
bool WriteBodyRecord(const FlatFilePos &pos, const std::vector<CTransactionRef> &bodies);

/** Inverse of WriteBodyRecord -- every body, in order. */
bool ReadBodyRecord(const FlatFilePos &pos, std::vector<CTransactionRef> &bodiesOut);

/** How many transactions are in the record at `pos`, without deserializing
 *  any of them. */
bool ReadBodyRecordCount(const FlatFilePos &pos, unsigned int &countOut);

/** Read only the transaction at `index` from the record at `pos` by seeking
 *  straight to its bytes -- no earlier transaction in the record is read or
 *  deserialized. */
bool ReadBodyAt(const FlatFilePos &pos, unsigned int index, CTransactionRef &txOut);

/** Load the per-file bookkeeping from pblocktree, so FindBodyPos continues
 *  from where a previous run left off instead of restarting at file 0 (and
 *  silently overwriting real data there). Safe to call against a fresh/empty
 *  database -- leaves the in-memory state at its all-zero default. Requires
 *  pblocktree to already exist; mirrors LoadBlockIndexDB's own block-file-info
 *  load (validation.cpp). */
bool LoadBodyFileInfo();

/** Write every body-file entry FindBodyPos has touched since the last flush to
 *  pblocktree. No-op (returns true) if nothing is dirty. */
bool FlushBodyFileInfo();

/** Test-only: reset FindBodyPos's in-memory state to simulate a fresh process
 *  that must reload from pblocktree via LoadBodyFileInfo(). Never called from
 *  production code. */
void TestOnlyResetBodyFileState();

#endif // BITCOIN_BODYSTORE_H
