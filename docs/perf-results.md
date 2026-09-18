<!-- lifecycle: append-only — entries are dated and not rewritten
     owns:      the measurement log: what was run, under what conditions, and what came out
     not mine:  what the numbers mean for the design (transaction-decoupling.md) or which value
                the project relies on (findings.md)
     rule:      a conclusion later overturned gets a forward-pointing correction banner added
                above it. It is not edited away — the retraction is the record. -->

# Raptoreum throughput — measured results

> **Corrections (2026-09-16).** This file is a chronological log; entries below stand as what
> was measured *at the time*, but three conclusions drawn from them were later overturned.
> The current picture is `findings.md` (values and their regimes) and `transaction-decoupling.md`
> (what they mean). `throughput-bottleneck.md`, which this banner used to name, was archived on
> 2026-09-18 — its own headline had been overturned; its unique measurements are in the
> 2026-09-18 back-entry below.
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
| shipped `INVENTORY_BROADCAST_INTERVAL` (5 s) | 930 tx/s | 37 |
| `-perfinvinterval=1` | 917 tx/s | 37 |

Both runs accepted 225,005 of 225,005 offered and took the same wall time (153.4 s vs
154.6 s). Cutting the trickle interval fivefold changed relay throughput by **-1.4%**, which
is noise. It certainly did not raise it.

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

**Caveats.** Single runs, one per condition, 37 sample rounds each. No smartnode features, so the true ceiling on
a mainnet-like network is lower. `-debug=mempool` was off for both, and no log streaming was
running.

### 2026-09-17 — where the relay cost is, from source (hypotheses, not yet measured)

Reading the path each accepted transaction takes through `msghand`, before pointing a
profiler at it. Nothing here is measured yet.

`RelayTransaction` (net_processing.cpp) does, per transaction:

```
CInv inv(CCoinJoin::GetDSTX(txid) ? MSG_DSTX : MSG_TX, txid);
connman.ForEachNode([&inv](CNode *pnode) { pnode->PushInventory(inv); });
```

1. **`CCoinJoin::GetDSTX`** takes `cs_mapdstx`, looks up `mapDSTX`, and returns a
   `CCoinJoinBroadcastTx` **by value** -- constructing an object solely to be tested as a
   bool. Runs on every relayed transaction regardless of whether the chain has any CoinJoin
   activity.

2. **`PushInventory` per peer**, each taking `LOCK(cs_inventory)`, a
   `CRollingBloomFilter::contains`, and a `std::set<uint256>::insert` -- an ordered tree
   insert with an allocation, not a hash set (net.h:1077).

With the swarm's 22 connections per node that is 23 lock acquisitions and 22 tree inserts
per transaction: roughly 21,000 locks/s and 20,000 allocations/s at the measured 930 tx/s,
before `SendMessages` does any batching, sorting or message construction.

`LogPrint` inside `PushInventory` short-circuits on the category (logging.h:208) and is free
with NET logging off, so it is not a suspect.

**What this predicts, and why it matters for the number.** The dominant term scales with
*peer count*, and the swarm runs a full mesh -- 11 outbound plus 11 inbound. A mainnet node
carries fewer. If relay cost is per-peer rather than per-transaction, **930 tx/s is
pessimistic for a realistic topology**, and the fix targets the per-peer path (the set, the
lock, the bloom filter) rather than the per-transaction path.

**Next experiment, before profiling:** sweep peer count at fixed offered rate and see whether
S moves. That separates the two and says which path to attack. Profiling with `perf` on
bowser then confirms the attribution -- the binary there is now unstripped for that purpose.

### 2026-09-17 — a note on knobs that look applied and are not

Three settings this rig has now been caught on, all with the same shape: the configuration
is accepted, the node starts, the run completes, and the knob did nothing.

1. **Network options outside a `[regtest]` section.** `rpcbind`, `rpcport`, `bind`, `listen`
   and `addnode` are ignored at conf top level. The node prints a warning and carries on with
   defaults, so a wrong port looks like it worked -- ours happened to match the default.

2. **`maxtipage` and initial block download.** A seed chain whose tip predates `nMaxTipAge`
   leaves every node in IBD, where it refuses to serve headers and ignores transaction
   announcements while still accepting everything offered locally over RPC. Six of twelve
   nodes once ran a whole experiment in that state, reporting 100% acceptance while holding
   only their own transactions. The preflight gate in `run_phase1.sh` now refuses to start
   unless every node is out of IBD and on the same height.

3. **`addnode` does not limit peers.** It *adds* peers; the node independently fills its
   outbound slots from `peers.dat`, which persists across restarts and is rewritten
   immediately after a chain wipe. Configuring three peers per node left the mesh at 19-21
   connections, indistinguishable from the full mesh. Only reading `getconnectioncount` back
   showed it. Pinning the count needs `connect=` (which disables automatic outbound) *and*
   deleting `peers.dat`.

The pattern is that none of these fail loudly, and each produces a plausible-looking result.
The defence that actually works is reading the applied value back out of the running node --
`getconnectioncount`, `getblockchaininfo`, the `PERF: relay trickle overridden` log line --
rather than trusting that the config was written. Every experiment script here now does that
before it measures anything.

### 2026-09-17 — peer count does not drive relay cost; and a correction on the 100 kB cap

**Peer-count sweep.** Same 1500 tx/s, only the mesh density changed. Peer count was read
back with `getconnectioncount` rather than assumed, after an earlier attempt showed that
`addnode` does not limit peers at all (the node redials from `peers.dat`); `connect=` plus
deleting `peers.dat` is what actually pins it.

| condition | connections (c4/ase/cor/bow) | msghand c4 | msghand ase | msghand cor | msghand bow |
|---|---|---|---|---|---|
| full mesh | 21/16/19/11 | 89.3% | 90.3% | 44.2% | 30.1% |
| sparse | 5/6/6/3 | 78.2% | 85.8% | 46.2% | 28.9% |

Comparing msghand percentages alone says the cut moved little. **That comparison is wrong**,
and was published here before being corrected: the two conditions did not carry the same
delivered load, so CPU percentages are not comparable. Both accepted 100% of what was
offered, but relay *delivery* differed sharply.

| condition | connections | relay delivery S | us of msghand per delivered tx (c4 / ase) |
|---|---|---|---|
| full mesh | ~21 | **824 tx/s** | **1,084 / 1,096** |
| sparse | ~5 | **1,221 tx/s** | **640 / 703** |

Cutting connections roughly fourfold delivered **48% more throughput at lower CPU**, and
per-transaction msghand cost fell **41% (c4) and 36% (ase)**.

**So peer count does drive relay cost, and substantially.** Roughly 40% of msghand time is
per-peer work -- consistent with `ForEachNode` -> `PushInventory` doing a `cs_inventory`
lock, a rolling-bloom lookup and a `std::set<uint256>` insert for every peer on every
transaction. It also means the swarm's full mesh **understates** what a mainnet node with
fewer connections would achieve, so ~930 tx/s measured at 21 connections is pessimistic
rather than representative.

The methodological lesson is the one this metric existed to prevent: normalise by work
delivered before comparing CPU. Percentages measured under different throughputs say
nothing on their own.

**Correction: `MAX_STANDARD_TX_SIZE` is consensus, not policy.** An earlier entry claimed
the 100 kB cap bound only relay and that the block path was limited only by block size,
allowing ~13,500 inputs and tens of seconds of hashing in one transaction. That is wrong.
`ContextualCheckTransaction` (validation.cpp:419) rejects oversize transactions with
`DoS(100)`/`REJECT_INVALID` whenever DIP0001 is active, and it runs on both the mempool
path (line 619) and the block path (line 4056). The grep that misled us looked only in
`consensus/tx_check.cpp`, which carries the older `MAX_LEGACY_BLOCK_SIZE` check.

Corrected: one transaction is capped at ~675 P2PKH inputs, so quadratic sighash costs on
the order of 67 MB of hashing and tens of milliseconds, with ~20 such transactions fitting
a 2 MB block. Removing the cap is a **hard fork** and removes that bound.

**The input-count benchmark did not produce usable data and is not reported.** Every row was
rejected -- first for a flat fee below the size-scaled minimum relay fee, then for
`txn-mempool-conflict`, then for `bad-txns-oversize` once past 100 kB. A rejected
transaction short-circuits before signature checking, so the timings measure rejection
paths. The harness needs a size-scaled fee, non-overlapping UTXOs, and a ceiling of ~675
inputs. Rerun before any claim about the shape of the curve.

### 2026-09-18 — input-count validation cost: the quadratic term is NOT visible at legal sizes

`bench_inputs.py` finally produced data. What it needed was not the fee (the size-scaled fee
was already in the script) but a **fresh fan-out set** -- every earlier row collided with a
corpus run and was rejected as `txn-mempool-conflict`, silently, so the timings measured
rejection -- and **`--counts` capped at 650**, since past ~675 P2PKH inputs a transaction
exceeds `MAX_STANDARD_TX_SIZE` and is rejected as `bad-txns-oversize` before any signature
work. `fanout.py` also needed a `--port` passthrough to drive a node off the chain default.

Method: an isolated regtest node on bowser (`/data/rtm-bench`, rpcport 19788, `listen=0`,
fresh chain, zero assets, **not** the swarm node), two independent fan-out sets of 12,000 and
24,000 P2PKH UTXOs at 0.001 RTM. `delta_ms` is a valid transaction minus a same-shape
transaction whose first prevout does not exist, which is rejected after the same parse and
transport but before signature work.

| inputs | bytes | run 1 delta_ms (reps 2) | run 2 delta_ms (reps 5) | us/input, run 1 | us/input, run 2 |
|---|---|---|---|---|---|
| 25 | 3,770 | 3.6 | -- | 142.9 | -- |
| 50 | 7,456 | 7.3 | -- | 145.8 | -- |
| 100 | 14,832 | 16.1 | -- | 161.5 | -- |
| 200 | 29,564 | 37.9 | 38.4 | 189.5 | 192.0 |
| 300 | 44,330 | 59.8 | 50.4 | 199.4 | 167.9 |
| 400 | 59,068 | 66.9 | 51.3 | 167.2 | 128.3 |
| 500 | 73,818 | 68.9 | 70.7 | 137.8 | 141.4 |
| 650 | 95,954 | 104.3 | 102.6 | 160.5 | 157.9 |

**The headline: cost is linear in input count over the whole legal range.** Per-input cost is
flat at roughly 130-200 us from 25 inputs to 650, with no trend -- it wobbles in both
directions across two independent runs. Pooled least squares through the origin gives
**156 us per input**. A quadratic term would have raised per-input cost by about a third
between N=200 and N=650 (sighash bytes per input scale with transaction size: 29.6 kB against
96 kB); that rise is not there, and the run-to-run scatter is about +/-25%, larger than the
effect it would produce.

**The O(N^2) term is real in the code and simply not dominant at legal sizes.** At 650 inputs
the sighash work is 650 x 96 kB = 62 MB of SHA256d, which on this box (Ryzen 9 3900X, SHA
extensions) is tens of milliseconds -- comparable to, not larger than, the 650 ECDSA
verifications beside it. Bounding it: the deviation from pure linearity at N=650 is inside the
noise, so the per-byte term is **under 0.4 ns/byte/input**, i.e. at most about a quarter of the
cost of the largest legal transaction.

**Worst legal transaction: ~100 ms.** 104.3 and 102.6 ms across the two runs at 650 inputs and
96 kB. Not seconds. The 100 kB cap is doing its job.

**What this does and does not settle.**

- **BIP143-style sighash buys nothing at legal sizes** and stays deferred. There is no
  throughput argument for it, and the DoS argument is bounded by the cap plus a per-transaction
  work rule (§1A of the design).
- **It does NOT measure the adversarial shape, and that is where the hazard actually lives.**
  These are P2PKH inputs: one sigop, one signature, one sighash each. A `CHECKMULTISIG` input
  can trigger up to ~15-20 sighash computations of the whole transaction, because the legacy
  interpreter has no `BASE` sighash cache and re-hashes per key tried. So the quantity to bound
  is **sigops x size, not inputs x size**, which is what §1A proposes -- and the constant in
  that rule cannot be set until a multisig arm is measured. **Next arm of this bench.**
- **A useful figure for the acceptance layer, by arithmetic from the measured constant.** A full
  2 MB commitment block of ordinary 2-in/2-out payments commits to 62,500 transactions =
  125,000 inputs, i.e. roughly **19.5 s of single-thread validation, ~2 s across twelve cores**,
  when the bodies were never seen at acceptance and miss the script cache. At the 8 MB point it
  is 78 s and ~7-10 s. That is the cold-connect budget the design has to live inside, and it is
  the cost the script cache exists to avoid paying.

**Caveats.** One box, and one with SHA extensions -- a node without them pays more for the
hashing half, which is the half this says is small. Timings are `sendrawtransaction` round
trips, so they include RPC, parse and mempool bookkeeping amortised per transaction, which is
why the N=1 row reads 900 us/input and should be ignored. Regtest with zero assets, so no
per-ATMP asset-cache copy is in these numbers (on mainnet that is 2.0-3.2 ms per call). Two
runs, reps 2 and 5. The bench node is left running at `/data/rtm-bench` on bowser for the
multisig arm.

### 2026-09-18 — mainnet spork state, and ChainLocks have not formed in ~14 months

First look at live mainnet configuration rather than compiled defaults, on a production node
(RPC as the `coins` user; the cookie is 0600 so this needed the operator). `IsSporkActive` is
`value < GetAdjustedTime()` (`spork.cpp:210-220`), so a past timestamp means ON and 4070908800
(year 2099) means OFF.

| spork | value | state |
|---|---|---|
| `SPORK_2_INSTANTSEND_ENABLED` | 1731085882 (2024-11-08) | **ON** |
| `SPORK_3_INSTANTSEND_BLOCK_FILTERING` | 4070908800 | **OFF** |
| `SPORK_17_QUORUM_DKG_ENABLED` | 1669826638 (2022-11-30) | ON |
| `SPORK_19_CHAINLOCKS_ENABLED` | 1715612303 (2024-05-13) | **ON** |
| `SPORK_21_LOW_LLMQ_PARAMS` | 4070908800 | OFF |
| `SPORK_22_SPECIAL_TX_FEE` | 25700 | value-carrying, set |
| `SPORK_23_QUORUM_ALL_CONNECTED` | 1665887472 | ON |
| `SPORK_25_QUORUM_POSE` | 4070908800 | **OFF** |
| `SPORK_9_SUPERBLOCKS_ENABLED` | 4070908800 | OFF |

**What this settles about four costs the decoupling plan had priced.**

- **The ChainLock safety walk is skipped.** It is gated on `IsInstantSendEnabled() &&
  RejectConflictingBlocks()` (`quorums_chainlocks.cpp:304`), and `RejectConflictingBlocks()`
  requires spork 3 (`quorums_instantsend.cpp:1703-1711`), which is off. So the requirement that
  every ChainLock-signing smartnode hold every transaction in the last six blocks is **inert on
  mainnet as configured**. A member signs its own connected tip regardless -- which still means
  it held that block's bodies, so §3's fallback argument is unaffected.
- **There is no ten-minute mining gate.** `IsTxSafeForMining` returns true immediately when
  `RejectConflictingBlocks()` is false (`quorums_chainlocks.cpp:465-468`). The 459 MB
  `maxmempool` figure derived from holding 312,000 transactions for 600 s is therefore not a
  current constraint; burst arrival volume is the only driver.
- **InstantSend is enabled but signs nothing in the mempool.**
  `IsInstantSendMempoolSigningEnabled()` is `GetSporkValue(SPORK_2) == 0`
  (`quorums_instantsend.cpp:1699-1701`) and the value is a timestamp, not zero. So islocks arise
  only through the retroactive path at block connect. The tip's coinbase reads
  `instantlock: false`. Per-node islock BLS verification (>=1.15 ms per message, 0.6 of a core
  at 520 tx/s) is therefore **latent rather than current** -- one spork value away.
- **And that is why it cannot simply be switched on.** Enabling mempool signing at the design
  point would need roughly 27x the measured signing capacity (§16.5).

**The finding nobody was looking for: ChainLocks are not forming.**

| | |
|---|---|
| `getbestchainlock` | height **1,122,354**, valid signature, `known_block: true` |
| `getbestblockhash` | height **1,432,200** |
| gap | **309,846 blocks**, about 430 days at a 2-minute target |
| tip block | `"chainlock": false` |

Spork 19 has been on since 2024-05-13, so ChainLocks were presumably forming and then stopped
around July 2025. RTM's own contract paper sells 51%-attack immunity on this mechanism
("an attacker would need to control over 60% of the active Smartnodes"), and it is not
currently operative.

**Caveat: one node, one observation.** `getbestchainlock` reports what that node knows, and a
node that missed the messages would look the same. The tip's `chainlock: false` corroborates it.
A second production host would make it solid.

**Hypotheses, cheapest explanation first, none verified.**

1. **`SPORK_25_QUORUM_POSE` is off**, so non-participating smartnodes are never punished or
   rotated out. Quorums can accumulate members that never sign, and a 60% threshold then becomes
   unreachable without anything logging an error.
2. **A quorum-params transition.** `UpdateLLMQParams` moves the ChainLock type from
   `llmq400_60` to `llmq200_60` once `QUORUMS_200_8` activates (`chainparams.cpp:1104-1109`), and
   that activation is counted by `NodeRoundVoting`'s 720-block body scan -- the same scan the
   plan removes in 3.2.
3. **DKG failure.** Spork 17 being on does not prove sessions complete.

`quorum list`, `smartnode count` and `quorum dkgstatus` would separate these: live quorums of
the ChainLock type point at signing, an empty set points at formation and moves hypothesis 1 to
the front.

**What it changes for decoupling.** The manufactured-quorum-split hazard (§3A.7, C2) is about
making a condition schedulable in a mechanism that is currently not running at all, which lowers
its urgency and raises the prior that the LLMQ layer needs work before decoupling adds load to
it. The DIP8 signing-attempts process (plan item 3.1) keeps its place, but its justification
should be re-read once the stall is diagnosed: it may be the fix, or it may be unrelated.

> **SCOPE CORRECTION (owner, 2026-09-18), applying to the entry above.** The spork readings stand
> as measured. The *conclusions* drawn from them do not: mainnet's configuration is **deliberate
> and temporary**. InstantSend is switched off on purpose and **will be switched on**; the quorum
> layer is broken and **will be fixed before decoupling**. Neither is this project's scope, and
> nothing may be descoped because it is currently unobservable.
>
> So read the entry as "why these costs cannot be measured on mainnet today", not as "these costs
> are not real". Under the designed configuration — sporks 2, 3 and 19 on, mempool signing
> enabled, healthy quorums — the safety walk runs (so every ChainLock-signing smartnode must
> converge), the ten-minute mining gate applies (so `maxmempool` is gate-sized), and every full
> node verifies every islock. The ChainLock stall is recorded here for one reason only: it is why
> 0.4's swarm bring-up, rather than mainnet observation, is the route to quorum measurements.
> **Do not open a diagnosis of it.**

### 2026-09-18 — multisig validation cost, and the sigop counter that charges nothing for it

The CHECKMULTISIG arm `bench_inputs.py` could not reach (`bench_sigops.py`, same isolated
regtest node on bowser). The legacy interpreter has no `BASE` sighash cache, so `CheckSig`
recomputes the whole-transaction sighash once per public key **tried**. Two encodings of the same
1-of-15, because the node's accounting treats them completely differently.

**A detail that inverted the first run.** `OP_CHECKMULTISIG` reads pubkeys from the top of the
stack downwards -- `ikey` starts at `stacktop(-2)`, the *last* key the script pushed
(`interpreter.cpp`, the `while (fSuccess && nSigsCount > 0)` loop) -- so keys are tried in
**reverse script order**. Signing with the last key matches on the first attempt. The first run
did exactly that and reported P2SH 1-of-15 as *cheaper per input than P2PKH*, which is what
exposed the error. Signing with `KEYS[0]` forces all fifteen attempts; both cases are reported
below because they cost the same in counted sigops and 16x different in reality.

**P2SH 1-of-15** -- `GetP2SHSigOpCount` charges the redeem script's accurate count, 15 per input.

| inputs | bytes | first key matches | all 15 tried | us/input, all tried | counted sigops | us per counted sigop |
|---|---|---|---|---|---|---|
| 10 | 6,401 | 1.2 ms | 28.0 ms | 2,800 | 152 | 184 |
| 25 | 15,890 | 2.9 ms | 60.7 ms | 2,426 | 377 | 161 |
| 50 | 31,706 | 5.8 ms | 93.3 ms | 1,865 | 752 | 124 |
| 100 | 63,327 | 12.6 ms | 192.0 ms | 1,920 | 1,502 | 128 |
| 150 | 94,952 | 19.7 ms | 298.7 ms | 1,992 | 2,252 | 133 |

**Bare 1-of-15** -- `OP_1 <15 keys> OP_15 OP_CHECKMULTISIG` as the scriptPubKey. The scriptSig is
`OP_0 <sig>`, which holds no sigop opcodes, and the spent scriptPubKey is never examined at spend
time, so `GetTransactionSigOpCount` charges **zero**. The two counted sigops below are this
transaction's own two P2PKH outputs, and they are the same whether it has 25 inputs or 800.

| inputs | bytes | delta_ms | us/input | counted sigops | us per counted sigop |
|---|---|---|---|---|---|
| 25 | 2,937 | 67.4 | 2,695 | 2 | 33,685 |
| 50 | 5,800 | 92.1 | 1,842 | 2 | 46,043 |
| 100 | 11,524 | 190.9 | 1,909 | 2 | 95,462 |
| 200 | 22,992 | 423.2 | 2,116 | 2 | 211,588 |
| 400 | 45,912 | 939.4 | 2,349 | 2 | 469,721 |
| 800 | 91,691 | **2,353.7** | 2,942 | **2** | 1,176,827 |

**Four findings.**

1. **The 15-key multiplier is real and it is ~16x.** P2SH 1-of-15 costs 120 us/input when the
   first key tried matches and 1,900-2,000 us/input when all fifteen are tried. Same bytes, same
   counted sigops.
2. **This is where the quadratic term finally shows.** In the bare sweep, us/input climbs
   1,842 -> 1,909 -> 2,116 -> 2,349 -> **2,942** as the transaction grows from 5.8 kB to 92 kB,
   because each input now hashes 15 x the whole transaction. `bench_inputs.py` could not see this
   on P2PKH, where one sighash per input is swamped by the ECDSA beside it. At the 100 kB ceiling
   the size-dependent term is roughly 40% of the cost.
3. **One transaction can cost 2.35 seconds and be charged 2 sigops.** 800 bare 1-of-15 inputs in
   91,691 bytes. `MaxBlockSigOps` is 40,000, so the counter permits twenty thousand such
   transactions; block size permits about twenty-one, which is **~49 s of validation for a 2 MB
   block charged ~42 sigops of a 40,000 budget**. Bare 15-key multisig is non-standard so it does
   not relay -- a miner can still mine it, and a miner is who fills blocks.
4. **P2SH, by contrast, is bounded.** At 15 counted sigops per input and ~130 us per counted
   sigop, the 40,000 cap holds a maximally P2SH-multisig-loaded 2 MB block to about **5.2 s** of
   validation. The counter works there because `GetP2SHSigOpCount` charges the redeem script.

**What this settles for the block resource budget (§1A of the design).**

The unit cannot be legacy sigops, and cannot be legacy + P2SH either. `GetLegacySigOpCount` sums
the sigops of the outputs a transaction **creates** plus its scriptSigs, so a 650-input P2PKH
transaction counts 2 and an 800-input bare multisig transaction counts 2. `GetP2SHSigOpCount`
adds spend-side cost for P2SH prevouts **only**. Bare multisig spends fall through both.

So the per-transaction rule has to charge **an accurate count over every spent scriptPubKey**, not
just P2SH ones -- which is a view-dependent quantity, checkable in `ConnectBlock` and not from a
body alone, exactly as §1A says of the P2SH term. With that unit, the measured constant is
**~130 us per sigop of work**, plus a size-dependent term worth about 40% more at the 100 kB
ceiling. Today's worst case is ~49 s per 2 MB block; a 2 MB *commitment* block naming 62,500 such
transactions implies **~41 hours** unless the budget bounds it, which is the unbounded-committed-
work hazard in measured form.

**Caveats.** One box with SHA extensions, which flatters the hashing half. The extended bare rows
are reps=1; the three lower rows are reps=2 and agree with them. An earlier attempt at the high
counts died on `too-long-mempool-chain, exceeds descendant size limit` because one block did not
confirm all the funding, so the harness now mines until the mempool is empty before timing -- a
limit on the scaffolding reading as a limit on the subject.

### 2026-09-18 — acceptance-layer probe, phase A: what a missing body does to the shipped node

Plan item 0.1. A test-only flag, `-perfwithholdbody=<hash>` / `-perfwithholdheight=<n>`, fails
`ReadBlockFromDisk`'s `CBlockIndex` overload for chosen blocks. Nineteen call sites reach that one
function -- `ConnectTip`, `DisconnectTip`, `ProcessGetBlockData`, GETBLOCKTXN, the compact-block
announce, `VerifyDB`, `RollbackBlock`, ZMQ, the smartnode list diff and several RPCs -- so the
flag reproduces "commitments held, bodies missing" across every consumer at once, on the shipped
serialization, with no fork and no second format. Local regtest node on mario, 20 blocks.

| path | withheld body does | severity |
|---|---|---|
| **`VerifyDB` at startup** | `ERROR: VerifyDB(): *** ReadBlockFromDisk failed at 10` then **"Corrupted block database detected. Please restart with -reindex"**, and the node **refuses to start** | fatal, and the advice is actively wrong for a windowed node |
| **P2P serving** (`ProcessGetBlockData`) | a fresh peer syncing stopped at **height 9**, the block before the withheld one, and the serving node died: **`Posix Signal: Aborted`** -- the `assert(!"cannot load block from disk")` | fatal, remote-triggered, confirms the security review's A1 |
| **`ConnectTip`** | `*** Failed to read block` / `Failed to connect best block (code 0)` -- `AbortNode` | fatal |
| RPC read paths (`getblock` v0, `getblockstats`) | `error code: -1`, node survives | graceful already |

**`DisconnectTip` could not be isolated**, for a reason that is itself a finding: the startup check
window always includes the tip, so withholding the tip's body blocks startup before any disconnect
can be attempted. It needs the phase-B predicate in `VerifyDB` before it can be tested at all.

**Against 0.1's kill criterion: no kill, and the cost signal is small.** None of the four paths
needs an *invariant relaxed*. Each needs the same thing -- consult a have-bodies predicate before
reading:

- `VerifyDB` skips blocks it knows it does not hold, and treats a missing body as corruption only
  when the index says it should be there. That is a new state beside the old one, not a weakened
  check, which is the distinction the kill criterion turns on.
- `ProcessGetBlockData` declines rather than asserts, and a windowed node stops advertising
  `NODE_NETWORK`.
- `ConnectTip` returns "not yet" instead of `AbortNode`.

**The one genuinely open path is `DisconnectTip`.** A node can be *compelled* to disconnect a block
whose body it lacks -- that is §14.2's A2, where ChainLock enforcement moves the tip -- and unlike
the other three there is no obvious "decline" available: the chain has to move. Phase B has to
answer it, and it is the most likely place for the kill criterion to fire.

**Phase A cost:** one flag, 40 lines, one afternoon. **Phase B** -- the two-state split, the
predicate, and the remaining scenarios (restart while incomplete, peer churn, competing tip at
equal work, ChainLock on an incomplete block, reorg across one, `-reindex`) -- is the 3-5 day item.
On today's code those scenarios all terminate in one of the four rows above, which is why they need
the split before they say anything new.

**Trap for whoever runs this next:** `-checkblocks=0` means *all* blocks, not none. Use
`-checkblocks=1`, or the startup verification hits the withheld body and the run measures the
scaffolding.

### 2026-09-18 — recovered from `throughput-bottleneck.md` before archiving it

Back-entry, not a new measurement. `throughput-bottleneck.md` was retired because its headline
conclusion — "the v1 target of 1,500 tx/s is reachable on the current single-threaded design,
with headroom" — was overturned by the twelve-node WAN swarm (F-1: relay delivers ~930 tx/s), and
a live document leading with a dead conclusion is the worst thing in a folder. But five of its
measurements exist nowhere else, and one of them is load-bearing for a deferral in the build
plan. They are recorded here so the archive copy is history rather than the only source.

All five: perf rig, Ryzen 9 3900X 12c/24t, 62 GB, regtest, `checkmempool=0`, `maxmempool=8000`,
3M pre-signed payments, 2026-09-16. Regime `loopback` throughout — one host, peers on loopback.

**1. Eight-node full mesh, distributed origination.** The load-bearing one. Each node fed its
own lineage shard, 12-14 connections each, cap 50,000:

| network-wide | per node | convergence | `msghand` per node |
|---:|---:|---:|---:|
| **1,500** | 187 tx/s | **100%** | **47.4-48.9%** |
| 3,000 | 375 tx/s | **100%** | 75.4-81.2% |
| 4,500 | 562 tx/s | 92% | 95.7-97.7% |
| 6,000 | 750 tx/s | 69% | 96.8-98.1% |

Every node ended holding every transaction (179,528 on all eight) at the v1 rate, with the
critical thread under half a core. Full convergence holds to **~3,000 tx/s network-wide**. Peak
host load was 9.4-11.5 on a 24-thread box, so the limit is the per-node `msghand` thread and not
the test host.

**Why it matters now:** this is the only evidence that the ~930 tx/s measured on the WAN swarm
(F-1) is a property of that topology and hardware rather than of the code — eight nodes on
loopback converge fully at twice that rate. The build plan defers the relay structural work on
exactly that reasoning, so deleting this measurement would leave the deferral unsupported.

**2. `dbcache` and `maxsigcachesize` do not move the acceptance ceiling.** Swept at saturation:

| case | sustained ingestion | `msghand` |
|---|---:|---:|
| defaults | **4,565 tx/s** | 89.4% |
| `-dbcache=4000` (13×) | 4,538 | 89.1% |
| `-maxsigcachesize=512` (16×) | 4,361 | 89.6% |
| both | 4,473 | 89.8% |

Within ±2.3%, baseline highest. The node log confirmed the setting applied, so this is not a
silently-ignored flag. The ceiling is real compute: the corpus is 3M *unique* transactions, so
every signature is verified exactly once and a bigger cache has nothing to re-hit.

**3. The socket busy-loop fix is not a throughput fix.** Counterfactual binary with both halves
of #480 reverted:

| | `rtm-net` | `msghand` | ingested | relayed |
|---|---:|---:|---:|---:|
| with #480 | **12.2%** | 91.5% | 636,480 | 186,768 |
| without | **83.6%** | 90.6% | 608,720 | 194,805 |

A whole core burned spinning, throughput unchanged within noise — the buggy build relayed
marginally more. The spin is on `rtm-net` while the bottleneck is `msghand`, and this host has
spare cores. This is the measurement that made #480's body an overclaim (R-series, corrected on
the PR 2026-09-18); it would matter on a core-constrained smartnode, where the spinning thread
contends with `msghand`.

**4. Why per-peer delivery is `accepted / N`, not a shared budget divided N ways.** The message
handler takes one message per peer per pass ("Just take one message", `ProcessMessages`), and the
requesting peer issues **single-entry getdata messages** — measured 40,555 one-entry getdatas
against 1 batched. So 1,405 / 1,205 / 698 tx/s at 1/4/8 peers is `accepted/N`, and above the cap
the limiter is the handler loop rather than relay capacity. Supports F-6.

**5. The stock cap is fully explained, to the byte.** `InvBroadcastMax()` = 140 × 2 = **280**
entries per trickle on a Poisson timer averaging 5 s → 56 tx/s per inbound peer (K-5, K-6). Every
inv payload on the link measured exactly **10,083 B = 3 + 280 × 36**. The 47-65 spread across
runs is Poisson trickle-count noise, ~±18% at one sigma over 150 s, not a second mechanism. This
is the mechanism behind F-4, and the reason the 74.7 figure is not used.

**And the four errors of that document's first pass**, which README's second standing warning
points at and which are worth keeping as a checklist: relay capacity measured while ingestion was
saturating the same thread; rates computed as `final_mempool / 120` when acceptance ran 135-175 s;
no per-peer time series, so concurrent relay could not be separated from the post-offer drain; and
an unbounded-backlog artefact from offering ~4× what could be relayed, which made a per-trickle
`make_heap` over the whole backlog look like a peer-count effect.

### 2026-09-18 — acceptance-layer probe, phase B: the third outcome, and what the harness cannot reach

> **CORRECTED — see the 18 Sep correction entry at the end of this log.** This entry's "no kill"
> was claimed for the read-time model, in which `BLOCK_HAVE_DATA` stays set on a block whose body
> cannot be read. That *is* a relaxation of `ConnectTip`'s guarantee that a selected block is
> readable — the criterion's second named invariant — not an addition beside it. The accept-time
> model that replaced it keeps the guarantee by construction, and makes the three gates this entry
> describes unreachable.

Plan item 0.1 phase B. A `HaveBodies(pindex)` predicate plus the three paths that were fatal in
phase A, and one finding that cost a rebuild to see.

**The three fatal paths are fixed, and two of them had a precedent to follow.**

| path | phase A | now |
|---|---|---|
| `VerifyDB` at startup | "Corrupted block database detected. Please restart with -reindex", refuses to start | logs `block verification stopping at height 10 (bodies not held)` and **starts**, tip intact |
| P2P serving | peer stopped at the block before, serving node died with `Posix Signal: Aborted` | peer still stops at that block; **the serving node survives** |
| `ConnectTip` | `AbortNode`: "Failed to read block" | holds the block incomplete, leaves validity untouched |

The precedent matters for 0.1's kill criterion. `VerifyDB` already stops when pruning has removed
data, and the serving path is already gated on availability with the comment "Pruned nodes may
have deleted the block, so check whether it's available before trying to send." Both are the same
shape as "bodies legitimately absent", so both were extensions of an existing sanctioned state
rather than the relaxation of a check. **No kill.**

**The finding: "not yet" had to be carried, not inferred.** Returning false from `ConnectTip` with
the state left valid lands in `ActivateBestChainStep`'s non-consensus branch, which neither calls
`InvalidChainFound` nor aborts -- so at runtime it already means "keep the tip you can validate",
exactly what §2.3 requires. At **startup** it is fatal: `ThreadImport` ends with

```cpp
if (!chainstate->ActivateBestChain(state, chainparams, nullptr)) { ...; StartShutdown(); return; }
```

and the node duly logged `holding it incomplete`, then `Failed to connect best block ( (code 0))`,
then exited. An empty state is indistinguishable from a disk failure, so the reason has to travel
with it. Added as `CValidationState::BodiesMissing()`, on the pattern `corruptionPossible` already
uses -- a non-consensus reason carried so callers can tell one kind of "no" from another -- with
`ThreadImport` starting on the validated tip instead of shutting down. **Four call sites: set it,
propagate it, tolerate it at init, and log it.** That is the cost signal the criterion anticipated,
and it is small.

**What the harness cannot reach, which is the more useful result.** Withholding at *read* time
cannot produce the state the remaining scenarios need. To exercise `ConnectTip` you need a block
that is valid, **unconnected**, on the best header chain, and body-less -- and on a single node
every route into that state goes through a *disconnect* across the same gap, which needs the very
bodies being withheld. Re-running S3 quietly proved it: the node started at height 20 with nothing
to connect, and the "holding it incomplete" line in the log was from the previous run.

So the probe's remaining scenarios -- competing tip at equal work, reorg across an incomplete
block, a ChainLock for one, `-reindex` -- need the **arrival** case: a node that receives a block
it cannot fill. Two consequences for the harness:

1. **Withhold at acceptance, not at read.** The flag must make a node refuse to store bodies for a
   chosen block, so the state arises the way it will in production rather than by erasing
   something already held.
2. **Two nodes, not one.** The interesting states are all about a block arriving from a peer.

`DisconnectTip` remains the open path and is now doubly interesting: it is both the scenario most
likely to fire the kill criterion and the reason the single-node scenarios are unreachable.

### 2026-09-18 — acceptance-layer probe, phase B complete: the state already exists, for pruning

> **CORRECTED — see the 18 Sep correction entry at the end of this log.** Three claims here are
> wider than the evidence: the withheld block was **fully body-validated** before being withheld,
> so this is the storage split and not the acceptance split; four of the six named scenarios were
> not run; and "no consensus check was relaxed" answers a question the kill criterion does not
> ask. The measurements stand; the verdict is restated in the correction.

Plan item 0.1, phase B. Read-time withholding could not reach the interesting states, so the
withhold flag now acts at **acceptance**: the chosen block is accepted with its commitments and
without its bodies, which is how a decoupled node will actually meet one. Two nodes, regtest,
`-checkblockindex=1` (regtest's default, so every index invariant is checked on every accept).

**The finding that matters: `ReceivedBlockTransactions` is the acceptance layer, in one function.**
It does five things at once, and they part cleanly along the commitment/body line:

| what it records | level | knowable from commitments alone? |
|---|---|---|
| `nTx` — transactions this block commits to | commitment | **yes** — the commitment list has them |
| `nChainTx` — cumulative count, via the descendant walk | commitment | **yes** — a count, not a possession |
| `nSequenceId`, entry into `setBlockIndexCandidates` | commitment | **yes** |
| `nFile` / `nDataPos` — where the bodies are | body | no |
| `BLOCK_HAVE_DATA` — the claim that we hold them | body | no |

`HaveTxsDownloaded()` reads `nChainTx`, so its name lies: it is a count, not a possession, and it
is satisfied by a commitment-only block. Which means candidate eligibility, the `assert` at the
head of `FindMostWorkChain`'s walk, and `LoadBlockIndex`'s re-linking all work on a commitment-only
block **unchanged**.

**And the hold-and-retry semantics §2.3 asks for are already implemented — for pruning.**
`FindMostWorkChain` already treats absent data as a third outcome beside valid and invalid: it
declines the chain without condemning it, and re-arms it through `m_blocks_unlinked` with the
comment "if the block arrives in the future we can try adding to setBlockIndexCandidates again."
Nothing in it needed changing.

**What it cost.** One function split, and **two** assertion relaxations — both of which pruning
already has, and both in `CheckBlockIndex`:

| invariant | what fired it | fix |
|---|---|---|
| `!(nStatus & BLOCK_HAVE_DATA) == (nTx == 0)`, guarded `if (!fHavePruned)` | the withheld block itself | add the new flag to the guard; the `else` branch's one-way implication `HAVE_DATA ⟹ nTx > 0` is already the right rule |
| `assert(fHavePruned); // We must have pruned.` | the **next** block: it holds data, every parent was received, but a parent's body is absent | `assert(fHavePruned \|\| fHaveCommitmentOnly)` |

The second is the interesting one — it says in as many words that only pruning can leave a body gap
under a block you hold. Decoupling leaves the same gap. The flag is re-derived at load from the
index itself (an entry with `nTx > 0` and no `BLOCK_HAVE_DATA`) rather than persisted, because the
relaxation has to be in place before the run's first `CheckBlockIndex`, which happens during
startup.

**Scenario results.** All four reachable ones pass:

| scenario | result |
|---|---|
| commitment-only block arrives | accepted; tip holds at the parent; the branch above reads `valid-headers`, **not** invalid; node alive under full consistency checks |
| blocks above the gap | bodies held at 14, 16 and 20 with 15 absent — a real gap, and `"pruned": false` |
| restart while incomplete | starts, re-derives the flag, comes up on the validated tip, alive |
| withholding removed | re-arms through `m_blocks_unlinked` and converges; one active tip |
| RPC for the withheld block | `getblockheader` answers with the commitment count; `getblock` declines via `IsBlockPruned` |

**`DisconnectTip` across a body gap cannot arise in the bodies-required path**, which retires the
scenario flagged as the likeliest to fire the kill criterion. Connecting requires the bodies, so
the tip can never be above a block whose bodies are absent: the gap is always above the tip. It
becomes reachable only in the **dual-validation** path, where a commitment block is connected on
the strength of a state root — and reversing it needs the bodies back, because `DisconnectBlock`
must remove the outputs the transactions created and undo data alone does not carry them. That is a
cost dual validation had not been charged. Separately, `DisconnectTip` reads the block *before*
mutating anything and returns a plain `error()` rather than aborting, so a failed disconnect leaves
the chainstate untouched.

**Verdict against 0.1's written kill criterion: no kill, and the estimate should come down.**
Nothing in `CheckBlock`, `ContextualCheckBlock` or `ConnectBlock` was touched; no consensus rule
was relaxed. Both relaxed assertions are index-bookkeeping invariants inside `CheckBlockIndex`,
which is debug-gated, and both were relaxed in the direction pruning already established.

**What this probe does not show.** It reaches "hold a commitment-only block without connecting it"
and no further. There is no body store, no fetch scheduler, and no separate `BLOCK_HAVE_BODIES`
bit. The absence of that bit is visible in the log: the withheld block was re-requested twice and
then dropped, because with only `BLOCK_HAVE_DATA` the download logic cannot tell "need commitments"
from "need bodies". That is the design's bit-256 decision confirmed from the other direction, and
it is the next increment.

### 2026-09-18 — correction: what 0.1 actually established, after adversarial review

An independent review of the phase B diff, briefed to attack the five claims rather than confirm
them, found that three of them were wider than the evidence. Every finding below was verified
against source before being accepted. The measurements in the two entries above stand; the verdict
does not, as written.

**1. The probe tested the storage split, not the acceptance split.** `AcceptBlock` runs `CheckBlock`
and `ContextualCheckBlock` on the full body at `validation.cpp:AcceptBlock`, and the withholding
branch is at the withholding branch below it. So the block that was accepted "commitment-only" had already passed every
body-level check and earned `BLOCK_VALID_TRANSACTIONS` **honestly, from a body it held at that
moment**. The design says this is exactly what production cannot do (§2.1: "the level cannot
honestly be granted at accept time, so the validity ladder gains a rung"). The probe demonstrated
an index and disk state; it did not demonstrate accepting a block whose transactions were never
seen.

This matters because of what happens next. Under the design's real rung there are two encodings
and the probe tested neither:

| encoding | consequence |
|---|---|
| `nTx` set from the commitment list, validity at the **new** rung | `validation.cpp:CheckBlockIndex` — `assert((VALID_MASK >= VALID_TRANSACTIONS) == (nTx > 0)); // This is pruning-independent` — fires. That is the `nTx > 0 ⇒ VALID_TRANSACTIONS` link the kill criterion **names**, and it is a third relaxation on the named chain |
| `nTx` left at 0 until bodies arrive | commitment-only becomes indistinguishable from header-only for chain selection; none of the probe's relaxations are needed; `nChainTx` does not count commitments, and the "count, not possession" property is never exercised |

**2. The verdict, restated honestly.** Still not a kill — but "no consensus check was relaxed" and
"the assertions are debug-gated" both answer questions the criterion does not ask. It asks whether
a scenario requires *relaxing* an invariant that catches database corruption — naming
`CheckBlockIndex`'s chain and `ConnectTip`'s guarantee — *rather than adding a state alongside it*.
`CheckBlockIndex` is debug-gated (`chainparams.cpp` enables it by default only for regtest), but
the criterion named it knowing that. The real distinction is the one the criterion draws:

- A **global flag** that turns the `nTx`/`HAVE_DATA` equivalence off for every block for the life
  of the datadir — what the probe did — **is a relaxation**. A lost status bit or an unwritten
  block file now reads as commitment-only, which is the corruption class the invariant exists to
  catch.
- A **per-block bit**, making the assertion `!HAVE_DATA == (nTx == 0 || COMMITMENT_ONLY)`, is
  **a state alongside** — which the criterion explicitly permits.

So the honest verdict is: **one bit plus one rung is a cost signal, not a kill.** That is the
criterion's own escape hatch, and it is the design's existing bit-256 plan. The probe's global flag
was a shortcut, and should not be read as the shape of the fix.

**3. Scenario accounting, which the completion entry did not give.** Six were named. What ran:

| scenario | status |
|---|---|
| restart while incomplete | **run** — comes up on the validated tip |
| reorg across an incomplete block | **not run.** F-29 retired a *different* case — an incomplete block *below* the tip. The named one is reachable: node on chain A, competing chain B contains a commitment-only block, B's bodies arrive, disconnect A and connect B |
| peer churn mid-fetch | not run |
| competing tip at equal work | not run — and see finding 6, which makes it a behavioural change rather than a neutral case |
| ChainLock on an incomplete block | not run |
| `-reindex` | not run |

Also run, unnamed: arrival, a body gap under held blocks, two gaps in one chain, re-arm on arrival,
and RPC behaviour. `build-plan.md`'s "four scenarios pass" is against a subset, and the criterion's
pass condition is "all scenarios reaching …".

**4. The read-time gates are artefacts of the read-time model.** Under accept-time withholding
`BLOCK_HAVE_DATA` is never set, so `FindMostWorkChain` never selects the block, so `ConnectTip`'s
`HaveBodies` check is unreachable, `SetBodiesMissing` is never called, and `ThreadImport`'s new
branch is never taken. **Restart passed because chain selection declined the chain, not because of
that plumbing** — the earlier entry credited the wrong mechanism. And in the read-time model those
gates were live *precisely because* `HAVE_DATA` was set on unreadable blocks, which is the
criterion's second named invariant being relaxed rather than extended. The accept-time model keeps
that guarantee by construction, and the design must keep it the same way: **whatever bit means
"bodies held" must be the bit `FindMostWorkChain` tests.**

**5. "Chain selection needed no changes" is encoding-dependent.** It holds only because the probe
made `BLOCK_HAVE_DATA` mean "bodies held". Under the design's separate bit, `FindMostWorkChain`
must test the new bit — a change, however small. The pruning machinery is still the right
precedent; it is not a free ride.

**6. One behavioural change in the diff, and it is not from the relaxations.**
`ReceivedBlockTransactions` assigns `nSequenceId` inside its descendant walk, and
`CBlockIndexWorkComparator` breaks **equal-work ties by lower `nSequenceId`** (`validation.cpp:CBlockIndexWorkComparator::operator`,
"earliest time received"). Calling it a second time when bodies arrive re-stamps the block and its
whole descendant subtree, so a branch whose commitments arrived first but bodies second loses a tie
it would have won on an unmodified node; `PreciousBlock`'s deliberately negative id is overwritten
too. This is the "competing tip at equal work" scenario, named twice and never run.

**7. `nTx != 0` is read as "we had this block and pruned it" at two sites the sweep missed.**
`validation.cpp:AcceptBlock` drops an unrequested block, and `net_processing.cpp:FindNextBlocksToDownload` treats a compact
block for such an entry as already-had, falling back to `getdata` only when it was already in
flight. So a body arriving by compact-block relay — the mainline path — or unsolicited is
discarded. The probe's re-arm went through the requested path only.

**8. And the same property that makes candidacy work is what strands the fetch.**
`net_processing.cpp:FindNextBlocksToDownload` advances `pindexLastCommonBlock` past any block with `HAVE_DATA` whose
`HaveTxsDownloaded()` is true. Blocks above a commitment-only block satisfy both — *because*
`nChainTx` counted the commitment-only block's commitments — so the download cursor jumps over the
gap and never returns to it. That is the observed "re-requested twice, then dropped". The "count,
not possession" property is therefore not the free win it was written up as: it makes candidacy
work and it makes the fetch skip, and 1.3 owes a fix at line 804.

**9. Two further states the probe never reached**, both predicted from source and neither
constructible in a single-peer, in-order harness:

- **Out-of-order body arrival.** `ReceivedBlockTransactions` inserts into `m_blocks_unlinked`
  whenever the parent is not `HaveTxsDownloaded()`, regardless of whether bodies were held. A
  commitment-only block whose parent's commitments have not arrived lands there with no
  `HAVE_DATA`, and `validation.cpp:CheckBlockIndex` — `if (!(HAVE_DATA)) assert(!foundInUnlinked)` —
  fires. `LoadBlockIndex` reproduces the same insert at startup. The design fetches from many peers
  with several blocks in flight, so this is the *normal* case, not an edge one. Settling it needs a
  two-peer harness where one peer stalls.
- **ChainLock enforcement across a gap.** `EnforceBestChainLock` runs every scheduler tick and
  `assert(false)`s if `MarkConflictingBlock` fails. With a commitment-only block on the chainlocked
  chain the tip sits at the fork, and each tick re-inserts the block and its descendants as
  candidates (they pass `IsValid(TRANSACTIONS) && HaveTxsDownloaded()`) for `FindMostWorkChain` to
  evict again — a rebuild/evict loop for as long as the bodies are missing. Not a wedge; a cost
  1.3's retry schedule has to absorb.

**10. Two fixes applied on the strength of this review.** The load-time derivation was dead code:
`fHavePruned` is read from the database *after* `LoadBlockIndex` returns, so inside that loop it is
always false and a pruned node would have set the new flag on every pruned block. It now runs where
both facts are known. And extending `IsBlockPruned()` was a mistake — `GetBlockChecked`'s own
second branch already answers this state truthfully ("Block not found on disk", with a comment
describing exactly it), so the extension replaced a true message with a false one and no caller
decides anything on the result. Reverted; `getblock` on a withheld block now gives the honest
message. 1.3 should add an `IsCommitmentOnly` predicate rather than overload this one, or a fetch
scheduler that skips `IsBlockPruned` blocks will skip commitment-only ones.

**11. F-29 is conditional, and one sentence of it was wrong.** "Bodies are required to disconnect"
is confirmed: `CTxUndo` is `vector<Coin> vprevout` — spent prevouts only (`undo.h:CTxUndo`) — and
`DisconnectBlock` needs the body to match `vtxundo.size()+1 == vtx.size()`, to run
`UndoSpecialTxsInBlock`, to undo assets, and to spend the created outputs. But "cannot arise"
depends on the design's own §6 promise that the retention window covers full reorg depth, so it
should read **settled given §6**. Two consequences that were not priced: `MIN_BLOCKS_TO_KEEP = 288`
(`validation.h:MIN_BLOCKS_TO_KEEP`) is below §6's 720-block floor, so `-prune` and decoupling conflict as written;
and a reorg deeper than the window is the pruning failure mode today. The claim that a failed
disconnect is graceful was **wrong at the caller**: `ActivateBestChainStep` answers a failed
`DisconnectTip` with `AbortNode` ("we should abort rather than stay on a less work chain"), and
`EnforceBestChainLock` answers a failed `MarkConflictingBlock` with `assert(false)`. The chainstate
is untouched, because the read precedes the mutation; the process is not.

**12. `-reindex` was named and not run, and from source it orphans the bodies above a gap.**
`LoadExternalBlockFile` parks a block whose parent is unknown in a call-local
`mapBlocksUnknownParent` and processes it only if the parent turns up in the same pass. After
`-reindex` a commitment-only block has no file entry, so every body above it is never indexed and
is re-downloaded and written a second time. `-reindex-chainstate` touches only connected blocks and
is fine under bodies-required.

**What 0.1 established, stated to the evidence.** Chain selection can hold a block it cannot
complete, without condemning it, and pick it up when the rest arrives — demonstrated, through one
arrival path, for a block that had been body-validated, under an encoding where `HAVE_DATA` means
bodies held. The pruning machinery is the right precedent and carries most of that half. What is
untouched: the validity rung, the persisted bit, out-of-order arrival, the unrequested and
compact-block arrival paths, `-reindex`, ChainLock enforcement, equal-work ties, and every
retention interaction.

### 2026-09-18 — 0.1c, the rung probe: there is no rung, there is a bit and a fetch

> **CORRECTED — the headline is retracted. See the correction entry below.** "There is no rung"
> rested on `ConnectBlock` re-checking what `BLOCK_VALID_TRANSACTIONS` certifies. It does not:
> its own comment says it does not re-invoke `ContextualCheckBlock`. The bit findings stand; the
> rung question is reopened, and convergence here was restart-driven, not live.

The question 0.1 did not reach: a commitment-only block cannot honestly hold
`BLOCK_VALID_TRANSACTIONS`, so what does it hold? Two encodings were on the table. Reading
`chain.h` killed both and suggested a third.

**Why a rung is the wrong shape.** Validity is an **ordinal in a 3-bit field**, and
`BLOCK_VALID_TREE = 2` and `BLOCK_VALID_TRANSACTIONS = 3` are **adjacent**. A rung between them
renumbers TRANSACTIONS, CHAIN and SCRIPTS — and `nStatus` is persisted (`chain.h` serializes it,
gating `nDataPos` and `nUndoPos` on its bits), so that is a block-index format change requiring a
migration of every entry on disk, to express something that is not ordinal in the first place. Of
what `BLOCK_VALID_TRANSACTIONS` certifies — coinbase present and well formed, no duplicate
identifiers, sigops, size, merkle root, transactions valid — a commitment block proves the
structural half **by itself**, and leaves exactly two body properties, "transactions valid" and
"sigops", which `ConnectBlock` re-checks anyway.

**So: no rung. One bit, `BLOCK_HAVE_BODIES = 256`** — the free bit the design had already
reserved — and the commitment block is **stored like any other block**, so `BLOCK_HAVE_DATA` stays
honest and every invariant written over it keeps its exact meaning.

**Result: zero assertions relaxed.** Both of 0.1's relaxations were reverted and are not needed.
The diff against the pre-probe baseline contains no weakened assertion. What it contains:

| change | kind |
|---|---|
| `pindexFirstMissingBodies`, a sibling of `pindexFirstMissing` | new tracker |
| `HAVE_BODIES ⇒ HAVE_DATA` | **added** assertion |
| on the active chain `⇒ HAVE_BODIES` | **added** assertion |
| two guards gain `&& pindexFirstMissingBodies == nullptr` | narrowed |

The two narrowed guards are the honest part of the ledger. Narrowing a guard does skip the
assertion in new cases — but on a node with no withholding `pindexFirstMissingBodies` is null
exactly when `pindexFirstMissing` is null, because bodies and data are recorded together. **The
invariant keeps full force over every state an unmodified node can reach**, and only states
decoupling creates fall to the new handling. That is the difference between relaxing an invariant
and adding a state alongside it, and it is checkable rather than rhetorical.

**Where the chain actually breaks, and it is not where 0.1 said.** The first run of the bit
encoding aborted, at the *candidacy* end of the criterion's `HAVE_DATA ⇒ nTx > 0 ⇒
VALID_TRANSACTIONS ⇒ candidacy` chain: a block that sorts above the tip, is valid, and has all
parent data "must be in setBlockIndexCandidates" — and chain selection had just removed it for
having no bodies. So **both encodings break the same chain at different links**: 0.1's at
`HAVE_DATA ⇒ nTx`, 0.1c's at `VALID_TRANSACTIONS ⇒ candidacy`. The sibling tracker is what lets the
rule be asked over both reasons a block can be unusable instead of only the pruning one.

**The finding that decides the schedule: the bit alone wedges the node permanently.** With
`BLOCK_HAVE_DATA` honest, every "do I need this block?" answers **yes, I have it** — and there are
four such sites: `validation.cpp:AcceptBlock`'s `fAlreadyHave`, and three in `net_processing.cpp`.
Measured: with the withholding *removed*, the node made **zero** requests for the missing bodies
and sat at the fork forever. Compare 0.1's encoding, which could at least express "I don't have
it" and did re-request. So:

> **The bit and the fetch layer are one deliverable, not two.** Encoding (c) costs no index
> relaxations but does not work at all until "do I have this block" means "and its bodies".

Restoring convergence took a body-aware `fAlreadyHave`, the same at the three download sites, and
one new function — `ReceivedBlockBodies`, which sets the bit and re-arms candidacy by draining
`m_blocks_unlinked`, deliberately **without** re-stamping `nSequenceId`, since first-seen order has
not changed (F-35). With that, the chain runs end to end:

| scenario | result |
|---|---|
| block arrives as commitments | tip holds at the parent, branch above `valid-headers`, node alive under `-checkblockindex=1` |
| restart while incomplete | comes up at the parent; **the bit survives the datadir** |
| bodies arrive | `bodies arrived for …` → converges to the full chain, one active tip |

**Still not run, and named so it is not lost:** out-of-order arrival (F-25j, the normal case for a
many-peer fetch), competing tip at equal work, reorg onto a branch with a gap, ChainLock across a
gap, `-reindex`, and the **migration** this bit implies — a pre-bit datadir loads with
`HAVE_BODIES` clear on every entry, which the new "active chain ⇒ HAVE_BODIES" assertion would fire
on immediately. A one-pass upgrade at first load is the obvious fix and is unwritten.

### 2026-09-18 — correction: 0.1c's headline is retracted, and three defects it hid

Second adversarial review, with reproductions. The bit findings survive. The conclusion that made
them interesting does not.

**Retracted: "no rung is needed".** The argument was that `BLOCK_VALID_TRANSACTIONS` leaves only
two body properties and `ConnectBlock` re-checks them anyway. `ConnectBlock`'s own comment says
otherwise, verbatim: *"We don't currently (re-)invoke ContextualCheckBlock() or
ContextualCheckBlockHeader() here."* And the list came from the `chain.h` enum comment, which omits
`ContextualCheckBlock` entirely. Body-dependent rules enforced **only** at accept time:

| rule | where |
|---|---|
| `nLockTime` finality (`bad-txns-nonfinal`) | `ContextualCheckBlock` — connect checks only BIP68 sequence locks, and says so |
| coinbase must be a CbTx under DIP3 (`bad-cb-type`) | `CheckCbTxMerkleRoots` returns true for a non-CbTx coinbase |
| `bad-txns-type` | `ContextualCheckTransaction` |
| BIP34 height, `bad-txns-oversize` | `ContextualCheckBlock` |

`CheckBlock` *is* re-run at connect — but only when `block.fChecked` is false, so the in-memory
path skips it. So a commitment block holding `VALID_TRANSACTIONS` without its bodies would connect
a block with a non-final transaction or a non-CbTx coinbase that today is rejected. **The rung
question is reopened**, and the reason it was answered wrongly is the same reason 0.1's answer was
wrong: the probe validates a body it holds and then flips a bit, so it cannot test the rung at all.
That limitation has now produced a wrong conclusion twice, which makes it the probe's defining
constraint rather than a caveat.

**"Two added assertions" was one.** `m_chain.Contains ⇒ HAVE_BODIES` is not an invariant. Pruning
removes bodies from blocks that stay connected — and worse, the assertion forbids **the design's
own steady state**, commitments for all of history with bodies for a window. Dropped, and
`PruneOneBlockFile` now clears the bit alongside `BLOCK_HAVE_DATA`, since pruning deletes the file
the bodies live in. Verified on a pruned regtest node at height 1,200: prune to 800, zero
assertions, restart clean.

**The ledger was missing a recovery rule, and that is where silence could have entered.** The two
narrowed guards skip the candidacy and unlinked assertions for a descendant of a body gap — so
such a block was checked by *nothing*. Added: a block we can build on, above a gap, outranking the
tip and not a candidate, must be parked in `m_blocks_unlinked` — the body-gap analogue of the rule
pruning already has.

**Three defects in the probe code, one fatal.**

1. **`ReceivedBlockBodies` dropped a guard its sibling has** and made a block a candidate while its
   parent was still header-only, tripping `FindMostWorkChain`'s `assert(HaveTxsDownloaded())` — a
   **production** assertion, not a debug one. Reproduced by the review: header for the parent,
   commitments for the child, restart, bodies for the child → `Aborted (core dumped)`, under both
   `-checkblockindex=0` and `=1`. This is F-25j, the out-of-order case, and the answer was that the
   node dies. Fixed and the reproduction re-run: no abort in either variant.
2. **A still-withheld re-arrival re-wrote the block** — the withhold branch fell through to
   `SaveBlockToDisk` and `ReceivedBlockTransactions`, giving a second on-disk copy and a fresh
   `nSequenceId` for the block and every descendant, which is precisely what the design took care
   not to do. Fixed by ordering the already-stored case first.
3. **The fetch fix does not converge with a live peer.** The withhold flags are parsed once at
   init, so every "bodies arrived" result in the entry above came from a **restart** — which resets
   every peer's `pindexLastCommonBlock`. Measured by the review with a live peer: exactly two
   requests for the gap block, then flat; a fresh peer bought exactly one more. F-25e2 is
   unfixed — the cursor still advances past the gap because blocks above it have bodies and a
   counted `nChainTx`. **The probe asks each peer once and gives up.**

**And one hypothesis worth running.** Not re-stamping `nSequenceId` was meant to preserve
first-seen order, but it lets a block whose *commitments* arrived early keep an earlier id than an
equal-work rival that arrived whole — so when its bodies land it displaces an already-connected
equal-work tip, which unmodified code never does. A miner could reserve a tie by announcing
commitments early and bodies late. Re-stamping changes the tie one way and not re-stamping changes
it the other; neither is "unchanged", and F-35 named only the first.

**What stands from 0.1c.** The bit costs no relaxed assertions. Chain selection, restart and the
pruned-node case all hold. The bit and the fetch layer are one deliverable — reinforced, since the
fetch as built does not deliver. What does not stand: that the rung can be avoided.

### 2026-09-18 — 0.4 opening: quorum formation is not unattempted, it is already green

The plan's 0.4 row says ProTx registration, DKG and quorum formation were "never attempted". That
is true of the **WAN swarm** and false of the tree. The functional suite already exercises all of
it, and on this branch, with a binary rebuilt to match, **all nine pass**:

| test | duration |
|---|---|
| `feature_dip3_deterministicmns` | 110 s |
| `feature_llmq_chainlocks` | 74 s |
| `feature_llmq_connections` | 65 s |
| `feature_llmq_data_recovery` | 281 s |
| `feature_llmq_dkgerrors` | 103 s |
| `feature_llmq_is_cl_conflicts` | 43 s |
| `feature_llmq_is_retroactive` | 159 s |
| `feature_llmq_simplepose` | 153 s |
| `feature_new_quorum_type_activation` | 11 s |

Quorums form through all six DKG phases and sign: 5 members, 5 contributions, 5 commitments, zero
complaints, zero justifications, a new quorum every DKG interval. So **the DKG mechanism works in
this tree** and 0.4 does not need to establish that.

What this does to 0.4's scope: the swarm is no longer the route to "does it work", it is the route
to **what it costs** — DKG timing across real WAN latency instead of loopback, islock and ChainLock
rates under the corpus load, quorum formation while the relay path is saturated, and the
ChainLock-across-a-body-gap scenario that has no local home. That is a narrower and better-defined
job than the row describes.

Two notes for whoever runs these next. They need **Python 3.11** — `test_framework/mininode.py`
imports `asyncore`, removed in 3.12, and mario's default is 3.13; `~/.pyenv/versions/3.11.9/bin/python3`
works. And rebuild before running: switching branches leaves a binary from the *other* branch in
`src/`, and the first attempt here was about to test probe code against rig-branch source.

### 2026-09-18 — 0.4: quorums, ChainLocks and InstantSend live on the WAN swarm

Plan item 0.4 delivered. Twelve hosts across the US, EU and Asia; ten live smartnodes; a quorum of
eight. What now works, in order of what had to be true first:

| | result |
|---|---|
| quorum formation | `llmq_test` **and** `llmq_test_v17` formed at quorum height 1230 — 8 members, **8/8 contributions**, zero complaints, **8/8 premature commitments**, commitment mined at stage 11 |
| ChainLocks | first lock at height **1249**, a few blocks after the quorum became signing-eligible |
| InstantSend | a transaction islocked **immediately** on submission |

**The measurement 0.4 exists for: a DKG phase needs about 15 seconds over real WAN latency.** Three
earlier attempts gave the contribute phase about *one* second and failed identically, with
`badMembers 7, receivedContributions 1` every time. That is the finding, and it is invisible
locally: on regtest the phase is derived purely from tip height, `SleepBeforePhase` returns
immediately under `MineBlocksOnDemand`, and `VerifyAndComplain` marks every member whose
contributions are still empty as bad **with no second chance**. Contributions do not broadcast
either — `Init` builds `relayMembers` as a ring where member *i* pushes to *i+1* and *i+2*, and each
hop costs INV → GETDATA → QCONTRIB plus a BLS verify, so the far side of an eight-member ring is
four hops. Mining a 30-block window in under a minute cannot work. `dkg-pace.py` mines two blocks
per phase and gates on what the phase is supposed to produce.

**What this does to every earlier swarm figure.** All of them were explicitly upper bounds because
nothing was paying for smartnode work. That caveat can now be retired measurement by measurement:
the swarm carries DKG traffic, ChainLock signing and per-node islock verification for real.

**Five traps, each of which cost real time and all of which are now in the tooling.**

1. **Sporks 2/3/19 on with no InstantSend quorum empties every block.** `miner.cpp` gates on
   `CChainLocksHandler::IsTxSafeForMining`, which skips every non-islocked transaction younger than
   ten minutes. Enabling those sporks to get DKG running is what caused 119 consecutive
   coinbase-only blocks — and then the empty blocks were chased as a separate mystery. Turn 17, 23
   and 25 on; leave 2, 3 and 19 off until a quorum exists, then 19, then 2, then 3.
2. **An all-smartnode mesh restarted together deadlocks permanently.** `net.cpp` refuses inbound
   connections while `fSmartnodeMode && !smartnodeSync.IsSynced()`, and sync needs peers. Every
   node refuses every other node, forever. Needs a **non-smartnode seed** plus **staggered starts**;
   a plain `swarmctl stop && start` kills the whole mesh.
3. **MNAUTH is one-way on links formed at restart.** It is sent once at VERACK and dropped on
   receipt if the receiver is not yet blockchain-synced, with no retry — so a simultaneous restart
   leaves links where only one side is verified, and DKG INVs only go to verified members.
   Disconnecting 18 such links took verified peers per node from a handful to 9-13.
4. **The funder cannot be a smartnode.** `You can not start a smartnode with wallet enabled` is a
   hard refusal, so the wallet lives on a node *outside* the registered set. And anything registered
   but not running as a smartnode still gets selected and contributes nothing — both the wallet node
   and an inbound-unreachable host had to be deregistered by spending their collateral.
5. **A config written to the wrong path reports success.** `smartnode-bringup` wrote
   `$base/raptoreum.conf` where the node reads `$base/data/raptoreum.conf`; `grep` failed, `printf`
   appended, `mv` succeeded, and the stage reported twelve nodes configured while changing nothing.

Plus one tooling defect that masqueraded as a node fault: `swarmctl.sh`'s `cli()` passed `$*`
unquoted to the remote shell, so every JSON argument arrived mangled and `createrawtransaction`
failed with a parse error from the node. Fixed by quoting each argument; `sw` is now the single
cwd-independent entry point, because a missing `conf/use.conf` silently empties the rpcpassword and
every RPC then reports missing credentials as if the node were down.

### 2026-09-18 — InstantSend at the design point: the same load, with and without

Two arms, identical offered load — 300 tx/s total (25 per node from disjoint corpus shards), 15 s
blocks, 20 minutes, ten live smartnodes on the WAN swarm. 300 was chosen to straddle the ~280 tx/s
ceiling a source reading predicted for un-batched InstantSend on `rtm-sigshares`.

| | control: IS off | treatment: sporks 2, 3, 19 on |
|---|---|---|
| offered / accepted | 360,012 / 360,012, **zero rejections** | 360,012 / 360,012, **zero rejections** |
| `rtm-sigshares` CPU | not exercised | **93.7% (use), 97.0% (c3)** — saturated |
| total node CPU | — | 245% / 209% of one core |
| mempool at the end | ~7,000 | **183,273 tx / 239 MB** |
| block fill (15 s blocks) | 4,257-4,582 tx (~285-305 tx/s) | 1 tx early, then 3,699-3,905 (~247-260 tx/s) |
| ChainLocks | n/a | **stopped forming** — a lock existed at height 1249 while idle, none during the run |
| submitters | all exactly 1,200 s | c1 1,286 s, c4 1,299 s, c3 1,219 s — RPC backpressure |

**The prediction was right, and it named the right thread.** `rtm-sigshares` signs, recovers *and*
verifies on one thread, and at 300 tx/s it is pinned at 94-97% on every smartnode measured. That is
**below stage 1's 520 tx/s** target (K-2), on a quorum of eight — and mainnet's is 200 of 400, where
each member carries the same per-transaction work.

**The failure mode is not graceful degradation.** Acceptance and relay are unaffected: every
transaction offered was accepted at both arms, zero rejections. What breaks is everything
downstream of locking:

1. **Locking does not keep up at all** — sampled mempool transactions were unlocked throughout.
2. **Blocks then starve**, because with spork 3 on, `IsTxSafeForMining` refuses any non-islocked
   transaction younger than ten minutes (K-12). The first block after the load started carried
   **one** transaction. Fill only recovers when transactions age past the gate, and then lands
   13-15% below the control.
3. **The backlog is unbounded in practice** — 183,273 transactions and 239 MB after twenty minutes,
   still climbing, against a `maxmempool` of 1500-2000 MB on these hosts. It is bounded only by
   eviction, which is itself a correctness problem for the transactions evicted.
4. **ChainLocks stop.** One formed while the chain was idle; none formed during the run. So the
   feature that turns probabilistic finality into an assertion is the first thing lost under load.

**What this settles.** §17.4's "batching is the gate for every row" is no longer an inference from a
loopback fit (F-18) — it is measured on a real network, and the thread it saturates is identified.
It also removes a possible escape: the retroactive signing path fires **regardless** of the
mempool-signing spork (`quorums_instantsend.cpp`, and its comment says so), so a timestamped spork 2
— today's mainnet configuration — does not avoid this load. Only spork 2 fully off does.

**The question this puts to RTM**, which no measurement here can answer: does 5.2 batched
InstantSend ride the fork, trail it, or does **InstantSend stay off at activation**? The third option
deletes 5.2, the 459 MB mempool sizing (K-11), the per-node verification cost and the ChainLock
convergence requirement in one stroke. The first puts a protocol with no reference implementation
anywhere — batch composition must be agreed across divergent mempools before a threshold signature
can be produced — on the critical path.

**Caveat on the control arm:** 20 of `use`'s transactions were rejected as already-in-chain. Those
are twenty transactions submitted by hand earlier to test whether the corpus was still live; they
fell inside this run's range. Nothing else was rejected in either arm.

### 2026-09-18 — 0.2 opening: the characterisation baseline is not this branch

Three findings before a single assertion was written, all about what "characterise the node" has to
mean here.

**1. The perf rig branch cannot be the baseline.** Against master it carries:

| delta | size |
|---|---|
| `MAX_DIP0001_BLOCK_SIZE` 2,000,000 → **8,000,000** | 1 line, enormous consequence |
| the 0.1 acceptance probe in `validation.cpp` | **165 lines**, 12 references to `HaveBodies` / `g_perf_withhold` |
| `validation.h`, `consensus/validation.h` | 42 and 27 lines |

Characterising here would pin *the rig* as though it were the node (F-46b), and the block-size delta
is not inert — every size, sigop and fill assertion would encode 8 MB. So 0.2 runs against a clean
detached worktree at master, which also makes the rig deltas explicit knobs rather than invisible
background.

**2. A detached worktree is required, not a branch one.** `git worktree add <path> master` refuses
when master is checked out elsewhere — it is, in a separate build tree on this machine. `--detach`
at master's commit is the way, and it avoids disturbing a checkout that belongs to someone else.

**3. The baseline needs the rig's `depends` prefix and its exact `CXXFLAGS`.** A plain `./configure`
dies with "Boost is not available". The working invocation, recovered from the rig's own
`config.log`, points `CONFIG_SITE` and `--prefix` at `depends/x86_64-pc-linux-gnu` and passes
`CXXFLAGS='-g -O2 -include stdexcept -include cstdint -include cstring'` — those forced includes are
what let this codebase compile under modern GCC. The depends tree is prebuilt libraries and is
independent of the source tree, so a second worktree can share it rather than rebuild it.

**What the first test pins.** `test/functional/feature_characterise_accept.py` submits a valid block
with exactly one rule violated, for each row of §2.4, and records the node's own rejection reason.
It is deliberately a characterisation and not a specification: a mutation that is *accepted* is a
finding rather than a test failure, because it means the rule is not enforced where §2.4 assumes it
is. The rows are split as §2.4 splits them — commitment-checkable (the rung can keep it) versus
body-dependent (it has to move to connect time, and three of those have no connect-time home).

**0.2, continued — the baseline took three attempts, and the reason is a finding in itself.**

`master` turned out to be the wrong baseline twice over. It is **v2.0.03.01** against the rig
branch's and `upstream/develop`'s **v2.0.04.01**, so characterising it would pin a superseded
revision. And its functional test framework **cannot run at all**: `test_framework/messages.py` does
`import dash_hash`, while the module actually published is `raptoreum_hash`, so every test dies at
import.

That second problem is not master's alone — `upstream/develop` has it too. This is **not a
discovery**: it is precisely what the nine open PRs fix, and those fixes live on our branch awaiting
review upstream. The only consequence for 0.2 is mechanical.

The baseline that is both upstream-equivalent in consensus and actually runnable is therefore a
composition: **`src/` from `upstream/develop`, `test/` from our branch.** That is what 0.2
characterises against — 2 MB block size, no probe, working harness.

**0.2, first characterisation run — the harness works, and `submitblock` is the wrong door.**

Nine rows of §2.4 submitted as otherwise-valid blocks with exactly one rule broken. Every one is
rejected, so no rule in the set is silently unenforced. But the *reasons* are nearly useless:

| row | node's reason |
|---|---|
| merkle root | `invalid` |
| merkle malleation | `bad-qc-missing` ← harness, not the rule |
| coinbase: founder payment | `invalid` |
| coinbase: DIP3 type | `invalid` |
| a second coinbase | `invalid` |
| `CheckTransaction`: no outputs / negative value / duplicate input | `invalid` |

**`submitblock` collapses almost every block-level failure to `invalid`.** Characterisation needs the
specific reason — pinning "this block fails" is worthless when the point is *which rule* fired and
whether a later change moved it. The p2p path carries the real reason, which is how
`feature_block.py` asserts `bad-txnmrklroot` and friends. That is the next step, and it is a
restructure rather than a tweak.

**Two traps found the hard way, both of which manufacture false findings.**

1. **Regtest's founder payment starts at height 500.** Below it the amount is zero and the coinbase
   has a single output, so a mutation that strips the founder output is a **no-op on an unmutated
   block** — and the node correctly accepts it. The first run reported "founder payment: ACCEPTED",
   which reads as "this consensus rule is not enforced". It was nothing being tested at all. Priming
   past 500 makes the row real, and it then rejects.
2. **A hand-built regtest block inside a DKG mining window must carry the null quorum commitments.**
   `create_coinbase` does not add them, so blocks are rejected `bad-qc-missing` from height 10
   (`dkgInterval` 30, `dkgMiningWindowStart` 10) regardless of the mutation under test — which is
   what the malleation row is still reporting. Either pick heights outside the window or add the
   commitments.

Both are the same class of error: a test that appears to exercise a rule while exercising nothing,
and whose output looks like a finding about the node.

**0.2 — the accept-path characterisation now pins real rules.**

Moving to the p2p path (this tree still has `DEFAULT_ENABLE_BIP61 = true`, so reject messages
carry the reason) turns nine useless `invalid`s into nine specific rules:

| §2.4 row | class | the node's reason |
|---|---|---|
| merkle root | commitment | `bad-txnmrklroot` |
| merkle malleation | commitment | `bad-txns-duplicate` |
| first transaction is a coinbase | commitment | `bad-cb-missing` |
| coinbase: **founder payment** | commitment | `bad-cb-founder-payment-not-found` |
| coinbase: DIP3 type | commitment | `bad-txns-cb-type` |
| a second coinbase | body | `bad-cb-multiple` |
| `CheckTransaction`: no outputs | body | `bad-txns-vout-empty` |
| `CheckTransaction`: negative output | body | `bad-txns-vout-negative` |
| `CheckTransaction`: duplicate input | body | `bad-txns-inputs-duplicate` |

The founder row is the one worth noting: `bad-cb-founder-payment-not-found` confirms from the
outside that `CheckTransaction` enforces it, which is what makes F-45's `fChecked` caching a
consensus hazard rather than a performance note.

**Three harness properties, each of which silently produced a wrong answer first.**

1. **Every row is a DoS-scoring block, so the node disconnects the peer.** Without reconnecting per
   row, row one characterises and the other eight report "Not connected" — a harness that looks
   like it works and measures nothing.
2. **A mutation must reseal the block.** Changing a transaction and rebuilding the root from stale
   transaction hashes leaves a root committing to the *pre-mutation* block, so the node answers
   `bad-txnmrklroot` for a block whose real fault is elsewhere. Six of the nine rows reported the
   merkle root until the transaction hashes were refreshed before the root was rebuilt.
3. **The malleation check needs an even-aligned duplicate.** `ComputeMerkleRoot` compares
   `hashes[pos]` against `hashes[pos+1]` only for **even** `pos`, so `[coinbase, tx, tx]` puts the
   equal pair at positions 1 and 2, where it is never compared — the block passes malleation
   entirely. `[coinbase, a, b, b]` puts it at 2 and 3 and is caught.

That third one sharpens F-43b. The requirement is not "adjacent" but **even-aligned**, which is a
narrower condition than recorded and makes identifier uniqueness a strictly necessary rung rule
rather than a belt-and-braces one: a commitment block could carry a duplicate identifier at an odd
boundary and satisfy every check the merkle tree performs.

**0.2 — the equal-work tie-break, pinned.**

The only behaviour the 0.1 probe was measured to *change* (F-35, F-42), and the one place 1.3 can
alter what a node does without altering what it accepts. Characterised on the baseline:

- Two valid blocks on the same parent, so identical work. **The first seen wins**, and the second
  does not displace it on arrival.
- The loser stays known as a chain tip at **equal height**, with status `valid-headers` — it has
  data but was never connected.
- The ordering lives in `CBlockIndexWorkComparator`: work, then lower `nSequenceId`, then pointer
  address. `nSequenceId` is assigned in `ReceivedBlockTransactions`, which is why it means
  first-seen and why 1.3 touches it.

1.3 threatens this from both directions, which is the reason to pin it rather than reason about it.
**Re-stamping**: `ReceivedBlockTransactions` runs again when bodies arrive and re-assigns
`nSequenceId` to the block and its whole descendant subtree, so a branch that arrived first can lose
a tie it had already won. **Not re-stamping**: a block whose commitments arrived early keeps that
early id, so when its bodies land it can displace an equal-work rival that was already connected —
which unmodified code never does, and which a miner could exploit by announcing commitments early
and bodies late. Neither option is "unchanged"; this test makes the difference visible either way.

One harness note reused from the accept-path work: the test seeks a height whose *next* block is
outside the DKG mining window, because a hand-built block inside it is rejected `bad-qc-missing`
whatever else is true of it (F-63).
