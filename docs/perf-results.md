# Raptoreum throughput — measured results

Node: `/Raptoreum Core:2.0.4.1/`, protocol 70220, built from `develop` plus the
functional-test series (`ft/09-run-by-default`), depends-pinned Boost 1.84.

| role | host | hardware |
|---|---|---|
| system under test | bowser | Ryzen 9 3900X, 12c/24t, 62 GB, NVMe |
| load generator | mario | Xeon 6517P, 16c/32t, 128 GB |

Different machines, LAN, 0.23 ms RTT. Transactions are 2-in 2-out P2PKH, payload
370–374 bytes, mean 373.

---

## 0. The measurement was wrong first, and by 60×

Regtest sets `fDefaultConsistencyChecks = true` (`chainparams.cpp:750`), which
`init.cpp:1589` turns into `setSanityCheck(1.0)`. `CTxMemPool::check()` therefore runs
after **every accepted transaction**, walking the whole mempool and re-validating it
against the coins view. Mainnet never does this.

Same corpus, same rate, only the flag changed:

| | `checkmempool=1` (regtest default) | `checkmempool=0` |
|---|---|---|
| accepted, 2,000 tx/s offered | 820/s decaying to 39/s | steady 2,000/s |
| delivered of 120,004 offered | ~7% | 120,004, none missing |
| node CPU | 187%, two cores pinned | 45% |

Found by `perf record`, not by reading: `SipHashUint256` was 11.6% of samples and
`CTxMemPool::check` was on the list. Three plausible hypotheses drawn from reading the
accept path — asset conflict maps, ancestor tracking, double script verification — were
all wrong.

## 1. Acceptance ceiling

| offered | peak accepted | sustained | CPU |
|---|---|---|---|
| 2,000/s | 2,001/s | 2,000/s, no decay | 45% |
| 5,000/s | 5,265/s | 4,893/s mean | 100% |
| 10,000/s | 5,722/s | ~5,520/s | max 213% |
| 25,000/s | 5,666/s | ~5,520/s | max 212% |

**Ceiling ≈ 5,600 tx/s**, at roughly two cores of twenty-four. Two independent
over-drives agree within 1%, so this is the node's number, not the generator's. The
limit is serial work in the message handler, not the machine.

For scale: a full 2 MB block every 120 s is ~5,350 transactions, or **~45 tx/s**. The
node accepts about 120× what the chain can carry.

## 2. Acceptance does NOT degrade with mempool occupancy

An over-driven run at 10,000 offered looked like it did:

```
5722 5688 5563 5534 5550 5525 5470 5520 5353 5539 5529 5545 5511 5521 5391 5311
4148 4355 4367 3088 3532 3360 3400 3220
```

~5,520/s falling to ~3,300/s by 122,600 entries, and I recorded that as an
occupancy effect. It is not. A later run offering 5,000/s — below the ceiling — held
exactly 5,000/s all the way to 2.3 M entries with flat CPU, a mempool nineteen times
larger.

The difference is the **receive backlog**, not the pool. Offering above the ceiling
builds a queue the node cannot drain, and the cost tracks that queue. Same confound as
§0: something that correlates with mempool growth without being caused by it. The pair
of runs separates them, which one run never could.

## 3. Mempool memory

Measured at 205,495 transactions: 76,647,106 bytes of transaction data occupying
299,998,576 bytes of mempool. **1,460 bytes per 373-byte transaction, 3.9×.**

`CTxMemPool::DynamicMemoryUsage` accounts for it: twelve pointers per entry for the
`boost::multi_index` indexes before the entry itself, plus node-based `mapNextTx`,
`mapLinks`, `mapDeltas` and `vTxHashes`.

Against a 2-minute target interval at 5,000 tx/s:

| gap | transactions | raw | mempool RAM at 3.9× |
|---|---|---|---|
| 2 min (target) | 600,000 | 224 MB | 876 MB |
| 10 min (0.67% of blocks) | 3,000,000 | 1.12 GB | 4.4 GB |
| 20 min (~monthly) | 6,000,000 | 2.24 GB | 8.8 GB |

The 300 MB default holds **41 seconds** at the node's own ceiling.

## 4. The constraint that does not yield to tuning

A 2 MB block removes 5,360 transactions. At 5,000 tx/s, 600,000 arrive between blocks.
The block drains **0.9%** of arrivals. Memory efficiency changes how long a node
survives; it does not change the direction.

To keep up, a block must commit to 600,000 transactions. At 32 bytes per identifier
that is **19.2 MB of commitments per block** — the figure the decoupling design has to
carry, and the thing that must propagate every two minutes.

## 5. ChainLocks cleanup stalls the node, proportional to mempool size

`CChainLocksHandler::Cleanup()` walks every entry in `txFirstSeenTime` — one entry per
accepted transaction — calling `GetTransaction()` on each, while holding **both**
`cs_main` and `mempool.cs`. It runs every 30 s (`CLEANUP_INTERVAL = 1000 * 30`), and it
runs on every node rather than only smartnodes: `TrySignChainTip()` calls `Cleanup()`
*before* the `if (!fSmartnodeMode) return;`.

Measured directly, with a timing log added to the function:

| entries | walk | µs per entry |
|---|---|---|
| 35,150 | 17 ms | 0.48 |
| 185,280 | 94 ms | 0.51 |
| 335,770 | 170 ms | 0.51 |
| 637,870 | 332 ms | 0.52 |
| 941,922 | 494 ms | 0.52 |
| 1,094,409 | 587 ms | 0.54 |

Linear at ~0.5 µs/entry. At 3 M entries that is roughly **1.5 seconds every 30 seconds
during which the node accepts nothing and answers no RPC**, because it holds the two
locks everything else needs. It is a latency defect, not a throughput one, and it scales
without bound with mempool size.

It is *not* the cause of the CPU cliff in §6 — the hypothesis that the walk outruns its
own interval was tested and killed by this data.

## 6. A CPU cliff at ~2.4 M entries, in the networking path

At 2.3–2.4 M mempool entries during a 10-minute fill, node CPU doubled from ~114% to
~210% at constant throughput, and acceptance began to slip below the offered rate.

`perf diff` of a healthy profile (250–405 k entries) against a degraded one (2.8 M),
same binary, same rate:

```
 Baseline   Delta   Symbol
  11.47%  +29.03%  [k] 0xffffffffa7405930
  32.39%  -18.45%  secp256k1_fe_mul_inner
  25.58%  -14.98%  secp256k1_fe_sqr_inner
   0.02%   +3.08%  CConnman::SocketHandler()
   0.31%   +1.75%  pthread_mutex_trylock
      -    +0.93%  epoll_wait
   0.01%   +0.86%  CConnman::SocketEventsEpoll
      -    +0.55%  CConnman::ThreadSocketHandler()
      -    +0.45%  CConnman::NotifyNumConnectionsChanged()
```

Signature verification shrank as a share while total CPU doubled, so the real work stayed
constant. Everything that grew is in the networking path, including per-loop-iteration
costs (`NotifyNumConnectionsChanged`, `vector<CNode*>::reserve`) rather than per-message
ones — the signature of a busy spin in the socket thread once `fPauseRecv` is set.

Confirmed by a per-thread capture triggered automatically the moment total CPU crossed
150%:

```
CLIFF REACHED: total 163%
   100.3%  rtm-msghand
    51.0%  rtm-net
    11.0%  rtm-scheduler
```

**`rtm-msghand` is pinned at 100.3% — one core exactly.** The message handler is
single-threaded, so it cannot exceed one core however many the machine has. That is the
5,600 tx/s ceiling, and it is architectural: bowser has 24 threads and 22 of them cannot
help.

**`rtm-net` at 51% and climbing is waste.** The socket thread does no validation; it
reads bytes and hands them over. Half a core there, on its way to a full one, is the
socket loop waking on a readable socket it will not drain because the receive buffer is
paused.

The transition is an **oscillation**, not a step:

```
mempool=2180262  cpu=207%
mempool=2185446  cpu=160%
mempool=2190746  cpu=115%
```

The node hovers at its ceiling, dips behind, spins, catches up, repeats. Thirty-second
sampling in the earlier run caught the high phase consistently and made it look like a
permanent doubling; it is really a growing fraction of time spent paying a penalty for
being briefly behind.

Corroborating from the other end: the very first measurement of the session, under a
completely different cause (`checkmempool=1`), showed `rtm-net` and `rtm-msghand` both
pinned at exactly 100%. Same signature, different trigger.

**Consequence for scaling.** The two levers are independent: making validation cheaper
does not raise the ceiling, because the ceiling is one thread's capacity, not the box's.
Reaching 25,000 tx/s needs the message-handling path parallelised, and separately needs
the socket thread to stop spending a core on a socket it has decided not to read.

## 7. Holding a full 10-minute backlog: it works

600 s at 5,000 tx/s, `checkmempool=0`, `maxmempool=8000`:

| | |
|---|---|
| offered | 2,969,380 (4,949/s), 1.0% withheld at the very end |
| accepted | mean **4,884/s** sustained for ten minutes |
| peak mempool | **2,969,380 transactions** |
| mempool accounting | **4.07 GB** (1,471 B/tx) |
| process RSS | **4.54 GB** (1,641 B/tx incl. baseline) |

No eviction, no stall. The requirement — accept over 5,000 tx/s and hold them until an
unlucky block — is met, at about **4.5 GB of RAM per ten-minute interval**.

Memory per transaction steps rather than creeps: 1,377 → 1,402 → **1,447** at ~1.07 M →
1,444 → **1,483** at ~2.11 M. Flat plateaus with sharp jumps is hash containers doubling
their bucket arrays, so memory arrives in lumps and a node at 90% of its cap can cross
it in one step.

The default `maxmempool` of 300 MB holds 204,000 of these — **41 seconds** of the ten
minutes, and would have evicted 93% of the backlog.

---

## Rig notes

Three rig defects were found by accounting rather than by failure, each of which would
have biased results:

1. `sendall` blocks under backpressure, so the offered rate silently became an outcome.
2. Sockets closed with bytes still queued — a contiguous tail vanished.
3. **The node discards a peer's unprocessed receive buffer on disconnect**, so closing
   at the end of a run destroys exactly the backlog that measures how far behind the
   node is. Any test that disconnects at the end reports the node as faster than it is.

## Still open

- Confirm the socket-thread spin in §6 with a per-thread capture taken at the cliff.
- Relay: the derived per-peer announcement ceiling is 56 tx/s inbound (see
  `perf-constants.md`), two orders below acceptance and never measured.
- Block connection at large sizes, and the acceptance gap it causes.
- Everything here is one box and one peer. Fan-out to several peers changes both the
  CPU and the bandwidth picture and is untested.
