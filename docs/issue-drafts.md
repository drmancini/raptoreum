# Two issue drafts for Raptor3um/raptoreum

Both measured on 2.0.4.1 (`develop`), regtest, with `checkmempool=0`. Review before
posting.

---

## Issue 1 — ChainLocks cleanup holds cs_main and mempool.cs for seconds, proportional to mempool size

**Title:** `CChainLocksHandler::Cleanup()` stalls the node for seconds at a large mempool, on every node

### What happens

`Cleanup()` walks every entry in `txFirstSeenTime`, calling `GetTransaction()` on each,
while holding `cs_main` **and** `mempool.cs`:

```cpp
// llmq/quorums_chainlocks.cpp
// need mempool.cs due to GetTransaction calls
LOCK2(cs_main, mempool.cs);
LOCK(cs);
...
for (auto it = txFirstSeenTime.begin(); it != txFirstSeenTime.end();) {
    uint256 hashBlock;
    CTransactionRef tx = GetTransaction(nullptr, &mempool, it->first,
                                        Params().GetConsensus(), hashBlock);
```

`txFirstSeenTime` gets one entry per accepted transaction
(`TransactionAddedToMempool` → `txFirstSeenTime.emplace(...)`), so the walk is O(mempool).

Two details make it bite harder than it first looks:

- It runs on **every node, not only smartnodes**. `TrySignChainTip()` calls `Cleanup()`
  *before* the `if (!fSmartnodeMode) return;`, and `TrySignChainTip()` is on a
  5-second scheduler.
- It holds the two locks the message handler needs, so the node accepts no transactions
  and answers no RPC for the duration.

### Measured

A timing log added to the function, during a fill at 5,000 tx/s:

| entries | walk time | µs/entry |
|---|---|---|
| 35,150 | 17 ms | 0.48 |
| 185,280 | 94 ms | 0.51 |
| 637,870 | 332 ms | 0.52 |
| 1,094,409 | 587 ms | 0.54 |
| 2,018,847 | 1,141 ms | 0.57 |
| 2,909,415 | 1,803 ms | 0.62 |

Linear, drifting slightly upward with cache pressure. At 3 million entries that is
**~1.9 seconds of total stall every 30 seconds** (`CLEANUP_INTERVAL = 1000 * 30`).

### Why it matters

A node that can accept ~5,600 tx/s reaches a million mempool entries in three minutes, so
this is not a hypothetical size. The stall is a latency defect rather than a throughput
one — it does not reduce the sustained rate much — but it blocks RPC and acceptance
entirely while it runs, and it scales without bound.

### Reproduce

Regtest with `checkmempool=0` and `maxmempool=8000`, push transactions at a few thousand
per second until the mempool passes a million, and watch RPC latency every 30 seconds.

### Possible directions

Bounding the work per invocation, or keying the map so expiry does not require a full
walk, or holding the locks only for the portion that needs them rather than the whole
scan.

---

## Issue 2 — Socket thread busy-polls at 100% CPU whenever a peer's receive buffer is paused

**Title:** `CConnman::SocketHandler()` spins with a zero-timeout poll while any peer has `fPauseRecv` set

### What happens

`SocketHandler()` decides whether to wait for events or poll with no timeout:

```cpp
// net.cpp
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

`fOnlyPoll` makes `epoll_wait` use a timeout of 0.

A node leaves `mapReceivableNodes` only when it has no data waiting:

```cpp
if (!it->second->fHasRecvData) {
    it = mapReceivableNodes.erase(it);
} else {
    if (!it->second->fPauseRecv && it->second->nSendMsgSize == 0 && !it->second->fDisconnect) {
        vReceivableNodes.emplace_back(it->second);
    }
    ++it;
}
```

A peer whose receive buffer is full has `fPauseRecv` set and still has data waiting, so it
stays in `mapReceivableNodes`, is skipped for reading, and keeps `fOnlyPoll` true. The
thread then polls with no timeout, finds the same paused peer, and repeats — burning a
full core while reading nothing.

The send side already guards against precisely this, and its comment says why. The receive
side has no equivalent check.

### Measured

Per-thread CPU on a node under load, one peer, sustained:

```
100.3%  rtm-msghand
 51.0%  rtm-net        (rising to ~100% as the pause persists)
 11.0%  rtm-scheduler
```

`perf diff` between a healthy and a backlogged node, same binary and rate, shows every
growth term in the networking path and none in validation: a kernel symbol 11% → 40%,
`CConnman::SocketHandler` +3.08%, `epoll_wait` +0.93%, `SocketEventsEpoll` +0.86%,
`ThreadSocketHandler` +0.55%, `NotifyNumConnectionsChanged` +0.45% — per-iteration costs
rather than per-message ones. Signature verification *shrank* as a share while total CPU
doubled.

### Why it matters

The cost lands exactly when the node is already behind: falling behind sets `fPauseRecv`,
which costs an extra core, which makes catching up harder. On a small node the wasted core
may be the one the message handler needs.

### Reproduce

Push transactions faster than the node can accept them until the receive buffer fills,
then look at per-thread CPU. The socket thread will sit near 100% while doing no reads.

### Possible directions

Excluding paused peers when deciding `fOnlyPoll`, or removing them from
`mapReceivableNodes` until they are unpaused, so the thread sleeps when there is genuinely
nothing it may do.
