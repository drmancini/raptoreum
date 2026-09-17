#!/usr/bin/env python3
"""Reproduce the ThreadSocketHandler busy-loop on a receive-paused peer.

A peer whose queued message bytes exceed -maxreceivebuffer is marked fPauseRecv
and is skipped by the receive path, but it stays in mapReceivableNodes with its
readable flag set. SocketHandler treats a non-empty receivable set as work and
polls with a zero timeout, so while the pause lasts the socket thread spins.

Run the node with a small -maxreceivebuffer so the pause is reached immediately,
then hold a peer that writes continuously and never lets the node catch up. The
measurement is the rtm-net thread's own CPU time from /proc, which needs no
instrumented build and no profiler:

    broken: rtm-net at ~100% of a core while nothing progresses
    fixed:  rtm-net near idle

Usage:
    repro_busyloop.py --port 19899 --pid <raptoreumd pid> [--seconds 10]
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

CLK = os.sysconf("SC_CLK_TCK")
RPC_PORT = 19941


def thread_cpu(pid, comm):
    """(utime, stime) in seconds for the named thread of a process.

    Split because a spin and memory thrash both raise total CPU: a spin is
    dominated by user time in the poll loop, page-fault churn by system time.
    """
    base = "/proc/%d/task" % pid
    for tid in os.listdir(base):
        try:
            with open(os.path.join(base, tid, "comm")) as fh:
                if fh.read().strip() != comm:
                    continue
            with open(os.path.join(base, tid, "stat")) as fh:
                fields = fh.read()
        except OSError:
            continue
        rest = fields[fields.rfind(")") + 2:].split()
        return int(rest[11]) / CLK, int(rest[12]) / CLK
    return None


def proc_mem(pid):
    """(rss_bytes, minor_faults, major_faults) for the whole process.

    The control for "was this memory pressure rather than a spin": a node that
    is thrashing grows its RSS and takes faults; one that is spinning does
    neither.
    """
    with open("/proc/%d/stat" % pid) as fh:
        fields = fh.read()
    rest = fields[fields.rfind(")") + 2:].split()
    minflt, majflt = int(rest[7]), int(rest[9])
    rss_pages = int(open("/proc/%d/statm" % pid).read().split()[1])
    return rss_pages * 4096, minflt, majflt


def node_bytes_recv(datadir, chain="regtest"):
    """Total bytes the node has received, from its own counters.

    The busy-loop wastes CPU, but polling a paused peer continuously is also how
    the shipped code re-checks it. Removing the spin means the peer is only
    re-checked when the socket thread next wakes, so drain throughput has to be
    measured too, not just CPU.
    """
    import http.client, base64, json as _json
    cookie = open(os.path.join(datadir, chain, ".cookie")).read().strip()
    auth = base64.b64encode(cookie.encode()).decode()
    conn = http.client.HTTPConnection("127.0.0.1", RPC_PORT, timeout=15)
    conn.request("POST", "/", _json.dumps({"method": "getnettotals", "params": [], "id": 1}),
                 {"Authorization": "Basic " + auth, "Content-Type": "application/json"})
    body = _json.loads(conn.getresponse().read())
    conn.close()
    return body["result"]["totalbytesrecv"]


def system_swap():
    used = total = 0
    for line in open("/proc/meminfo"):
        if line.startswith("SwapTotal:"):
            total = int(line.split()[1]) * 1024
        elif line.startswith("SwapFree:"):
            used = total - int(line.split()[1]) * 1024
    return used


def handshake(sock):
    sock.sendall(wire.frame(b"version", msg_version().serialize()))
    deadline = time.time() + 15
    while time.time() < deadline:
        if not sock.recv(65536):
            raise RuntimeError("peer closed during handshake")
        sock.sendall(wire.frame(b"verack", b""))
        return
    raise RuntimeError("no version from peer")


def flood(sock, stop):
    """Write continuously and never read.

    Not reading is the point: it keeps the node's queued bytes above the flood
    size so the pause persists, which is the window the loop spins in.
    """
    # An inv of 50,000 entries is a large payload the node must queue, and is
    # cheap for us to produce. Contents do not matter; they are never processed.
    count = 50000
    payload = b"\xfd" + struct.pack("<H", count) + b"".join(
        struct.pack("<I", 1) + os.urandom(32) for _ in range(count))
    msg = wire.frame(b"inv", payload)
    while not stop.is_set():
        try:
            sock.sendall(msg)
        except OSError:
            return


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=19899)
    ap.add_argument("--pid", type=int, required=True, help="raptoreumd pid")
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--thread", default="rtm-net")
    ap.add_argument("--datadir", default="", help="node datadir, to also measure drain throughput")
    a = ap.parse_args()

    if thread_cpu(a.pid, a.thread) is None:
        sys.exit("no thread named %s in pid %d" % (a.thread, a.pid))

    sock = socket.create_connection((a.host, a.port), timeout=20)
    handshake(sock)

    stop = threading.Event()
    t = threading.Thread(target=flood, args=(sock, stop), daemon=True)

    recv0 = node_bytes_recv(a.datadir) if a.datadir else None
    u0, s0 = thread_cpu(a.pid, a.thread)
    rss0, min0, maj0 = proc_mem(a.pid)
    swap0 = system_swap()
    t0 = time.time()
    t.start()
    time.sleep(a.seconds)
    elapsed = time.time() - t0
    u1, s1 = thread_cpu(a.pid, a.thread)
    recv1 = node_bytes_recv(a.datadir) if a.datadir else None
    rss1, min1, maj1 = proc_mem(a.pid)
    swap1 = system_swap()
    stop.set()
    sock.close()

    user = (u1 - u0) / elapsed * 100
    sys_ = (s1 - s0) / elapsed * 100
    print("%s over %.1fs with one receive-paused peer:" % (a.thread, elapsed))
    print("  CPU          %.1f%% of a core  (user %.1f%%, system %.1f%%)" % (user + sys_, user, sys_))
    print("  process RSS  %.1f MB -> %.1f MB  (delta %+.1f MB)"
          % (rss0 / 1e6, rss1 / 1e6, (rss1 - rss0) / 1e6))
    print("  page faults  minor %+d, major %+d" % (min1 - min0, maj1 - maj0))
    print("  system swap  %.1f MB -> %.1f MB" % (swap0 / 1e6, swap1 / 1e6))
    if recv0 is not None:
        print("  DRAINED      %.1f MB in %.1fs = %.1f MB/s"
              % ((recv1 - recv0) / 1e6, elapsed, (recv1 - recv0) / 1e6 / elapsed))
    print()
    print("  A spinning socket thread sits near 100%% with a flat RSS and no major")
    print("  faults. Memory pressure would instead show RSS growth, major faults or")
    print("  swap, and would put the time in the system column.")
    return 0 if (user + sys_) < 50 else 1


if __name__ == "__main__":
    sys.exit(main())
