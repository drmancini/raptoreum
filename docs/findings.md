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
| `wan-12` | twelve nodes across three continents, 4-thread VPS class |
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
| C1 | block size cap, upstream | 2,000,000 | `MAX_DIP0001_BLOCK_SIZE`, `consensus/consensus.h` | stands |
| C2 | block sigop cap | `MaxBlockSize()/50` → 40,000 at 2 MB | `MaxBlockSigOps`, `consensus/consensus.h` | stands |
| C3 | per-transaction size cap | 100,000 B, **consensus** when DIP0001 active | `MAX_STANDARD_TX_SIZE` via `ContextualCheckTransaction`, `validation.cpp` | stands |
| C4 | script cache | 524,288 entries (32 MB default, halved) | `scriptExecutionCache.setup_bytes`, `validation.cpp` | stands |
| C5 | relay cap per trickle | `140 × MaxBlockSize()/1e6` → 280 at 2 MB | `InvBroadcastMax`, `net_processing.cpp` | stands |
| C6 | trickle interval | 5 s inbound (Poisson mean), 2 s outbound, 1 s verified smartnode | `INVENTORY_BROADCAST_INTERVAL`, `net_processing.cpp` | stands |
| C7 | max protocol message | 3 MB | `MAX_PROTOCOL_MESSAGE_LENGTH`, `net.h` | stands |
| C8 | mempool default | 300 MB | `DEFAULT_MAX_MEMPOOL_SIZE`, `policy/policy.h` | stands |
| C9 | ancestor / descendant limits | 25 count, 101 kB each | `DEFAULT_ANCESTOR_LIMIT` etc., `validation.h` | stands |
| C10 | compact-block index ceiling | 65,535, an explicit deserialisation **guard**, not a wire limit (the wire form is CompactSize) | `CBlockHeaderAndShortTxIDs`, `blockencodings.h` | stands |
| C11 | extra payload cap | 10,000 B | `MAX_TX_EXTRA_PAYLOAD`, `consensus/consensus.h` | stands |
| C12 | IS wait gate | 600 s | `WAIT_FOR_ISLOCK_TIMEOUT`, `llmq/quorums_chainlocks.h` | stands |
| C13 | mainnet quorum sizes above 600 smartnodes | IS 50; ChainLocks 200 (with `QUORUMS_200_8`) or 400 | `UpdateLLMQParams`, `chainparams.cpp` | stands |

## Measured facts

### Relay

| ID | fact | value | regime | status |
|---|---|---|---|---|
| F1 | relay delivery against 1,500 offered, 21 connections | **~900-930 tx/s** | `wan-12` | stands |
| F2 | relay is message-handling-bound, not schedule-bound | cutting the trickle interval 5× moved it **1.4%** | `wan-12` | stands |
| F3 | per-peer share of msghand cost | ~40%: 21 conns → 824 tx/s at 1,084 µs/tx; 5 conns → 1,221 tx/s at 640 µs/tx | `wan-12` | stands |
| F4 | per-peer delivery under the shipped cap | **44-48**, **56** (arithmetic: 280 ÷ 5 s inbound), and **74.7** have all been measured or derived | `loopback` / `source` / `loopback` | **unreconciled** — three conditions, no single answer; do not quote one alone |
| F5 | re-indexed cap that converges | 50,000 converges 1,500 tx/s to 8 and 16 peers; 10,000 suffices at the v1 point; 7,500 is schedule-bound | `loopback` | stands |
| F6 | peers hold nested, not complementary, subsets | union across peers contributed nothing over the best single peer | `loopback` | stands |

### Acceptance and validation

| ID | fact | value | regime | status |
|---|---|---|---|---|
| F7 | sustained acceptance ceiling | **~4,400-4,500 tx/s** (3M unique txs, saturated, 1 peer) and **5,269 tx/s** (median, corpus v2, 25,000 offered) | `loopback` | **unreconciled** — different corpora and offer patterns; both stand in their own run |
| F8 | msghand at 1,500 tx/s on a 4-thread VPS | 61-72% of one core (89% with 3 s blocks) | `wan-12` | stands |
| F9 | signature verification share of acceptance | **two thirds** (removing it: 5,269 → 15,842 tx/s) | `loopback` | stands |
| F10 | naive pooling of ATMP script checks | **10% slower** than stock at 2× CPU — synchronisation exceeds ~59 µs/input of work | `loopback` | stands |
| F11 | cost against input count, P2PKH | **linear** over the legal range; **156 µs/input**; worst legal transaction (650 inputs, 96 kB) **~100 ms** | `bench-rt` | stands |
| F12 | cost against input count, multisig | P2SH 1-of-15 all keys tried: **1,900-2,000 µs/input**, ~**130 µs per counted sigop**; first key matching: 120 µs/input | `bench-rt` | stands |
| F13 | the sigop counter does not price spend-side work | 800 **bare** 1-of-15 inputs, 91,691 B: **2,353 ms** of validation charged **2 sigops**. Legacy counts outputs created; the P2SH pass covers P2SH prevouts only | `bench-rt` + `source` | stands |
| F14 | where the quadratic term is visible | µs/input climbs 1,842 → 2,942 as the transaction grows 5.8 kB → 92 kB (15 sighashes per input); invisible on P2PKH | `bench-rt` | stands |
| F15 | worst-case block validation today | ~**49 s** for a 2 MB block of bare-multisig spends (charged ~42 sigops); ~**5.2 s** if P2SH-multisig-loaded | derived from F12/F13 | stands |
| F16 | cold vs warm connect, ordinary payments | 2 MB commitment block: **~16-20 s** single-thread cold, **~0.8 s** warm. 8 MB: ~65-78 s cold, ~3.2 s warm | derived from F11 | stands |
| F17 | asset-cache deep copy per ATMP call | 6.5× throughput loss at 3,500 assets: **695 vs 4,502 tx/s**; 2.0-3.2 ms per call at K≈2,500 | `loopback` + `mainnet` | stands |

### Quorums, InstantSend, ChainLocks

| ID | fact | value | regime | status |
|---|---|---|---|---|
| F18 | InstantSend lock rate | **60-76 locks/s** at quorum 3-5; cost ≈ 6.7 × quorum size − 15 ms per session; round-trip-bound | `quorum-rt` | stands — **never measured at the live quorum of 50** |
| F19 | signing sessions per transaction | **inputs + 1** (5,845 sessions for 2,915 one-input locks) | `quorum-rt` | stands |
| F20 | saturation behaviour | above capacity sessions are purged and never retried — work is lost, not delayed | `quorum-rt` + `source` | stands |
| F21 | threshold recovery parallelises | 10.7× at 16 threads (107 → 1,151 recoveries/s); not wired to the async pool | `loopback` | stands |
| F22 | islock verification is not smartnode-only | every full node verifies every islock, ≥1.15 ms per message | `source` + `quorum-rt` | stands |
| F23 | mainnet spork state | 2 **ON**, 3 **OFF**, 19 **ON**, 17 ON, 23 ON, 21/25 OFF; IS mempool signing **off** (spork 2 carries a timestamp, not 0) | `mainnet` 2026-09-18 | stands — and **temporary**, see A3 |
| F24 | mainnet ChainLocks are not forming | best chainlock **1,122,354** against tip **1,432,200**; tip reads `chainlock: false` | `mainnet` 2026-09-18 | stands, one node — **out of scope**, see A4 |

### The acceptance-layer probe

| ID | fact | value | regime | status |
|---|---|---|---|---|
| F25 | `VerifyDB` on a missing body | reports **"Corrupted block database detected. Please restart with -reindex"** and refuses to start | `bench-rt` | stands |
| F26 | P2P serving a missing body | serving node **aborts** (`assert(!"cannot load block from disk")`); the peer stops at the block before | `bench-rt` | stands |
| F27 | `ConnectTip` on a missing body | `AbortNode`: "Failed to read block" | `bench-rt` | stands |
| F28 | RPC read paths on a missing body | `error code: -1`, node survives | `bench-rt` | stands |
| F29 | `DisconnectTip` on a missing body | **not yet measurable** — the startup check window always covers the tip, so withholding it blocks startup first | — | open |

### Sizing

| ID | fact | value | regime | status |
|---|---|---|---|---|
| F30 | bytes per transaction | corpus is 2-in/2-out at **373 B**; §8.5's storage arithmetic uses **400 B** | `bench-rt` / assumption | **unreconciled** — pick one for all storage maths |
| F31 | full replication cost | ~**12.6 GB/yr per sustained tx/s** at 400 B (F30) | derived | stands |
| F32 | retention tripwire | quoted as 100 tx/s (§8.5), ~85 (§8 banner), and 79 (1 TB ÷ 12.6 GB) | derived from F30/F31 | **unreconciled** — one number, chosen once, when F30 is settled |
| F33 | BLOCKTXN worst case at 2 MB | 62,500 × 373 B = **23 MB**, against C7's 3 MB and no chunking anywhere | derived + `source` | stands |

---

## Assumptions taken on faith

Not measured, not in the repo, and load-bearing. Each says what breaks if it is wrong.

| ID | assumption | source | if wrong |
|---|---|---|---|
| A1 | the 520-2,083 tx/s target is **peak and burst**, not sustained | RTM team, via owner 2026-09-17 | retention returns to v1 scope (F31: 6-25 TB/yr/node), then the state root |
| A2 | Smartnodes will hold the **entire** chain history | same | replay-then-prune stops being available, so the state root becomes required again |
| A3 | InstantSend is off **deliberately** and will be switched on | owner 2026-09-18 | nothing — the design already prices IS on; this only explains why F18-F23 cannot be measured on mainnet |
| A4 | the quorum layer will be fixed before decoupling, and is **not our scope** | owner 2026-09-18 | F24 becomes a blocker rather than an observation |
| A5 | smartnode spec: 4-6,000 nodes at 8 cores / 16 GB / **512 GB SSD** | trí's contract paper, May 2023 | every storage figure moves; A1's margin is set by this |
| A6 | contract jobs create "a special type of transaction"; results go on chain **or** a smartnode database | same | the load shape changes, and C11 says a payload can be 27× the 373 B in F30 |

## Decisions

| ID | date | decision | reopen when |
|---|---|---|---|
| D1 | 11 Sep | Smartnodes hold everything; retention, sharding, challenges, incentives, repair deferred | sustained load > F32, or growth crosses 1 TB/yr/node |
| D2 | 12 Sep | the Smartnode attestation is a **record**, not a validity gate | a decision to make RTM a hybrid chain, which is not the availability question |
| D3 | 13 Sep | the commitment format is the design (provisional) | superseded by D4 |
| D4 | 17 Sep | **the fork is closed**: commitment format, target = commitments filling a 2-8 MB block | the probe (D9) finding the acceptance layer intractable |
| D5 | 17 Sep | the state root is **optional** | A2 failing, or replay bootstrap becoming impractical |
| D6 | 17 Sep | the dual validation path stays **live**, not deferred | answers from trí on what is attested and what it may change |
| D7 | 18 Sep | design for IS **on** and healthy quorums; mainnet's current state is temporary | — |
| D8 | 16 Sep | the relay cap re-index is **not** a standalone upstream PR | upstream adopting the decoupling direction |
| D9 | 18 Sep | 0.1's kill criterion: not tractable only if a scenario needs an invariant **relaxed** | — |
| D10 | 18 Sep | our `docs/` never rides an upstream PR; nothing on upstream GitHub without asking | — |
| D11 | 11 Sep | taken now because retrofitting is a migration: a delete-capable body store, a non-punitive historical miss, and never pruning commitments | — |

## Retractions

What we believed, what is true, and what killed it. Kept in one place so "we no longer believe X"
is findable.

| ID | we said | actually | killed by |
|---|---|---|---|
| R1 | the cascade was an absorption gap | it is relay throughput | running at an absorbing cadence |
| R2 | relay cost is per-transaction, not per-peer | ~40% is per-peer (F3) | normalising CPU by delivered load |
| R3 | 1,500 tx/s is reachable on the current design "with headroom" | relay delivers ~930 (F1) | the twelve-node swarm |
| R4 | build-whole-split-after is a live option | it cannot express a commitment-sized target | D4 |
| R5 | the state root is a hard prerequisite | optional under A1+A2 | D5 |
| R6 | validation shows no quadratic signal | true for P2PKH only; multisig shows it plainly (F14) | the multisig arm |
| R7 | today's worst-case block hashing is ~0.4 s, later ~4 GB / 8-16 s | ~49 s through an uncounted path (F15) | F13 |
| R8 | the compact-block `uint16_t` is a wire limit | an explicit guard; the wire form is CompactSize (C10) | reading `blockencodings.h` |
| R9 | a committed state root makes an attested-invalid transaction detectable | the root commits to **agreement**, not validity | the cross-check |
| R10 | skipping script checks at acceptance merely relocates the cost | connect pays it **batched** on the check queue, which acceptance cannot do | F10 |
| R11 | parallel acceptance is a port-and-go | naive pooling is slower than stock (F10) | the §17 arms |
| R12 | three spork-dependent costs are inert | inert **today**, real for the design | D7 |
| R13 | all eleven PR branches revert upstream's #444 | none of them touches that file | comparing against the merge base, not develop's tip |
| R14 | the design's anchors are valid at tag `2.0.3.01` | they were only ever valid on `perf/throughput-rig` | resolving `validation.cpp:419` at the tag |

---

## Open — named so they are not mistaken for known

| what | why it matters | who answers |
|---|---|---|
| F4, F7, F30, F32 | four numbers with no single value | a measurement at a stated regime, or one choice recorded here |
| F29 | `DisconnectTip` is the probe path most likely to fire the kill criterion | phase B of 0.1 |
| F18 at quorum 50 | the InstantSend gap is priced from a quorum a tenth the live size | 0.4, the smartnode swarm |
| A6 | the load shape, and whether a contract transaction is 373 B or 10 kB | trí |
| the work cap's value | F12 gives the constant; the budget is a choice | 1.2 |
