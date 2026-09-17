# The asset-cache deep-copy drag on mempool acceptance

Every transaction that enters the mempool pays an unconditional deep copy of the
entire in-memory asset cache. The copy scales O(confirmed assets), is used by only
three transaction types, and is **never even read** for an ordinary payment. This
is a per-transaction tax on the whole throughput path that regtest — with zero
assets — has hidden from every acceptance figure measured so far.

## Mechanism (source-verified)

`AcceptToMemoryPoolWorker` (`validation.cpp:683`) runs, for **every** transaction:

```cpp
CAssetsCache assetsCache = *passetsCache.get();
```

`CAssetsCache`'s copy constructor calls the base `CAssets(const CAssets&)`
(`assets.h`), which deep-copies three `std::map`s — `mapAsset`
(name → `CDatabaseAssetData`), `mapAssetId`, and `mapAssetAddressAmount` — plus four
dirty-tracking `std::set`s. `mapAsset` and `mapAssetId` hold one entry per confirmed
asset, and each `mapAsset` value carries several heap-allocated strings (assetId,
name, referenceHash), so the copy is a few heap allocations **per confirmed asset**,
paid on every acceptance.

Two facts make it worse than a fixed cost:

1. **The copy is used by almost nothing.** The only consumer is `CheckSpecialTx`
   (`validation.cpp:793`). That function returns `true` at its first line for any
   `tx.nVersion != 3 || tx.nType == TRANSACTION_NORMAL` (`specialtx.cpp:20`) — i.e.
   every ordinary payment — **without dereferencing the cache at all**. The pointer
   is only touched by `TRANSACTION_NEW_ASSET`, `TRANSACTION_UPDATE_ASSET`, and
   `TRANSACTION_MINT_ASSET`. For the entire payment path the copy is pure waste.

2. **The in-memory cache grows without bound over uptime.** `LoadAssets()`
   (`assetsdb.cpp:100`) caps the cache at `MAX_CACHE_ASSETS_SIZE = 2500` **only at
   startup**. During operation `CAssetsCache::Flush()` (`assets.cpp:460`) merges every
   newly-confirmed asset into the global cache with `passetsCache->mapAsset[id] =
   data` — no eviction, no cap. `DumpCacheToDatabase()` → `ClearDirtyCache()` clears
   only the four sets, never the maps. So a node reloads ≤2,500 assets on restart and
   then climbs from there for as long as it stays up.

## The test

`src/bench/assets_cache_copy.cpp` — a compiled microbenchmark (nanobench, built into
`bench_raptoreum`, `--enable-bench`). It builds a `CAssetsCache` holding *K* assets and
times exactly the copy at `validation.cpp:683`, for *K* from 0 to 100,000, with and
without the `-assetindex` map populated. Entries are conservative — short names and an
**empty** referenceHash — so every figure is a lower bound on mainnet, where
referenceHash (e.g. an IPFS hash) is often set and assets have many holders.

Isolating the one line is deliberate: it removes every confound (wallet, mining,
relay, signature verification) and gives the cost of that copy alone as a clean
function of asset count. The copy is serialized inline on the single `rtm-msghand`
acceptance thread, so it adds directly to the per-transaction acceptance time.

## Results

Two independent measurements agree. The **live-node differential** is the authoritative
one; the microbenchmark corroborates it but is an unstable instrument and is reported
second, with its caveat.

### Live mainnet measurement (authoritative)

Method (independent of the microbenchmark): time `sendrawtransaction` over keep-alive HTTP
on the running mainnet node for two tx shapes — one v2/type-0 tx that spends a random
outpoint (runs to line 683, then fails `HaveCoin`) minus a dust tx (rejected in
`IsStandardTx`, *before* 683). The difference isolates the copy path. 1,600+ calls, nothing
entered the mempool, nothing relayed. On the running v2.0.03.01 node with an in-memory cache
of **K ≈ 2,500**:

> **the copy path costs 2.0 ms (min) / 2.7 (p10) / 2.9–3.5 ms (median) more than the
> pre-copy path** — everything else in ATMP is ~15–40 µs.

That is a **copy-alone acceptance ceiling of ~300–500 tx/s** on a mario-class core. The
~480 tx/s figure quoted earlier is the *optimistic edge* of this range, not an
overstatement. (Caveat: measured on an RPC worker thread's malloc arena, not `msghand`'s;
the arena differs but the copy is identical.)

### Microbenchmark (corroborating; unstable instrument)

`src/bench/assets_cache_copy.cpp`, mario (Xeon 6517P), `-O2`, pinned. **Read this as an
order-of-magnitude check, not a precise number:** the copy time swings ±2.5× with heap
state. nanobench runs cases alphabetically, so `_2500` executes right after `_100000` has
churned ~130 MB of heap and reads ~2× high; run in isolation it reads ~1.15 ms. This is
allocator layout, not page faults (measured minflt = 0). Isolated single-K runs:

| confirmed assets | copy time (isolated) | note |
|---:|---:|---|
| 0        | 25 ns    | empty containers |
| 100      | 29 µs    | |
| 1,000    | ~165 µs  | |
| 2,500    | **~1.15 ms** | ~2.0–2.6 ms when heap-churned (default suite) |
| 3,439    | ~1.83 ms | |
| 10,000   | ~6.1 ms  | |
| 100,000  | ~82 ms   | |

The isolated ~1.15 ms at K=2,500 is what a *defragmented* process would pay; the live node,
with a busier arena, pays 2–3 ms — so the live number is the one to quote. A probe confirmed
the copy performs **exactly 5.0 heap allocations per asset** (strings use the C++11 ABI, no
COW), and is not elided.

`-assetindex` adds `mapAssetAddressAmount` on top (~1.5–2× again). Synthetic entries here
use an empty referenceHash and 5 allocs/asset; **real mainnet assets average ~6 allocs**
(61% carry a 46-char IPFS referenceHash, 19% have long names), so these figures **understate
the live cost by ~20%** — a floor, as intended.

## Why this matters for v1

The v1 plan assumed ~1,500 tx/s single-thread acceptance was safe — a number from
**zero-asset** regtest. The live node says the copy alone caps acceptance at **~300–500
tx/s** at mainnet's asset count, a factor of **~3–5× below** the 1,500 target, before any
other bottleneck, on transactions that never touch an asset. It costs nothing today only
because mainnet load is far below that ceiling — but the ceiling is already there.

**On the asset count (corrected 2026-09-16).** Mainnet's asset *database* holds **3,439**
assets, but that is **not** the number copied per transaction. `LoadAssets` caps the
in-memory cache at **2,500 at startup** (`assetsdb.cpp:117`); after boot it grows only by
assets *created or touched* since — and a block scan of the sampled node's 8-day uptime
found just **2 NEW_ASSET + 1 UPDATE + 1 MINT**. So the operative K on a restarted node is
**~2,500 + a handful**, not "climbing toward 3,439" (an earlier draft said that — it was
wrong; reaching 3,439 needs every asset touched via RPC/wallet). The 2,500 figure is what
matters, and the live measurement above was taken at exactly that K. Any chain with ≥2,500
assets — RTM already qualifies — puts every restarted node at this floor.

## The fix (trivial, safe for the payment path)

Because `CheckSpecialTx` never reads the cache for a normal payment, the copy should not
be made for one. Guard it behind the asset tx types:

```cpp
CAssetsCache assetsCache;                       // empty
const bool isAssetTx = tx.nVersion == 3 &&
    (tx.nType == TRANSACTION_NEW_ASSET ||
     tx.nType == TRANSACTION_UPDATE_ASSET ||
     tx.nType == TRANSACTION_MINT_ASSET);
if (isAssetTx) assetsCache = *passetsCache.get();
```

This preserves current behaviour exactly for asset transactions (they still get their
private working copy) and removes the cost entirely from every other transaction —
making payment throughput **independent of asset count**. It is contained to one
function, changes no consensus rule, and is independent of the MemPoolAccept port.

## Verified: before/after the fix

Two binaries built from this tree, differing only in the guard at `validation.cpp:683`.
Each boots a regtest node loading exactly 2,500 assets (a 2,598-asset template, capped by
`LoadAssets` — the mainnet boot floor). The copy path is isolated by the same differential
as the live measurement: a normal (v2/type-0) tx spending a random outpoint (reaches 683,
then fails `HaveCoin`) minus a dust tx (rejected in `IsStandardTx`, before 683). Node run
with `-acceptnonstdtxn=0` so the standardness gate is active on regtest. N=400.

| normal payment, in-memory K=2,500 | copy-path cost (missing − dust) |
|---|---|
| **old** (unguarded copy) | **1.16 ms** median · 0.94 ms min · 1.21 ms p10 |
| **fixed** (guarded copy) | **0.02 ms** median · ~0 min · 0.04 ms p10 |

The copy is removed from the payment path (`~1.15 ms/tx → ~0`). The old figure here matches
the *clean-process* microbench (~1.15 ms); a fragmented long-running node pays the 2–3 ms
the live mainnet measurement showed — the fix removes it in either regime. Asset
transactions still receive the copy (the guard fires only for the three asset tx types),
confirmed by `feature_assets.py` passing against the fixed binary (create/mint/update/send/
ownership/immutability/survive-restart all green). Harness: `scratchpad/{load_assets,measure}.py`.

A secondary refinement — copying only the specific asset(s) a tx touches, rather than
the whole cache, or capping/evicting the in-memory cache so it stops growing over
uptime — would also help the asset transactions themselves, but the guard above is the
whole win for the payment path and is the one worth shipping first.

## Confirmed under load on a 3,500-asset chain

The before/after above isolates the copy. A saturating end-to-end run on a rig chain carrying
**3,500 confirmed assets** (mainnet holds 3,439) measures what it costs in throughput:

| binary | chain | sustained ingestion |
|---|---|---:|
| unfixed | 3,500 assets | **695 tx/s** |
| fixed | 3,500 assets | **4,460 tx/s** |
| unfixed (control) | 0 assets | 4,502 tx/s |

The same unfixed binary does 4,502 tx/s on a bare chain and 695 on an asset-bearing one, so
the copy costs **6.5x throughput**; the fix restores it fully.

## Follow-ups (deliberately NOT in the guard fix)

The shipped fix (`fix/asset-cache-atmp-copy`) guards the per-ATMP copy behind the three
asset transaction types. That removes the O(assets) cost from the payment path — the whole
throughput story — and is behaviour-preserving, so it carries no asset-path risk. These two
items are what it deliberately leaves on the table, in risk order.

## 1. Bound / evict the global in-memory cache  (lower risk — do this first)

`LoadAssets` caps the cache at `MAX_CACHE_ASSETS_SIZE` (2500) **only at startup**.
`CAssetsCache::Flush()` then merges every newly-confirmed asset into the global cache with
no eviction, and on-demand loads (`GetAssetMetaData`, `GetAssetId`, `CheckIfAssetExists`)
only ever add. So the cache grows unbounded over uptime and is re-capped only by a restart.

After the guard, this no longer affects payments — but it still sets the size of the copy
that **asset** transactions pay, and it is unbounded memory growth in its own right. Adding
a real bound (LRU or a size check in `Flush`) caps that cost without touching any validation
semantics, which is why it is the safer of the two.

## 2. Eliminate the copy for asset transactions too  (higher risk)

The three checkers (`CheckNewAssetTx`, `CheckUpdateAssetTx`, `CheckMintAssetTx`) are
**read-only** on the cache — `GetAssetMetaData`, `GetAssetId`, `CheckIfAssetExists`,
`MakeSignString(cache)` — and every one of those falls back to the LevelDB on a miss. So the
cache is a read-through performance cache, never required for correctness, and in principle
the copy could be replaced by a const view or a one-asset lookup.

Why it was not done here:

- Those "read" methods are **not const** — they lazy-*insert* into the cache on a miss, so
  `const CAssetsCache&` is not a drop-in; they need const-ifying or read-only variants.
- **The correctness trap:** the global cache holds recently-confirmed assets in its dirty
  sets that are **not yet flushed to the DB**. Copying the whole global sees them. A naive
  "pass empty", "pass the global directly", or "copy only the touched asset" can miss them
  and let a duplicate-name asset through ATMP (rejected later at block time — a policy
  divergence, not consensus, but a real behaviour change).
- Scope: 3 checker signatures + bodies, 3-4 cache methods, `MakeSignString` — roughly
  100-200 lines, all in consensus-adjacent asset validation on a live chain, needing a full
  asset-validation re-test (dup detection, sub-asset root lookup, signature verification).

## 3. Residual DoS (narrowed, not closed)

Before the guard, **any** cheap invalid transaction could force a ~2 ms whole-cache copy by
reaching `validation.cpp:683` and then failing `HaveCoin` — a tiny input costing the single
`msghand` thread milliseconds. The guard removes that for the entire normal-transaction
surface; an attacker must now send asset-typed (v3 / type 8-10) transactions to force a copy
at all. Item 1 caps what such a transaction can cost; item 2 removes it entirely.
