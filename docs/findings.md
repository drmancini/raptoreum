# Findings — what we actually know

<!-- owns:     every number and every decision this project relies on, one line each
     not mine: the design (transaction-decoupling.md), the schedule (build-plan.md),
               the narrative of how a measurement was taken (perf-results.md)
     rule:     living docs cite an ID. They do not restate a value. A number appearing
               in prose outside this file and the log is a bug. -->

The design says what we intend to build. The log says what happened on which day. This says
**what we know**, one line per item, with the regime it was measured in and a status.

**Why the regime column exists.** Nearly every contradiction the cross-check found was two
correct numbers from different conditions presented as one disputed number. A value without its
regime is not a finding.

| regime | what it means |
|---|---|
| `source` | read from the code, not measured |
| `loopback` | one host, Ryzen 9 3900X 12c/24t, peers on loopback |
| `wan-12` | twelve nodes across three continents. **Mostly 4-thread VPS, but the set includes a 24-thread and a 12-thread host**, and **no smartnode features were active** — no quorums, no islocks, no chainlocks — so every `wan-12` figure is an **upper bound** |
| `mario` | the Xeon 6517P, 16 physical cores / 32 threads |
| `bench-rt` | isolated regtest node on bowser, hand-driven (`bench_inputs`, `bench_sigops`, the probe) |
| `mainnet` | observed on a live mainnet node |
| `quorum-rt` | regtest with real smartnodes and a small quorum |
| `rig-8mb` | measured on this branch, where `MAX_DIP0001_BLOCK_SIZE` is 8 MB and **not** upstream's 2 MB |

Status is `stands`, `unreconciled` (two correct values, no single answer yet), or
`superseded by <ID>`.

---

## Constants (read from source)

| ID | constant | value | symbol / file | status |
|---|---|---|---|---|
| K-1 | block size cap, upstream | 2,000,000 | `consensus/consensus.h:MAX_DIP0001_BLOCK_SIZE` | stands |
| K-2 | block sigop cap | `MaxBlockSize()/50` → 40,000 at 2 MB | `consensus/consensus.h:MaxBlockSigOps` | stands |
| K-3 | per-transaction size cap | 100,000 B, **consensus** when DIP0001 active | `validation.cpp:MAX_STANDARD_TX_SIZE`, checked in `validation.cpp:ContextualCheckTransaction` | stands |
| K-4 | script cache | 524,288 entries (32 MB default, halved) | `validation.cpp:scriptExecutionCache.setup_bytes` | stands |
| K-5 | relay cap per trickle | `140 × MaxBlockSize()/1e6` → 280 at 2 MB | `net_processing.cpp:InvBroadcastMax` | stands |
| K-6 | trickle interval | 5 s inbound (Poisson mean), 2 s outbound, 1 s verified smartnode | `net_processing.cpp:INVENTORY_BROADCAST_INTERVAL` | stands |
| K-7 | max protocol message | 3 MB | `net.h:MAX_PROTOCOL_MESSAGE_LENGTH` | stands |
| K-8 | mempool default | 300 MB | `policy/policy.h:DEFAULT_MAX_MEMPOOL_SIZE` | stands |
| K-9 | ancestor / descendant limits | 25 count, 101 kB each | `validation.h:DEFAULT_ANCESTOR_LIMIT` | stands |
| K-10 | compact-block index ceiling | 65,535, an explicit deserialisation **guard**, not a wire limit (the wire form is CompactSize) | `blockencodings.h:BlockTxCount` | stands |
| K-11 | extra payload cap | 10,000 B | `consensus/consensus.h:MAX_TX_EXTRA_PAYLOAD` | stands |
| K-12 | IS wait gate | 600 s | `llmq/quorums_chainlocks.h:WAIT_FOR_ISLOCK_TIMEOUT` | stands |
| K-14 | **planning** bytes per transaction, for all storage arithmetic | **400 B** → 12.6 GB/yr per sustained tx/s | a choice, not a measurement — see D-12 and F-30 | stands |
| K-13 | mainnet quorum sizes above 600 smartnodes | IS 50; ChainLocks 200 (with `QUORUMS_200_8`) or 400 | `chainparams.cpp:UpdateLLMQParams` | stands |

## Measured facts

### Relay

| ID | fact | value | regime | status |
|---|---|---|---|---|
| F-1 | relay delivery against 1,500 offered, 21 connections | **~900-930 tx/s** (queueing-slope fit 936, cross-validated 864-922 by counting the miner's acceptances) | `wan-12` | stands. F-3's 824 is the **peer sweep's** run at the same peer count — 12% lower, run-to-run, not a contradiction |
| F-2 | relay is message-handling-bound, not schedule-bound | cutting the trickle interval 5× moved it **1.4%** | `wan-12` | stands |
| F-3 | per-peer share of msghand cost | ~40%: 21 conns → 824 tx/s at 1,084 µs/tx; 5 conns → 1,221 tx/s at 640 µs/tx | `wan-12` | stands |
| F-4 | per-peer delivery under the shipped cap | **56 tx/s** — 280 entries per trickle (K-5) on a Poisson timer averaging 5 s (K-6). Measured 44-65 across runs, which is Poisson trickle-count noise of ±18% at one sigma over 150 s, and every inv payload on the link was exactly 10,083 B = 3 + 280 × 36 | `source` + `loopback` | **stands.** The **74.7** figure quoted elsewhere is a single pre-#480 run on the busy-looping node and was never reproduced — do not use it |
| F-5 | re-indexed cap that converges | 50,000 converges 1,500 tx/s to 8 and 16 peers; 10,000 suffices at the v1 point; 7,500 is schedule-bound | `loopback` | stands |
| F-6 | peers hold nested, not complementary, subsets | union across peers contributed nothing over the best single peer | `loopback` | stands |

### Acceptance and validation

| ID | fact | value | regime | status |
|---|---|---|---|---|
| F-7 | acceptance ceiling | one ceiling, two statistics of it: **~4,400-4,500 tx/s** is the whole-run mean over a long saturated fill; **5,269 tx/s** is the per-second median (p90 5,550, peak 5,779). Both corpora are 2-in/2-out at depth 5 | `loopback` | **stands** — quote the mean for capacity planning and the median for a per-second ceiling, and say which |
| F-8 | msghand at 1,500 tx/s on a 4-thread VPS | **61-72%** of one core in the 15-s-block run, **89-90%** in the peer sweep, 89% when blocks come every 3 s | `wan-12` | stands — and this is why F-3's per-delivered cost is the comparable metric: a CPU percentage without the delivered rate beside it says nothing |
| F-9 | signature verification share of acceptance | **two thirds** (removing it: 5,269 → 15,842 tx/s) | `loopback` | stands |
| F-10 | naive pooling of ATMP script checks | **10% slower** than stock at 2× CPU — synchronisation exceeds ~59 µs/input of work | `loopback` | stands |
| F-11 | cost against input count, P2PKH | **linear** over the legal range; **156 µs/input**; worst legal transaction (650 inputs, 96 kB) **~100 ms** | `bench-rt` | stands |
| F-12 | cost against input count, multisig | P2SH 1-of-15 all keys tried: **1,900-2,000 µs/input**, ~**130 µs per counted sigop**; first key matching: 120 µs/input | `bench-rt` | stands |
| F-13 | the sigop counter does not price spend-side work | 800 **bare** 1-of-15 inputs, 91,691 B: **2,353 ms** of validation charged **2 sigops**. Legacy counts outputs created; the P2SH pass covers P2SH prevouts only | `bench-rt` + `source` | stands |
| F-14 | where the quadratic term is visible | µs/input climbs 1,842 → 2,942 as the transaction grows 5.8 kB → 92 kB (15 sighashes per input); invisible on P2PKH | `bench-rt` | stands |
| F-15 | worst-case block validation today | ~**49 s** for a 2 MB block of bare-multisig spends (charged ~42 sigops); ~**5.2 s** if P2SH-multisig-loaded | derived from F-12/F-13 | stands |
| F-15b | warm block connection | **~12.8 µs/tx** (1.1 s for 85,624 transactions) on the rig; 33-57 ms per 3,700 tx on the swarm, 111-157 ms on a 4-thread host | `loopback` / `wan-12` | stands — this is the value the warm column of F-16 rests on |
| F-16 | cold vs warm connect, ordinary payments | 2 MB commitment block: **~16-20 s** single-thread cold, **~0.8 s** warm. 8 MB: ~65-78 s cold, ~3.2 s warm | derived from F-11 | stands |
| F-17 | asset-cache deep copy per ATMP call | 6.5× throughput loss at 3,500 assets: **695 vs 4,502 tx/s**; 2.0-3.2 ms per call at K≈2,500 | `loopback` + `mainnet` | stands |

### Quorums, InstantSend, ChainLocks

| ID | fact | value | regime | status |
|---|---|---|---|---|
| F-18 | InstantSend lock rate | **74-76 locks/s** at quorum size 5 (threshold 3), **41.2** at size 9 (6), **32.5** at size 13 (8); cost ≈ 6.7 × quorum size − 15 ms per session; round-trip-bound, not compute-bound | `quorum-rt` | stands — **never measured at the live size of 50** (K-13), where the fit implies ~3 sessions/s |
| F-19 | signing sessions per transaction | **inputs + 1** (5,845 sessions for 2,915 one-input locks) | `quorum-rt` | stands |
| F-20 | saturation behaviour | above capacity sessions are purged and never retried — work is lost, not delayed | `quorum-rt` + `source` | stands |
| F-21 | threshold recovery parallelises | 10.7× at 16 threads (107 → 1,151 recoveries/s); near-linear to 4 threads; not wired to `CBLSWorker`'s async pool | `mario` | stands |
| F-22 | islock verification is not smartnode-only | every full node verifies every islock, ≥1.15 ms per message | `source` + `quorum-rt` | stands |
| F-23 | mainnet spork state | 2 **ON**, 3 **OFF**, 19 **ON**, 17 ON, 23 ON, 21/25 OFF; IS mempool signing **off** (spork 2 carries a timestamp, not 0) | `mainnet` 2026-09-18 | stands — and **temporary**, see X-3 |
| F-24 | mainnet ChainLocks are not forming | best chainlock **1,122,354** against tip **1,432,200**; tip reads `chainlock: false` | `mainnet` 2026-09-18 | stands, one node — **out of scope** (X-4). Owner's read, 2026-09-18: the nodes themselves are healthy and the signing path is the code Dash runs in production, so the likely cause is **operational — a rollout or a networking change — rather than a defect**. Recorded so nobody re-derives it as ours |

### The acceptance-layer probe

| ID | fact | value | regime | status |
|---|---|---|---|---|
| F-25 | `VerifyDB` on a missing body | reports **"Corrupted block database detected. Please restart with -reindex"** and refuses to start | `bench-rt` | stands |
| F-26 | P2P serving a missing body | serving node **aborts** (`assert(!"cannot load block from disk")`); the peer stops at the block before | `bench-rt` | stands |
| F-27 | `ConnectTip` on a missing body | `AbortNode`: "Failed to read block" | `bench-rt` | stands |
| F-28 | RPC read paths on a missing body | `error code: -1`, node survives | `bench-rt` | stands |
| F-25b | the three fatal paths, gated | `VerifyDB` starts and stops early; serving declines and survives; `ConnectTip` holds the block incomplete without touching validity | `bench-rt` | stands |
| F-25c | "not yet" must be a carried signal | returning false is non-fatal at runtime but fatal at startup — `ThreadImport` shuts down on any failed `ActivateBestChain`, and an empty state is indistinguishable from a disk error. Fixed with `CValidationState::BodiesMissing()`, four call sites | `bench-rt` + `source` | stands |
| F-25d | read-time withholding cannot reach the interesting states | to exercise `ConnectTip` a block must be valid, unconnected and body-less, and on one node every route there disconnects across the same gap. The remaining scenarios need **accept-time** withholding and **two** nodes | `bench-rt` | stands |
| F-29 | `DisconnectTip` on a missing body | **still open**, and doubly interesting: the path most likely to fire 0.1's kill criterion, and the reason the single-node scenarios are unreachable | — | open |

### Sizing

| ID | fact | value | regime | status |
|---|---|---|---|---|
| F-30 | bytes per transaction, measured | **373 B** (bench corpus, 2-in/2-out) and **397 B** mean (the `wan-12` corpus v3, which is what F-1 to F-3 and F-8 were measured with) | `bench-rt` / `wan-12` | stands — two corpora, two sizes, both real |
| F-31 | full replication cost | ~**12.6 GB/yr per sustained tx/s**, from K-14's planning size | derived from K-14 | stands |
| F-32 | retention tripwire | **80 tx/s sustained** — 1 TB/yr/node ÷ K-14's 12.6 GB/yr per tx/s, rounded down. Supersedes the 100 and 85 quoted elsewhere | derived from K-14 | stands, per D-12 |
| F-33 | BLOCKTXN worst case at 2 MB | 62,500 × 373 B = **23 MB**, against K-7's 3 MB and no chunking anywhere | derived + `source` | stands |

---

## What limits throughput, in order

Nothing owned this ranking after `throughput-bottleneck.md` was archived, and it is the first
thing anyone asks. Lowest binding constraint first, at the 2 MB design point:

| binds at | constraint | fix | entry |
|---|---|---|---|
| **167 tx/s** | the block sigop cap, which a commitment block hits at ~20,000 ordinary payments | re-base to committed count, jointly with a per-transaction work cap | K-2, F-13 |
| **~74 locks/s** | InstantSend coverage — not throughput, but it caps what can be locked | batch: one signature over N items | F-18 |
| **~930 tx/s** | relay delivery | re-index the cap (required at any target), then the per-peer path | F-1, F-4, F-5 |
| **~4,400 tx/s** | acceptance | nothing at this target; ~3× headroom | F-7 |
| **~49 s/block** | worst-case validation, through a path the sigop counter charges nothing for | the work cap | F-13, F-15 |

The order matters more than the numbers: the two lowest are **accounting and coverage**, not
speed, and neither is fixed by making anything faster.

## Assumptions taken on faith

Not measured, not in the repo, and load-bearing. Each says what breaks if it is wrong.

| ID | assumption | source | if wrong |
|---|---|---|---|
| X-1 | the 520-2,083 tx/s target is **peak and burst**, not sustained | RTM team, via owner 2026-09-17 | retention returns to v1 scope (F-31: 6-25 TB/yr/node), then the state root |
| X-2 | Smartnodes will hold the **entire** chain history | same | replay-then-prune stops being available, so the state root becomes required again |
| X-3 | InstantSend is off **deliberately** and will be switched on | owner 2026-09-18 | nothing — the design already prices IS on; this only explains why F-18-F-23 cannot be measured on mainnet |
| X-4 | the quorum layer will be fixed before decoupling, and is **not our scope** | owner 2026-09-18 | F-24 becomes a blocker rather than an observation |
| X-5 | smartnode spec: 4-6,000 nodes at 8 cores / 16 GB / **512 GB SSD** | trí's contract paper, May 2023 | every storage figure moves; X-1's margin is set by this |
| X-6 | contract jobs create "a special type of transaction"; results go on chain **or** a smartnode database | same | the load shape changes, and K-11 says a payload can be 27× the 373 B in F-30 |

## Decisions

| ID | date | decision | reopen when |
|---|---|---|---|
| D-1 | 11 Sep | Smartnodes hold everything; retention, sharding, challenges, incentives, repair deferred | sustained load > F-32, or growth crosses 1 TB/yr/node |
| D-2 | 12 Sep | the Smartnode attestation is a **record**, not a validity gate | a decision to make RTM a hybrid chain, which is not the availability question |
| D-3 | 13 Sep | the commitment format is the design (provisional) | superseded by D-4 |
| D-4 | 17 Sep | **the fork is closed**: commitment format, target = commitments filling a 2-8 MB block | the probe (D-9) finding the acceptance layer intractable |
| D-5 | 17 Sep | the state root is **optional** | X-2 failing, or replay bootstrap becoming impractical |
| D-6 | 17 Sep | the dual validation path stays **live**, not deferred | answers from trí on what is attested and what it may change |
| D-7 | 18 Sep | design for IS **on** and healthy quorums; mainnet's current state is temporary | — |
| D-8 | 16 Sep | the relay cap re-index is **not** a standalone upstream PR | upstream adopting the decoupling direction |
| D-9 | 18 Sep | 0.1's kill criterion: not tractable only if a scenario needs an invariant **relaxed** | — |
| D-10 | 18 Sep | our `docs/` never rides an upstream PR; nothing on upstream GitHub without asking | — |
| D-12 | 18 Sep | **400 B is the planning size** for every storage figure (K-14), which fixes the retention tripwire at F-32's 80 tx/s and supersedes the 100 and 85 quoted elsewhere | a measured production body-size distribution, which does not exist yet |
| D-13 | 12 Sep | pre-existing RTM defects are **out of scope** unless decoupling makes them reachable, or unless decoupling cannot ship without them fixed | — |
| D-14 | 17 Sep | **one fork**: format, the 8 MB cap and the message-length raise ride the same activation; 2 MB is then mined by **policy** | — |
| D-15 | 16 Sep | `getblocktemplate`'s full `ConnectBlock` per poll is **deferred**, not fixed — decoupling was expected to dissolve it. **Note:** F-16 weakens that premise, since a cold connect is 20-25× a warm one | sustained mining at scale, or F-16's cold path becoming the common case |
| D-11 | 11 Sep | taken now because retrofitting is a migration: a delete-capable body store, a non-punitive historical miss, and never pruning commitments | — |

## Retractions

What we believed, what is true, and what killed it. Kept in one place so "we no longer believe X"
is findable.

| ID | we said | actually | killed by |
|---|---|---|---|
| R-1 | the cascade was an absorption gap | it is relay throughput | running at an absorbing cadence |
| R-2 | relay cost is per-transaction, not per-peer | ~40% is per-peer (F-3) | normalising CPU by delivered load |
| R-3 | 1,500 tx/s is reachable on the current design "with headroom" | relay delivers ~930 (F-1) | the twelve-node swarm |
| R-4 | build-whole-split-after is a live option | it cannot express a commitment-sized target | D-4 |
| R-5 | the state root is a hard prerequisite | optional under X-1+X-2 | D-5 |
| R-6 | validation shows no quadratic signal | true for P2PKH only; multisig shows it plainly (F-14) | the multisig arm |
| R-7 | today's worst-case block hashing is ~0.4 s, later ~4 GB / 8-16 s | ~49 s through an uncounted path (F-15) | F-13 |
| R-8 | the compact-block `uint16_t` is a wire limit | an explicit guard; the wire form is CompactSize (K-10) | reading `blockencodings.h` |
| R-9 | a committed state root makes an attested-invalid transaction detectable | the root commits to **agreement**, not validity | the cross-check |
| R-10 | skipping script checks at acceptance merely relocates the cost | connect pays it **batched** on the check queue, which acceptance cannot do | F-10 |
| R-11 | parallel acceptance is a port-and-go | naive pooling is slower than stock (F-10) | the §17 arms |
| R-12 | three spork-dependent costs are inert | inert **today**, real for the design | D-7 |
| R-13 | all eleven PR branches revert upstream's #444 | none of them touches that file | comparing against the merge base, not develop's tip |
| R-14 | the design's anchors are valid at tag `2.0.3.01` | they were only ever valid on `perf/throughput-rig` | resolving `validation.cpp:419` at the tag |
| R-15 | the block size cap is what limits throughput | relay, InstantSend and the sigop cap all bind first — see the ranking above | the swarm, then F-13 |
| R-16 | `(height, index)` is a universal address | false off the active chain, where a competing branch commits to different identifiers | §5.2 |
| R-17 | the recent window's floor is 720 blocks "set by round-voting" | persisting that count at connect removes the reason; the floor now needs a new justification | plan item 3.2 |
| R-18 | the state commitment is "a port, not research" | the multiset hash is a port; a persisted, reorg-symmetric, consensus-critical accumulator across two databases has no reference | the cross-check |
| R-19 | the design does not rest on the shipped chain anchors | quorum membership is derived SPV-style against headers, so it does — and they are stale | the cross-check |
| R-20 | "nothing is deleted" | losing siblings' bodies must be dropped once buried | §8.4 / C6 of the security review |
| R-21 | relay is a blanket precondition for decoupling | it binds at the upper stage; 520 tx/s sits inside F-1 | F-1 |
| R-22 | InstantSend is "marginal" at the 2 MB point | it is a blocker there, ~27× short | F-18 |
| R-23 | there is a fixed thread budget | withdrawn; the handler takes one message per peer per pass | perf-results §18 |
| R-24 | the second socket stall is a defect | it is not — corrected 2026-09-16 | perf-results §18 |
| R-25 | ChainLocks `Cleanup` is what caps throughput | not binding at the sizes measured: no periodicity at the cleanup interval, no size dependence | the swarm |
| R-26 | the operative asset count climbs toward 3,439 | pinned at ~2,500 by `LoadAssets` on restart; reaching 3,439 needs every asset touched | asset-cache-drag, 2026-09-16 |
| R-27 | "any throughput ceiling measured on an affected node is a measurement of this bug" (#480's body) | true only on a core-constrained node; with spare cores the counterfactual showed no throughput difference | the #480 counterfactual; the PR body was corrected 2026-09-18 |

---

## Open — named so they are not mistaken for known

| what | why it matters | who answers |
|---|---|---|
| F-4, F-7, F-30, F-32 | four numbers with no single value | a measurement at a stated regime, or one choice recorded here |
| F-29 | `DisconnectTip` is the probe path most likely to fire the kill criterion | phase B of 0.1 |
| F-18 at quorum 50 | the InstantSend gap is priced from a quorum a tenth the live size | 0.4, the smartnode swarm |
| X-6 | the load shape, and whether a contract transaction is 373 B or 10 kB | trí |
| the work cap's value | F-12 gives the constant; the budget is a choice | 1.2 |
