#!/usr/bin/env python3
"""Emit one corpus transaction with a signature that is well-formed but wrong.

The control for the -perfskipsigs arm. A stock node must reject this; a node
with signature verification disabled must accept it. Without that pair the arm
is not proven to do anything, and a null result from it would mean nothing.

The signature is not corrupted by flipping bytes: that usually breaks the DER
encoding, and the node would then reject it on CheckSignatureEncoding even with
verification skipped, which would make the control pass for the wrong reason.
Instead the signature is replaced with a genuine one over an unrelated digest,
so it is valid DER and low-S and fails only the verification itself.
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "functional"))

import coincurve  # noqa: E402
from test_framework.messages import CTransaction  # noqa: E402
from test_framework.script import CScript  # noqa: E402
from io import BytesIO  # noqa: E402

HEADER = 24  # magic + command + length + checksum


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--index", type=int, default=0)
    a = ap.parse_args()

    with open(a.corpus, "rb") as fh:
        for _ in range(a.index + 1):
            head = fh.read(4)
            if len(head) < 4:
                sys.exit("corpus ended before index %d" % a.index)
            record = fh.read(struct.unpack("<I", head)[0])

    tx = CTransaction()
    tx.deserialize(BytesIO(record[HEADER:]))
    assert tx.vin, "transaction has no inputs"

    pushes = list(CScript(tx.vin[0].scriptSig))
    assert len(pushes) == 2, "expected a P2PKH scriptSig, got %d pushes" % len(pushes)
    sig, pubkey = pushes
    hashtype = sig[-1:]

    wrong = coincurve.PrivateKey(os.urandom(32)).sign(os.urandom(32), hasher=None)
    tx.vin[0].scriptSig = CScript([wrong + hashtype, pubkey])
    tx.rehash()
    print(tx.serialize().hex())


if __name__ == "__main__":
    main()
