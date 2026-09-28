#!/usr/bin/env python3
"""A byte-cursor for RTM's special-transaction payloads (vExtraPayload).

Shared by the 0.2 full-scope characterisation files (feature_characterise_assets.py,
feature_characterise_futures.py, feature_characterise_specialtx.py). Not a general
serializer: GetTxPayload (evo/specialtx.h) only ever needs to be WRITTEN by the
node's own wallet/RPC path, so these tests never construct a payload from
scratch. They take a genuine, node-produced vExtraPayload, walk it field by
field with this cursor to find one field's byte range, and splice in a new
value of arbitrary length -- everything before and after that field is passed
through unchanged, so nothing else needs to be understood or re-encoded.

This works because CheckSpecialTx (evo/specialtx.cpp) runs, for both the
mempool path (validation.cpp:AcceptToMemoryPoolWorker) and the block-connect
path (validation.cpp:ConnectBlock -> ProcessSpecialTxsInBlock), BEFORE
Consensus::CheckTxInputs and BEFORE script/signature verification (CheckInputs).
A payload mutation invalidates the outer transaction's ECDSA signature (the
sighash serializes vExtraPayload in full, script/interpreter.cpp:
CTransactionSignatureSerializer::Serialize, line ~1385), but that is never
reached: CheckSpecialTx's own rejection fires first, which is exactly the
observation these tests want. It is also why these tests can use throwaway,
unspendable coin references for the outer transaction (mirroring
feature_characterise_accept.py's own mutation transactions) rather than a real
wallet spend, for any field whose rule does not itself depend on chain state
(an existing asset, a registered masternode, ...).

Field types, matching the C++ SERIALIZE_METHODS exactly (primitives/transaction.h
CKeyID/uint256 style base_blob<N>::Serialize writes N raw bytes, no length
prefix; CAmount is int64_t; bool/uint8_t are one byte; uint16_t is two bytes,
all little-endian, matching every other field in this codebase's wire format):

    u8, u16, u32       fixed-width, little-endian
    i32                fixed-width signed, little-endian
    i64                CAmount, little-endian
    bytes20/32         CKeyID / uint256, raw, no prefix
    compact_bytes      CompactSize-prefixed byte string (std::string, CScript,
                       std::vector<unsigned char> all serialize this way)
"""
import struct


def compact_size_len(n):
    if n < 253:
        return 1
    elif n < 0x10000:
        return 3
    elif n < 0x100000000:
        return 5
    else:
        return 9


def ser_compact_size(n):
    if n < 253:
        return struct.pack("<B", n)
    elif n < 0x10000:
        return struct.pack("<BH", 253, n)
    elif n < 0x100000000:
        return struct.pack("<BI", 254, n)
    else:
        return struct.pack("<BQ", 255, n)


class PayloadCursor:
    """Walks a vExtraPayload byte string field by field, matching a
    SERIALIZE_METHODS field list one call at a time. Raises on underrun --
    that mirrors GetTxPayload's own CDataStream exception path, so a test
    calling this on a too-short buffer sees the same class of failure the
    node would."""

    def __init__(self, data):
        self.data = data
        self.pos = 0

    def _take(self, n):
        if self.pos + n > len(self.data):
            raise ValueError("payload cursor: underrun reading %d bytes at %d of %d"
                              % (n, self.pos, len(self.data)))
        start = self.pos
        self.pos += n
        return start, self.pos

    def skip_fixed(self, n):
        """A fixed-width field (u8/u16/u32/i32/i64/bytesN). Returns (start, end)."""
        return self._take(n)

    def read_compact_size(self):
        b0 = self.data[self.pos]
        if b0 < 253:
            self.pos += 1
            return b0
        elif b0 == 253:
            v = struct.unpack_from("<H", self.data, self.pos + 1)[0]
            self.pos += 3
            return v
        elif b0 == 254:
            v = struct.unpack_from("<I", self.data, self.pos + 1)[0]
            self.pos += 5
            return v
        else:
            v = struct.unpack_from("<Q", self.data, self.pos + 1)[0]
            self.pos += 9
            return v

    def skip_compact_bytes(self):
        """A CompactSize-prefixed field (std::string / CScript / vector<uchar>).
        Returns (start, end) covering the length prefix AND the content, so
        the caller can replace the whole field including its own length."""
        start = self.pos
        n = self.read_compact_size()
        self._take(n)
        return start, self.pos

    def splice(self, field_start, field_end, new_bytes):
        """Everything outside [field_start, field_end) is untouched -- the
        offsets of fields already walked (before field_start) don't move, and
        fields not yet walked (after field_end) don't need to be understood
        at all, only carried through byte for byte."""
        return self.data[:field_start] + new_bytes + self.data[field_end:]


def new_compact_bytes(content: bytes) -> bytes:
    return ser_compact_size(len(content)) + content
