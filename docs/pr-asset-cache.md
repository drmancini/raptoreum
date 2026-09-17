# validation: don't copy the asset cache for every tx

## What is wrong

`AcceptToMemoryPoolWorker` copies the entire global asset cache on every transaction that
enters the mempool:

```cpp
CAssetsCache assetsCache = *passetsCache.get();
```

`CAssetsCache`'s copy constructor deep-copies three `std::map`s holding one entry per
confirmed asset, so the cost grows with the number of assets on the chain — several heap
allocations per asset, paid on every acceptance.

That copy has exactly one consumer: `CheckSpecialTx`. It returns at its first line for any
`nVersion != 3 || nType == TRANSACTION_NORMAL` — every ordinary payment — without ever
dereferencing the cache. Only `NEW_ASSET`, `UPDATE_ASSET` and `MINT_ASSET` read it.

So every payment on the network pays an O(confirmed-assets) copy that nothing reads.

## Why it matters now

`LoadAssets` caps the in-memory cache at `MAX_CACHE_ASSETS_SIZE` (2500) at startup, and
`Flush` grows it from there without eviction. Mainnet's asset database currently holds
3,439 assets, so every restarted node sits at that 2,500 floor.

At that size the copy costs roughly 1.2–3 ms per transaction — it varies with allocator
state, a freshly started process paying the low end and a long-running one the high end.
Mempool acceptance is single-threaded, so that one line alone caps acceptance at a few
hundred transactions per second, for every transaction type, whether or not it touches an
asset.

It is invisible at today's load only because the load is far below that ceiling. The
ceiling is already there, and it drops as the asset count grows.

## The fix

Construct the working copy only for the three asset transaction types, and pass `nullptr`
otherwise:

```cpp
std::unique_ptr<CAssetsCache> assetsCache;
if (tx.nVersion == 3 && (tx.nType == TRANSACTION_NEW_ASSET ||
                         tx.nType == TRANSACTION_UPDATE_ASSET ||
                         tx.nType == TRANSACTION_MINT_ASSET)) {
    assetsCache = std::make_unique<CAssetsCache>(*passetsCache.get());
}
```

`CheckSpecialTx` dereferences the pointer only for those three types, so acceptance
decisions are identical for every transaction — this elides a computation that was never
read, nothing more. Asset transactions still receive exactly the copy they did before.

## Measurements

`bench/assets_cache_copy.cpp`, added here, measures the copy against asset count on one
core of an x86-64 server CPU:

| in-memory assets | copy cost |
|---:|---:|
| 0 | ~25 ns |
| 1,000 | ~0.2 ms |
| 2,500 | ~1.2–3 ms |
| 10,000 | ~6 ms |

Before and after on regtest with 2,500 assets in memory, two builds differing only in this
change. The copy path is isolated by timing `sendrawtransaction` for a transaction that
reaches the copy and then fails on missing inputs, minus one rejected earlier in
`IsStandardTx`:

| normal payment | copy-path cost |
|---|---|
| before | 1.16 ms |
| after | 0.02 ms |

The same differential run against a synced mainnet node reproduces the effect on real data.

## Testing

`feature_assets.py` passes against the patched build — create, mint, update, send,
ownership transfer, immutability, and the survive-restart case — so asset behaviour is
unchanged. The benchmark is the permanent instrument for the cost itself.
