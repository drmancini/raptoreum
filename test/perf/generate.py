#!/usr/bin/env python3
"""The load generator.

Reads pre-framed transactions and writes them to sockets at a fixed offered
rate. Nothing is serialised, signed or hashed here; the hot path is a memory
read and a send, so the generator's own ceiling stays far above the node's.
"""
import argparse
import os
import socket
import struct
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "functional"))

import wire  # noqa: E402
from test_framework.messages import msg_version  # noqa: E402


def load(path, limit):
    records = []
    with open(path, "rb") as f:
        while len(records) < limit:
            head = f.read(4)
            if len(head) < 4:
                break
            records.append(f.read(struct.unpack("<I", head)[0]))
    return records


def handshake(sock):
    sock.sendall(wire.frame(b"version", msg_version().serialize()))
    buf = b""
    deadline = time.time() + 15
    while time.time() < deadline:
        chunk = sock.recv(65536)
        if not chunk:
            raise RuntimeError("peer closed during handshake")
        buf += chunk
        while len(buf) >= 24:
            length = struct.unpack("<I", buf[16:20])[0]
            if len(buf) < 24 + length:
                break
            command = buf[4:16].rstrip(b"\x00")
            if command == b"version":
                sock.sendall(wire.frame(b"verack", b""))
            if command == b"verack":
                return
            buf = buf[24 + length:]
    raise RuntimeError("no verack")


def drain(sock):
    """Read and discard whatever the peer sends so its buffer never fills."""
    try:
        while sock.recv(1 << 20):
            pass
    except OSError:
        pass


def null_sink():
    """A listener that accepts and discards, for measuring the generator alone."""
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(64)

    def serve():
        while True:
            try:
                conn, _ = srv.accept()
            except OSError:
                return
            threading.Thread(target=drain, args=(conn,), daemon=True).start()
    threading.Thread(target=serve, daemon=True).start()
    return srv.getsockname()[1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=19899)
    ap.add_argument("--rate", type=float, required=True, help="offered tx/s")
    ap.add_argument("--count", type=int, default=0, help="0 = whole corpus")
    ap.add_argument("--connections", type=int, default=1)
    ap.add_argument("--sent-log", default="")
    ap.add_argument("--null", action="store_true", help="measure the generator alone")
    ap.add_argument("--batches-per-second", type=float, default=500.0)
    ap.add_argument("--duration", type=float, default=0.0, help="seconds; 0 = until corpus ends")
    ap.add_argument("--queue-cap", type=int, default=4 << 20, help="bytes buffered per socket before we count a shortfall")
    ap.add_argument("--linger", type=float, default=30.0, help="seconds to hold the connection open after the offer ends")
    a = ap.parse_args()

    limit = a.count if a.count else 1 << 30
    records = load(os.path.join(a.corpus, "txs.bin"), limit)
    txids = open(os.path.join(a.corpus, "txids.txt")).read().split()[:len(records)]
    print("loaded %d records, %.1f MB" % (len(records), sum(map(len, records)) / 1e6))

    port = null_sink() if a.null else a.port
    host = "127.0.0.1" if a.null else a.host
    socks = []
    for _ in range(a.connections):
        s = socket.create_connection((host, port), timeout=15)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if not a.null:
            handshake(s)
            threading.Thread(target=drain, args=(s,), daemon=True).start()
        socks.append(s)
    print("%d connection(s) to %s:%d%s" % (len(socks), host, port, " [null sink]" if a.null else ""))

    # A batch every 1/batches_per_second seconds keeps the offered rate smooth
    # without one syscall per transaction. Every transaction in a batch shares
    # the batch's timestamp, which at the default is a 2 ms granularity.
    per_batch = max(1, int(round(a.rate / a.batches_per_second)))
    interval = per_batch / a.rate
    for s in socks:
        s.setblocking(False)
    pending = [b""] * len(socks)

    def flush(k):
        """Push what we can without blocking; return bytes still queued."""
        if not pending[k]:
            return 0
        try:
            n = socks[k].send(pending[k])
            pending[k] = pending[k][n:]
        except BlockingIOError:
            pass
        return len(pending[k])

    sent = []
    withheld = 0
    start = time.perf_counter()
    next_t = start
    i = 0
    deadline = start + a.duration if a.duration else float("inf")
    while i < len(records) and time.perf_counter() < deadline:
        now = time.perf_counter()
        if now < next_t:
            time.sleep(next_t - now)
        k = (i // per_batch) % len(socks)
        flush(k)
        chunk = records[i:i + per_batch]
        if len(pending[k]) > a.queue_cap:
            # The node is not reading. Count the shortfall rather than block,
            # so the offered rate stays a measurement instead of an outcome.
            withheld += len(chunk)
        else:
            ts = time.time()
            pending[k] += b"".join(chunk)
            flush(k)
            for j in range(len(chunk)):
                sent.append((txids[i + j], ts))
        i += len(chunk)
        next_t += interval
    elapsed = time.perf_counter() - start

    # Anything still queued was counted as offered, so it has to reach the
    # kernel before the run ends or the tail is silently lost.
    stuck = 0
    for k, sk in enumerate(socks):
        if pending[k]:
            sk.setblocking(True)
            sk.settimeout(30)
            try:
                sk.sendall(pending[k])
            except OSError:
                stuck += len(pending[k])
            pending[k] = b""
    if stuck:
        print("WARNING: %d bytes could not be flushed at the end" % stuck)

    if a.linger:
        # The node discards a peer's unprocessed receive buffer when the peer
        # goes away, so closing at the end of the offer throws away whatever it
        # had not reached yet and understates what it accepted.
        print("holding connections open %.0fs so the node can drain" % a.linger)
        time.sleep(a.linger)

    print("target %.0f tx/s, batch %d, %.1fs" % (a.rate, per_batch, elapsed))
    print("offered  %d (%.0f tx/s)" % (len(sent), len(sent) / elapsed))
    print("withheld %d (%.1f%%) because the node stopped reading"
          % (withheld, 100.0 * withheld / max(1, withheld + len(sent))))
    if a.sent_log:
        with open(a.sent_log, "w") as f:
            for txid, ts in sent:
                f.write("SENT txid=%s ts=%d\n" % (txid, int(ts * 1000)))
        print("wrote", a.sent_log)
    for s in socks:
        s.close()


if __name__ == "__main__":
    main()
