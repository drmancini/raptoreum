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
