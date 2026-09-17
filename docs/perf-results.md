# Raptoreum throughput — measured results

> **Corrections (2026-09-16).** This file is a chronological log; entries below stand as what
> was measured *at the time*, but three conclusions drawn from them were later overturned.
> The current picture is `docs/throughput-bottleneck.md`.
>
> * **The ~5,600 tx/s ceiling is high.** Rates were computed over a fixed duration while
>   acceptance ran on past the offer (still accepting 3,088 tx/s at t=130s, 640 at t=147s).
>   Sustained is **~4,400-4,500 tx/s**; per-second peaks near 5,200 occur early and decay as
>   the mempool grows. Confirmed as real compute — raising `dbcache` 13x and
>   `maxsigcachesize` 16x changed nothing.
> * **Relay ordering at "over 40% of the message handler"** was measured at a 300k backlog
>   with the cap at 50k-100k. At the v1 operating point `-perfinvnosort` moves throughput
>   **<1%**, and the backlog never exceeds one trickle. It is not a v1 concern.
> * **Parallel validation is not the primary wall.** Acceptance has ~3x headroom over the
>   1,500 tx/s target; the binding constraint is the relay cap, a scheduling constant.

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
| 2,018,847 | 1,141 ms | 0.57 |
| 2,909,415 | 1,803 ms | 0.62 |

Linear, drifting slightly upward. At 2.9 M entries the walk takes **1.8 seconds every 30
seconds**, during which the node accepts nothing and any RPC taking `cs_main` blocks.

It does **not** scale without bound: entries are erased once confirmed six deep
(`:707-712`), so the map is bounded by the mempool plus ~6 blocks, and the cost is linear
in `-maxmempool`. At the 300 MB default (~204,000 entries) the walk is ~105 ms, not
seconds. The multi-second figures here required `-maxmempool=8000`.

The per-entry cost measured is the **mempool-hit path only** — no blocks were mined during
the fill. With `-txindex`, which smartnodes require, an entry that has left the mempool but
is under six deep falls through to `g_txindex->FindTx` (`validation.cpp:983-985`), which is
far more expensive.

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

## 8. Relay is indexed to block size, and decoupling breaks that

Two nodes on one box, the peer connected *to* the SUT so the SUT sees it as inbound
(the 5-second trickle). Load offered to the SUT only.

At the shipped 2 MB block size:

| | |
|---|---|
| SUT accepted | 957 tx/s |
| peer received | **74.7 tx/s** |
| ratio | 13× |
| delivered over the run | 14,560 of 120,002, **12.1%** |

Every non-zero sample was **exactly 280** transactions — `140 × MaxBlockSize()/1e6` —
arriving on the Poisson trickle. The derived constant is confirmed to the transaction.

The backlog is **not lost**: with load stopped, the peer kept draining at the same
74.7/s. Relay is a constant-rate pipe, not a lossy one.

And the rate is deliberately proportioned. Over a 120-second block interval, 74.7/s
delivers ~9,000 transactions per peer against the ~5,350 a full 2 MB block holds — about
70% margin. The `4 *` in `4 * 7 * 5` is the comment's "4 times smaller block times"
doing exactly that job.

**Tested by patching the block size** (test-only, never leaves the rig branch):

| block size | peer received/s | burst quantum |
|---|---|---|
| 2 MB | 74.7 | exactly 280 |
| 8 MB | 230.3 | up to 2,211 |

4× the block size gives 3.2× the relay rate. Proportional, if not perfectly linear.

**The consequence for decoupling.** The formula treats block size as a proxy for how
much needs to propagate, and that proxy holds only while blocks carry bodies. A
decoupled block committing to 600,000 transactions at 32 bytes each is 19.2 MB, so
relay would scale to roughly 700 tx/s — while those 600,000 bodies still have to cross
at 5,000 tx/s. The block shrinks twelvefold; the propagation requirement does not shrink
at all.

Under decoupling `INVENTORY_BROADCAST_MAX_PER_1MB_BLOCK` must be re-indexed to something
that still tracks transaction volume — committed transaction count — rather than to the
size of a block that no longer carries them.

## 9. Peer fan-out: the ceiling holds

Every figure above came from a node with one peer that ignores announcements — the only
configuration where relay costs nothing. Eight peer nodes were attached to the SUT (each
connecting *to* it, so all inbound) and the over-drive repeated.

| | 1 peer | 8 peers |
|---|---|---|
| peak accepted | ~5,520/s | **5,697/s** |
| mean accepted | ~5,000/s | ~3,700/s |
| CPU, per-thread capture | ~210% | **209%** |

Peak throughput and CPU are unchanged. Announcement work to eight peers does not eat the
acceptance ceiling, and the per-thread split is the same as always:

```
 99.6%  rtm-msghand
 96.6%  rtm-net
 12.9%  rtm-scheduler
```

The lower mean is backpressure, not slowdown: at 10,000 offered the node refuses 42% of
what is pushed and the generator's spare capacity shows up as dips in the accepted rate.

### A phantom, and what it cost

An earlier version of this section reported CPU bursts to eleven cores under fan-out, and
before that a collapse of the ceiling to 1,142 tx/s. Both were wrong, and the sequence is
worth recording because each error was found by a different discipline:

1. **1,142 tx/s** came from one mid-run sample taken during a dip. One sample is not a
   measurement.
2. **The bursts** came from the collector dividing the CPU delta by the *nominal* 1 s
   interval. Under load the RPC calls in the same loop block for seconds, so a node
   steadily using two cores was reported as using eight — and the inflation correlated
   with load, which made it look like a real effect.
3. Chasing it produced two further mistakes of my own: a hunt run *without* the collector,
   which removed the only trigger and returned a meaningless null; and a check of the
   sampling gaps that was off by one, which briefly "disproved" the correct explanation.

What settled it was running a per-thread capture *concurrently* with the collector: two
instruments reading the same `/proc` data in the same run, disagreeing 2,085% against
209%. When two measurements of one quantity disagree, at most one of them is the node.

`collect.py` now measures the interval between the CPU reads themselves.

## 10. Block template assembly: flat in mempool, but it is a full block validation

`getblocktemplate` timed every 10 s during a fill from an empty mempool to 1,852,790
entries, at the shipped 2 MB block size. Sixty samples.

| mempool | assembly |
|---|---|
| 23,003 | 180.9 ms |
| 321,246 | 176.7 ms |
| 739,229 | 207.6 ms |
| 1,224,795 | 230.8 ms |
| 1,845,150 | 309.7 ms |

Excluding lock-stalled samples: **median 254 ms, max 318 ms**. An eightyfold larger pool
costs perhaps 50% more. `BlockAssembler` walks the fee-ordered index and stops when the
block is full, touching ~5,400 entries regardless of how many are behind them. Assembly
scales with the **block**, not the pool.

### What the 254 ms actually is

Not selection. `CreateNewBlock` ends with:

```cpp
// miner.cpp:262
if (!TestBlockValidity(state, chainparams, *pblock, pindexPrev, false, false)) {
```

and `TestBlockValidity` asserts `cs_main` held and calls `ContextualCheckBlockHeader`,
`CheckBlock`, `ContextualCheckBlock` and then **`ConnectBlock`** on the candidate. Every
template is a complete block validation of 5,378 transactions, performed under `cs_main`.

That has a consequence the RPC's name hides: **a polling pool spends the node's acceptance
budget.** `cs_main` is the same lock the message handler needs, so template production and
transaction acceptance are in direct competition on it.

### What it cost acceptance

| | no template polling | polling every 10 s, one peer |
|---|---|---|
| effective acceptance | 4,884 tx/s | **3,194 tx/s** |
| offer refused by the node | ~1% | **18.7%** |

A 35% drop. Template lock time accounts for about 2.5% (254 ms per 10 s) and the six
observed stalls for perhaps another 2.6%, so roughly 5% is attributable and **the rest is
not explained**. A controlled A/B — the same fill with and without polling, peer count
held constant — would separate template cost from peer cost. Not yet run.

### The stalls are lock waits, not assembly

Six samples ran long, and in five of them `getmempoolinfo` — which reads three counters
and has a median of 1.3 ms — stalled in the same iteration:

| mempool | assembly | `getmempoolinfo` |
|---|---|---|
| 604,525 | 975 ms | 663 ms |
| 1,327,170 | 2,118 ms | 1,740 ms |
| 1,631,327 | 4,726 ms | 1,830 ms |
| 1,792,489 | 2,802 ms | 839 ms |

A trivial call taking 1.8 seconds is blocked, not working. Both calls were waiting on the
same holder, and the wait grows with mempool size. This is the first **measured** evidence
for §5's stall — previously inferred from lock scope — and it arrived by accident, on the
clean build, via RPC latency. Magnitudes run about 2.3× the measured ChainLocks walk, so
either that walk is slower here or there is a second O(mempool) holder. Not attributed.

### For decoupling

Assembly cost follows block contents. A block committing to 600,000 transactions rather
than 5,400 makes each template call roughly a hundred times more expensive — unless
commitments are validated differently from bodies, which is a design requirement rather
than an optimisation.

### PARKED: template production over RPC needs replacing before any throughput increase

Measured at two block sizes, same node:

| block | transactions | `getblocktemplate` |
|---|---|---|
| 2 MB | 5,374 | 0.21 s |
| 32 MB | 85,624 | **2.7–3.5 s** |

Two separable costs, and both scale with block contents:

1. **Redundant revalidation.** `CreateNewBlock` ends in `TestBlockValidity` → `ConnectBlock`
   (`miner.cpp:262`), a full validation of transactions that were *already validated* when
   they entered this node's mempool. Warm-cache cost is 0.002 ms/txin, so ~350 ms at
   85,624 transactions and several seconds at 600,000. It holds `cs_main` throughout, so it
   competes directly with acceptance.
2. **JSON marshalling.** The RPC returns every transaction as hex — roughly 64 MB of JSON
   for a 32 MB block, doubling the payload. Pools poll this continuously.

At 5,000 tx/s (600,000 transactions per block) both become untenable: seconds of `cs_main`
per poll, and hundreds of megabytes of JSON per poll.

**Directions, in rough order of leverage:**

- **Stratum V2's Template Distribution Protocol.** Replaces polling with the node *pushing*
  a template carrying the coinbase and a merkle path rather than transaction bodies. Removes
  the marshalling cost entirely and the polling with it. Note a Raptoreum-specific
  complication: the coinbase carries a `CCbTx` payload with smartnode-list merkle roots, so
  this is a port rather than a drop-in. SV2's mining and job-declaration protocols are
  pool-side and do **not** help with any of this.
- **Incremental templates** — maintain a running template updated as transactions arrive,
  rather than rebuilding per call. Selection is already cheap; revalidation is not.
- **Reduce the revalidation** to the coinbase and special-transaction payload. Its purpose is
  to stop a miner wasting hashpower on an invalid template; for transactions drawn from the
  node's own mempool it re-proves what was already proven.

Not a blocker at 2 MB — 0.21 s per poll is tolerable. It becomes one well before 5,000 tx/s.
**Parked, not scheduled.**

## 11. Mempool convergence across nodes

The question the decoupling design rests on: does the network converge on a shared pending
set? A block that commits to transaction identifiers is reconstructable only by a peer
that already holds those bodies.

**Setup.** Four nodes in a star — three peers each connected to a hub. Transactions
originate at *all four*, using disjoint lineage shards of the same corpus (`--shard N/M`;
lineages are linear, so transaction `j` of generation `k` spends the outputs of
transaction `j` of generation `k-1`, and taking every Mth lineage yields a self-contained
set). 250 tx/s per node, 1,000 aggregate. `getrawmempool` on every node every 20 s,
compared against the union.

A star is the friendliest topology — every node is two hops from every other — so it is a
floor, not a typical case.

### The first run was invalid

It reported a flat 75% of the union missing from every peer, which looked like total
failure to converge. It was not. The peers were restored from a snapshot with an old tip
and were latched in initial block download, and during IBD the node requests **only**
sporks:

```cpp
// net_processing.cpp:2840
static std::set<int> allowWhileInIBDObjs = { MSG_SPORK };
```

The hub announced faithfully and the peers ignored every announcement. Diagnosed from the
per-message counters, which were flatly incompatible with "relay is slow":

```
hub -> peer:  sent_inv = 283,057 bytes    sent_tx = 0
hub <- peer:  recv_getdata = 0
```

Fix: `maxtipage=999999999` on every node, now in the rig scripts. Mining a block works too
but changes chain state between runs.

### The valid run

With all four nodes out of IBD, `sent_tx` and `recv_getdata` are both non-zero and the
hub delivers to each peer at **~79 tx/s** — the trickle cap, exactly.

| | |
|---|---|
| each peer must learn | ~750 tx/s (the other three nodes' output) |
| each peer can be told | ~79 tx/s |
| shortfall | **~670 tx/s, accumulating** |

Worst-node missing: 10,810 → 25,010 → 38,082 → 52,010 → 66,181 → 77,148. About 650 per
second, without bound.

### After the input stops

The informative part. With no new transactions, the union froze at 113,659 and the nodes
drained:

| node | holds | of union |
|---|---|---|
| hub | 110,688 | 97% |
| peer 1 | 42,548 | 37% |
| peer 2 | 38,198 | 34% |
| peer 3 | 36,563 | 32% |

Peers caught up at 42 tx/s falling to 28 tx/s — **roughly thirty minutes to converge on
six minutes of traffic**. Convergence is not impossible, it is about five times slower
than the traffic that created it. On a two-minute block interval a node would be ~15
blocks behind knowing what the current block commits to.

The peers also diverged from *each other* once input stopped (42,548 vs 36,563), which
they did not while it flowed. In a star every leaf is equidistant, so that spread is the
trickle's randomisation — reconstruction success would vary per node under identical
conditions.

### The control: at the design point it converges

1,000 tx/s is twenty-two times the ~45 tx/s a 2 MB block carries, so that run says the cap
binds — not whether the design is coherent. Repeating it at the **design point**, 45 tx/s
aggregate across the same four nodes, same star, same cap:

```
worst-node missing:  402 (57.0%)  <- startup
                     174 (10.8%)
                     423 (16.9%)
                     123 ( 3.6%)
                     192 ( 4.5%)
                     587 (11.3%)
                     212 ( 3.5%)
```

Bounded, oscillating between ~120 and ~590 — a few seconds of in-flight traffic — with no
trend across seven samples. At one point the **peers held more than the hub** (4,113 /
4,125 / 4,155 against 4,116), which is what genuine convergence looks like: everyone has
everything except what is in flight, and whoever is momentarily ahead is arbitrary.

| offered | vs 2 MB capacity | outcome |
|---|---|---|
| 45 tx/s | 1x | **converges**, gap ~4 s of traffic, stable |
| 1,000 tx/s | 22x | diverges, gap grows ~650/s without bound |

And the drain phase is the sharpest contrast of all. With input stopped:

| | at 45 tx/s | at 1,000 tx/s |
|---|---|---|
| steady-state gap | ~4 s of traffic, bounded | grows ~650/s, unbounded |
| once input stops | converged within seconds | ~30 min for 6 min of traffic |
| final worst node | **0.7% missing** | 65% missing, still falling |

At the design point a peer repeatedly held the *entire* union while the hub was one sample
behind — convergence complete, and which node leads is arbitrary.

**So the coupled design is self-consistent.** `INVENTORY_BROADCAST_MAX_PER_1MB_BLOCK *
MaxBlockSize()/1e6` is provisioned against the block with roughly 2.3x headroom, and
Raptoreum as shipped converges comfortably at the throughput it actually supports. There
is no defect in the relay configuration.

### What this means for decoupling

The proportionality survives any block size **because block size is a proxy for how many
transactions must propagate**. That proxy holds only while blocks carry bodies.

A decoupled block committing to 600,000 transactions at 32 bytes is 19.2 MB, against the
224 MB those bodies occupy — twelve times smaller. The relay cap it indexes falls by
twelve while the propagation requirement does not fall at all. Everything else in the
relay design carries over unchanged; this is the single place it breaks, and it is now a
specific measured claim rather than an assertion.

### An untested assumption, flagged rather than measured

Convergence requires a node's inbound learning to cover what the rest of the network
produces:

```
peers x per-peer rate  >=  offered rate x (N-1)/N
```

With the measured 79 tx/s per peer that gives 1 peer at 45 tx/s (which matched), ~13 at
1,000 tx/s, and ~63 at 5,000 tx/s.

**The formula assumes the per-peer rate is independent of peer count, and that is not
established.** The sender rebuilds and heapifies its entire un-announced backlog *per peer
per trickle*, with a mempool lookup per comparison (§8), and a single peer was measured to
cost about a third of sustained acceptance. If that work degrades with peer count, a node
with sixty peers does not relay at 60 x 79 tx/s and the peer-count requirement is worse
than the formula says.

Testing it is small — one node, peers attached at 1, 2, 4, 8, per-peer delivery rate
recorded at each step — but it only matters once a design leans on high peer counts. Not
run, and noted here so the formula is not quoted as though it were measured end to end.

Also still unrun: the same convergence test at a raised block size with the offered rate
held at that block's capacity, which would confirm the invariant holds across block sizes
rather than only at 2 MB.

## 12. A latent infinite loop at 128 MiB of block

Raising `MAX_DIP0001_BLOCK_SIZE` to test large blocks found a hard failure with no error
message: the node allocates without bound until the machine dies.

### The measurements

| block | serialised bytes | vs 134,217,728 | result |
|---|---|---|---|
| 64 MB | 63,999,073 | under | works, 4.87 s, peak RSS 0.85 GB |
| 128 MB | 127,999,015 | **under, by 6 MiB** | works, 10.0 s, peak RSS 1.30 GB |
| 192 MB | ~192,000,000 | over | never completes, 0.47 GB/s until OOM |
| 255 MB | 254,999,843 | over | OOM, >60 GB, `signal=ABRT` |

Assembly always succeeded and was logged; the hang is after `CreateNewBlock` returns.
`-debug=bench` recorded **zero** completed connections at the failing sizes.

### The cause

Found with `perf record --call-graph dwarf` during the runaway (RSS 4.00 GB → 37.24 GB
across the 25 s profile):

```
14.93%  std::vector<CBlockFileInfo>::_M_default_append(unsigned long)
 2.22%  SaveBlockToDisk(CBlock const&, int, CChainParams const&, FlatFilePos const*)
```

```cpp
// src/validation.h:97
static const unsigned int MAX_BLOCKFILE_SIZE = 0x8000000;   // 128 MiB

// src/validation.cpp:3770-3775, FindBlockPos()
while (vinfoBlockFile[nFile].nSize + nAddSize >= MAX_BLOCKFILE_SIZE) {
    nFile++;
    if (vinfoBlockFile.size() <= nFile) {
        vinfoBlockFile.resize(nFile + 1);
    }
}
```

The loop looks for a block file with room for the block. **If the block is itself larger
than `MAX_BLOCKFILE_SIZE`, no file can ever have room — including a fresh empty one, where
`0 + nAddSize >= MAX_BLOCKFILE_SIZE` still holds.** It increments `nFile` forever, resizing
`vinfoBlockFile` by one element per iteration, which is the `_M_default_append` in the
profile.

### Why it matters, and why it has never been seen

This is inherited Bitcoin Core code and is unreachable at any shipped block size, in
Bitcoin or in Raptoreum. It is a **latent landmine that detonates the moment a block
exceeds 128 MiB** — and it fails in the worst possible way: no error, no log line, no
exception, just silent unbounded allocation until the kernel intervenes. On a machine
running anything else, the OOM killer may take the other process.

Any block-size increase past 128 MiB must raise `MAX_BLOCKFILE_SIZE` with it, or change
the loop to allow a block file to hold a single oversized block.

For decoupling: a commitment block of 19.2 MB is nowhere near this. The "build whole,
split after" variant in `transaction-decoupling.md` §15 moves ~224 MB blocks and would hit
it immediately.

### What the bisect actually found

The threshold is **128 MiB of serialised block**, not a transaction count. The apparent
cliff "between 343,327 and 515,000 transactions" was an artefact of testing at decimal
128 MB, which is 122 MiB — under the limit by six mebibytes.

## 13. Compact-block reconstruction, and why fee variation decides it

A compact block is decoupling in miniature. The sender transmits short identifiers, the
receiver fills the block from its own mempool, and fetches only what it lacks. The
fraction it must fetch is the decoupling premise made measurable: if a peer that has seen
only a slice of the traffic can still fill a block, then committing to transactions
instead of carrying them costs nothing at reconstruction time.

`test/perf/reconstruct.py` measures it directly — mempool sets before the block, the
block's transaction list after, and the `getblocktxn`/`blocktxn` byte counters that say
what actually had to be re-fetched.

The first run said decoupling was hopeless. The second said it was free. The corpus was
the difference.

| corpus | offered | peer's share of the hub mempool | block fill |
|---|---|---|---|
| uniform fee | 500 tx/s | 12% | **6.3%** |
| varied fee | 500 tx/s | 13.4% | **100%** |
| varied fee | 5,000 tx/s | **1.5%** (8,400 of 550,010) | **100%** (5,333 of 5,333) |

**The uniform-fee number is an artefact, and it is worth stating why.** Relay announces in
`CompareDepthAndScore` order; the block selects in `CompareTxMemPoolEntryByAncestorFee`
order. Both are fee-ordered, so on real traffic they agree: the transactions a peer is
told about first are the transactions the miner puts in the block. Give every transaction
the same fee and both comparators fall back on tie-breaks that have nothing to do with
each other, and the two subsets become independent draws. 6.3% is not a property of the
protocol; it is what two unrelated orderings over the same pool produce. The corpus was
uniform-fee because it was built for throughput, where fee is irrelevant — carrying it
into a selection experiment silently changed what was being measured.

**The real result is the third row.** At 5,000 tx/s the peer held 8,400 of the hub's
550,010 transactions — one and a half percent — and still filled the entire block from its
own mempool with nothing to fetch. Fee ordering is doing the work: the trickle is not a
random 1.5% sample, it is the top 1.5% by fee, which is precisely the set the miner
selects from.

**What this proves, and what it does not.** It proves the *coupled* design is
self-consistent. Relay's cap (~75 tx/s at 2 MB) exceeds a 2 MB block's appetite (~44 tx/s
averaged over a 120-second interval), and both are indexed to `MaxBlockSize()`, so the
margin holds at any block size — §8's `4 *`.

It does not prove the decoupled case, and the same indexing is why. A 19.2 MB commitment
block sets relay to roughly 538 tx/s while the traffic it commits to runs at 5,000 tx/s.
Reconstruction fill stays perfect only while the peer has seen the transactions that end
up in the block; at nine times the relay rate it will not have. The fill measured here is
evidence that *fee-ordered relay is the right mechanism*, not evidence that it is
provisioned for decoupled volumes. Re-indexing the constant to committed transaction
count, per §8, is what would carry this result across.

## 14. Peers hold the same subset, not complementary ones

If relay delivers only a slice, the obvious mitigation is more peers: eight neighbours,
eight different slices, better coverage. The trickle's ordering makes that false.

`CompareDepthAndScore` is a single global ordering over the mempool, evaluated the same
way on every peer link. Each link announces its own prefix of the same sequence, so what
differs between peers is how far down that sequence they have got — not which
transactions they got. The subsets are nested, not disjoint.

`test/perf/overlap.py` measures it: the union across all peers against the best single
peer. Adding peers moved the union by nothing worth reporting; the second and third peer
contributed no transaction the first did not already have.

This is the structural finding behind §8. A node that is missing a transaction cannot
route around it, because every peer it could ask is missing the same one — redundancy in
the peer graph buys resilience against a peer *failing*, and buys no coverage at all
against relay being rate-limited. It also means the fix for a relay shortfall is
necessarily the rate constant or the ordering, and can never be topology.

## 15. The quorum signing path, and what it costs

Trí's proposal is that transactions attested by the smartnode network take a shorter
validation path, with InstantSend named as the existing precedent. That makes the cost of
producing an attestation the load-bearing number, and nobody in the discussion had one.

### What the live parameters actually are

Two facts from the source, neither of them the test-network values:

**The InstantSend quorum is 50 members, not 3.** `UpdateLLMQParams` rescales by smartnode
count, and above 600 smartnodes `LLMQ_50_60` resolves to `llmq50_60` — size 50, threshold
30, `signingActiveQuorumCount` 24, `recoveryMembers` 25 (`src/chainparams.cpp:1101`,
`src/llmq/quorums_parameters.h:301`). The size-3 `llmq3_60` only applies below five
smartnodes.

**InstantSend signs each input separately, then the lock.**
`CInstantSendManager::TrySignInputLocks` loops over `tx.vin` calling `AsyncSignIfMember`
per input (`src/llmq/quorums_instantsend.cpp:548-561`), and `TrySignInstantSendLock` signs
the islock as a further session once the inputs are locked (`:724`). A two-input
transaction is **three threshold signatures**, not one.

### The crypto, measured

`src/bench/bls.cpp` on bowser. `BLS_Recover_30` and `BLS_Recover_240` were added for this;
the rest ship with the tree. Threshold recovery is Lagrange interpolation and is purely
algebraic, so random shares measure its real cost.

| operation | per op | per core |
|---|---|---|
| BLS sign (produce one share) | 0.99 ms | 1,006/s |
| BLS verify, single, unbatched | 2.48 ms | 404/s |
| batched verify, per distinct session | 1.15–1.59 ms | 628–870/s |
| **threshold recovery, 30 of 50 (InstantSend)** | **9.41 ms** | **106/s** |
| threshold recovery, 240 of 400 (ChainLocks) | 85.3 ms | 11.7/s |

Batching matters and it works: `CBLSBatchVerifier` groups by message hash, aggregates the
public keys for each hash, and does a single `VerifyInsecureAggregated` over the distinct
hashes (`src/bls/bls_batchverifier.h:130-178`). All 50 shares of one session carry the same
`signHash`, so they collapse to one verification. Share count is nearly free; **session
count is what costs**.

Recovery does not batch, and at 9.41 ms it is nine times the cost of everything else in the
session put together.

### It is one thread

`CSigSharesManager::WorkThreadMain` (`src/llmq/quorums_signing_shares.cpp:1447`) runs the
whole pipeline serially on a single thread: `ProcessPendingRecoveredSigs`, then
`ProcessPendingSigShares`, then `SignPendingSigShares`, then `SendMessages` at most once
per 100 ms, then two `Cleanup` calls — and `workInterrupt.sleep_for(100ms)` when idle, with
a `// TODO Wakeup when pending signing is needed?` still in place.

This is the same shape as the `rtm-msghand` ceiling in §1: one thread, everything on it,
the rest of the machine idle.

### The arithmetic

Per session a recovering member pays sign + verify + recover ≈ **11.5 ms**, so ~87
sessions/s on its one thread; the recovery term alone caps it at 106/s.

| | sessions/s | transactions/s (2-input, 3 sessions each) |
|---|---|---|
| one smartnode, one quorum | ~87–106 | **~29–35** |
| network, 24 active quorums, no member doubled up | ~2,400 | **~800** |
| required at the 5,000 tx/s target | 15,000 | 5,000 |

The network-wide row is the generous reading: it assumes enough smartnodes that no node
sits in two active InstantSend quorums at once, and that each has a core free for signing
and nothing else. It is still **six times short** of the target, and the per-node row is
what a single smartnode actually experiences.

Against §1's measured 5,600 tx/s of plain mempool acceptance, the attestation path is
roughly **160 times slower per node** than the path it is proposed to relieve.

### What this does and does not say

It does not say quorum attestation is a bad idea. It says the cost is in **recovery**, that
recovery is **per signing session**, and that the current design spends three sessions on a
two-input transaction and does them all on one thread.

Three consequences follow, and they are all engineering rather than architecture:

1. **One session per transaction, not one per input.** Signing the transaction hash once
   instead of each input would cut the work by the input count plus one — a 3× reduction on
   this corpus, before anything else is touched.
2. **Recovery is the target, not verification.** Optimising share verification, which
   already batches, buys nothing; 9.41 ms of Lagrange interpolation is the whole cost.
3. **The signing thread has the same problem as the message handler.** Recovery is
   independent per session and embarrassingly parallel, and `CBLSWorker` — the thread pool
   `BLS_Verify_BatchedParallel` uses — already exists in the tree.

The measurement is raw crypto cost on bowser, whose CPU governor was `schedutil` with turbo
enabled (nanobench warned). A typical smartnode is weaker than this machine, not stronger,
so these are optimistic figures.

## 16. InstantSend end to end: the attestation path measured

Section 15 costs the cryptography in isolation. This is the assembled pipeline, on
regtest, with real smartnodes forming a real quorum: `test/perf/islock_rate.py`.

Transactions are handed to every node over RPC rather than relayed, so relay (section 8) is
not in the measurement. They are pre-signed before the clock starts, so the wallet is not
either. The rate is read from the nodes' own `recovered signature` log lines, which are
written by the thread doing the work.

### What it reaches

Three thousand transactions, one input and one output each, so two signing sessions per
transaction — one input lock and the islock.

| quorum | locked | locks/s | per-node sessions/s | wall ms per session | recovery, benched |
|---|---|---|---|---|---|
| 5 of 3 | **3,000 / 3,000** | 74.2 | 55.4 | 18.1 | 0.90 ms |
| 9 of 6 | 2,924 / 3,000 | 41.2 | 23.0 | 43.5 | 1.85 ms |
| 13 of 8 | 2,773 / 3,000 | 32.5 | 13.9 | 71.9 | ~2.5 ms |

The cost per session fits **≈ 6.7 × quorum size − 15 ms** across all three points. It is
linear in the number of *members* and nearly independent of the threshold — recovery, the
expensive cryptography of section 15, is 3 to 5 per cent of it.

That is the opposite of what section 15's crypto-only model assumed. The model's conclusion
was pessimistic enough, but its reasoning was wrong: the cost is not the signing, it is
everything each member does with every other member.

### It is not compute-bound

At 5 of 3 the signing thread uses **9.1 ms of CPU per session against 18.1 ms of wall
clock** — a duty cycle of about one half. The thread is idle as often as it is busy.

A profile of one smartnode under load puts 78.8% of the process's CPU in `rtm-sigshares`
and 17.5% in `rtm-isman`, with the leaf symbols almost entirely GMP and relic field
arithmetic and `bls::LegacySchemeMPL::AggregateVerify` above them. So the busy half is
genuinely cryptography — there is just not very much of it, and the thread spends the other
half waiting.

What it waits on is round trips. `CSigSharesManager::SendMessages` runs at most once per
100 ms (`quorums_signing_shares.cpp:1461`), and a session needs an announce, an inventory,
a request and the shares themselves. The number of those exchanges grows with the quorum,
which is what the linear-in-size fit is measuring.

**This matters for what to fix.** Parallelising BLS verification — the obvious reading of
section 15 — addresses the half that is already fast. The cadence and the session state
machine are where the time actually goes.

### Where the cliff is

A burst measures how long a fixed amount of work takes. A paced offer measures the rate at
which the quorum stops keeping up, which is the number that matters because the failure is
not graceful. Quorum 5 of 3, 90 seconds at each rate:

| offered | locked | achieved |
|---|---|---|
| 20 tx/s | 1,800 / 1,800 | 19.8 locks/s |
| 40 tx/s | 3,600 / 3,600 | 39.3 locks/s |
| 60 tx/s | 5,400 / 5,400 | 58.1 locks/s |
| 80 tx/s | **7,093 / 7,200** | 75.5 locks/s |
| 100 tx/s | **7,906 / 9,000** | 76.2 locks/s |

**Full coverage holds to 60 tx/s and the ceiling is ~76 locks/s.** Past it the achieved rate
does not rise — it pins at 76 while coverage falls, so the excess is lost rather than
delayed. At 100 offered, 1,094 transactions never received a lock.

The ceiling agrees with the burst measurement of 74.2 locks/s above, which is a useful
cross-check: two different offer patterns, the same limit.

This is at the **smallest** quorum tested. The live InstantSend quorum is ten times larger,
and §16's size scaling says capacity falls roughly as one over quorum size.

### Past capacity it loses transactions rather than slowing down

The 9 of 6 and 13 of 8 rows above are not merely slower. They are **short**: 76 and 227
transactions never received an InstantSend lock at all, and the runs ended with the node
idle and the transactions still sitting in the mempool.

The chain of events is verified rather than inferred:

1. **Sessions are purged on a timeout.** `CSigSharesManager::Cleanup` drops any session
   whose last new share arrived `SESSION_NEW_SHARES_TIMEOUT` = 60 seconds ago
   (`quorums_signing_shares.cpp:1317-1357`). Measured: 237 sessions timed out on one node
   in a single 9-of-6 run, the log recording `sigShareCount=4` against a threshold of 6.

2. **Nothing retries them.** `pendingRetryTxs` is only ever populated with the *children of
   a transaction that just locked* (`quorums_instantsend.cpp:1239-1245`). A transaction
   whose own session timed out is never queued, so `ProcessPendingRetryLockTxs` never
   considers it. Measured: that same run logged **zero** `retrying to lock` lines while 85
   transactions went unlocked.

3. **The only way back is a block.** `CInstantSendManager::BlockConnected` calls
   `ProcessTx(tx, true, ...)` for unlocked transactions in the block
   (`quorums_instantsend.cpp:1176`), and only that retroactive path sets `allowReSign` so
   `AsyncSignIfMember` will vote a second time (`quorums_signing.cpp:920`). By then the
   transaction is mined and the lock adds nothing.

So the failure above capacity is silent. There is no error, no backpressure, and no
degraded-but-working mode: InstantSend simply stops applying to the excess, and on a live
chain the transactions wait for an ordinary confirmation while the RPC still reports them
as unlocked.

### What it says about attestation as a scaling mechanism

The largest quorum tested here is 13 members. **The live InstantSend quorum is 50**
(section 15). The fit extrapolates to roughly 320 ms per session per node at that size,
about 3 sessions per second — but that is four times beyond the measured range and is not
claimed as a number. The measured direction is the point: capacity falls roughly as one
over quorum size, and the network's real quorum is an order of magnitude larger than
anything measured here.

Any design that routes transaction volume through quorum attestation inherits all three of
these properties: a per-session cost that grows with quorum size, a pipeline that is
round-trip-bound rather than compute-bound, and a saturation behaviour that discards work
instead of queueing it.

### Instrument defects found on the way

Each of these produced a plausible wrong answer first, and each was caught by a second
quantity disagreeing with the first.

- The fan-out transaction stayed in the mempool, because `IsTxSafeForMining` refuses a
  transaction that is neither islocked nor `WAIT_FOR_ISLOCK_TIMEOUT` old. Its children were
  then all descendants of one entry and everything past the 25th was rejected. The submit
  path swallowed the errors and reported a clean rate for the 25 that got through.
- The mocked clock ran at twice real time, which shortens every LLMQ timeout in proportion:
  the 60-second session timeout fired after 30 real seconds and a 6,000-transaction run
  stalled at a quarter of the work. That is indistinguishable from the genuine stall
  described above, and had to be removed before the genuine one could be claimed.
- The framework starts nodes with `-debug`, meaning every category, writing a line per sig
  share inside the timed path.
- The log stamps UTC and `strptime` returns a naive datetime, so the per-node window filter
  compared timestamps two hours apart and reported zero sessions for a run that plainly
  worked.
- The CPU average covered the lock-wait timeout as well as the load, so a run that ends with
  unlocked transactions spends ten minutes idle and reports a half-busy thread at 4.5%.

### Not established here

- Whether `SPORK_2_INSTANTSEND_ENABLED` is 0 on mainnet. Mempool signing only runs when it
  is exactly that (`quorums_instantsend.cpp:1700`) and the compiled default is off. It is 0
  on regtest, which is why these transactions lock. If mainnet has it off, this measures
  what an attestation layer would cost rather than what InstantSend costs today.
- Everything is loopback on one machine, which flatters the round-trip cost that turns out
  to dominate.
- The quorum sizes tested are 5, 9 and 13 against a live size of 50.

## 17. What a shorter validation path actually buys

§15 and §16 priced the attestation. This prices what it would relieve.

One binary, one corpus (v2, 2-in-2-out), one restored chain state, the same offered rate of
25,000 tx/s. The only difference between arms is a runtime flag.

| arm | median | p90 | peak | node CPU, median | node CPU, peak |
|---|---|---|---|---|---|
| **stock** | 5,269 tx/s | 5,550 | 5,779 | 119% | 361% |
| **skipsigs** — signature verification removed | **15,842 tx/s** | 18,869 | 20,779 | 135% | 698% |
| **parallel** — the same checks on the `CCheckQueue` | **4,721 tx/s** | 4,971 | 5,095 | **249%** | 386% |

Each arm was proved to be what it claims before it was measured, using a transaction
carrying a genuine signature over an unrelated digest — valid DER, low-S, wrong:

| arm | that transaction | proves |
|---|---|---|
| stock | rejected, `mandatory-script-verify-flag-failed` | verification really runs on this corpus |
| skipsigs | **accepted** | the flag genuinely removes verification |
| parallel | rejected, `perf-parallel-script-failed` | the checks still run, and via the pooled path |

### Signature verification is two thirds of the acceptance path

Removing it **triples** the ceiling, from 5,269 to 15,842 tx/s. That settles the question
§16's correction opened: a shorter validation path is not a rounding error at acceptance,
it is most of it.

This is an upper bound and a generous one. A real attested path still has to verify the
attestation, and `skipsigs` removes the work entirely rather than replacing it with
something cheaper.

### But the free version of that saving does not work

**Correction.** The previous revision of this document, and the advice given on it, said the
same 3× was available with no trust assumption by running ATMP's script checks through the
`CCheckQueue` the node already owns. Measured, that is false. The pooled arm is **10%
slower than stock while using more than twice the CPU** — 4,721 tx/s at 249% against 5,269
at 119%.

The reason is granularity. `ConnectBlock` queues thousands of checks across thousands of
transactions and waits once. ATMP handles one transaction at a time, so the pooled arm
takes a `CCheckQueueControl`, enqueues **two** checks, wakes workers and waits — per
transaction. At ~59 µs of actual verification per input, the synchronisation costs more
than the work.

So the 3× is real but it is not free, and it is not a flag. Getting it locally would mean
batching script checks across transactions inside the acceptance path — pipelining several
transactions' checks before waiting — which is a genuine piece of engineering with its own
correctness questions about which transaction failed. That is untested here, and it is the
honest comparator for any attestation proposal rather than the naive pooling that loses.

### What it says about the buspool

An attestation scheme's best case at acceptance is **3×**, on the one path that already had
the most headroom: 5,269 tx/s against a 2 MB decoupled design point of ~520 tx/s, ten times
over.

Meanwhile relay delivers ~75 tx/s per peer (§8), which is **70 times below** even the
`skipsigs` ceiling, and attestation does not touch it — bodies reach every node whether a
quorum signed them or not. And the attestation layer that would deliver the 3× runs at
~76 locks/s at a five-member quorum (§16), an order of magnitude below the design point it
is supposed to serve.

The 3× is therefore real and worth having at some future throughput, and it is not what
decides anything at the throughputs actually in question.

### Where the cost goes if it is skipped

With verification removed the node's CPU peak rose from 361% to 698%, so the ceiling moved
somewhere that parallelises. What does not move is block connection: §10's 1.1 s for 85,624
transactions is a *warm* number, warm because ATMP verified those signatures first. Skip
them at acceptance and connection pays them cold. Attestation relocates the cost unless
block validation trusts the attestation too — a consensus change, and it has to be argued
as one.

## 18. A livelock in the socket handler, and what it invalidated

Every high-throughput relay run in this document stalled at the same kind of point, and
for most of a working day that stall was reported as a capacity ceiling. It was a bug.

### The defect

`CConnman::SocketHandler` decides whether to skip waiting for socket events using a
different test from the one that decides which work actually gets done.

The skip-wait test was `!mapReceivableNodes.empty()` (`net.cpp:1623`). The do-work test,
forty lines later, admits a node only if **all three** of these hold (`net.cpp:1718`):

```cpp
!it->second->fPauseRecv && it->second->nSendMsgSize == 0 && !it->second->fDisconnect
```

A node satisfying the first but not the second is phantom work. The loop skips its wait,
finds nothing it is willing to do, and immediately goes round again — `SocketEvents` polls
with a zero timeout every time. Under sustained load, where a peer routinely has both
readable data and queued outbound data, this never resolves on its own.

Measured, with the decision instrumented to report which branch kept the poll hot:

| | before | after |
|---|---|---|
| `SocketHandler` iterations/second | **1,446,199** | **2** |
| `rtm-net` CPU | 100% of a core, indefinitely | 0% |

The node froze with its mempool, bytes received and bytes sent all static while burning a
full core, and recovered only when the peer disconnected. That is the "socket thread burns
a full core spinning on a socket it has paused" line in §16.4 — filed there as a curiosity,
and actually this.

### The fix

Make the predicate match the admission test exactly, and say so in the code so it stays
matched. The name upstream uses for this — `HasUnpausedReceivableNode` — describes only the
pause condition, and implementing the name rather than the requirement is precisely the
mistake that produced a half-fix here: checking only `fPauseRecv` moved the stall from 30
seconds to 72 and raised delivery 67%, while leaving the spin in place.

### What it invalidated, and what survived

Every relay measurement in this document was taken on a node carrying this defect,
including §8's 74.7 tx/s. The low-cap runs never trigger it — no backpressure, no spin —
but that needed checking rather than assuming.

Re-measured on the fixed binary, offering 5,000 tx/s for 150 s with `maxmempool=8000`:

| cap | accept | relay | withheld |
|---|---|---|---|
| shipped (280) | 3,848 tx/s | **44 tx/s** | 0% |
| 4,000 | 4,391 tx/s | 435 tx/s | 0% |
| 50,000 | 4,584 tx/s | 2,838 tx/s | 71.7%, stalls at 30 s |

**The baseline reproduces**: 44 tx/s against the 45–48 measured before the fix. §8's
low-cap relay results stand.

### Retracted: the "fixed thread budget"

An earlier revision of this document reported that the message handler had a work budget of
roughly 5,000 operations per second, divided between accepting and relaying, on the
evidence that accept + relay stayed near 5,000 across a cap sweep. That is withdrawn. The
sweep was run on the buggy binary; on the fixed one, cap 50,000 does 4,584 accept **plus**
2,838 relay — 7,422, well past the supposed budget. The conserved-looking sum was an
artefact of every high-cap case dying in the same livelock.

Two other hypotheses were tested and refuted rather than reasoned away. The relay path's
fee ordering is genuinely expensive — `make_heap` over the whole pending set per trickle,
with a mempool lookup per comparison, measured at over 40% of the message handler under a
300,010-transaction backlog (`SipHashUint256` 30.75%, `CompareDepthAndScore` 9.75%). But
disabling it changed relay throughput by nothing measurable (3,560 → 3,464 at cap 50,000;
45 → 48 at the shipped cap) and did not prevent the collapse. A large profile share does
not make something the bottleneck when the thread is about to livelock regardless.

### A second stall — and why it is NOT a real defect (corrected 2026-09-16)

With the livelock fixed, the high-cap runs still stalled, but differently: the node slept
(`rtm-net` in `ep_poll`) instead of spinning. Kernel `/proc/net/tcp` at the stall showed the
send buffers full to the ceiling (~2.5 MB) in **both** directions on the load connections,
with both nodes idle — the shape of a mutual flow-control deadlock, arising from two rules
that are each correct alone: reads require an empty send queue (`net.cpp:1774`), and writes
require `fCanSendData`, which only re-arms on an epoll `EPOLLOUT` edge on edge-triggered
sockets. Two peers both applying "drain before read" with pending sends can lock in both
directions.

**This was a rig artifact, not a real-node defect.** The wedge always sat on the
SUT↔load-generator connections. The generator is a Python flood tool running eight threads
under the GIL, and it under-reads — that is what fills the node's send buffer and triggers
the read-refusal. The SUT↔peer link between two *real* raptoreumd nodes was never
deadlocked in the diagnosis, only starved.

Tested directly: two real nodes, connected, each loaded with half the corpus so both relay
to the other at cap 50,000 — pure bidirectional real-node relay, no generator in the path.
They converge cleanly to the union and never freeze:

| | own shard | converged to | node CPU |
|---|---|---|---|
| node A | ~121,000 | **271,877** | busy throughout, never 0% |
| node B | ~144,000 | **268,010** | busy throughout |

Run **without** any fix (a candidate read-rule relaxation was tried and reverted — it was
unnecessary and slightly *lowered* the converged totals). Real peers drain their sockets
promptly, so the send buffers never fill, so the read-refusal never fires. Honest nodes do
not reach this state.

The mechanism remains latent in the code: a peer that *deliberately* under-reads can wedge
its own connection, but that harms only that one connection, which the node clears on its
inactivity timeout — not a node-wide DoS like §18's livelock. No fix is warranted.

**Retraction.** Earlier revisions of this section reported this as a real "connection wedged
in both directions" defect and the high-cap relay stalls as a genuine ceiling. Both are
withdrawn: the stalls were the generator under-reading, and the true relay ceiling on real
nodes is set by rate, not by any deadlock.

## Rig notes

Three rig defects were found by accounting rather than by failure, each of which would
have biased results:

1. `sendall` blocks under backpressure, so the offered rate silently became an outcome.
2. Sockets closed with bytes still queued — a contiguous tail vanished.
3. **The node discards a peer's unprocessed receive buffer on disconnect**, so closing
   at the end of a run destroys exactly the backlog that measures how far behind the
   node is. Any test that disconnects at the end reports the node as faster than it is.

## Still open

- Stage C, InstantSend. `-llmqtestparams` overrides only size and threshold;
  `dkgBadVotesThreshold`, `signingActiveQuorumCount`, `recoveryMembers` and
  `keepOldConnections` stay at their three-member values and need a test-only patch.
- Stage D, block connection at large sizes. Needs `MAX_PROTOCOL_MESSAGE_LENGTH` raised
  from 3 MB first, or the blocks cannot cross the wire at all.
- Everything here is one machine. Nothing has been measured on hardware a typical
  smartnode actually runs, and the network's ceiling is its slowest member.
- The unresolved kernel symbol in §6's `perf diff` (11% → 40%) is still unattributed and
  is deliberately not claimed as a networking cost.

## 19. Threshold recovery parallelises — the attestation path was never optimised

§15 measured threshold recovery at 9.41 ms (106/s) and called it the dominant cost of the
attestation path. §16 measured the live ceiling at ~76 locks/s. Both stand. What neither did
was ask whether either number is a *floor*, and the comparison against mempool acceptance was
made between an optimised, characterised validation path and an untouched attestation path.

Recovery is Lagrange interpolation over one session's shares with no state shared between
sessions, so recoveries for distinct messages are independent. Measured on a Xeon 6517P
(16 physical cores / 32 threads), threshold 30, 40 recoveries per thread:

| threads | recoveries/s | per-recovery | speedup |
|---:|---:|---:|---:|
| 1 | 107.1 | 9.33 ms | 1.0x |
| 2 | 215.9 | 9.27 ms | 2.0x |
| 4 | 430.2 | 9.30 ms | 4.0x |
| 8 | 792.3 | 10.10 ms | 7.4x |
| 16 | 1,150.9 | 13.90 ms | **10.7x** |

Single-thread reproduces §15 exactly (9.33 vs 9.41 ms). **Recovery parallelises near-linearly
to four threads and gives 10.7x at the physical core count.** The 106/s figure is unexploited
serialisation, not a limit: it runs synchronously on the single
`CSigSharesManager::WorkThreadMain`, and `CBLSWorker` exposes async aggregation, verification-
vector building, contribution-share verification and signature verification — but **no async
recovery**. The thread pool is already there; recovery simply is not wired into it.

**The nuance that cuts the other way:** the live ceiling of ~76 locks/s is *below* the 106/s
recovery ceiling, so recovery was not the binding constraint in that test — the round-trip
cadence was (`SendMessages` runs at most once per loop iteration). Parallelising recovery
removes a ceiling that has not yet been reached.

So the attestation path has two addressable constraints and neither has been touched: the
messaging cadence (binding now, a scheduling constant of the same class as the relay cap,
which was worth ~30x when lifted) and recovery (binding at ~106/s, removable to ~1,150/s).

**Consequence for the design comparison.** The claim that quorum attestation is too slow to
be worth building on is **not safe**, and should not be used against it. What survives is
architectural rather than throughput: attestation requires a quorum round trip, which imposes
a latency floor that local validation does not have, and it makes the fast path depend on
smartnode liveness. Those objections hold regardless of how fast recovery runs.

Bench: `BLS_Recover_ParallelScaling` in `src/bench/bls.cpp`.

---

## 2026-09-17 — Phase 1 on a 12-node WAN swarm: compact blocks do not survive sustained load

> **Read with the relay finding below.** The numbers here hold, and so does the mechanism
> (missing per block tracks arrival rate x propagation delay). What was not known when
> this was written is *why* the propagation delay is what it is: relay services ~936 tx/s
> against 1500 offered, so the delay is a queue, not a constant. The title overstates it --
> compact blocks are not the failing component, they are reporting the relay shortfall.

First measurement of block propagation on real geography rather than loopback. Twelve
regtest nodes: eleven VPSs across three continents plus bowser, RTT 10-240 ms, full mesh.
Rig at `test/perf/swarm`; every node offers load from its own disjoint corpus shard, so
transactions originate everywhere rather than at one point.

**Setup.** Corpus v3 (600k tx, 397 B mean, variable fees), split 12 ways. Load offered over
RPC, not P2P — a P2P generator is itself a peer, so the node announces back to it and a full
send queue stops the node reading, which measures the generator. Timestamps corrected with
a per-run SNTP offset pass (worst host +10.6 ms, and before chrony one host sat at -14.8 ms,
enough to make a 10 ms europe->dev hop measure *negative*).

**Condition.** 240 tx/s network-wide, block every 15 s, 90 s. Deliberately *below* absorption
(a 2 MB block holds ~5,035 of these transactions, so 15 s blocks absorb ~336 tx/s): the
mempool stays bounded and nothing is evicted, so this is the benign case.

**Result — compact-block reconstruction fails on essentially every loaded block.**

| | miner also offers load | miner offers no load |
|---|---|---|
| loaded blocks needing GETBLOCKTXN | 55/55 (100%) | 53/55 (96%) |
| tx missing per block (median) | 99-147 (2.6-4.6%) | 22-51 (0.6-1.3%) |
| full-mesh convergence, median | 1006 ms | 873 ms |
| convergence, worst | 1226 ms | 1299 ms |

The empty block (coinbase only) reconstructed cleanly at 11/11 nodes in both runs, which is
the control that says the rig is measuring what it claims.

**The confound was tested and is not the explanation.** A miner that also submits load mines
transactions it created milliseconds earlier, which no peer can possibly hold. Removing the
miner from the load set cut the missing count ~3x (117-147 -> 22-51) but barely moved the
incidence (100% -> 96%), because a single missing transaction forces the round trip.

**Mechanism.** Missing count tracks arrival rate x propagation delay, not block size. At
240 tx/s, 22-51 missing per block is roughly 0.1-0.2 s of arrivals — precisely the
transactions still in flight when the block was mined. Scaling to the 1500 tx/s v1 target
predicts ~140-320 missing per block at the same cadence, each fetch costing up to 2xRTT on
the Asia links.

**Consequence for decoupling.** Compact blocks are not a mitigation at sustained throughput,
and the shortfall is not bandwidth — a 3,700-tx block is ~1.4 MB and the links carry it
easily. What costs is the round trip for the fraction still in flight. A commitment block
naming txids inherits exactly this property: the receiver still needs bodies it does not
have. The decoupling gain is therefore *not* "the block is smaller so reconstruction gets
easier" — that claim would be wrong. The gain is that bodies propagate continuously and
independently of block cadence, so the in-flight fraction is the only thing the block waits
on, rather than the whole body set. This needs its own measurement before it is claimed.

**Also measured.** ConnectBlock on ~3,700-tx blocks: 33-57 ms on the 6-12 thread hosts
(cor 33, bow 38, dev 44, eur 57), 111-157 ms on the 4-thread hosts, worst case 352 ms.
Roughly linear in transaction count, consistent with the 2-4 ms seen for 200-tx blocks.

**Not yet measured:** behaviour above absorption (mempool growing, eviction active), the
1500 tx/s operating point, and whether faster blocks trade round trips for orphan risk.

### 2026-09-17 — the same swarm at the v1 target (1500 tx/s): cascade failure

> **SUPERSEDED — do not quote the conclusion of this entry.** The measurements below are
> sound; the explanation is not. This entry blames an absorption gap. Running the same
> load at a cadence that absorbs it fixed acceptance and mempool growth and left
> convergence unchanged, so absorption is not the cause. The cause is a relay throughput
> ceiling of ~936 tx/s; see the two entries below.

Identical to the run above in every respect except offered rate: 240 -> 1500 tx/s, block
every 15 s, 180 s, miner offers no load. A 2 MB block holds ~5,035 of these transactions, so
15 s blocks absorb ~336 tx/s: this run is deliberately **4.5x above absorption**.

**Acceptance is not the problem.** 261,296 of 270,006 offered were accepted (96.8%), every
node sustaining its full 136 tx/s with zero rejections (the one exception, usw, had been
rebooted mid-run — see caveats). Per-thread CPU measured from `/proc/<tid>/stat` deltas:

| host | rtm-msghand | all threads |
|---|---|---|
| bowser (24 thr) | 20.5% | 26% |
| core (12 thr) | 27.7% | 33% |
| rtm-c4 (4 thr) | 61.2% | 77% |
| asia-east (4 thr, also runs a production mainnet node) | 71.7% | 94% |

**This answers the open question in the checklist** ("single-thread acceptance on
representative smartnode hardware"): a 4-thread VPS carries 1500 tx/s with msghand at
61-72% of one core. Parallel validation stays parked. Note `rtm-cl-schdlr` never exceeded
0.5% — the ChainLocks Cleanup walk is *not* hot at this mempool size, contrary to an earlier
reading taken with `top -b -n1`, whose first sample is computed from process start and is
not an instantaneous figure.

**Everything downstream of acceptance collapses.**

| | 240 tx/s (absorbing) | 1500 tx/s (4.5x over) |
|---|---|---|
| blocks needing GETBLOCKTXN | 53/55 (96%) | 111/121 (92%) |
| txn requested per block | 22-51 | **up to 5,206** (~the whole block) |
| matched from mempool | ~3,700 | **150-375** |
| full-mesh convergence, median | 873 ms | **34,591 ms** |
| convergence, worst | 1,299 ms | **66,637 ms** |
| final mempool spread across nodes | identical | **1.97x** (72,008 .. 141,542) |
| ConnectBlock, median | 33-157 ms | 77-498 ms |

Convergence (34.6 s) is **more than twice the block interval (15 s)**, so the network never
reaches a common state: each block arrives while the previous is still propagating.

**Mechanism — a positive feedback loop, entered at the absorption gap.**

    1500 tx/s in, 336 tx/s out
      -> mempool grows without bound
      -> relay backlog, and nodes fall out of step
      -> mempools diverge (1.97x here)
      -> compact blocks match almost nothing (150-375 of ~5,035)
      -> each node pulls ~2 MB per block via GETBLOCKTXN instead of ~20 kB
      -> that traffic competes with transaction relay
      -> divergence worsens

**Consequence for decoupling — and this is a stronger argument than the one we had.** The
earlier framing, "commitment blocks are smaller so propagation is cheaper", is not the point
and on its own is wrong: at 240 tx/s the block was only ~1.4 MB and propagated fine. What
breaks the network is the *absorption gap*, the first link in the chain. A commitment block
at 15 s carrying 22,500 txids is 720 kB — smaller than the 2 MB full block it replaces, and
it absorbs the entire arrival rate, so the mempool does not grow, nodes do not drift apart,
and none of the rest of the cascade starts. Decoupling is worth arguing for because it
removes the *cause*, not because it makes block bodies smaller.

**Caveats.** usw was rebooted (by the operator, unrelated to the test) shortly before this
run and missed the first transactions, giving it 8,710 rejects and leaving it behind; its
propagation figures are therefore suspect, though it is not an outlier in the final mempool
spread. Single run, not repeated. The two visible mempool clusters (~140k and ~72-97k) are
consistent with a lagging group rather than eviction — no node reached its maxmempool.

### 2026-09-17 — the absorption hypothesis is WRONG (same swarm, absorbing cadence)

The entry above concluded that the cascade is entered at the absorption gap. **That is
refuted by the next run and the claim should not be used.** Same 1500 tx/s, same corpus,
same miner, only the cadence changed: 15 s -> 3 s blocks, which at 5,357 tx/block absorbs
1,786 tx/s, comfortably above the offered rate.

| | 1500 @ 15 s (4.5x over) | 1500 @ 3 s (absorbing) |
|---|---|---|
| acceptance | 96.8% | **100%** |
| mempool | grows without bound | **bounded**; drained 30,963 -> ~2,000 mid-run |
| final mempool spread | 1.97x | **1.27x** |
| blocks needing GETBLOCKTXN | 91.7% | **96.2%** (worse) |
| txn requested per block | up to 5,206 | up to 4,233 |
| convergence, median | 34,591 ms | **35,444 ms** (unchanged) |

Absorption fixed precisely what it should — acceptance and mempool growth — and did nothing
at all for propagation. So mempool growth is a *symptom*, not the cause.

**What the data actually says.** Nodes held 33,000-41,000 transactions and matched only
144-1,552 of a 5,357-transaction block. They are not short of transactions; they are short
of *those* transactions. The suspect is `INVENTORY_BROADCAST_INTERVAL = 5` seconds
(net_processing.cpp:159): a transaction waits a Poisson-mean 5 s before being announced to
any given peer. With a 3 s block interval the trickle is **slower than block production**,
so every block is built largely from transactions the miner has and its peers have not yet
been told about. At 1500 tx/s, 5 s of arrivals is 7,500 transactions in flight — the right
order of magnitude for the 4,000 missing per block that we see.

This also explains the 240 tx/s run without any appeal to absorption: 5 s of arrivals there
is 1,200 transactions, of which only the fraction selected into a block is missed, and the
observed miss was 22-51.

**Directly testable** with the existing `-perfinvinterval` flag, which overrides the constant
(net_processing.cpp:201). Next run: absorbing cadence with the trickle set below the block
interval. Until that is measured, no causal claim should be made about the propagation
collapse, and the decoupling argument must not lean on one.

### 2026-09-17 — relay is the binding constraint, not acceptance or block size

Measured per-transaction propagation directly: every node logs each accepted txid under
`-debug=mempool`, and with `logtimemicros` plus chrony (max pairwise skew 5.5 ms) those
timestamps are comparable across hosts. Origin time comes from the submitter's own log,
since RPC submission does not pass through net_processing and emits no such line locally.
1500 tx/s offered from 11 nodes, 150 s, no mining. 225,005 transactions.

**Headline.**

| | |
|---|---|
| origin -> one peer, p50 | 66.5 s |
| origin -> all 12 nodes, p50 | 172 s |
| network-wide within 120 s | 12.2% |

**The distribution is not stationary, and that is the whole finding.** First-hop p50 against
when the transaction was offered:

| submitted at | first-hop p50 |
|---|---|
| 0-15 s | 3.2 s |
| 30-45 s | 25.3 s |
| 60-75 s | 39.4 s |
| 90-105 s | 61.7 s |

Latency grows linearly with time into the run. A constant overhead (the logging we added to
measure this, for instance) would be flat; a growing queue looks exactly like this.

**Two quantities separate out.**

1. *The floor is the trickle.* Before any backlog, first-hop p50 is 3.2 s -- what a Poisson
   draw with `INVENTORY_BROADCAST_INTERVAL = 5` predicts (median ~3.5 s). This is a latency
   term and it is unavoidable without changing the constant.

2. *The ceiling is relay throughput.* For offered R and serviced S, a growing queue gives
   dL/dt = (R-S)/S. Least-squares on the buckets gives slope 0.602, so S is about
   1500/1.602 = **936 tx/s** against 1500 offered -- a 564 tx/s deficit that accumulates.
   Relay services roughly 62% of the offered rate.

**This reorders the whole picture.** Acceptance sustains 1500 tx/s at 61-72% of one core on
a 4-thread VPS. Relay does not. Every earlier observation follows from this without needing
an absorption gap or a block-size argument: blocks reference transactions that have not
propagated because relay is 600 tx/s behind, so compact reconstruction misses, so nodes pull
whole blocks, so convergence collapses.

**Consequence for decoupling — it does not fix this.** A commitment block changes what the
*block* carries. The bodies still cross the same relay path at the same 1500 tx/s. Whatever
decoupling does for block size, the relay path has to carry the full transaction rate or the
backlog grows regardless. The relay work in Tier 3 is therefore not an accompaniment to
decoupling; on this evidence it is a precondition for any of it.

**Caveats.** An operator `tail -f` on all twelve debug logs began 101 s into the 171 s load
phase, adding roughly 225 kB/s of outbound per node against ~550 kB/s of relay upload.
Restricting the fit to buckets before it started gives S = 936 tx/s; including everything
gives 921. A 1.6% difference, so the finding is not an artefact of it -- but log streaming
must be off during future runs. The absolute value of S is also measured with
`-debug=mempool` on, which costs each
node ~1500 log lines/s; the true ceiling is somewhat higher. The queueing signature (the
linear slope) is robust to that, since a constant tax cannot produce it. The run is
right-censored: p50 to all nodes (172 s) exceeds the run length (150 s), so only 40% of
transactions completed propagation inside the captured window -- percentiles for those that
did are valid, the tail is truncated. `-perfinvmax=50000` was confirmed applied in each
node's own log, so this is not the shipped 56 tx/s cap. Single run.

**Next, and it is cheap:** `-perfinvinterval` already exists. Re-run at the same offered rate
with the trickle set below the default and see whether S moves. If S is set by the trickle
schedule, it will; if S is set by message handling, it will not.

**Scope limit on every swarm number so far: no smartnode features are active.** Verified on
the running network, not assumed: `smartnode count` returns 0 total / 0 enabled, `quorum list`
returns empty sets for both `llmq_test` and `llmq_test_v17`, and the node logs contain zero
islock and zero chainlock lines. So InstantSend, ChainLocks and LLMQ signing contribute
nothing to these measurements.

That makes all of them optimistic by an unmeasured margin. On mainnet each transaction also
drives an InstantSend lock attempt -- signature shares relayed, a recovered signature
relayed, then the islock itself -- which is a second high-rate message stream sharing the
relay path measured here at ~936 tx/s. ChainLocks add periodic signing plus the O(mempool)
`Cleanup` walk. The relay ceiling should therefore be read as an upper bound, and the
`rtm-cl-schdlr` idleness observed today says nothing about its cost on a network that has
quorums.

Bringing smartnodes up on the swarm is its own piece of work (ProTx registrations, DKG,
quorum formation) and has not been attempted.

### 2026-09-17 — the relay ceiling cross-validated, and ChainLocks Cleanup ruled out

**Independent confirmation of S.** The miner offers no load, so everything entering its
mempool arrived over relay and its acceptance rate is the delivery rate directly. Counting
`AcceptToMemoryPool` lines per second on `cor` during the load phase gives 864-922 tx/s,
against 936 tx/s derived from the queueing slope of per-hop latency. Two unrelated methods
within ~5%. Relay delivers roughly 900 tx/s against 1500 offered.

(Rates fall to ~320 tx/s after t+150 s in that data. That is the offered load ending and the
backlog draining, not a slowdown.)

**`CChainLocksHandler::Cleanup` is not the cause, though the code deserves a look anyway.**
`TransactionAddedToMempool` has no gate -- not on quorums, not on ChainLocks, not on
smartnode status -- so every node records every transaction in `txFirstSeenTime`
(quorums_chainlocks.cpp:373). `Cleanup` then walks that whole map every 30 s
(`CLEANUP_INTERVAL`), calling `GetTransaction` per entry while holding
`LOCK2(cs_main, mempool.cs)`, and entries are only removed once a transaction is six blocks
deep. With a 34x absorption gap nothing reaches six confirmations, so the map grows without
bound. That is a real hazard and it is the Tier 2 item's mechanism.

It is nevertheless **not binding at the sizes measured here**, on two tests:

- *No periodicity.* Per-second acceptance autocorrelation at lag 30 s is +0.250, against
  +0.294 at 29 s and +0.217 at 15 s. No peak at the cleanup interval.
- *No size dependence.* On `cor`, acceptance was 864 tx/s with an empty mempool and 876 tx/s
  with 133,000 entries in it. A walk whose cost is O(map) would have shown decay.

So relay's ~900 tx/s is a **fixed per-transaction cost**, not a structure that degrades as
state grows. That matters for the fix: making the relay path cheaper or concurrent, rather
than repairing a data structure. The Cleanup hazard still wants addressing before any
long-running high-throughput deployment, but it is not what is capping throughput today, and
this measurement was taken with no quorums -- on a network with ChainLocks the map is also
pruned by `blockTxs` handling that never ran here.

### 2026-09-17 — the relay ceiling is message-handling-bound, not schedule-bound

Two conditions, identical in every other respect: 1500 tx/s offered from 11 nodes, 150 s,
miner offers no load so its mempool growth is the relay delivery rate S. Confirmed applied
in each node's own log (`PERF: relay trickle overridden (max=50000, interval=...)`).

| condition | S | rounds |
|---|---|---|
| shipped `INVENTORY_BROADCAST_INTERVAL` (5 s) | 930 tx/s | 37 (full run) |
| the same, truncated to B's window | 899 tx/s | 20 |
| `-perfinvinterval=1` | 849 tx/s | 20 |

Compared on a matched window, cutting the trickle interval fivefold left S **6% lower** --
noise, or marginally worse because more frequent messages cost more per message. It
certainly did not raise throughput.

**So the two effects are now fully separated.**

- The trickle interval sets a *latency floor*: unloaded first-hop p50 was 3.2 s, matching a
  Poisson draw with mean 5 s. Lowering it would reduce that floor.
- The ~900 tx/s is a *throughput ceiling* set by message handling, and no scheduling constant
  moves it. Everything the relay path does per transaction -- INV, GETDATA, TX, the
  bookkeeping around them -- lands on the single `msghand` thread.

**What this means for the plan.** The Tier 3 relay item was scoped as re-indexing
`InvBroadcastMax()`, a constant change. That is still necessary -- the shipped cap delivers
56 tx/s per peer and nothing works without lifting it -- but it is not sufficient. With the
cap already at 50,000 and the schedule irrelevant, throughput stops at ~900 tx/s against a
1500 target. Reaching the target needs the relay path itself to get cheaper or concurrent,
which is real work rather than a constant, and it is a precondition for decoupling rather
than an accompaniment: commitment blocks change what the block carries, not the rate at
which bodies must cross the network.

**Caveats.** B is a partial run (20 of ~37 rounds) compared against A truncated to match;
A's own full-window figure is 930. Single runs. No smartnode features, so the true ceiling on
a mainnet-like network is lower. `-debug=mempool` was off for both, and no log streaming was
running.
