# Code changes before / around decoupling (v1 target ~1,500 tx/s)

> **Revised 2026-09-17 after the twelve-node WAN swarm.** Three things changed:
> the open question at the bottom of this document is **answered**, the Tier 3
> relay item is **no longer sufficient and has been promoted to a precondition**,
> and a new hazard was found that bears directly on the design
> (quadratic sighash, see Tier 0). Read `docs/perf-results.md` from
> "relay is the binding constraint" onward for the measurements.

## Tier 0 — the two facts that reorder everything else

**1. Relay, not acceptance, is the binding constraint.** Measured on twelve nodes
across three continents at 1,500 tx/s offered:

| | |
|---|---|
| acceptance | fine -- msghand 61-72% of one core on a 4-thread VPS |
| relay delivery | **~930 tx/s**, confirmed four independent ways |
| deficit | ~570 tx/s, accumulating as backlog |
| schedule-bound? | **no** -- cutting the trickle interval 5x moved it 1.4% |
| peer-count-bound? | **yes, ~40%** -- 21 connections give 824 tx/s, 5 give 1,221 |
| mempool-size dependent? | **no** -- 864 tx/s empty vs 876 tx/s at 133k entries |

Every downstream symptom follows from this: mempools diverge, compact blocks match
almost nothing, block convergence reaches 35 s against a 15 s interval. None of it
needs an absorption or block-size explanation, and an earlier entry blaming the
absorption gap has been retracted.

**Decoupling does not address this.** A commitment block changes what the *block*
carries; bodies still cross the same relay path at the same rate. Fixing relay is
therefore a **precondition** for decoupling, not something shipped alongside it.

**2. Quadratic sighash is live, and the 100 kB cap is what contains it.**
`SigVersion` has one value, `BASE` -- no SegWit, no BIP143 -- and `SignatureHash`
reserialises the whole transaction per input, so an N-input transaction costs
O(N^2). `MAX_STANDARD_TX_SIZE` (100 kB) is policy, and its own comment says it is
there "to mitigate CPU exhaustion attacks".

| limit | kind | binds |
|---|---|---|
| `MAX_STANDARD_TX_SIZE` 100 kB | **consensus** while DIP0001 is active | ~675 P2PKH inputs, on both paths |
| `MAX_STANDARD_TX_SIGOPS` 4000 | policy, not gated on standardness | mempool path |
| block size 2 MB | consensus | ~20 maximum-size transactions per block |

The 100 kB limit is enforced in `ContextualCheckTransaction` (validation.cpp:419) with
`DoS(100)`/`REJECT_INVALID`, and that function is called from both the mempool path
(line 619) and the block path (line 4054). It is therefore **consensus, not policy** --
an earlier revision of this document said otherwise and was wrong. The limit in
`consensus/tx_check.cpp` is the older `MAX_LEGACY_BLOCK_SIZE` one, which is what misled us.

So quadratic sighash is currently **bounded by consensus**: worst case is ~675 inputs
hashing ~100 kB each, on the order of 67 MB and tens of milliseconds for one transaction,
and ~20 such transactions in a 2 MB block. Unpleasant, not fatal.

**What this means for the proposal to remove the cap.** It is a hard fork, not a relay
policy change, and it removes the only bound on quadratic sighash. Bitcoin's 2015
megatransaction (1 MB, ~5,570 inputs, ~25 s to validate) is what that regime looks like,
and BIP143 is what fixed it. RTM has no SegWit -- `SigVersion` has one value, `BASE` --
so the O(N^2) is live and only the cap holds it down.

**This is most likely what motivates the dual validation path**, and it is worth
being precise about: quorum pre-attestation *concentrates* the cost on quorum
members rather than removing it, and since full block verification is retained,
every node still pays O(N^2) when the block arrives. The fix that actually removes
the cost is BIP143-style sighash -- precompute the hash components once per
transaction and reuse them across inputs, turning O(N^2) into O(N).


Three tiers: what must land *before* decoupling code starts, what is design-independent
foundation that can go before or in parallel, and what is *part of* the decoupling build
itself. Parallel validation and the MemPoolAccept port are parked (v1 = ~1,500 tx/s, within
single-thread capacity).

**3. Decoupling removes the bound that block size currently provides.** Today the block
carries bodies, so 2 MB of block is 2 MB of validation work. A commitment block carries
txids, so what it commits to is not bounded by its own size:

| | block holds | implied body bytes |
|---|---|---|
| today | 2 MB of bodies | 2 MB -- bounded |
| decoupled, 32-byte txids | 62,500 txids | up to 6.25 GB at the 100 kB cap |
| decoupled, cap also removed | 62,500 txids | unbounded |

What makes this survivable is that bodies are validated once at mempool acceptance and
`ConnectBlock` skips re-verification on a `scriptExecutionCache` hit
(validation.cpp:1488; `CheckInputsFromMempoolAndCache` inserts with
`cacheFullScriptStore=true`). **So the CPU story of decoupling rests entirely on that cache
hitting**, and it is a fixed-size CuckooCache that evicts:

| | |
|---|---|
| `-maxsigcachesize` default | 32 MB, half to the script cache = 16 MB |
| capacity | ~524,288 entries |
| at 1500 tx/s | ~350 s of arrivals, i.e. ~2.9 blocks at a 2-minute cadence |
| what `maxmempool=2000` can hold | ~1.5M transactions |

The cache is **smaller than the mempool can be**, so under backlog eviction is certain, and
an evicted transaction is fully re-validated at block connect -- quadratic sighash included.

Three things therefore have to be decided in the design rather than discovered later:

- a bound on committed body bytes (or committed validation work) per block;
- a cache sized for the committed set, which makes `-maxsigcachesize` a consensus-adjacent
  tuning parameter rather than a local one;
- what a node does when a commitment names a transaction it has never seen -- fetch and
  fully validate, with no bound, is the default behaviour today.

None of this is measured yet. It is the first thing the architecture session should price.

## Carried into the design session — state these before quoting any number

**Every throughput figure here is an upper bound.** The swarm runs with no smartnode
features at all: `smartnode count` returns 0, both `llmq_test` quorum sets are empty, and
the node logs contain zero islock and zero chainlock lines. On mainnet each transaction also
drives an InstantSend lock attempt -- signature shares relayed, a recovered signature
relayed, then the islock -- which is a second high-rate stream on the same relay path that
is already the constraint. Nothing here says what that costs.

**Open measurement, not yet usable.** `test/perf/swarm/bench_inputs.py` was written to test
the O(N^2) sighash claim directly and produced nothing usable: every row was rejected, first
on a flat fee below the size-scaled minimum relay fee, then `txn-mempool-conflict`, then
`bad-txns-oversize` past 100 kB. A rejected transaction short-circuits before signature
checking, so the timings measured rejection paths. It needs a size-scaled fee,
non-overlapping UTXOs, and a ceiling of ~675 inputs. **Until it runs, the quadratic claim in
Tier 0 is source-read, not measured.**

## Tier 1 — must precede decoupling (protects the work)

These are not throughput fixes; they are the safety net that makes every later change safe.

1. **Stabilise the functional test suite — DONE (2026-09-16), one documented skip.**
   `feature_llmq_is_cl_conflicts.py` now passes. It was failing on a single case,
   `test_chainlock_overrides_islock(True, True)` (`mine_conflicting`), which asserts that
   IS-locked transactions stay queryable (`confirmations=0`, `height=-1`) after a ChainLock
   overrides them. They do not: the ChainLocked block that double-spends them connects and
   evicts them, so they are legitimately gone by the time the case checks.

   **Not an RTM divergence.** The node code on this path matches Dash line for line —
   `ResolveBlockConflicts` uses `InvalidateBlock` (Dash #4146, which RTM does carry),
   `MarkConflictingBlock` is structurally identical including its
   `UpdateMempoolForReorg(disconnectpool, true)` re-add, and both mark
   `BLOCK_CONFLICT_CHAINLOCK`. The expectation is inherited from Dash #4146 and is unverified
   here; upstream ships the test marked `# NOTE: needs dash_hash to pass`, and before the
   Sep-10 quorum-test repair series these tests could not run at all. Ruled out along the way:
   Python version, `dash_hash`/`raptoreum_hash` (the cpython-311 module is installed and
   imports), and `-txindex` (tested, no effect).

   That one case is commented out with the reasoning inline; the other four still cover
   CL-vs-IS conflicts. **Note:** mario cannot run the functional suite at all (Python 3.13
   removed `asyncore`) — it runs on the rig host under pyenv 3.11.9.

2. **Characterisation tests of the acceptance path.** Pin exactly what the current node
   accepts/rejects across the full surface — normal, multi-input, every asset op, futures,
   every special-tx type, InstantSend interactions, and every *rejection* path. Decoupling
   will touch acceptance (commitment blocks change block handling); these tests catch any
   behavioural drift. Extend the block-connect and relay coverage similarly. No-regret and
   design-independent.

## Tier 2 — design-independent foundation (before or in parallel)

Real code, contained, out of the socket layer. Needed before v1 *performs* at 1,500, not
strictly before decoupling *starts* — can be built alongside early decoupling work.

3. **ChainLocks `Cleanup` rewrite.** Replace the O(mempool) `GetTransaction` walk (under
   `cs_main`+`mempool.cs` every 30 s) with event-driven pruning off the mempool-removal /
   block-connect signals. ~50-150 lines; care needed because pruning timing feeds
   `IsTxSafeForMining` and retroactive signing. Removes a ~0.5 s (at 900k) / 1.9 s (at 3M)
   periodic stall on every node.

4. **`getblocktemplate` base fix — DEFERRED (2026-09-16).** Stop running a full
   `ConnectBlock` on every poll. **Moved out of Tier 2**: the expensive half is the design
   question (what safely replaces the full re-validation), and decoupling *dissolves* it
   rather than re-answering it — a commitment block carries txids whose bodies were already
   validated at acceptance, so re-validating bodies at template time is precisely what the
   design removes. Doing it now means designing against a paradigm about to be replaced.
   The paradigm-independent half (cache the template, invalidate on tip/mempool change)
   survives decoupling and can be lifted later. **Trigger to revisit:** when we start mining
   at sustained high throughput — not "before decoupling".

5. **Assets-cache deep-copy fix — measured & live-confirmed, v1-blocking.** RTM copies the
   whole `CAssetsCache` (`validation.cpp:683`) on *every* ATMP call, scaling O(in-memory
   assets), yet `CheckSpecialTx` never even reads it for a normal payment — pure waste on the
   whole payment path. **Confirmed by an independent live measurement on the mainnet node**
   (differential `sendrawtransaction`, see `docs/asset-cache-drag.md`): the copy path costs
   **2.0–3.2 ms** at the in-memory K≈2,500 → **~300–500 tx/s copy-alone ceiling, ~3–5× below
   the 1,500 v1 target**, on transactions that never touch an asset. The in-memory cache is
   capped at 2,500 at boot (`LoadAssets`) and grows only by assets created/touched since (the
   DB holds 3,439; the sampled node saw ~4 asset ops in 8 days, so operative K≈2,500). Since
   mainnet's DB is already >2,500, every restarted node sits at this floor. The fix is trivial
   and safe: construct the copy only for the three asset tx types, leaving payment throughput
   independent of asset count. **Required** for v1; a few lines, no consensus change.

## Tier 3 — part of the decoupling build (not "before", listed so they are planned)

These only make sense *with* decoupling and ship as part of it.

- **Relay cap re-index — required, and size it generously.** Small formula change
  (`InvBroadcastMax()` reads a throughput target, not `MaxBlockSize()`). The shipped
  block-indexed cap delivers only **50-65 tx/s per peer**, so no target is reachable without
  this, *regardless of threading*. **Measured 2026-09-16:** the arithmetic minimum
  (target x interval = 7,500) is **too tight** — it bounds per-peer relay by schedule and
  peers lagged ~34k; **50,000 was verified to converge** 1,500 tx/s to 8 and to 16 peers with
  lag under one trickle. Size well above the minimum to absorb Poisson jitter in the trickle.
  Still under `MAX_INV_SZ` (50,000) — and the send loop already chunks at that bound
  (`net_processing.cpp:4545/4551/4587`), so message-splitting needs no work at all.

  **Decision (2026-09-16): not a standalone upstream PR.** It is not a bug and not a blocker
  for anything RTM ships today — at ~45 tx/s of block capacity the shipped cap is correctly
  matched, which is exactly what Dash #2300 intended. It only becomes wrong once decoupling
  severs the block-size/propagation link, and its correct value is determined *by* that
  design. Pushing it now would change network-wide relay behaviour on the strength of a
  design upstream has not adopted, and would need revising later. Keep it on the dev build
  via the existing `-perfinvmax` / `-perfinvinterval` flags and ship it with the decoupling
  change. **Measured: cap 10,000 is already sufficient at the v1 operating point** (1,500
  tx/s to 8 peers: 0% withheld, msghand 57%); 7,500 is not (schedule-bound, ~34k lag).
- **`maxmempool`** default → ~2 GB (1,500 tx/s × 10 min ≈ 900k tx ≈ 1.3 GB). Config.
- **`MAX_PROTOCOL_MESSAGE_LENGTH`** raise. A commitment block at 1,500 tx/s is **5.8 MB at a
  2-min block, ~29 MB at a 10-min block** (900k txids × 32 B) — both exceed the 3 MB limit.
  So the decoupled block cannot cross the wire without this. A block-format prerequisite,
  part of the decoupling implementation, not a standalone pre-step.

## Measured as NOT needed for v1

Each of these was on the earlier list as required or primary. All were disproven:

| item | why it is off the list |
|---|---|
| **parallel validation / MemPoolAccept port** | acceptance sustains ~4,400 tx/s against a 1,500 target — 3× headroom. The earlier "primary rate wall" framing was wrong. |
| **relay ordering rewrite** (O(backlog) heap) | `-perfinvnosort` moved throughput **<1%** at the v1 point. The "~40% of msghand" figure was measured at a 300k backlog with cap 50k-100k, a regime v1 never reaches. |
| **`getblocktemplate`** full `ConnectBlock` per poll | deferred: decoupling *dissolves* the hard part (a commitment block's bodies were validated at acceptance). Trigger to revisit is sustained mining at scale. |
| **message-size raises** (`MAX_INV_SZ`, `MAX_PROTOCOL_MESSAGE_LENGTH`) | the send loop already chunks at `MAX_INV_SZ`; no splitting work needed. |
| **`dbcache` / `maxsigcachesize`** | swept at saturation: no effect (13× and 16× raises, application confirmed in the node log). The ceiling is real compute. |

## Parked for v2 (higher throughput)

- Parallel validation + the Dash `MemPoolAccept` port (the big one).
- Relay O(backlog) ordering rewrite (probably fine at 1,500 — confirm once the cap is
  re-indexed; the 40%-of-msghand cost appeared only at high cap + huge backlog + high
  throughput together).
- `MAX_INV_SZ` message-splitting (only needed if the cap target exceeds 50k, i.e. toward 10K).

## The one measurement that could change the plan — ANSWERED 2026-09-17

Single-thread acceptance on representative smartnode hardware. **Measured on a 4-thread
VPS carrying 1,500 tx/s: msghand at 61-72% of one core** (89% when blocks come every 3 s,
because block processing also runs on msghand). Acceptance is not over budget, so
**parallel validation stays parked**.

What is over budget is relay, which was not on this list at all. See Tier 0.

---

### Summary

**Strictly before decoupling:** Tier 1 only — stabilise the suite, write characterisation
tests. Everything else is either design-independent foundation you can build in parallel
(Tier 2: ChainLocks Cleanup, getblocktemplate) or part of the decoupling build (Tier 3: cap
re-index, maxmempool, message-length). The heavy architectural work stays parked.
