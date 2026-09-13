#!/usr/bin/env python3
"""Prove the rig's bytes are bytes this node accepts.

Builds and signs one transaction offline from the fan-out set, frames it, pushes
it in over a raw socket with a real handshake, and checks it reaches the mempool.
Everything the corpus builder and the generator rely on is exercised here.
"""
import argparse
import json
import os
import socket
import struct
import sys
import time

import coincurve

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "functional"))

import wire  # noqa: E402
from test_framework.messages import msg_version  # noqa: E402
from fanout import rpc  # noqa: E402


def handshake(sock):
    sock.sendall(wire.frame(b"version", msg_version().serialize()))
    seen = set()
    buf = b""
    deadline = time.time() + 15
    while b"verack" not in seen and time.time() < deadline:
        chunk = sock.recv(65536)
        if not chunk:
            raise RuntimeError("peer closed during handshake")
        buf += chunk
        while len(buf) >= 24:
            length = struct.unpack("<I", buf[16:20])[0]
            if len(buf) < 24 + length:
                break
            command = buf[4:16].rstrip(b"\x00")
            seen.add(command)
            if command == b"version":
                sock.sendall(wire.frame(b"verack", b""))
            buf = buf[24 + length:]
    if b"verack" not in seen:
        raise RuntimeError("no verack; saw %s" % sorted(seen))
    return sorted(seen)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--datadir", required=True)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--wallet", default="perf")
    ap.add_argument("--port", type=int, default=19899)
    ap.add_argument("--fee", type=int, default=10000)
    a = ap.parse_args()

    utxos = json.load(open(os.path.join(a.corpus, "utxos.json")))
    spend, pay = utxos[:2], utxos[2:4]

    inputs = [(int(u["txid"], 16), u["vout"], u["value"], bytes.fromhex(u["pubkey"]))
              for u in spend]
    total = sum(u["value"] for u in spend)
    each = (total - a.fee) // 2
    outputs = [(bytes.fromhex(u["pubkey"]), each) for u in pay]

    tx = wire.build_tx(inputs, outputs)
    keys = [coincurve.PrivateKey(bytes.fromhex(u["secret"])) for u in spend]
    wire.sign_tx(tx, inputs, lambda i, msg32: keys[i].sign(msg32, hasher=None))
    payload = wire.tx_message(tx)
    print("txid %s, %d bytes on the wire (%d payload)"
          % (tx.hash, len(payload), len(payload) - 24))

    node = rpc(a.datadir, wallet=a.wallet)
    before = set(node.getrawmempool())

    sock = socket.create_connection(("127.0.0.1", a.port), timeout=15)
    print("handshake saw:", " ".join(c.decode() for c in handshake(sock)))
    sock.sendall(payload)

    for _ in range(30):
        now = set(node.getrawmempool())
        if tx.hash in now:
            entry = node.getmempoolentry(tx.hash)
            print("ACCEPTED: in mempool, size %s, fee %s" % (entry["size"], entry["fee"]))
            sock.close()
            return 0
        if now != before:
            print("mempool changed but not to our txid:", now - before)
        time.sleep(0.5)

    sock.close()
    print("NOT ACCEPTED: txid never reached the mempool")
    return 1


if __name__ == "__main__":
    sys.exit(main())
