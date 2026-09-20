// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bodystore.h>

#include <clientversion.h>
#include <streams.h>
#include <sync.h>
#include <util/system.h>

#include <vector>

namespace {

    RecursiveMutex cs_LastBodyFile;

    struct CBodyFileInfo {
        unsigned int nSize = 0; //!< number of used bytes of this body file
    };

    // 2.1.1 (in-memory only -- see bodystore.h's file-level comment).
    std::vector<CBodyFileInfo> vinfoBodyFile GUARDED_BY(cs_LastBodyFile);
    int nLastBodyFile GUARDED_BY(cs_LastBodyFile) = 0;

} // namespace

FlatFileSeq BodyFileSeq() {
    return FlatFileSeq(GetBlocksDir(), "bdy", BODYFILE_CHUNK_SIZE);
}

FILE *OpenBodyFile(const FlatFilePos &pos, bool fReadOnly) {
    return BodyFileSeq().Open(pos, fReadOnly);
}

bool FindBodyPos(FlatFilePos &pos, unsigned int nAddSize) {
    LOCK(cs_LastBodyFile);

    unsigned int nFile = nLastBodyFile;
    if (vinfoBodyFile.size() <= nFile) {
        vinfoBodyFile.resize(nFile + 1);
    }

    // A whole body record must land in one file -- WriteBodyRecord/ReadBodyRecord
    // open a single file and read or write it sequentially -- so a record that
    // would cross MAX_BODYFILE_SIZE starts a fresh file instead, exactly as
    // FindBlockPos does for blk*.dat. COMMITMENT_BUDGET_BODY_BYTES (~110 MB) is
    // comfortably under MAX_BODYFILE_SIZE (128 MiB) today; a future budget raise
    // past that would need this reconsidered, same latent constraint blk*.dat
    // already accepts for an oversized single block.
    while (vinfoBodyFile[nFile].nSize + (uint64_t) nAddSize >= MAX_BODYFILE_SIZE) {
        nFile++;
        if (vinfoBodyFile.size() <= nFile) {
            vinfoBodyFile.resize(nFile + 1);
        }
    }
    pos.nFile = (int) nFile;
    pos.nPos = vinfoBodyFile[nFile].nSize;

    nLastBodyFile = (int) nFile;
    vinfoBodyFile[nFile].nSize += nAddSize;

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

    std::vector<uint64_t> offsets;
    offsets.reserve(bodies.size());
    uint64_t cumulative = 0;
    for (const auto &tx : bodies) {
        cumulative += GetSerializeSize(*tx, SER_DISK, CLIENT_VERSION);
        offsets.push_back(cumulative);
    }

    WriteCompactSize(fileout, bodies.size());
    for (uint64_t offset : offsets) {
        WriteCompactSize(fileout, offset);
    }
    for (const auto &tx : bodies) {
        fileout << *tx;
    }

    return true;
}

namespace {

    // Reads the [count][offsets...] header at the current file position, leaving
    // the file positioned at the start of the transaction bytes.
    bool ReadBodyRecordHeader(CAutoFile &filein, std::vector<uint64_t> &offsetsOut) {
        try {
            uint64_t count = ReadCompactSize(filein);
            offsetsOut.resize(count);
            for (uint64_t &offset : offsetsOut) {
                offset = ReadCompactSize(filein);
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

    std::vector<uint64_t> offsets;
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
        countOut = (unsigned int) ReadCompactSize(filein);
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

    std::vector<uint64_t> offsets;
    if (!ReadBodyRecordHeader(filein, offsets)) {
        return false;
    }
    if (index >= offsets.size()) {
        return error("ReadBodyAt: index %u out of range (%u bodies)", index, (unsigned int) offsets.size());
    }

    // Skip every body before `index` without deserializing it -- the offset
    // table's whole point (bodystore.h).
    uint64_t skipBytes = (index == 0) ? 0 : offsets[index - 1];
    try {
        filein.ignore(skipBytes);
        filein >> txOut;
    } catch (const std::exception &e) {
        return error("ReadBodyAt: %s", e.what());
    }

    return true;
}
