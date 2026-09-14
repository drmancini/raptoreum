# Two issue drafts for Raptor3um/raptoreum

Measured on `develop` (2.0.4.1), regtest, `checkmempool=0`. Review before posting.
Revised after an adversarial review; corrections noted at the end.

---

## Issue 1 — ChainLocks cleanup holds cs_main and mempool.cs proportionally to mempool size

**Title:** `CChainLocksHandler::Cleanup()` holds cs_main and mempool.cs for a time linear in mempool size, on every node

### What happens

`Cleanup()` walks every entry in `txFirstSeenTime`, calling `GetTransaction()` on each,
while holding `cs_main` **and** `mempool.cs`:

```cpp
// src/llmq/quorums_chainlocks.cpp:675
// need mempool.cs due to GetTransaction calls
LOCK2(cs_main, mempool.cs);
LOCK(cs);
...
// :700
for (auto it = txFirstSeenTime.begin(); it != txFirstSeenTime.end();) {
    uint256 hashBlock;
    CTransactionRef tx = GetTransaction(nullptr, &mempool, it->first,
                                        Params().GetConsensus(), hashBlock);
```

`txFirstSeenTime` gets one entry per accepted transaction (`:368-374`, driven from
`dsnotificationinterface.cpp:80`), and entries are erased once confirmed six deep
(`:707-712`), so its size is bounded by the mempool plus roughly six blocks.

Two details widen the blast radius:

- It runs on **every node, not only smartnodes**. `TrySignChainTip()` calls `Cleanup()`
  at `:248`, *before* the `if (!fSmartnodeMode) return;` at `:250`. `TrySignChainTip()`
  is invoked every 5 s (`:56-61`); `Cleanup()` self-throttles to at most once per 30 s
  (`CLEANUP_INTERVAL`, `quorums_chainlocks.h:63`).
- It holds the two locks the message handler needs, so while it runs the node accepts no
  transactions and any RPC that takes `cs_main` blocks.

### Measured

A timing log added to the function, during a sustained fill at 5,000 tx/s with
`-maxmempool=8000`:

| entries | walk | µs/entry |
|---|---|---|
| 35,150 | 17 ms | 0.48 |
| 185,280 | 94 ms | 0.51 |
| 335,770 | 170 ms | 0.51 |
| 637,870 | 332 ms | 0.52 |
| 941,922 | 494 ms | 0.52 |
| 1,094,409 | 587 ms | 0.54 |
| 2,018,847 | 1,141 ms | 0.57 |
| 2,909,415 | 1,803 ms | 0.62 |

Linear, drifting slightly upward. **This is the mempool-hit path only** — no blocks were
mined during the fill, so every `GetTransaction()` resolved from the mempool. Smartnodes
must run `-txindex` (`init.cpp:1726`), and for an entry that has left the mempool but is
under six deep, `GetTransaction` falls through to `g_txindex->FindTx`
(`validation.cpp:983-985`): a LevelDB lookup and a block-file read, far above 0.5 µs.
The per-entry figure above should not be read as the general case.

### Scale in real configurations

Because the map is bounded by the mempool, the cost is linear in `-maxmempool`:

| `-maxmempool` | approx. entries | walk, every 30 s |
|---|---|---|
| 300 (default) | ~204,000 | **~105 ms** |
| 1,000 | ~680,000 | ~360 ms |
| 8,000 | ~5,400,000 | ~3.3 s |

So at stock settings this is a ~100 ms hiccup twice a minute, not a multi-second stall.
The seconds-long figures require a deliberately enlarged mempool. It is raised because
the relationship is linear and unavoidable, and because any plan to raise throughput
raises `-maxmempool` with it.

### The stall, measured from outside

The figures above are the walk itself. The user-visible effect was measured separately and
by accident, on an unmodified build, by timing ordinary RPC calls during a fill.
`getmempoolinfo` reads three counters and has a median latency of 1.3 ms. During the same
fill it recorded:

| mempool | `getmempoolinfo` latency |
|---|---|
| 604,525 | 663 ms |
| 1,327,170 | 1,740 ms |
| 1,631,327 | 1,830 ms |
| 1,792,489 | 839 ms |

Six of sixty samples stalled; the rest were ~1 ms. A call that does no work taking 1.8
seconds is blocked on a lock, and the wait grows with mempool size. `getblocktemplate`
stalled in the same iterations, so the node was answering nothing at all.

Note the magnitudes run roughly 2.3x the measured walk above, and a caller arriving
mid-walk should wait *less* than a full walk, not more. So either the walk is slower on
the unmodified build or there is a second O(mempool) holder of these locks. **This report
does not claim the two are the same thing** — the walk is measured, the stall is measured,
and the link between them is not yet established.

### Reproduce

Regtest, `checkmempool=0`, `-maxmempool=8000`, **with at least one peer connected** —
`Cleanup()` returns early unless `smartnodeSync.IsBlockchainSynced()` (`:661`), which on
regtest does not advance without a peer. Push transactions at a few thousand per second
and log the walk duration, or poll `getblockcount` and watch the latency every 30 s.

### Prior art

The same walk under the same two locks is present in Dash `develop`
(`src/chainlock/handler.cpp`), so this is inherited rather than Raptoreum-specific.
Worth raising there too.

### Possible directions

Bounding the work per invocation, keying the map so expiry does not need a full scan, or
narrowing the lock scope to the part that needs it.

---

## Issue 2 — Socket thread busy-polls at 100% CPU while any peer is receive-paused (fixed upstream in Dash)

**Title:** Backport dashpay/dash `cb6afcbe5f` — `SocketHandler()` busy-loops on receive-paused peers

### Summary

**This is already fixed upstream.** Dash commit
[`cb6afcbe5f`](https://github.com/dashpay/dash/commit/cb6afcbe5f) (2026-06-29), "net:
avoid ThreadSocketHandler busy-loop on receive-paused peers", first released in Dash Core
**v23.1.7 (2026-07-01)** and present in every release since; it is not in v23.1.5.
Raptoreum's `net.cpp` predates the `Sock` refactor, but the change is about ten lines.
This report is a backport request with a local reproduction.

### What happens

`SocketHandler()` chooses between waiting for events and polling with no timeout:

```cpp
// src/net.cpp:1619
bool fOnlyPoll = false;
{
    LOCK2(cs_vNodes, cs_mapNodesWithDataToSend);
    if (!mapReceivableNodes.empty()) {
        fOnlyPoll = true;
    } else if (!mapSendableNodes.empty() && !mapNodesWithDataToSend.empty()) {
        // we must check if at least one of the nodes with pending messages is also
        // sendable, as otherwise a single node would be able to make the network
        // thread busy with polling
        ...
```

`fOnlyPoll` gives a timeout of 0 — in every socket-events backend, not only epoll
(`:1413-1414` kqueue, `:1448` epoll, `:1501` poll, `:1531` select).

A node is removed from `mapReceivableNodes` when it has no data waiting (`:1707-1708`) or
on disconnect (`:508`). A receive-paused peer has neither: `fPauseRecv` is set once its
queued bytes exceed `-maxreceivebuffer` (5 MB default, `net.h:107`), it is skipped for
reading (`:1718`, `:1758`), and `fHasRecvData` is only cleared by `SocketRecvData()`
(`:1810`), which it never reaches. So it stays in the map, `fOnlyPoll` stays true, and the
thread polls with a zero timeout continuously — reading almost nothing, since the peer is
only drained when the message handler unpauses it (`net_processing.cpp:3972`), after which
one 64 KB read re-pauses it.

**The send branch already guards against exactly this case and its comment explains why.
The receive branch has no equivalent check.** The upstream fix adds
`HasUnpausedReceivableNode()` and gates the fast path on it, plus a unit test.

As the upstream commit message notes, this is remotely triggerable: any single peer that
sends faster than the node processes will pin the socket thread. It does not require a
flood.

### Measured

Per-thread CPU on a node whose receive buffer is backed up:

| condition | rtm-msghand | rtm-net |
|---|---|---|
| 1 peer, at the moment total CPU crossed 150% | 100.3% | 51.0% |
| 8 peers, under load | 99.6% | 96.6% |
| 1 peer, mempool consistency checks on (heavy) | ~100% | ~100% |

`perf diff` between a healthy and a backlogged node, same binary and rate, shows the
growth terms in the networking path and none in validation: `CConnman::SocketHandler`
+3.08%, `epoll_wait` +0.93%, `SocketEventsEpoll` +0.86%, `ThreadSocketHandler` +0.55%,
`NotifyNumConnectionsChanged` +0.45%, `vector<CNode*>::reserve` +0.41% — per-iteration
costs rather than per-message ones. Signature verification *shrank* as a share of samples
while total CPU doubled, so the extra cycles are not validation.

### Reproduce

Send transactions faster than the node accepts them until its receive buffer fills, then
read per-thread CPU. `rtm-net` sits near 100% while performing almost no reads.
`strace -c -p <rtm-net tid>` will show `epoll_wait` returning tens of thousands of times a
second with a zero timeout.

### Mitigation available today

Raising `-maxreceivebuffer` delays the pause but does not prevent it.

---

## Issue 3 — FindBlockPos() loops forever if a block exceeds MAX_BLOCKFILE_SIZE

**Title:** `FindBlockPos()` never terminates for a block larger than `MAX_BLOCKFILE_SIZE`, allocating until OOM

### What happens

```cpp
// src/validation.h:97
static const unsigned int MAX_BLOCKFILE_SIZE = 0x8000000;   // 128 MiB

// src/validation.cpp:3770-3775
while (vinfoBlockFile[nFile].nSize + nAddSize >= MAX_BLOCKFILE_SIZE) {
    nFile++;
    if (vinfoBlockFile.size() <= nFile) {
        vinfoBlockFile.resize(nFile + 1);
    }
}
```

The loop looks for a block file with room for a block of `nAddSize`. If `nAddSize` is
itself `>= MAX_BLOCKFILE_SIZE`, no file can ever satisfy it — including a fresh empty one,
where `0 + nAddSize >= MAX_BLOCKFILE_SIZE` still holds. The loop increments `nFile`
without bound and resizes `vinfoBlockFile` once per iteration.

There is no error, no log line and no exception. The node allocates silently until the
kernel kills it, and on a shared machine the OOM killer may choose a different process.

### Measured

With `MAX_DIP0001_BLOCK_SIZE` raised for testing (regtest, `-blockmaxsize` varied):

| block | serialised bytes | result |
|---|---|---|
| 63,999,073 | under the limit | connects normally, 4.87 s, peak RSS 0.85 GB |
| 127,999,015 | under by 6 MiB | connects normally, 10.0 s, peak RSS 1.30 GB |
| ~192,000,000 | over | never completes, RSS grows 0.47 GB/s |
| 254,999,843 | over | `Error: Out of memory. Terminating.`, `signal=ABRT`, >60 GB |

`perf record --call-graph dwarf` during the runaway, while RSS went 4.00 GB → 37.24 GB in
25 seconds:

```
14.93%  std::vector<CBlockFileInfo>::_M_default_append(unsigned long)
 2.22%  SaveBlockToDisk(CBlock const&, int, CChainParams const&, FlatFilePos const*)
```

### Reachability

**Not reachable at the shipped 2 MB block size**, so this is not a live vulnerability. It
is latent, and it detonates the moment any block can exceed 128 MiB. The same code and the
same constant are in Bitcoin Core, equally unreachable there.

It is worth fixing pre-emptively because the failure mode is silent and total, and because
any future block-size work would trip over it with no diagnostic pointing at the cause.

### Possible directions

Either raise `MAX_BLOCKFILE_SIZE` alongside any block-size increase, or let a block file
hold a single oversized block — e.g. accept the position when the file is empty regardless
of `nAddSize`. A guard that errors instead of looping would at least make it diagnosable.

### The broader fix: assert the invariants at compile time

Three relationships govern whether a block can exist at all, and none is stated anywhere
in the source. All four constants are compile-time `static const unsigned int`, so the
compiler can enforce them:

```cpp
// A block must fit inside one block file, or FindBlockPos() cannot terminate.
static_assert(MAX_DIP0001_BLOCK_SIZE < MAX_BLOCKFILE_SIZE,
              "block size must be below MAX_BLOCKFILE_SIZE or FindBlockPos() loops forever");

// A block must fit inside one p2p message.
static_assert(MAX_DIP0001_BLOCK_SIZE < MAX_PROTOCOL_MESSAGE_LENGTH,
              "block size must be below MAX_PROTOCOL_MESSAGE_LENGTH or blocks cannot be relayed");

// A block's transaction vector must be deserialisable.
static_assert(MAX_DIP0001_BLOCK_SIZE <= MAX_SIZE,
              "block size must be within MAX_SIZE or ReadCompactSize() rejects the block");
```

All three hold at the shipped values (2 MB against 128 MiB, 3 MB and 32 MB), so they are
silent today and fire only when someone raises the block size without the accompanying
limits. That is exactly the mistake this report documents, and it was made three separate
times while producing these measurements — each constraint discovered by crashing into it
rather than by reading, because nothing in the code says the constants are related.

A runtime check at startup would also work, but compile-time is strictly better here: the
values cannot change at runtime, and a build failure cannot be ignored.

---

## Corrections made after review

Recorded so the reasoning is auditable:

- Issue 2 was written as a novel finding. It is fixed upstream; verified against the
  GitHub API that `cb6afcbe5f` exists, is dated 2026-06-29, carries that subject, touches
  `net.cpp`/`net.h`/`net_tests.cpp`, and adds `HasUnpausedReceivableNode()`. The review
  said it shipped in v23.1.8; comparing the commit against the tags shows it is contained
  in **v23.1.7 (2026-07-01)** and absent from v23.1.5, so v23.1.8 was a month late.
- Issue 1 claimed the map "scales without bound". It does not — entries are erased at six
  confirmations, so it is bounded by `-maxmempool`, and at stock settings the walk is
  ~105 ms rather than seconds. The default-config table was added and the severity
  reduced accordingly.
- Issue 1 said the node "answers no RPC". Only RPCs taking `cs_main` or `mempool.cs`
  block.
- The per-entry cost is the mempool-hit path; with `-txindex` (which smartnodes require)
  out-of-mempool entries cost far more. Now stated.
- The two measurement tables in this file and `perf-results.md` quoted different subsets
  of the same log; reconciled to the full set.
- An unresolved kernel symbol was being cited as a networking growth term. It cannot be
  attributed and has been removed from the evidence.
- The repro omitted that `Cleanup()` needs a connected peer on regtest to run at all.
- "Sustained" was claimed for a figure taken from a trigger snapshot of an oscillating
  state; the three measurements are now presented separately as what they are.
