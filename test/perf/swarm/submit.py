#!/usr/bin/env python3
"""Offer a corpus shard to a local node over JSON-RPC at a fixed rate.

Stdlib only, so it runs on every Python from 3.6 up -- no test_framework, no
asyncore, no raptoreum_hash. Submitting over RPC rather than P2P also keeps the
generator off the node's peer list: a P2P generator is itself a peer, so the
node announces transactions back to it, and a full send queue then stops the
node reading our offered load (measuring the generator, not the node).

Writes one line per submission to --log: monotonic_ns, wall_ns, txid, status.
"""
import argparse, base64, http.client, json, os, struct, sys, threading, time
from queue import Queue, Empty

def load(path, limit, skip=0):
    """Read the corpus and return raw transactions.

    Each corpus record is a complete P2P message, not a bare transaction: a
    24-byte header (magic, 12-byte command, payload length, checksum) followed
    by the serialised tx. The P2P generator writes them to the socket as-is;
    over RPC we need the payload alone, so unwrap and verify rather than
    assuming a fixed offset.
    """
    recs = []
    skipped = [0]
    with open(path, "rb") as f:
        while True:
            head = f.read(4)
            if len(head) < 4:
                break
            n = struct.unpack("<I", head)[0]
            rec = f.read(n)
            if len(rec) < n:
                break
            cmd = rec[4:16].rstrip(b"\x00")
            plen = struct.unpack("<I", rec[16:20])[0]
            if cmd != b"tx" or plen != n - 24:
                raise SystemExit("corpus record %d is not a tx message "
                                 "(command=%r, payload=%d, record=%d)"
                                 % (len(recs), cmd, plen, n))
            seen = len(recs) + skipped[0]
            if skipped[0] < skip:
                skipped[0] += 1
                continue
            recs.append(rec[24:])
            if limit and len(recs) >= limit:
                break
    return recs

class RPC:
    def __init__(self, host, port, user, pw):
        self.addr = (host, port)
        self.hdr = {"Content-Type": "application/json",
                    "Authorization": "Basic " + base64.b64encode(
                        ("%s:%s" % (user, pw)).encode()).decode()}
        self.c = http.client.HTTPConnection(host, port, timeout=30)
    def call(self, method, params):
        body = json.dumps({"jsonrpc": "1.0", "id": "s", "method": method, "params": params})
        for attempt in (0, 1):
            try:
                self.c.request("POST", "/", body, self.hdr)
                r = self.c.getresponse()
                data = json.loads(r.read())
                return data.get("result"), data.get("error")
            except Exception as e:
                if attempt:
                    return None, {"message": "transport: %s" % e}
                self.c.close()
                self.c = http.client.HTTPConnection(*self.addr, timeout=30)
        return None, {"message": "unreachable"}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shard", required=True)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=19898)
    ap.add_argument("--user", default="swarm")
    ap.add_argument("--password", required=True)
    ap.add_argument("--rate", type=float, required=True, help="offered tx/s")
    ap.add_argument("--duration", type=float, default=0.0, help="seconds; 0 = whole shard")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--skip", type=int, default=0,
                    help="skip this many transactions; successive runs must not "
                         "re-offer transactions an earlier run already spent")
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--log", default="")
    a = ap.parse_args()

    recs = load(a.shard, a.limit, a.skip)
    print("loaded %d tx, %.1f MB (skipped %d)" % (len(recs), sum(map(len, recs)) / 1e6, a.skip), flush=True)

    q = Queue(maxsize=20000)
    stats = {"ok": 0, "err": 0}
    reasons = {}
    lock = threading.Lock()
    logf = open(a.log, "w") if a.log else None
    stop = threading.Event()

    def worker():
        rpc = RPC(a.host, a.port, a.user, a.password)
        while not stop.is_set():
            try:
                hexed = q.get(timeout=0.5)
            except Empty:
                continue
            t = time.monotonic_ns(); w = time.time_ns()
            res, err = rpc.call("sendrawtransaction", [hexed])
            with lock:
                if err is None:
                    stats["ok"] += 1
                    st = "ok"
                else:
                    stats["err"] += 1
                    st = (err.get("message") or "?")[:60]
                    reasons[st] = reasons.get(st, 0) + 1
                if logf:
                    logf.write("%d %d %s %s\n" % (t, w, res or "-", st.replace(" ", "_")))
            q.task_done()

    ws = [threading.Thread(target=worker, daemon=True) for _ in range(a.threads)]
    for w in ws:
        w.start()

    t0 = time.monotonic()
    deadline = t0 + a.duration if a.duration else None
    sent = 0
    for rec in recs:
        if deadline and time.monotonic() >= deadline:
            break
        target = t0 + sent / a.rate
        now = time.monotonic()
        if target > now:
            time.sleep(target - now)
        q.put(rec.hex())
        sent += 1
    q.join()
    stop.set()
    dur = time.monotonic() - t0
    if logf:
        logf.close()
    print("offered %d in %.1fs = %.0f tx/s | accepted %d | rejected %d"
          % (sent, dur, sent / dur, stats["ok"], stats["err"]), flush=True)
    for r, c in sorted(reasons.items(), key=lambda kv: -kv[1])[:5]:
        print("  reject %6d  %s" % (c, r), flush=True)

if __name__ == "__main__":
    main()
