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
 *  2.1.2 scope: persisting the file bookkeeping via pblocktree (LoadBodyFileInfo
 *  below), on CBlockFileInfo's own pattern, so FindBodyPos can survive a
 *  restart. Body-file-info's write side was originally a separate
 *  WriteBodyFileInfoBatch call, independently timed from the block-index
 *  flush -- decided (owner, 2026-09-20, F-132) to instead fold it into
 *  CBlockTreeDB::WriteBatchSync's own atomic batch, since 2.1.3 made the two
 *  genuinely coupled (an index entry can name a body position that isn't safe
 *  to trust durable unless the file-size bookkeeping protecting it is durable
 *  too). See GetDirtyBodyFileInfo below and WriteBatchSync's own doc (txdb.h).
 *  **The write side is now genuinely auto-invoked on every real flush**
 *  (FlushStateToDisk calls GetDirtyBodyFileInfo unconditionally) -- but the
 *  LOAD side (LoadBodyFileInfo) is still not called anywhere near real
 *  startup, and this asymmetry is now load-bearing, not just incomplete
 *  (F-133): whoever wires 2.1.4's real FindBodyPos caller MUST land
 *  LoadBodyFileInfo() in LoadBlockIndexDB in the SAME change, never after --
 *  otherwise the first flush after that change persists a fabricated
 *  nLastBodyFile=0 (the unloaded in-memory default) over whatever was really
 *  last written, reproducing F-130's own overwrite hazard by a different
 *  route. F-133 also found FlushStateToDisk has no body-file equivalent of
 *  FlushBlockFile() (validation.cpp) -- nothing flushes the CURRENT body
 *  file's data to disk outside of FindBodyPos's own rollover-time finalize,
 *  so a crash right after a flush batch goes durable can leave a durable
 *  index entry naming a position whose bytes never made it out of the page
 *  cache. 2.1.4 must add that call (guarded against vinfoBodyFile being empty
 *  before anything has loaded or written) at the same point FlushBlockFile()
 *  runs.
 *  2.1.3 scope: CBlockIndex nBodyFile/nBodyPos + BLOCK_HAVE_BODY_RECORD
 *  (chain.h) -- turned out to need NO migration (a fresh status bit, false for
 *  every existing entry, decodes correctly without one).
 *  Still ahead: 2.1.4 (wire the write into AcceptBlock). */

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
// making FindBodyPos loop forever (F-127).
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
 *  Bookkeeping persists via LoadBodyFileInfo/GetDirtyBodyFileInfo below (2.1.2)
 *  once a caller invokes them -- see the file-level comment above for what's
 *  still not wired to a real startup/shutdown. */
bool FindBodyPos(FlatFilePos &pos, unsigned int nAddSize);

/** Write one block's worth of bodies as a self-delimiting record at `pos`:
 *  [CompactSize count][count x fixed 4-byte cumulative end-offset][tx
 *  bytes...]. Offsets are fixed-width, not CompactSize -- serialize.h's
 *  ReadCompactSize refuses anything over MAX_SIZE (32 MiB), well under
 *  COMMITMENT_BUDGET_BODY_BYTES (110 MB), which made every record over 32 MiB
 *  of bodies unreadable (F-127). The offset table is what
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

/** Flush the CURRENT body file's data to disk -- the FindBodyPos analogue of
 *  FlushBlockFile (validation.cpp), called unconditionally on every real full
 *  flush, right alongside FlushBlockFile's own call, so a crash right after
 *  WriteBatchSync's batch goes durable can't leave an index entry naming body
 *  bytes that never left the page cache (F-133's recorded gap in F-132,
 *  closed here). `fFinalize` matches FlatFileSeq::Flush's own meaning
 *  (truncate to used size and fsync) -- always false from the periodic-flush
 *  call site; FindBodyPos's own rollover-time finalize is unrelated and
 *  untouched by this function.
 *
 *  A no-op (returns true) if nothing has been loaded or written yet
 *  (`vinfoBodyFile` empty) -- a fresh in-memory state before LoadBodyFileInfo
 *  has run has no current file to flush, and indexing into an empty vector
 *  would be undefined behaviour rather than the harmless no-op this needs to
 *  be at that point in startup. */
bool FlushBodyFile(bool fFinalize = false);

/** Gather every body-file entry FindBodyPos has touched since the last call
 *  (`vFilesOut`) and the current last-body-file number (`nLastFileOut`),
 *  clearing the dirty set as it goes -- mirroring how validation.cpp's own
 *  FlushStateToDisk drains setDirtyFileInfo for block-file info, on the same
 *  attempt regardless of whether the caller's write actually succeeds. Always
 *  reports the current last-body-file, dirty or not, exactly as
 *  FlushStateToDisk always passes nLastBlockFile to WriteBatchSync whether or
 *  not any block file is dirty.
 *
 *  Deliberately a getter, not a flush: the caller (FlushStateToDisk) must pass
 *  the result into CBlockTreeDB::WriteBatchSync's own body-file-info
 *  parameters so it lands in the SAME atomic batch as the block-index write
 *  (F-132) -- persisting it separately reopens the ordering hazard that
 *  decision closed.
 *
 *  Returns entries BY VALUE (not pointers into the internal vinfoBodyFile,
 *  the way the block-file-info family does), deliberately, and this is not
 *  parity with CBlockFileInfo for its own sake (F-133, review of F-132):
 *  the block-file-info equivalent is safe only because FlushStateToDisk holds
 *  cs_LastBlockFile across its entire gather-then-WriteBatchSync span, so
 *  vinfoBlockFile can't be resized out from under the pointers it hands out.
 *  This function's own lock (cs_LastBodyFile) is released on return, so a
 *  pointer into vinfoBodyFile would depend on no concurrent FindBodyPos call
 *  resizing it before the caller finishes reading through it -- true only by
 *  accident today (nothing calls FindBodyPos in production yet) and CBodyFileInfo
 *  is one unsigned int, cheap enough that copying it removes the hazard class
 *  entirely instead of documenting an invariant every future caller must honour. */
void GetDirtyBodyFileInfo(std::vector<std::pair<int, CBodyFileInfo>> &vFilesOut, int &nLastFileOut);

/** Reset FindBodyPos's in-memory state -- validation.cpp's UnloadBlockIndex
 *  calls this alongside its own vinfoBlockFile.clear()/nLastBlockFile=0 reset
 *  (F-135, 2.1.4 review: UnloadBlockIndex had no body-store counterpart,
 *  so a reindex-retry within one process kept stale in-memory body-file
 *  bookkeeping after pblocktree itself was wiped). */
void ResetBodyFileState();

/** Test-only wrapper for the above, kept as its own name at every existing
 *  call site (bodystore_tests.cpp, acceptancebit_tests.cpp) -- simulates a
 *  fresh process that must reload from pblocktree via LoadBodyFileInfo(). */
void TestOnlyResetBodyFileState();

/** Test-only: the in-memory size LoadBodyFileInfo/FindBodyPos currently hold
 *  for body file `nFile`, or 0 if it's never been touched. LoadBodyFileInfo
 *  itself reads every file 0..nLastBodyFile from pblocktree, not just the
 *  last one -- this is what makes that actually observable in a test, since
 *  FindBodyPos alone only ever consults the LAST file to decide where to
 *  write next. */
unsigned int TestOnlyGetBodyFileSize(int nFile);

/** 2.2.1 (F-140, build-plan.md's 2.2 row): a body-store-owned index, entirely
 *  decoupled from `cs_main` and `CBlockIndex`, so a future serving handler
 *  (2.2.3) can resolve `(height|hash) -> position` without touching chain
 *  state at all. `CBlockIndex::GetBodyPos()` alone cannot do this -- reaching
 *  a `CBlockIndex` object at all requires looking it up in the `cs_main`-
 *  guarded block-index map, which is exactly the coupling B4 (§14.3,
 *  transaction-decoupling.md) identifies as the seek-amplified `cs_main`-
 *  pinning DoS this index exists to avoid.
 *
 *  Two halves, deliberately asymmetric:
 *
 *  - `ByHash`: the primary index. Populated the moment a body record is
 *    WRITTEN (`ReceivedBlockTransactions`'s own two call sites, `AcceptBlock`
 *    and `AddGenesisBlock`), never at connect -- side-chain blocks are
 *    accepted and persisted but never connected at all
 *    (transaction-decoupling.md §5), and the tip-critical fetch path is
 *    ALWAYS hash-form (a block being fetched is, by definition, not yet on
 *    the requester's own active chain; build-plan.md's own "2.2 is the tip
 *    critical path, not a history service"). Works on and off the active
 *    chain; never removed today (no pruning exists yet, 2.1's own still-open
 *    item).
 *  - `AtHeight`: the active chain's own position at a height, maintained only
 *    at `ConnectTip`/`DisconnectTip`. On disconnect the entry is removed
 *    outright, not redirected -- the winning branch's own `ConnectTip` is
 *    what supplies the correct one. A reader landing in the gap mid-reorg
 *    sees no entry (a clean miss, not wrong data) exactly the way a genuine
 *    history gap does under §8.4 decision 2's own non-punitive-miss rule.
 *
 *  Locking: both halves share `cs_bodyIndex`, a mutex deliberately independent
 *  of `cs_LastBodyFile` (which holds across an `fsync` on file rollover,
 *  `FindBodyPos`) and never taken together with it. `cs_bodyIndex` is held
 *  ONLY for the map lookup/update itself, never across any I/O -- a future
 *  serving handler (2.2.3) must resolve through this index, release the lock,
 *  then read the record, exactly the discipline that makes the "no `cs_main`"
 *  property real rather than nominal. */

/** Record where a block's body record was written, keyed by the block's own
 *  hash. No `cs_main` required to call or to read back. */
void RecordBodyPositionByHash(const uint256 &hash, const FlatFilePos &pos);

/** Look up a body position by hash. False if never recorded. */
bool LookupBodyPositionByHash(const uint256 &hash, FlatFilePos &posOut);

/** Record the ACTIVE CHAIN's body position and hash at a height -- call at
 *  `ConnectTip`, after the connect itself succeeds. Overwrites any existing
 *  entry at that height (a reorg's own `ConnectTip` for the winning branch is
 *  what corrects a stale one -- no separate "is this a reorg" branch needed
 *  here). */
void RecordBodyPositionAtHeight(int nHeight, const uint256 &hash, const FlatFilePos &pos);

/** Remove the active chain's entry at a height -- call at `DisconnectTip`,
 *  for the height being disconnected. */
void EraseBodyPositionAtHeight(int nHeight);

/** Look up the ACTIVE CHAIN's body position and hash at a height. False if
 *  this height has no entry (never connected, disconnected mid-reorg, or
 *  beyond the highest height ever recorded). */
bool LookupBodyPositionAtHeight(int nHeight, FlatFilePos &posOut, uint256 &hashOut);

/** Discard both halves of the index. Called once, at the start of a full
 *  rebuild (`CChainState::LoadChainTip`, validation.cpp -- no separate
 *  on-disk format for this index; it is rebuilt every boot from
 *  `CBlockIndex`'s own already-persisted `nBodyFile`/`nBodyPos`/
 *  `BLOCK_HAVE_BODY_RECORD`, the same "rebuilt every boot" convention
 *  `vinfoBlockFile`/`setBlockIndexCandidates` already use), and directly by
 *  tests that need to simulate a fresh process. */
void ResetBodyIndex();

#endif // BITCOIN_BODYSTORE_H
