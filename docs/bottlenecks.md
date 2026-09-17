# Throughput bottlenecks and priority

Rewritten 2026-09-16 after measurement overturned the earlier version. Evidence and method
are in `docs/throughput-bottleneck.md`; this is the short priority list.

Reference target: **v1 = ~1,500 tx/s**, accepted and relayed network-wide.

## The state in one line

**v1 is reachable on the current single-threaded design, with headroom.** An 8-node full mesh
carries 1,500 tx/s at 100% convergence with `rtm-msghand` at ~48%. Full convergence holds to
~3,000 tx/s network-wide. Nothing architectural is required to get there.

| measured | value |
|---|---|
| acceptance ceiling (sustained, saturated) | **~4,400-4,500 tx/s** — real compute, not cache pressure |
| pure relay ceiling, 8 peers | ~37,000 tx/s total (bounded by `rtm-net` and peers' own acceptance) |
| `msghand` at the v1 operating point | 48% (mesh) / 62-76% (star, 8 peers) |
| end-to-end ceiling, 8 peers | ~2,600 tx/s (star), ~3,000 tx/s (mesh) |

## The one thing that gates v1

| # | item | status |
|---|---|---|
| 1 | **Relay cap indexed to block size.** Delivers 50-65 tx/s per peer as shipped, which no target can live with. The cost is a *scheduling constant*, not CPU. | **Not an upstream PR.** It is not a bug and not a blocker for what RTM ships today (block capacity is ~45 tx/s, so the cap is correctly matched). Its right value comes *from* the decoupling design. Keep it on the dev build via `-perfinvmax`; ship it with decoupling. Measured: 10,000 suffices at the v1 point, 7,500 does not, 50,000 is ample. |

## Shipped

| item | note |
|---|---|
| asset-cache deep copy (`validation.cpp`) | **fixed, PR sent.** 695 → 4,460 tx/s on a 3,500-asset chain. Required on mainnet; invisible on the zero-asset rig. |
| socket busy-loop | **fixed, PR #480.** Counterfactual-tested: `rtm-net` 12% → 84% without it, **throughput unchanged** on a multi-core host. Value is the wasted core and the DoS vector. |

## Config, not code

| item | note |
|---|---|
| `maxmempool` default 300 MB | 1,500 tx/s × 2-min blocks ≈ 180k tx. Raise on the dev build; a default change is a separate argument. |

## Measured as NOT needed for v1

Each of these was on the earlier list as required or primary. All were disproven:

| item | why it is off the list |
|---|---|
| **parallel validation / MemPoolAccept port** | acceptance sustains ~4,400 tx/s against a 1,500 target — 3× headroom. The earlier "primary rate wall" framing was wrong. |
| **relay ordering rewrite** (O(backlog) heap) | `-perfinvnosort` moved throughput **<1%** at the v1 point. The "~40% of msghand" figure was measured at a 300k backlog with cap 50k-100k, a regime v1 never reaches. |
| **`getblocktemplate`** full `ConnectBlock` per poll | deferred: decoupling *dissolves* the hard part (a commitment block's bodies were validated at acceptance). Trigger to revisit is sustained mining at scale. |
| **message-size raises** (`MAX_INV_SZ`, `MAX_PROTOCOL_MESSAGE_LENGTH`) | the send loop already chunks at `MAX_INV_SZ`; no splitting work needed. |
| **`dbcache` / `maxsigcachesize`** | swept at saturation: no effect (13× and 16× raises, application confirmed in the node log). The ceiling is real compute. |

## Known, deferred

| item | why it can wait |
|---|---|
| **ChainLocks `Cleanup`** walks the mempool every 30 s under `cs_main`+`mempool.cs` | ~0.5 µs/entry. At the v1 mempool (~180k) that is 0.09 s per 30 s ≈ **0.3%** — the mesh run at 1,500 tx/s hit 179,528 entries and converged fully with it running. Scales with mempool size (0.5 s at 900k, 1.9 s at 3M), so it matters in a large-pending-set regime, not for building decoupling. Design-independent, so no sequencing pressure. |
| asset-cache unbounded growth / const-view | `docs/asset-cache-followups.md` |

## The standing methodological lesson

Two conclusions in the first pass were confidently wrong, and both came from the same class of
error: measuring a subsystem's capacity while a *shared* thread was saturated by something
else, and computing rates over a fixed duration when the work ran past it. Measure at the
operating point, not only at saturation, and take the denominator from the active window.
