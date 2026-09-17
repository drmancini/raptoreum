# Upstream ledger

Every way this tree diverges from `Raptor3um/raptoreum:develop`, and where each piece is
meant to go. The single source of truth for what still owes a trip upstream.

`perf/throughput-rig` is the integration branch. It holds `develop` plus, in substance,
everything marked **upstreamable** and everything marked **local-only** below — but as its
own history, not as the topic-branch commits. It is the superset the clean PR branches were
extracted *from*, so its content matches theirs while its commits do not, and its net layer
additionally carries local diagnostic instrumentation the PR does not. It is never pushed
for merge.

Going forward, upstreamable changes are authored on their own topic branch off `develop`
first, then merged in, so nothing new gets trapped in this lineage the way the flood fix
was.

Base: `develop` is `master` + 31 commits with no divergence (fast-forward), so rebasing
topic branches onto a moved `develop` stays cheap.

## Upstream's own decoupling branch

`Raptor3um/raptoreum` carries **`ft/breaking-up`**, created by trí for the decoupling work.

Checked 2026-09-17 at `d02e91323`: it is `develop` plus exactly **one unrelated commit** --
`test(amount): cap GetFeeTest's overflow case at OLD_MAX_MONEY (#444)`, touching only
`src/test/amount_tests.cpp`. **There is no decoupling code in it yet.** It is a starting
point, not a design to react to.

Worth re-checking before and during the design session; if trí pushes there, that is the
first place his direction becomes concrete rather than described.

Fetch without creating a tracking ref:

```
git fetch upstream ft/breaking-up && git log --oneline upstream/develop..FETCH_HEAD
```

## Upstreamable — has a topic branch, PR'd

| change | branch | PR | state |
|---|---|---|---|
| Functional test suite (9-part stacked series) | `ft/01-ghostrider-hash` … `ft/09-run-by-default` | #471-#479 on `develop`, #482-#490 on `ft/breaking-up` | submitted |

> **Each change is now proposed twice, deliberately** (2026-09-17). The eleven PRs against
> `develop` stay as they were; an identical set was opened against trí's `ft/breaking-up` so
> the decoupling work can build on them without waiting for `develop` review. Diffs were
> verified byte-identical pair by pair.
>
> | head branch | on `develop` | on `ft/breaking-up` |
> |---|---|---|
> | ft/01-ghostrider-hash | #471 | #482 |
> | ft/02-prune-dead-tests | #472 | #483 |
> | ft/03-regtest-quorums | #473 | #484 |
> | ft/04-test-harness | #474 | #485 |
> | ft/05-repair-suite | #475 | #486 |
> | ft/06-rpc-and-llmq-fixes | #476 | #487 |
> | ft/07-new-coverage | #477 | #488 |
> | ft/08-wallet-and-rpc-fixes | #478 | #489 |
> | ft/09-run-by-default | #479 | #490 |
> | fix/socket-handler-busy-loop | #480 | #491 |
> | fix/asset-cache-atmp-copy | #481 | #492 |
>
> If one of a pair merges, close the other rather than letting it go stale. Two `gh` traps
> worth remembering: `gh pr edit --base` fails silently on this repo (deprecated
> Projects-classic GraphQL field) -- use `gh api -X PATCH .../pulls/N -f base=X`; and the
> topic branches live on the `drmancini` fork, so `gh pr create` needs `--head drmancini:<branch>`
> or it reports "No commits between", which reads like a branch problem rather than a lookup one.

> **Open issue on the ft series (found 2026-09-16):** `feature_llmq_is_cl_conflicts.py`
> fails 3/3 standalone on `ft/09` at line 174 (`getrawtransaction` on rawtx1 → `-5 No such
> transaction`). This contradicts the "passes three of three on its own" note in
> `pr-series.md`; that note is now inaccurate. Fix-independent (pure ft/09 and ft/09+net-fix
> fail identically), so it is an ft-suite problem, not the socket fix. Needs its own look
> before or alongside the ft merge — the RTM team will hit it if they run the suite.
| Socket-handler busy-loop / DoS fix (+ wakeup) | `fix/socket-handler-busy-loop` | open | submitted, awaiting review |

> **What the wakeup half actually does** (net_processing.cpp, `ProcessMessages`). The socket
> thread skips any peer with `fPauseRecv` set, so when a peer is unpaused -- the moment
> `nProcessQueueSize` drops back under `GetReceiveFloodSize()` after a message is spliced off
> `vProcessMsg` -- nothing tells the socket thread to look at it again:
>
> ```cpp
> const bool was_paused = pfrom->fPauseRecv;
> pfrom->fPauseRecv = pfrom->nProcessQueueSize > connman->GetReceiveFloodSize();
> if (was_paused && !pfrom->fPauseRecv) {
>     wake_select = true;      // acted on after the lock is released
> }
> ...
> if (wake_select) connman->WakeSelect();
> ```
>
> The wait is up to `SELECT_TIMEOUT_MILLISECONDS`, and it is not cut short by traffic,
> because the sockets are edge-triggered: bytes already sitting in the kernel buffer raise no
> fresh readable event. `WakeSelect()` writes one byte to `wakeupPipe[1]` (net.cpp:1951),
> which is in the select set, so the thread returns immediately and re-reads the peer.
> `WakeSelect` is called after `cs_vProcessMsg` is released, deliberately -- signalling while
> holding it would invert the lock order against the socket thread.
>
> This is carried on `perf/throughput-rig` as well (commit 970c095cd), so the rig keeps it
> even if the PR is revised. Same mechanism, and the rig copy additionally sits alongside
> local diagnostic instrumentation the PR does not carry.

## Not a bug — withdrawn (2026-09-16)

| item | resolution |
|---|---|
| "Second socket stall" / connection wedged both directions | **Rig artifact, not a defect.** The wedge only occurred against the Python load generator, which under-reads (GIL). Two real nodes relaying bidirectionally at cap 50,000 converge cleanly and never deadlock (perf-results.md §18, corrected). A candidate fix was tried and reverted as unnecessary. Nothing to upstream. |

## Reported as findings, not code — issues/discussion, no branch

These are constants or costs that any throughput increase must address. They belong in
`docs/issue-drafts.md` and go upstream as issues or design notes, not patches, until the
decoupling direction is settled.

| finding | where |
|---|---|
| `MAX_BLOCKFILE_SIZE` infinite loop past 128 MiB | perf-results.md §12 |
| `maxmempool` default far too low for target throughput | transaction-decoupling.md §16.6 |
| `MAX_PROTOCOL_MESSAGE_LENGTH` 3 MB blocks >3 MB messages | §16.6 |
| `getblocktemplate` runs a full `ConnectBlock` per call | perf-results.md §10 |
| per-ATMP deep copy of the global assets cache scales with asset count | perf-results.md §17 note |
| relay cap indexed to block size breaks under decoupling | §8 / §16.3 |
| **`MAX_DIP0001_BLOCK_SIZE` raised 2 MB -> 8 MB on `perf/throughput-rig` only (commit `e55f029d6`)** — NOT on the measurement host's `ft/09` tree | big-block testing; **not upstream**. Changes block capacity and the derived relay cap (280/trickle upstream vs 1,120 here). Any rig run using the *default* cap is therefore not measuring shipped relay behaviour. |
| InstantSend attestation throughput and its failure mode | §15 / §16 |

## Local-only — never upstreamed

Lives in `perf/throughput-rig` and nowhere else.

| item | what |
|---|---|
| `-perfinvmax`, `-perfinvinterval`, `-perfinvnosort` | relay trickle overrides |
| `-perfskipsigs`, `-perfparallelatmp` | validation-cost measurement |
| `-perfalwaystrysend` | send-latch bypass probe |
| per-node `SocketHandler` state logging | livelock diagnosis |
| `test/perf/*` | the whole rig: corpus, generator, collector, analysis |
| `docs/perf-results.md`, `docs/transaction-decoupling.md`, `docs/perf-constants.md` | research write-ups |

## Housekeeping notes

- The livelock fix exists in **two** lineages: the clean commit on
  `fix/socket-handler-busy-loop`, and inline history on `perf/throughput-rig` (where it was
  first developed — the pattern we are stopping). When the PR merges and `develop` is synced
  into the rig, expect to resolve that as an already-applied change; take develop's version.
- When `develop` moves: rebase each open topic branch onto it (small), then merge the new
  `develop` into `perf/throughput-rig`.
