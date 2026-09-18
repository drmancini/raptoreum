<!--
status: LIVING — UNPOSTED. Draft for the owner to send or discard.
Nothing here has been posted to Raptor3um/raptoreum or sent to anyone.
-->

# Draft note to trí — InstantSend collapses under sustained load, and we cannot name the cause

**Why we are raising it:** we were measuring what InstantSend costs at the throughput the decoupling
work targets, and found something that looks like a node-level defect rather than a capacity limit.
It changes whether batched InstantSend is a prerequisite for that target, so it seemed worth your
eyes before we plan around it.

We are not proposing a fix. We tried one and it did not work, which is part of why we think the
cause is not what it appears to be.

---

## What we ran

A 12-host regtest swarm across the US, EU and Asia — real WAN latency, not loopback. Ten registered
smartnodes, quorum size 8, threshold 5. Transactions offered simultaneously from every node out of
disjoint pre-signed corpora, so load originates everywhere rather than at one point.

Two arms at identical offered load — **300 tx/s total, 15-second blocks, 20 minutes** — differing only
in whether InstantSend was enabled.

## What happened

| | IS off | IS on |
|---|---|---|
| offered / accepted | 360,012 / 360,012 | 360,012 / 360,012 — **zero rejections either way** |
| mempool at the end | ~7,000 | **183,273 tx / 239 MB**, still climbing |
| block fill (15 s) | 4,257-4,582 tx | **1 tx**, then 13-15% below the control |
| ChainLocks | n/a | **stopped forming entirely** |
| `rtm-sigshares` | idle | **93.7% / 97.0%** on the two nodes measured |

Acceptance and relay are untouched — every transaction offered was accepted in both arms. What
breaks is everything downstream of locking, and ChainLocks are the first thing lost.

## The signature: it is bistable, not saturated

We instrumented `CSigSharesManager::WorkThreadMain` with per-step timers. `SignPendingSigShares` is
**~99% of that thread**; recovery is 0 ms (it already times itself — median 3 ms over 350 events),
inbound share processing 0 ms, sending under a second across minutes.

The interesting part is how it changes over a single run:

```
iters=46  sign=0ms        send=95ms   pendingSigns=0
iters=13  sign=7104ms     send=48ms   pendingSigns=9487
iters=1   sign=21298ms    send=120ms  pendingSigns=21275
iters=1   sign=122993ms   send=498ms  pendingSigns=115873
```

It runs healthily — the work loop cycles dozens of times per window, the pending queue sits at zero —
and then it tips. By the last line **one loop iteration has taken 123 seconds**, and during it the
thread never reaches `SendMessages` or `ProcessPendingSigShares`.

**Per-signature cost is not constant.** At shallow queue depth it is 1-2 ms, which is a BLS signature
and nothing more. At depth it is **~3.8 s** — three orders of magnitude worse. Every unit of backlog
appears to make the next signature slower, which deepens the backlog.

## The fix we tried, and why it failed

The obvious reading is that `SignPendingSigShares` takes the whole queue —
`v = std::move(pendingSigns)`, unbounded — so one call can run for minutes and starve the send path.
We bounded it to 32 items per pass so the loop would come back round to sending, built it, and ran
the same load.

**It still collapsed.** The profile above *is* the patched node: it starts clean, then tips anyway,
and 123 seconds for at most 32 signatures is ~3.8 s each. So batch size was not the quantity that
mattered, and the superlinearity is upstream of it.

## What we can say, and what we cannot

**Measured:** the bistability; `SignPendingSigShares` at ~99% of the thread; per-item cost rising
with backlog depth by >1000×; recovery cheap at 3 ms median; `rtm-sigshares` pinned at 100% while the
rest of the node idles at ~101% of one core total, so by then it is not even receiving.

**Not established — and we would rather not guess again.** Why per-item cost grows with depth.
`ProcessSigShare` takes `LOCK(cs)` once per item; there are 22 such sites in that file; the network
threads contend for the same mutex; and the maps it guards grow with the backlog, which would make
each hold longer. That is a coherent story and we have no evidence for it beyond coherence. We have
already been wrong twice on this path — first attributing the cost to threshold recovery, then to
batch size — so we are stopping at the signature.

Two measurements would settle it, and either is a short job for someone who knows this code:

1. **Lock held-time on `cs`** — histogram of acquisition wait and hold duration, against queue depth.
2. **Per-item work against queue depth** — instrument one `ProcessSigShare` call and plot cost versus
   `pendingSigns.size()`.

## Why it matters beyond the defect

Our plan had carried batched InstantSend as a prerequisite for the throughput target, on the premise
that per-transaction signing cannot reach it. That premise survives — but for a different reason
than we had written down, and **a batching protocol does not obviously address a superlinear collapse
under backlog.** If the collapse is a bug, the target may be reachable without new consensus
machinery at all; if it is inherent, batching still has to cope with the same curve.

Either way it seemed wrong to design consensus-level machinery around a node-level behaviour nobody
has diagnosed.

## Reproducing it

Any regtest network with enough smartnodes to form a quorum, sporks 17/23/25 on and spork 2 on,
offered load around 300 tx/s sustained for ten minutes or more. Watch `pendingSigns` and the
per-iteration time in `SignPendingSigShares`; the tip is unmistakable once it happens. Our harness
and the instrumentation patch are in our tree if useful.

One incidental note that cost us a while: with sporks 2, 3 and 19 on and **no** InstantSend quorum
yet formed, `IsTxSafeForMining` skips every non-islocked transaction younger than ten minutes, so
blocks come out empty and it reads like a mining fault. Enabling 17/23/25 first, then forming a
quorum, then 19, then 2, avoids chasing it.
