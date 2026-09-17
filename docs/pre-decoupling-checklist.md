# Code changes before / around decoupling (v1 target ~1,500 tx/s)

Three tiers: what must land *before* decoupling code starts, what is design-independent
foundation that can go before or in parallel, and what is *part of* the decoupling build
itself. Parallel validation and the MemPoolAccept port are parked (v1 = ~1,500 tx/s, within
single-thread capacity).

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

## Parked for v2 (higher throughput)

- Parallel validation + the Dash `MemPoolAccept` port (the big one).
- Relay O(backlog) ordering rewrite (probably fine at 1,500 — confirm once the cap is
  re-indexed; the 40%-of-msghand cost appeared only at high cap + huge backlog + high
  throughput together).
- `MAX_INV_SZ` message-splitting (only needed if the cap target exceeds 50k, i.e. toward 10K).

## The one measurement that could change the plan

Single-thread acceptance on representative smartnode hardware, cold cache, mixed tx types.
If it sits comfortably above 1,500, the parked items stay parked. If a real smartnode does
~1,200, 1,500 is over budget and parallel validation returns to the v1 path. A day of work,
and it is the only thing that would undo the "park it" decision.

---

### Summary

**Strictly before decoupling:** Tier 1 only — stabilise the suite, write characterisation
tests. Everything else is either design-independent foundation you can build in parallel
(Tier 2: ChainLocks Cleanup, getblocktemplate) or part of the decoupling build (Tier 3: cap
re-index, maxmempool, message-length). The heavy architectural work stays parked.
