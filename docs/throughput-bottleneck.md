# Where the throughput bottleneck actually is

Measured on the perf rig (Ryzen 9 3900X, 12c/24t, 62 GB), regtest, `checkmempool=0`,
`maxmempool=8000`, 3M pre-signed payments, 2026-09-16. Results below are the corrected
set: an earlier version of this document reached the opposite conclusion and was wrong.
See "Errors in the first pass".

## Summary

**The v1 target of 1,500 tx/s, accepted and relayed to 8 peers, is reachable on the current
single-threaded design** — demonstrated directly, with headroom. It requires raising the INV
trickle cap; it does **not** require new threading.

| quantity | measured |
|---|---|
| ingestion ceiling (saturated, 1 peer) | **~4,300–4,500 tx/s sustained** (msghand-bound, ECDSA-dominated) |
| `rtm-msghand` at saturation | 98.8–99.5% — the ingestion bottleneck |
| `rtm-msghand` **at the v1 point** (1,500 in, 8 peers) | **62% mean, 93.8% peak** — ~30% headroom |
| pure relay ceiling, 8 peers | **~37,000 tx/s total** (~4,500/peer), bounded by `rtm-net` and by the *receiving* peers' own acceptance |
| pure relay ceiling, 16 peers | ~53,000 tx/s total |
| 1,500 tx/s → 8 peers | **all 8 converge exactly** (180,003 each); lag ≈ one trickle |
| 1,500 tx/s → 16 peers | converges; per-peer 1,414 tx/s |
| end-to-end ceiling at 8 peers (star) | **~2,600 tx/s** |
| full-convergence ceiling, 8-node mesh | **~3,000 tx/s network-wide** |

**msghand is the ingestion bottleneck, not the relay bottleneck.** Relay is limited by the
socket thread and by peers' own single-threaded acceptance, an order of magnitude above what
v1 needs.

## Mesh test: distributed origination, 8 nodes, full mesh

The star topology has one node receive every transaction once and fan it out. A real network
originates transactions everywhere, and every node receives the *same* transaction announced
by several peers and must reject each duplicate INV. That work is absent from a star, so the
mesh was run as the realistic case: 8 nodes, full mesh (12-14 connections each), each node
fed its own lineage shard (`generate.py --shard i/8`, which keeps dependency chains intact),
cap 50,000.

| network-wide rate | per node | convergence | `msghand` per node |
|---:|---:|---:|---:|
| **1,500 (v1 target)** | 187 tx/s | **100%** | **47.4-48.9%** |
| 3,000 | 375 tx/s | **100%** | 75.4-81.2% |
| 4,500 | 562 tx/s | 92% | 95.7-97.7% |
| 6,000 | 750 tx/s | 69% | 96.8-98.1% |

At the v1 target every node ended holding **every** transaction (179,528 on all eight) with
the critical thread under half a core. Full convergence holds to **~3,000 tx/s network-wide,
twice the v1 target**; the node saturates near 4,500 and convergence then degrades.

The limit is the per-node `msghand` thread (97-98% at saturation), **not the test host**:
peak load was 9.4-11.5 on a 24-thread box, roughly 45% of capacity, with each node process at
~117% CPU. Distributed origination is *easier* than the star, because no single node carries
the whole fan-out.

## The one change v1 actually requires

The INV trickle cap bounds per-peer relay **by schedule, not by CPU**:
`INVENTORY_BROADCAST_MAX_PER_1MB_BLOCK * MaxBlockSize()/1e6` announcements per trickle, with
`INVENTORY_BROADCAST_INTERVAL = 5` seconds.

- stock cap → **56 tx/s per peer**, and this is fully explained. `InvBroadcastMax()` =
  `140 x MaxBlockSize()/1e6` = 140 x 2 = **280 entries per trickle**, drained on a Poisson
  timer averaging 5 s, so 280/5 = 56 tx/s. Directly confirmed: every inv payload on the link
  measured exactly **10,083 B = 3 + 280x36**. The 47-65 spread across runs is Poisson
  trickle-count noise (~±18% at 1 sigma over a 150 s window), not a second mechanism. No v1
  target is reachable at this cap regardless of threading.

  *(An earlier revision of this file claimed the mechanism was unexplained. That was my
  error: I read `MAX_DIP0001_BLOCK_SIZE` from mario's `perf/throughput-rig` checkout, where
  commit `e55f029d6` raises it to 8 MB, and applied it to measurements taken on the rig host,
  whose tree is `ft/09-run-by-default` at 2 MB. There was never a discrepancy.)*

So the cap re-index is **required**, and 7,500 (target x interval) is too tight — it leaves no
margin for the Poisson jitter in the trickle. Size it well above the arithmetic minimum.

## Errors in the first pass

All four were found by an independent review on a different model, and all four are verified:

1. **Relay budget measured under the wrong load.** Relay capacity was measured while ingestion
   was saturating msghand (85–99%), then applied to the v1 point where ingestion needs ~30%.
   The "698 tx/s per peer at 8 peers" was what remained after saturation, not a relay ceiling.
   The conclusion drawn from it — "1,500 tx/s supports only 3–4 peers" — is **withdrawn**.
2. **Wrong denominator.** Rates were computed as `final_mempool / 120`, but acceptance ran
   135–142 s at 1 peer and ≥175 s at 8 (still accepting 3,088 tx/s at t=130, 640 at t=147).
   Every rate in the first pass was inflated, unevenly across cases.
3. **No per-peer time series in the peer sweep**, so concurrent relay could not be separated
   from the post-offer drain tail; the "divides across peers" pattern rested on end-of-run
   totals alone.
4. **Unbounded-backlog artifact.** The 4→8 peer drop came from multi-second stalls in the
   per-trickle `make_heap` over the whole un-announced backlog — which only existed because
   the test offered ~4x more than could be relayed. At the target rate the backlog never
   exceeds one trickle.

Also: `threadcpu.py` averages only samples above 0.5%, so its secondary-thread figures
(e.g. `rtm-cl-schdlr`) are computed from a handful of samples and are not meaningful. The
msghand figures, sampled every second, are sound.

The "±13% run-to-run variance" claimed earlier came from unsaturated, artifact-dominated runs.
Saturated runs reproduce far better: an independent repeat of the 8-peer case matched
per-peer totals to **0.1%**.

## What still holds from the first pass

- Under saturating ingestion, `rtm-msghand` is pinned ~99% and is the ingestion ceiling.
- The stock block-indexed relay cap delivers only ~50–65 tx/s per peer — the network cannot
  converge at any interesting rate without changing it.
- The O(backlog) ordering sort is **not** a factor at the v1 operating point
  (`-perfinvnosort` moved throughput <1%); it only bites when a large backlog is allowed to
  build.
- The asset-cache fix causes no regression here (zero-asset chain; it strictly removes work).

## Is the acceptance ceiling real, or cache pressure?

Two parameters that bear on acceptance were never set in any run: `dbcache` (300 MB default;
one UTXO lookup per input in ATMP) and `maxsigcachesize` (32 MB default; signature
verification is ~2/3 of acceptance cost). Swept at saturation:

| case | sustained ingestion | `msghand` |
|---|---:|---:|
| baseline (defaults) | **4,565 tx/s** | 89.4% |
| `-dbcache=4000` (13x) | 4,538 tx/s | 89.1% |
| `-maxsigcachesize=512` (16x) | 4,361 tx/s | 89.6% |
| both | 4,473 tx/s | 89.8% |

All within +/-2.3%, with the baseline highest — i.e. no effect. The node log confirms the
setting applied (`Using 256 MiB out of 512/2 requested for signature cache` against 16 MiB by
default), so this is not a silently-ignored flag.

**The ceiling is real compute, not cache pressure.** The reason is structural: the corpus is
3M *unique* transactions, so every signature is verified exactly once and a larger signature
cache has nothing to re-hit — it pays off only when a transaction is verified twice (mempool
accept, then block connect). The fanout UTXO set likewise fits easily in 300 MB.

Every "is X enough" judgement in this document rests on ~4,400-4,500 tx/s, and that figure
now survives a direct attempt to inflate it.

## Do the two fixes matter for these numbers?

Both were tested directly by building counterfactual binaries.

### Asset-cache fix: required on a chain that carries assets

The rig chain has zero assets, so the fix is a no-op there and earlier runs could not
show its value. A snapshot carrying **3,500 confirmed assets** was built to match mainnet's
3,439 (`LoadAssets` caps the in-memory cache at 2,500 on start, which is the mainnet
condition). Saturating offer, rates over the actual active window:

| binary | chain | sustained ingestion | `msghand` |
|---|---|---:|---:|
| unfixed | 3,500 assets | **695 tx/s** | 99.4% |
| **fixed** | 3,500 assets | **4,460 tx/s** | 90.3% |
| unfixed (control) | 0 assets | 4,502 tx/s | 90.3% |

The same unfixed binary manages 4,502 tx/s with no assets and **695 with 3,500** — the
per-ATMP copy costs **6.5x throughput**. The fix recovers all of it: an asset-bearing chain
performs like a bare one. Without the fix a mainnet-like node sits at **695 tx/s, below the
1,500 target**; with it, comfortably above. This is consistent with the live-node
differential measured separately (~2-3 ms per transaction at 2,500 cached assets).

**So the asset fix is required to reach v1 on mainnet**, even though it is invisible on the
zero-asset rig.

### Socket busy-loop fix (#480): not required for throughput here

A pre-#480 binary was built by reverting both halves (the skip-wait predicate and the
companion `WakeSelect`). The bug reproduces exactly:

| | `rtm-net` | `msghand` | ingested | relayed |
|---|---:|---:|---:|---:|
| with #480 | **12.2%** (peak 74.8) | 91.5% | 636,480 | 186,768 |
| without #480 | **83.6%** (peak 99.7) | 90.6% | 608,720 | 194,805 |

The socket thread goes from 12% to 84% — a core burned spinning. **Throughput is unchanged**
(within noise; the buggy build even relayed marginally more), because the spin is on
`rtm-net` while the bottleneck is `msghand`, and this host has spare cores.

The fix's value is therefore the wasted core and the remote DoS vector, not throughput on a
multi-core machine. It would matter on a core-constrained smartnode, where a spinning socket
thread contends with `msghand`. The claim in the PR that any throughput ceiling measured on
an affected node "is a measurement of this bug" is **too strong for this hardware** and
should be softened.

## Consequence for the plan

Acceptance work is not the v1 constraint, and neither is relay CPU. The binding constraint is
a **scheduling constant**. Raise the trickle cap (and consider the interval), and 1,500 tx/s
to 8 — or 16 — peers works today. The next real ceiling is ~2,600 tx/s end-to-end at 8 peers,
which is where parallel acceptance would start to matter.
