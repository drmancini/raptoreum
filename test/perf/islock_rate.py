#!/usr/bin/env python3
# Copyright (c) 2026 The Raptoreum developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Measure how fast a quorum can produce InstantSend locks.

The BLS benchmarks give the cost of the cryptography in isolation. This gives
the rate the assembled pipeline actually reaches, so the difference between the
two is the plumbing: the message round trips, the 100 ms send cadence in
CSigSharesManager::SendMessages, and the fact that the whole signing pipeline
runs on one thread.

Transactions are submitted directly to every node rather than relayed, so relay
is not part of what is being measured; see docs/perf-results.md section 8 for
that. Everything is pre-signed before the clock starts, so the wallet is not
either.

Configured through the environment because the framework builds the node set in
__init__, before options are parsed:

    ISLOCK_MNS        smartnodes to start          (default 5)
    ISLOCK_SIZE       quorum size                  (default 5)
    ISLOCK_THRESHOLD  quorum threshold             (default 3)
    ISLOCK_COUNT      transactions to submit       (default 200)
    ISLOCK_RATE       offered rate in tx/s, 0 = burst (default 0)
    ISLOCK_OUT        write the result as JSON here
"""
import json
import os
import re
import threading
import time
from datetime import datetime, timezone
from decimal import Decimal

from test_framework.authproxy import AuthServiceProxy
from test_framework.test_framework import RaptoreumTestFramework
from test_framework.util import get_chain_folder, satoshi_round

MNS = int(os.environ.get("ISLOCK_MNS", "5"))
SIZE = int(os.environ.get("ISLOCK_SIZE", "5"))
THRESHOLD = int(os.environ.get("ISLOCK_THRESHOLD", "3"))
COUNT = int(os.environ.get("ISLOCK_COUNT", "200"))
RATE = float(os.environ.get("ISLOCK_RATE", "0"))   # transactions/s; 0 submits as fast as possible
OUT = os.environ.get("ISLOCK_OUT", "")

# Outputs per fan-out transaction. More than this exceeds MAX_STANDARD_TX_SIZE.
FANOUT_CHUNK = 1000

# A debug.log line from CSigSharesManager::TryRecoverSig, with -logtimemicros on:
# 2026-09-15T05:21:48.592089Z (mocktime: ...) ... recovered signature. id=..., msgHash=..., time=1
RECOVERED = re.compile(
    r"^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d+)Z.*"
    r"recovered signature\. id=\w+, msgHash=\w+, time=(\d+)")


class ISLockRateTest(RaptoreumTestFramework):
    def set_test_params(self):
        # The framework starts nodes with -debug, meaning every category. That
        # writes a line per sig share under a lock and lands squarely in what is
        # being measured, so turn it all off and re-enable only the one category
        # the rate is read from. Later arguments win.
        debug = ["-nodebug", "-debug=llmq-sigs"]
        self.set_raptoreum_test_params(MNS + 1, MNS, extra_args=[debug] * (MNS + 1),
                                       fast_dip3_enforcement=True)
        self.set_raptoreum_llmq_test_params(SIZE, THRESHOLD)

    def run_test(self):
        self.nodes[0].spork("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()
        self.mine_quorum()

        node = self.nodes[0]
        self.log.info("Building %d spendable outputs" % COUNT)
        fee = satoshi_round(Decimal("0.0001"))
        value = satoshi_round(Decimal("0.01"))
        # One sendmany per FANOUT_CHUNK outputs: a single transaction with
        # thousands of them exceeds the standard size limit and is refused with
        # "Transaction too large".
        fans = []
        remaining = COUNT
        while remaining > 0:
            n = min(FANOUT_CHUNK, remaining)
            addrs = [node.getnewaddress() for _ in range(n)]
            fans.append((node.sendmany("", {a: value for a in addrs}), set(addrs)))
            remaining -= n

        # The fan-out has to be confirmed before its children are submitted, or
        # they are all descendants of one mempool entry and everything past the
        # 25th is rejected with too-long-mempool-chain. IsTxSafeForMining refuses
        # to mine a transaction that is neither islocked nor WAIT_FOR_ISLOCK_TIMEOUT
        # old, so wait for the locks rather than just generating.
        for fan, _ in fans:
            self.wait_for_instantlock(fan, node, timeout=120)
        node.generate(2)
        self.sync_all()

        outs = []
        for fan, addrs in fans:
            tx = node.getrawtransaction(fan, True)
            assert tx["confirmations"] >= 1, \
                "fan-out did not confirm; its children would hit the descendant limit"
            outs += [(fan, v["n"]) for v in tx["vout"]
                     if v["value"] == value and v["scriptPubKey"]["addresses"][0] in addrs]
        outs = outs[:COUNT]
        assert len(outs) == COUNT, "fan-out produced %d outputs, wanted %d" % (len(outs), COUNT)

        self.log.info("Pre-signing %d transactions" % len(outs))
        sink = node.getnewaddress()
        raws = []
        for fan, n in outs:
            raw = node.createrawtransaction([{"txid": fan, "vout": n}],
                                            {sink: satoshi_round(value - fee)})
            raws.append(node.signrawtransactionwithwallet(raw)["hex"])

        stop = threading.Event()
        bumper = threading.Thread(target=self._bump_until, args=(stop,), daemon=True)
        sampler = CpuSampler(self.nodes, stop)
        bumper.start()
        sampler.start()

        self.log.info("Submitting to all %d nodes" % len(self.nodes))
        t0 = time.time()
        txids, errors = self._submit(raws)
        t_submitted = time.time()
        if errors:
            self.log.warning("%d submissions failed, first: %s" % (len(errors), errors[0]))
        assert len(txids) == len(raws), \
            "only %d of %d transactions were accepted" % (len(txids), len(raws))

        self.log.info("Submitted in %.2fs, waiting for locks" % (t_submitted - t0))
        locked_at = self._wait_for_locks(node, txids)
        t_last = max(locked_at.values()) if locked_at else t_submitted
        stop.set()
        sampler.join()

        recoveries = self._recoveries()
        elapsed = t_last - t0
        sessions = sum(len([r for r in v if r[0] >= t0]) for v in recoveries.values())

        result = dict(
            smartnodes=MNS, quorum_size=SIZE, quorum_threshold=THRESHOLD,
            transactions=len(txids),
            locked=len(locked_at),
            offered_rate=RATE or None,
            submit_seconds=round(t_submitted - t0, 2),
            total_seconds=round(elapsed, 2),
            locks_per_second=round(len(locked_at) / elapsed, 1) if elapsed > 0 else None,
            sessions_per_second=round(sessions / elapsed, 1) if elapsed > 0 else None,
            recovery_ms=self._recovery_stats(recoveries),
            per_node_sessions=self._session_rates(recoveries, t0),
            sigshares_cpu_pct=sampler.summary(until=t_last),
        )
        self.log.info(json.dumps(result, indent=2))
        if OUT:
            with open(OUT, "w") as fh:
                json.dump(result, fh, indent=2)

    def _bump_until(self, stop):
        # Relay and several LLMQ timers read the mocked clock. Keep it moving at
        # roughly real time for the duration of the run. Its own connections:
        # an AuthServiceProxy holds one HTTP connection and is not shareable
        # across threads.
        conns = [AuthServiceProxy(n.url, timeout=60) for n in self.nodes]
        base, started = self.mocktime, time.time()
        while not stop.is_set():
            # Track real time exactly. Running the mocked clock faster shortens
            # every LLMQ timeout in proportion: at twice real time the 60 s
            # SESSION_NEW_SHARES_TIMEOUT fires after 30 real seconds, which is
            # less than a large backlog takes to drain, and sessions are purged
            # mid-flight. That stalls the run and looks exactly like a node
            # defect.
            t = base + int(time.time() - started)
            for c in conns:
                try:
                    c.setmocktime(t)
                except Exception:
                    pass
            stop.wait(1.0)

    def _submit(self, raws, workers=8):
        """Submit every transaction to every node.

        Relay would deliver them on the trickle, which is measured separately in
        section 8 and would dominate here; handing each node the transaction
        directly leaves only the signing path in the measurement.
        """
        txids = [None] * len(raws)
        errors = []

        # A paced offer answers a different question from a burst. A burst finds
        # how long a fixed amount of work takes; a paced offer finds the rate at
        # which the quorum stops keeping up, which is where sessions start timing
        # out and their transactions stop locking altogether.
        started = time.time()

        def send(node_url, idxs, record):
            conn = AuthServiceProxy(node_url, timeout=120)
            for i in idxs:
                if RATE > 0:
                    # i is the global index, so every shard and every node paces
                    # against the same schedule.
                    delay = started + i / RATE - time.time()
                    if delay > 0:
                        time.sleep(delay)
                try:
                    txid = conn.sendrawtransaction(raws[i])
                    if record:
                        txids[i] = txid
                except Exception as e:
                    # A node that already has the transaction is not an error;
                    # anything else is, and silently dropping it once hid a
                    # descendant-limit rejection behind a plausible-looking result.
                    msg = str(e)
                    if "already" not in msg and record:
                        errors.append(msg)

        shards = [list(range(k, len(raws), workers)) for k in range(workers)]
        threads = []
        for node_i, node in enumerate(self.nodes):
            for shard in shards:
                th = threading.Thread(target=send, args=(node.url, shard, node_i == 0))
                th.start()
                threads.append(th)
        for th in threads:
            th.join()
        return [t for t in txids if t], errors

    def _wait_for_locks(self, node, txids, timeout=600):
        locked_at = {}
        deadline = time.time() + timeout
        pending = list(txids)
        conn = AuthServiceProxy(node.url, timeout=120)
        while pending and time.time() < deadline:
            still = []
            for txid in pending:
                try:
                    if conn.getrawtransaction(txid, True)["instantlock"]:
                        locked_at[txid] = time.time()
                    else:
                        still.append(txid)
                except Exception:
                    still.append(txid)
            pending = still
            if pending:
                time.sleep(0.2)
        if pending:
            self.log.warning("%d of %d transactions never locked" % (len(pending), len(txids)))
        return locked_at

    def _recoveries(self):
        """Per node, the (timestamp, cost_ms) of every recovered signature.

        The node's own log is the honest clock here: it is written by the thread
        doing the work, so the rate derived from it is the rate that thread
        achieved, not the rate a poller observed.
        """
        out = {}
        for i, node in enumerate(self.nodes):
            chain = get_chain_folder(node.datadir, self.chain)
            path = os.path.join(node.datadir, chain, "debug.log")
            rows = []
            try:
                with open(path, "r", errors="replace") as fh:
                    for line in fh:
                        m = RECOVERED.match(line)
                        if m:
                            # The log stamps UTC; a naive datetime would be read
                            # as local and land hours away from time.time().
                            ts = datetime.strptime(m.group(1), "%Y-%m-%dT%H:%M:%S.%f")
                            rows.append((ts.replace(tzinfo=timezone.utc).timestamp(),
                                         int(m.group(2))))
            except FileNotFoundError:
                pass
            out[i] = rows
        return out

    @staticmethod
    def _recovery_stats(recoveries):
        allt = sorted(c for rows in recoveries.values() for _, c in rows)
        if not allt:
            return None
        return dict(count=len(allt), min=allt[0], median=allt[len(allt) // 2], max=allt[-1])

    @staticmethod
    def _session_rates(recoveries, since):
        """Sessions per second each node recovered, over its own active window.

        Every quorum member recovers independently, so the per-node rate is what
        one node's signing thread sustained. Summing across nodes would count the
        same session once per member.

        Only sessions after the load started count: the DKG and the fan-out sign
        earlier in the same log, and including them stretched the window by a
        third and understated the rate.
        """
        per_node = {}
        for i, rows in recoveries.items():
            rows = [r for r in rows if r[0] >= since]
            if len(rows) < 2:
                continue
            ts = [t for t, _ in rows]
            span = max(ts) - min(ts)
            if span <= 0:
                continue
            per_node[i] = dict(sessions=len(rows), seconds=round(span, 2),
                               per_second=round(len(rows) / span, 1))
        return per_node


class CpuSampler(threading.Thread):
    """Sample the CPU each node's sigshares thread uses.

    The whole LLMQ signing pipeline runs on one thread (CSigSharesManager::
    WorkThreadMain), so if that thread is the ceiling it saturates a core while
    the rest of the machine is idle.
    """

    def __init__(self, nodes, stop, interval=1.0):
        super().__init__(daemon=True)
        self.nodes = nodes
        self.stop = stop
        self.interval = interval
        self.samples = {}

    def run(self):
        prev = {}
        while not self.stop.is_set():
            now = time.time()
            for i, node in enumerate(self.nodes):
                pid = node.process.pid if node.process else None
                if pid is None:
                    continue
                ticks = _thread_ticks(pid, "rtm-sigshares")
                if ticks is None:
                    continue
                if i in prev:
                    p_ticks, p_t = prev[i]
                    dt = max(1e-3, now - p_t)
                    pct = (ticks - p_ticks) / os.sysconf("SC_CLK_TCK") * 100 / dt
                    self.samples.setdefault(i, []).append((now, pct))
                prev[i] = (ticks, now)
            self.stop.wait(self.interval)

    def summary(self, until=None):
        """Mean and peak over the active window.

        A run that ends with transactions that never lock spends the rest of its
        timeout idle, and averaging over that reports a busy thread as a quiet
        one.
        """
        out = {}
        for i, vals in self.samples.items():
            v = [p for t, p in vals if until is None or t <= until]
            if v:
                out[i] = dict(mean=round(sum(v) / len(v), 1), max=round(max(v), 1),
                              samples=len(v))
        return out


def _thread_ticks(pid, comm):
    base = "/proc/%d/task" % pid
    try:
        tids = os.listdir(base)
    except OSError:
        return None
    for tid in tids:
        try:
            with open(os.path.join(base, tid, "stat")) as fh:
                fields = fh.read()
        except OSError:
            continue
        name = fields[fields.find("(") + 1:fields.rfind(")")]
        if name != comm:
            continue
        rest = fields[fields.rfind(")") + 2:].split()
        return int(rest[11]) + int(rest[12])   # utime + stime
    return None


if __name__ == "__main__":
    ISLockRateTest().main()
