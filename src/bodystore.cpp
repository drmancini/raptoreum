// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bodystore.h>

#include <clientversion.h>
#include <saltedhasher.h>
#include <streams.h>
#include <sync.h>
#include <txdb.h>
#include <uint256.h>
#include <util/system.h>

#include <unordered_map>

#include <cstdio>
#include <memory>
#include <set>
#include <tinyformat.h>
#include <vector>

// validation.h's own declaration -- redeclared here rather than including all
// of validation.h (and its own large dependency graph) for one pointer.
extern std::unique_ptr<CBlockTreeDB> pblocktree;

std::string CBodyFileInfo::ToString() const {
    return strprintf("CBodyFileInfo(size=%u)", nSize);
}

namespace {

    RecursiveMutex cs_LastBodyFile;

    // 2.1.1: in-memory. 2.1.2: persisted via pblocktree (LoadBodyFileInfo/
    // GetDirtyBodyFileInfo below) -- see bodystore.h's file-level comment for
    // what "persisted" does and doesn't mean yet.
    std::vector<CBodyFileInfo> vinfoBodyFile GUARDED_BY(cs_LastBodyFile);
    int nLastBodyFile GUARDED_BY(cs_LastBodyFile) = 0;
    std::set<int> setDirtyBodyFileInfo GUARDED_BY(cs_LastBodyFile);

    // 4 bytes per offset, matching WriteBodyRecord's fixed-width encoding
    // (F-127 -- CompactSize's 32 MiB ceiling made a
    // full-budget record unreadable).
    const size_t BODY_OFFSET_WIDTH = 4;

    // 2.2.1 (F-140): the body-store-owned height+hash index -- see
    // bodystore.h's file-level comment for the design. Deliberately its own
    // lock, never nested with cs_LastBodyFile (which holds across an fsync on
    // rollover) and never held across I/O by any caller.
    Mutex cs_bodyIndex;

    // F-143 (2.2.2 spec, retroactive amendment): fServeable alongside
    // position -- a withheld block (BLOCK_HAVE_BODY_RECORD set,
    // BLOCK_HAVE_BODIES clear) is hash-resolvable but must never be handed
    // to a serving handler with no cs_main/CBlockIndex to check HaveBodies.
    struct BodyHashEntry {
        FlatFilePos pos;
        bool fServeable;
    };
    std::unordered_map<uint256, BodyHashEntry, StaticSaltedHasher> mapBodyPosByHash GUARDED_BY(cs_bodyIndex);

    struct BodyHeightEntry {
        uint256 hash;
        FlatFilePos pos;
    };
    std::unordered_map<int, BodyHeightEntry> mapBodyPosByHeight GUARDED_BY(cs_bodyIndex);

} // namespace

FlatFileSeq BodyFileSeq() {
    return FlatFileSeq(GetBlocksDir(), "bdy", BODYFILE_CHUNK_SIZE);
}

FILE *OpenBodyFile(const FlatFilePos &pos, bool fReadOnly) {
    return BodyFileSeq().Open(pos, fReadOnly);
}

uint64_t GetBodyRecordSerializedSize(const std::vector<CTransactionRef> &bodies) {
    uint64_t size = ::GetSizeOfCompactSize(bodies.size());
    size += (uint64_t) bodies.size() * BODY_OFFSET_WIDTH;
    for (const auto &tx : bodies) {
        size += GetSerializeSize(*tx, SER_DISK, CLIENT_VERSION);
    }
    return size;
}

bool FindBodyPos(FlatFilePos &pos, unsigned int nAddSize) {
    // A whole body record must land in one file -- WriteBodyRecord/ReadBodyRecord
    // open a single file and read or write it sequentially. A record this size
    // could never fit in a fresh file, so rolling over would loop forever
    // (F-127) -- the static_assert in bodystore.h already
    // guarantees a real body record can't reach this, but a caller passing a
    // bad nAddSize (or a future budget change without updating the assert)
    // must fail loudly here instead.
    if (nAddSize >= MAX_BODYFILE_SIZE) {
        return error("FindBodyPos: %u bytes cannot fit in one body file (limit %u)",
                     nAddSize, MAX_BODYFILE_SIZE);
    }

    LOCK(cs_LastBodyFile);

    unsigned int nFile = nLastBodyFile;
    if (vinfoBodyFile.size() <= nFile) {
        vinfoBodyFile.resize(nFile + 1);
    }

    // Exactly FindBlockPos's own rollover condition (validation.cpp) against
    // MAX_BODYFILE_SIZE in place of MAX_BLOCKFILE_SIZE.
    bool fRolled = false;
    while (vinfoBodyFile[nFile].nSize + (uint64_t) nAddSize >= MAX_BODYFILE_SIZE) {
        nFile++;
        fRolled = true;
        if (vinfoBodyFile.size() <= nFile) {
            vinfoBodyFile.resize(nFile + 1);
        }
    }
    pos.nFile = (int) nFile;
    pos.nPos = vinfoBodyFile[nFile].nSize;

    if (fRolled) {
        // FindBlockPos's own FlushBlockFile(finalize=true) truncates the
        // finished file to its used size and fsyncs it, rather than leaving it
        // at its full chunk-sized preallocation forever (2.1.1 review finding
        // #4). Finalize the file we're LEAVING, not the one we're entering.
        FlatFilePos finishedPos((int) nLastBodyFile, vinfoBodyFile[nLastBodyFile].nSize);
        BodyFileSeq().Flush(finishedPos, /*finalize=*/true);
    }
    nLastBodyFile = (int) nFile;
    vinfoBodyFile[nFile].nSize += nAddSize;
    setDirtyBodyFileInfo.insert((int) nFile);

    bool out_of_space = false;
    BodyFileSeq().Allocate(pos, nAddSize, out_of_space);
    if (out_of_space) {
        return error("FindBodyPos: out of disk space");
    }

    return true;
}

bool WriteBodyRecord(const FlatFilePos &pos, const std::vector<CTransactionRef> &bodies) {
    CAutoFile fileout(OpenBodyFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull()) {
        return error("WriteBodyRecord: OpenBodyFile failed");
    }

    std::vector<uint32_t> offsets;
    offsets.reserve(bodies.size());
    uint64_t cumulative = 0;
    for (const auto &tx : bodies) {
        cumulative += GetSerializeSize(*tx, SER_DISK, CLIENT_VERSION);
        offsets.push_back((uint32_t) cumulative);
    }

    WriteCompactSize(fileout, bodies.size());
    for (uint32_t offset : offsets) {
        fileout << offset;
    }
    for (const auto &tx : bodies) {
        fileout << *tx;
    }

    return true;
}

namespace {

    // Reads the [count][offsets...] header at the current file position, leaving
    // the file positioned at the start of the transaction bytes.
    bool ReadBodyRecordHeader(CAutoFile &filein, std::vector<uint32_t> &offsetsOut) {
        try {
            uint64_t count = ReadCompactSize(filein);
            // A corrupt count byte must not drive an oversized allocation before
            // a single real offset has been read (F-127) --
            // no legal record ever names more transactions than the input-count
            // budget allows (every non-coinbase tx needs >=1 input, F-115's own
            // reasoning).
            if (count > COMMITMENT_BUDGET_MAX_INPUTS) {
                return error("ReadBodyRecordHeader: implausible count %llu", (unsigned long long) count);
            }
            offsetsOut.resize(count);
            for (uint32_t &offset : offsetsOut) {
                filein >> offset;
            }
        } catch (const std::exception &e) {
            return error("ReadBodyRecordHeader: %s", e.what());
        }
        return true;
    }

} // namespace

bool ReadBodyRecord(const FlatFilePos &pos, std::vector<CTransactionRef> &bodiesOut) {
    CAutoFile filein(OpenBodyFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) {
        return error("ReadBodyRecord: OpenBodyFile failed for %s", pos.ToString());
    }

    std::vector<uint32_t> offsets;
    if (!ReadBodyRecordHeader(filein, offsets)) {
        return false;
    }

    bodiesOut.clear();
    bodiesOut.reserve(offsets.size());
    try {
        for (size_t i = 0; i < offsets.size(); i++) {
            CTransactionRef tx;
            filein >> tx;
            bodiesOut.push_back(tx);
        }
    } catch (const std::exception &e) {
        bodiesOut.clear();
        return error("ReadBodyRecord: %s", e.what());
    }

    return true;
}

bool ReadBodyRecordCount(const FlatFilePos &pos, unsigned int &countOut) {
    CAutoFile filein(OpenBodyFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) {
        return error("ReadBodyRecordCount: OpenBodyFile failed for %s", pos.ToString());
    }

    try {
        uint64_t count = ReadCompactSize(filein);
        if (count > COMMITMENT_BUDGET_MAX_INPUTS) {
            return error("ReadBodyRecordCount: implausible count %llu", (unsigned long long) count);
        }
        countOut = (unsigned int) count;
    } catch (const std::exception &e) {
        return error("ReadBodyRecordCount: %s", e.what());
    }
    return true;
}

bool ReadBodyAt(const FlatFilePos &pos, unsigned int index, CTransactionRef &txOut) {
    CAutoFile filein(OpenBodyFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) {
        return error("ReadBodyAt: OpenBodyFile failed for %s", pos.ToString());
    }

    std::vector<uint32_t> offsets;
    if (!ReadBodyRecordHeader(filein, offsets)) {
        return false;
    }
    if (index >= offsets.size()) {
        return error("ReadBodyAt: index %u out of range (%u bodies)", index, (unsigned int) offsets.size());
    }

    // Seek straight to body `index`'s bytes -- a real fseek, not a read-and-
    // discard loop (F-127: CAutoFile::ignore() still reads
    // every skipped byte from disk/page cache, which is not what "seek"
    // claimed and defeats the offset table's whole purpose for a large record).
    uint32_t skipBytes = (index == 0) ? 0 : offsets[index - 1];
    if (fseek(filein.Get(), (long) skipBytes, SEEK_CUR) != 0) {
        return error("ReadBodyAt: fseek failed");
    }
    try {
        filein >> txOut;
    } catch (const std::exception &e) {
        return error("ReadBodyAt: %s", e.what());
    }

    return true;
}

bool LoadBodyFileInfo() {
    LOCK(cs_LastBodyFile);

    int nFile = 0;
    // ReadLastBodyFile failing means an empty/pre-2.1.2 database -- leave the
    // all-zero default rather than treating it as an error, exactly how
    // LoadBlockIndexDB's own block-file-info load (validation.cpp) starts a
    // fresh datadir at file 0.
    pblocktree->ReadLastBodyFile(nFile);
    nLastBodyFile = nFile;

    vinfoBodyFile.assign(nFile + 1, CBodyFileInfo());
    for (int i = 0; i <= nFile; i++) {
        pblocktree->ReadBodyFileInfo(i, vinfoBodyFile[i]);
    }
    setDirtyBodyFileInfo.clear();

    return true;
}

bool FlushBodyFile(bool fFinalize) {
    LOCK(cs_LastBodyFile);

    // F-133: before LoadBodyFileInfo has ever run (or on a freshly reset
    // test fixture), vinfoBodyFile is empty and there is no current file to
    // flush -- indexing vinfoBodyFile[nLastBodyFile] here would be UB, not a
    // caught error, since operator[] does not bounds-check.
    if (vinfoBodyFile.empty()) {
        return true;
    }

    FlatFilePos pos((int) nLastBodyFile, vinfoBodyFile[nLastBodyFile].nSize);
    return BodyFileSeq().Flush(pos, fFinalize);
}

void GetDirtyBodyFileInfo(std::vector<std::pair<int, CBodyFileInfo>> &vFilesOut, int &nLastFileOut) {
    LOCK(cs_LastBodyFile);

    vFilesOut.clear();
    vFilesOut.reserve(setDirtyBodyFileInfo.size());
    for (std::set<int>::iterator it = setDirtyBodyFileInfo.begin(); it != setDirtyBodyFileInfo.end();) {
        vFilesOut.emplace_back(*it, vinfoBodyFile[*it]);
        setDirtyBodyFileInfo.erase(it++);
    }
    nLastFileOut = nLastBodyFile;
}

void ResetBodyFileState() {
    LOCK(cs_LastBodyFile);
    // F-133 review: vector::clear() drops elements but keeps capacity, so in
    // a process that reuses this global across a reset (a reindex retry in
    // production, or many test cases sharing one binary) the old buffer can
    // still have real (if logically removed) CBodyFileInfo objects sitting
    // in it -- indexing past size() then reads stale-but-plausible data
    // instead of reliably faulting, which is not what "reset" is supposed to
    // mean. Assignment from a temporary gives a true zero-capacity vector,
    // matching a genuinely fresh process's own default-constructed global.
    vinfoBodyFile = std::vector<CBodyFileInfo>();
    nLastBodyFile = 0;
    setDirtyBodyFileInfo.clear();
}

void TestOnlyResetBodyFileState() {
    ResetBodyFileState();
}

unsigned int TestOnlyGetBodyFileSize(int nFile) {
    LOCK(cs_LastBodyFile);
    if (nFile < 0 || (size_t) nFile >= vinfoBodyFile.size()) {
        return 0;
    }
    return vinfoBodyFile[nFile].nSize;
}

void RecordBodyPositionByHash(const uint256 &hash, const FlatFilePos &pos, bool fServeable) {
    LOCK(cs_bodyIndex);
    mapBodyPosByHash[hash] = BodyHashEntry{pos, fServeable};
}

bool LookupBodyPositionByHash(const uint256 &hash, FlatFilePos &posOut, bool *fServeableOut) {
    LOCK(cs_bodyIndex);
    auto it = mapBodyPosByHash.find(hash);
    if (it == mapBodyPosByHash.end()) {
        return false;
    }
    posOut = it->second.pos;
    if (fServeableOut) {
        *fServeableOut = it->second.fServeable;
    }
    return true;
}

bool LookupServeableBodyPositionByHash(const uint256 &hash, FlatFilePos &posOut) {
    bool fServeable = false;
    if (!LookupBodyPositionByHash(hash, posOut, &fServeable) || !fServeable) {
        return false;
    }
    return true;
}

void RecordBodyPositionAtHeight(int nHeight, const uint256 &hash, const FlatFilePos &pos) {
    LOCK(cs_bodyIndex);
    mapBodyPosByHeight[nHeight] = BodyHeightEntry{hash, pos};
}

void EraseBodyPositionAtHeight(int nHeight) {
    LOCK(cs_bodyIndex);
    mapBodyPosByHeight.erase(nHeight);
}

bool LookupBodyPositionAtHeight(int nHeight, FlatFilePos &posOut, uint256 &hashOut) {
    LOCK(cs_bodyIndex);
    auto it = mapBodyPosByHeight.find(nHeight);
    if (it == mapBodyPosByHeight.end()) {
        return false;
    }
    posOut = it->second.pos;
    hashOut = it->second.hash;
    return true;
}

void ResetBodyIndex() {
    LOCK(cs_bodyIndex);
    mapBodyPosByHash.clear();
    mapBodyPosByHeight.clear();
}
