# net: don't busy-loop on peers the receive path will not drain

## What is wrong

`CConnman::SocketHandler()` decides whether to skip waiting for socket events using a
different test from the one that decides which nodes are actually read.

The skip-wait test is `!mapReceivableNodes.empty()` (`net.cpp:1623`). The receive path, forty
lines later, drains a node only when **all three** of these hold (`net.cpp:1718`):

```cpp
!it->second->fPauseRecv && it->second->nSendMsgSize == 0 && !it->second->fDisconnect
```

A node satisfying the first but not the second stays in the receivable set with its readable
flag set. The loop skips its wait, polls with a zero timeout, finds nothing it is willing to
do, and immediately repeats. Nothing notices it is not progressing, and the state persists
until the peer disconnects: the socket thread pinned at 100% of a core, the mempool and the
byte counters static.

Two things make a node undrainable:

- **pause** — the peer's queued message bytes exceed `-maxreceivebuffer`, which at the 5 MB
  default takes a large backlog.
- **send queue** — the node has bytes queued to send to that peer. This is not gated on the
  receive buffer and can arise in ordinary operation.

The second is why this is worth fixing rather than tuning around. A remote peer can reach
and hold either state, so it is remotely triggerable. While the thread spins it still
services other sockets each iteration, so the cross-peer effect is through CPU and
`cs_vNodes` contention rather than blocked I/O — but one connection can pin the single socket
thread, and a node that accepts inbound connections can be held this way cheaply.

## How to see it

`contrib/devtools/repro-socket-busyloop.py` reproduces both routes using only the standard
library — no instrumented build, no profiler, one peer, ten seconds. It reads the thread's
CPU from `/proc`, which `top -H -p <pid>` shows equally well.

```
raptoreumd -regtest -datadir=/tmp/spin -listen=1 -port=19940
contrib/devtools/repro-socket-busyloop.py --port 19940 --pid $(pidof raptoreumd) --mode sendqueue
```

On an AMD Ryzen 9 3900X (12 cores / 24 threads):

| route | node options | before | after |
|---|---|---|---|
| send queue | **none** | **99.9%** of a core | 6.8% |
| pause | `-maxreceivebuffer=1` | 99.3% of a core | 5.1% |

`--mode sendqueue` is the RTM-specific route: it sends pings and never reads the replies, so
the node's send queue to it stays non-empty while its socket stays readable. It needs no
node options.

The repro is a review aid, not something that needs to live in the tree. The permanent
regression guard is the unit test in `src/test/net_tests.cpp`, which is mutation-verified and
runs in CI. If you would rather not carry a standalone DoS script under `contrib/`, drop it
before merge — the fix and the unit test stand without it.

## The fix

Two parts. It is wrong with only the first.

**1. Make the skip-wait test agree with the receive path.** `HasUnpausedReceivableNode()`
counts a receivable node as work only when the receive path would actually drain it. Both
sites carry a comment saying the two must stay in agreement, because that is the whole bug.

**2. Wake the socket thread when the pause clears.** The busy-loop was also how a paused peer
got re-examined. With it gone, nothing wakes the socket thread when `ProcessMessages` clears
`fPauseRecv`; the fallback wait is `SELECT_TIMEOUT_MILLISECONDS`, 500 ms with the wakeup pipe
enabled, and the sockets are edge-triggered, so data already buffered raises no fresh event
to cut that short.

The script also reports how much it managed to write; the socket is blocking, so that tracks
how fast the node drains it, and it shows why both parts are needed:

| | socket thread CPU | drained |
|---|---|---|
| before | full core | 20.9 MB/s |
| predicate only | near idle | **9.0 MB/s** |
| predicate + wakeup | near idle | 20.9 MB/s |

Shipping the predicate alone would have traded a visible problem for an invisible one.

`WakeSelect()` is called outside `cs_vProcessMsg`: writing the wakeup pipe under a lock the
socket thread contends on invites a deadlock.

## Relationship to the Dash fix

Dash fixed the pause route in `cb6afcbe5fea`, released in v23.1.7, describing the same
mechanism.

Their predicate tests only `fPauseRecv`, and their receive path does not require an empty
send queue. Ours does, so applying their version here leaves the send-queue route spinning.
That is why the predicate in this PR tests all three conditions rather than one.

The unit test encodes that. It covers each of the three conditions separately and checks that
one undrainable node does not mask a drainable one. It is mutation-verified: replacing the
send-queue term with `true`, which reduces the predicate to Dash's form, makes it fail.

## Why this is worth fixing before any throughput work

Two reasons that apply to any change raising sustained transaction volume, including the
transaction decoupling work being discussed.

**The trigger gets more common, not less.** The send-queue route fires when a node has bytes
queued to a peer whose socket also has data waiting. More transactions in flight means more
announcements queued per peer, more of the time. A condition that is already easy to reach
becomes close to permanent.

**It burns a core, and can mislead capacity work.** A node in this state stops making
progress on that connection for reasons that have nothing to do with its capacity, while
still looking busy — full CPU, no errors, no log output. How much it costs depends on the
host: measured against a build with the fix reverted, on a 24-thread machine the socket
thread went from 12% to 84% while ingestion was unchanged, because the spin is on a
different thread from the bottleneck. On a core-constrained node it contends with the
message handler instead, and there is nothing in the symptoms to say which case you are in.

## One thing to expect

This removes the busy-loop, not every way a connection can stop making progress. A peer can
still end up undrainable for longer than it should, and after this change that shows as a
quiet wait rather than a pinned core — so a stall seen under load after applying this is not
necessarily this bug returning. That path involves the send-readiness flag and edge-triggered
sockets, and is worth a separate look; it is not touched here.
