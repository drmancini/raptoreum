# Asset-cache follow-ups (deliberately NOT in the guard fix)

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
