#!/usr/bin/env python3
"""Reproduce the ThreadSocketHandler busy-loop on a receive-paused peer.

Self-contained: standard library only, no test framework, no build flags.

BACKGROUND

CConnman::SocketHandler() skips waiting for socket events whenever a node is in
mapReceivableNodes, and polls with a zero timeout instead. But the receive path
drains a node only when it is unpaused, has an empty send queue, and is not
disconnecting. A node failing any of those lingers in the set with its readable
flag set, the poll returns nothing actionable, and the socket thread spins at
100% of a core for as long as the condition holds.

There are two ways to make a node undrainable, and they need very different
things from the operator:

  pause      the peer's queued message bytes exceed -maxreceivebuffer, which at
             the 5 MB default takes a sustained flood.

  sendqueue  the node simply has bytes queued to send to that peer. This needs
             no configuration and no flood -- an ordinary peer you are mid-
             exchange with will do. It is the route seen under normal relay
             load, where the socket handler was measured at 1.4 million
             iterations per second with -maxreceivebuffer set to 600 MB, i.e.
             with the pause route unreachable.

HOW TO RUN

  raptoreumd -regtest -datadir=/tmp/spin -listen=1 -port=19940 [-maxreceivebuffer=1]
  contrib/devtools/repro-socket-busyloop.py --port 19940 --pid $(pidof raptoreumd) \
      --mode sendqueue

--mode pause needs -maxreceivebuffer=1, which puts the pause threshold at 1000
bytes (nReceiveFloodSize is 1000 * the option) so one peer reaches it at once.
It then writes continuously and never reads, holding the pause.

--mode sendqueue needs no special option. It sends pings and never reads the
replies: the node queues a pong for each, its socket buffer to us fills, and its
send queue stays non-empty -- while our pings keep its socket readable. The node
then wants to read from a peer it will not read from, which is the spin.

WHAT TO LOOK FOR

  rtm-net CPU near 100% of a core   -> the bug is present
  rtm-net CPU near 0                -> fixed

Read from /proc, so no profiler and no instrumented build are needed; `top -H -p
<pid>` shows the same thing.

The script also reports how much it managed to write. The socket is blocking, so
that tracks how fast the node drains us, and it has to be checked alongside the
CPU: removing the busy-loop without also waking the socket thread when the pause
clears halves the drain rate while appearing to be a pure CPU win.
"""
import argparse
import hashlib
import os
import socket
import struct
import sys
import threading
import time

CLK = os.sysconf("SC_CLK_TCK")
MAGIC = {"regtest": b"\xfc\xc1\xb7\xdc", "main": b"\xbf\x0c\x6b\xbd", "test": b"\xce\xe2\xca\xff"}


def frame(command, payload, net="regtest"):
    checksum = hashlib.sha256(hashlib.sha256(payload).digest()).digest()[:4]
    return MAGIC[net] + command.ljust(12, b"\x00") + struct.pack("<I", len(payload)) + checksum + payload


def version_payload():
    return (struct.pack("<iQq", 70220, 0, int(time.time()))
            + b"\x00" * 26 + b"\x00" * 26
            + struct.pack("<Q", 0) + b"\x00" + struct.pack("<i", 0) + b"\x00")


def thread_cpu(pid, comm):
    base = "/proc/%d/task" % pid
    for tid in os.listdir(base):
        try:
            if open(os.path.join(base, tid, "comm")).read().strip() != comm:
                continue
            fields = open(os.path.join(base, tid, "stat")).read()
        except OSError:
            continue
        rest = fields[fields.rfind(")") + 2:].split()
        return (int(rest[11]) + int(rest[12])) / CLK
    return None


class Written:
    """Bytes the peer has managed to write.

    The socket is blocking, so writes only complete as fast as the node drains
    them. That makes this a usable measure of drain throughput, and it matters
    here: removing the busy-loop without waking the socket thread when the pause
    clears cuts the rate roughly in half while looking like a pure CPU win.
    """

    def __init__(self):
        self.n = 0


def flood_pause(sock, stop, net, written):
    """Push message bytes faster than the node processes them, to hold fPauseRecv.

    Never reading is the point: it keeps the node's queued bytes above the flood
    size, which is the window the loop spins in.
    """
    # A large inv the node must queue. Contents are irrelevant; it is never processed.
    count = 50000
    payload = b"\xfd" + struct.pack("<H", count) + b"".join(
        struct.pack("<I", 1) + os.urandom(32) for _ in range(count))
    msg = frame(b"inv", payload, net)
    while not stop.is_set():
        try:
            sock.sendall(msg)
            written.n += len(msg)
        except OSError:
            return


def flood_sendqueue(sock, stop, net, written):
    """Make the node's send queue to us non-empty, and keep our socket readable.

    Each ping is answered with a pong. We never read, so the node's writes back
    up until nSendMsgSize stays above zero; meanwhile the pings keep arriving, so
    the node sees a readable socket it is not willing to read from. That is the
    undrainable state, reached without touching -maxreceivebuffer.
    """
    ping = frame(b"ping", os.urandom(8), net)
    burst = ping * 256
    while not stop.is_set():
        try:
            sock.sendall(burst)
            written.n += len(burst)
        except OSError:
            return


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--pid", type=int, required=True, help="raptoreumd pid")
    ap.add_argument("--net", default="regtest", choices=sorted(MAGIC))
    ap.add_argument("--thread", default="rtm-net")
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--mode", default="sendqueue", choices=("pause", "sendqueue"),
                    help="how to make the peer undrainable; sendqueue needs no node options")
    a = ap.parse_args()

    if thread_cpu(a.pid, a.thread) is None:
        sys.exit("no thread named %s in pid %d" % (a.thread, a.pid))

    sock = socket.create_connection((a.host, a.port), timeout=20)
    sock.sendall(frame(b"version", version_payload(), a.net))
    sock.recv(65536)
    sock.sendall(frame(b"verack", b"", a.net))

    stop = threading.Event()
    before = thread_cpu(a.pid, a.thread)
    t0 = time.time()
    written = Written()
    worker = flood_pause if a.mode == "pause" else flood_sendqueue
    threading.Thread(target=worker, args=(sock, stop, a.net, written), daemon=True).start()
    time.sleep(a.seconds)
    elapsed = time.time() - t0
    pct = (thread_cpu(a.pid, a.thread) - before) / elapsed * 100
    stop.set()
    sock.close()

    print("%s CPU over %.1fs with one undrainable peer (%s): %.1f%% of a core"
          % (a.thread, elapsed, a.mode, pct))
    print("  near 100 -> busy-loop present;  near 0 -> fixed")
    print("  drained %.1f MB = %.1f MB/s  (should not fall when the spin goes away)"
          % (written.n / 1e6, written.n / 1e6 / elapsed))
    return 1 if pct >= 50 else 0


if __name__ == "__main__":
    sys.exit(main())
