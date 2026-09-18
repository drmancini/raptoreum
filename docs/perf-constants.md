<!-- lifecycle: living — revised in place
     owns:      the exhaustive source-read audit: which symbol, in which file, and the LLMQ and
                spork tables in full
     not mine:  a value the design relies on — findings.md is authoritative for that (C-series).
                Where this file and findings.md disagree about a number, findings.md wins and
                this file is stale.
     warning:   read on perf/throughput-rig, which is not shipped RTM. See upstream-ledger.md. -->

# Constants — where each one lives in the source

> **The rig tree is not shipped RTM.** Constants below are upstream values unless marked.
> The perf working tree carries local modifications that change behaviour, and reading one
> of them as a shipped value has already produced one wrong conclusion. Known divergences:
>
> | constant | upstream | rig tree | effect |
> |---|---|---|---|
> | `MAX_DIP0001_BLOCK_SIZE` | **2,000,000** | 8,000,000 on `perf/throughput-rig` (commit `e55f029d6`), which is **this branch** | Changes block capacity and the relay cap derived from it. **Measurements exist on both**: earlier runs were taken on a 2 MB checkout and later ones on this branch, so the cap they reflect is 280/trickle or 1,120/trickle depending on the run. Each log entry states its own conditions, and `findings.md`'s `rig-8mb` regime marks the ones taken here. An earlier version of this row claimed every measurement reflects 2 MB; that was false once the 8 MB runs existed. |
>
> Check `git diff upstream/develop -- src/consensus/consensus.h` before quoting any
> consensus constant from this tree. Full divergence list: `upstream-ledger.md`.

Read on `perf/throughput-rig`. Every value below is quoted
from source, not assumed from Dash or Bitcoin. Values that differ from Bitcoin's are
marked.

## Relay and request

| constant | value | where |
|---|---|---|
| `INVENTORY_BROADCAST_INTERVAL` | 5 s | `net_processing.cpp:159` |
| `INVENTORY_BROADCAST_MAX_PER_1MB_BLOCK` | `4 * 7 * 5` = 140 | `net_processing.cpp:163` |
| effective cap per trickle | `140 * MaxBlockSize()/1e6` = **280** | `net_processing.cpp:4492, 4595` |
| trickle interval, inbound peer | 5 s (Poisson mean) | `net_processing.cpp:4515` |
| trickle interval, outbound peer | `5 >> 1` = 2 s | `net_processing.cpp:4520` |
| trickle interval, verified smartnode | `5 >> 1 >> 1` = 1 s | `net_processing.cpp:4520` |
| `MAX_INV_SZ` | 50,000 | `net.h:67` |
| `MAX_PEER_OBJECT_ANNOUNCEMENTS` | `2 * MAX_INV_SZ` = 100,000 | `net_processing.cpp:72` |
| `MAX_GETDATA_SZ` | 1,000 | `net_processing.cpp:95` |
| `INBOUND_PEER_TX_DELAY` | 2 s | `net_processing.cpp:74` |
| `GETDATA_TX_INTERVAL` | 60 s | `net_processing.cpp:79` |
| `MAX_GETDATA_RANDOM_DELAY` | 2 s | `net_processing.cpp:84` |
| `TX_EXPIRY_INTERVAL_FACTOR` | 10 | `net_processing.cpp:88` |

**Bitcoin differs.** Upstream's per-MB figure is `7 * INTERVAL` = 35. Raptoreum
multiplies by four, with the comment "we have 4 times smaller block times", giving 140.

**The derived relay ceiling, per peer link:**

| peer type | announcements per second |
|---|---|
| inbound | 280 / 5 = **56** |
| outbound | 280 / 2 = **140** |
| verified smartnode outbound | 280 / 1 = **280** |

A full 2 MB block of ~370-byte transactions every 120 s is about 5,400 transactions,
or **~45 tx/s**. The shipped inbound cap sits roughly a quarter above the chain's own
steady-state rate. Measurement therefore confirms a number the source already gives; it
does not discover one.

## Mempool and policy

| constant | value | where |
|---|---|---|
| `DEFAULT_MAX_MEMPOOL_SIZE` | 300 MB | `policy/policy.h:31` |
| `DEFAULT_MEMPOOL_EXPIRY` | 336 h | `validation.h:93` |
| `DEFAULT_MIN_RELAY_TX_FEE` | 1000 | `validation.h:83` |
| `DEFAULT_INCREMENTAL_RELAY_FEE` | 1000 | `policy/policy.h:33` |
| `DEFAULT_ANCESTOR_LIMIT` | 25 | `validation.h:85` |
| `DEFAULT_ANCESTOR_SIZE_LIMIT` | 101 kvB | `validation.h:87` |
| `DEFAULT_DESCENDANT_LIMIT` | 25 | `validation.h:89` |
| `DEFAULT_DESCENDANT_SIZE_LIMIT` | 101 kvB | `validation.h:91` |

The depth-5 corpus sits well inside the ancestor limit, so the A7 control is measuring
tracking cost, not a limit being hit.

## Block and validation

| constant | value | where |
|---|---|---|
| `MAX_DIP0001_BLOCK_SIZE` | 2,000,000 | `consensus/consensus.h:14` |
| `MaxBlockSize()` | that, when DIP0001 is active | `consensus/consensus.h:16` |
| max block sigops | `MaxBlockSize() / 50` | `consensus/consensus.h:22` |
| `COINBASE_MATURITY` | 100 | `consensus/consensus.h:28` |
| `DEFAULT_SCRIPTCHECK_THREADS` | 0 (auto) | `validation.h:106` |
| `MAX_SCRIPTCHECK_THREADS` | 15 | `validation.h:104` |
| `MAX_PROTOCOL_MESSAGE_LENGTH` | 3 MB | `net.h:81` |
| `DEFAULT_MAX_PEER_CONNECTIONS` | 125 | `net.h:95` |
| `DEFAULT_MAXRECEIVEBUFFER` | 5,000 kB | `net.h:107` |
| `DEFAULT_MAXSENDBUFFER` | 1,000 kB | `net.h:108` |

**A blocker the early patch lists missed** (now plan item 1.4 and 4.5). `MAX_PROTOCOL_MESSAGE_LENGTH`
is 3 MB. Raising `MaxBlockSize()` to 100 MB without raising this produces a miner whose
blocks cannot cross the wire at all. The send and receive buffer defaults are the next
thing to check after it.

## LLMQ on regtest

| parameter | `llmq_test` | `llmq_test_v17` |
|---|---|---|
| type | `LLMQ_5_60` (100) | `LLMQ_TEST_V17` (101) |
| size | 3 | 3 |
| minSize | 2 | 2 |
| threshold | 2 | 2 |
| dkgInterval | 30 blocks | 30 |
| dkgPhaseBlocks | 2 | 2 |
| dkgMiningWindow | 10 → 18 | 10 → 18 |
| dkgBadVotesThreshold | 2 | 2 |
| signingActiveQuorumCount | 2 | 2 |
| keepOldConnections | 3 | 3 |
| recoveryMembers | 3 | 3 |

Source: `llmq/quorums_parameters.h:413` and `:433`. Registered on regtest at
`chainparams.cpp:808-809`, where ChainLocks, InstantSend and Platform all use
`LLMQ_5_60`.

**The `-llmqtestparams` override is half a solution** (plan item 0.4). `-llmqtestparams=<size>:<threshold>` exists
(`chainparamsbase.cpp:33`, applied at `chainparams.cpp:956`), so a 50-member quorum
needs no chainparams patch. But the override reaches **only size and threshold**.
`dkgBadVotesThreshold`, `signingActiveQuorumCount`, `recoveryMembers` and
`keepOldConnections` stay at their three-member values, which is wrong for a fifty-member
quorum and would have to be patched separately. A run that sets only size and threshold
is not a realistic fifty-member quorum and must not be reported as one.

Also required: `ft/03-regtest-quorums`. Without it `UpdateLLMQParams` rescales on every
block and overwrites whatever the test chose.

## Sporks

> **These are COMPILED DEFAULTS, not what mainnet runs.** Every value in `spork.h` is
> 4070908800 (year 2099 = off), and a spork is active when its value is *below* the current time.
> Live mainnet was read on 2026-09-18 and is **not** the default: sporks 2, 17, 19 and 23 are ON.
> See **F23** in `findings.md`, which is authoritative. A reader who takes this table as mainnet
> state gets four things wrong, which already happened once.

| spork | number | compiled default | mainnet (F23) |
|---|---|---|---|
| `SPORK_2_INSTANTSEND_ENABLED` | 10001 | OFF | **ON** (mempool signing off) |
| `SPORK_3_INSTANTSEND_BLOCK_FILTERING` | 10002 | OFF | OFF |
| `SPORK_17_QUORUM_DKG_ENABLED` | 10016 | OFF | **ON** |
| `SPORK_19_CHAINLOCKS_ENABLED` | 10018 | OFF | **ON** |
| `SPORK_21_LOW_LLMQ_PARAMS` | 10020 | OFF | OFF |
| `SPORK_23_QUORUM_ALL_CONNECTED` | 10023 | OFF | **ON** |
| `SPORK_25_QUORUM_POSE` | 10025 | OFF | OFF |

Source `spork.h:37-45`, defaults `spork.h:74-78`. Regtest spork address:
`yaackz5YDLnFuuX6gGzEs9EMRQGfqmNYjc` (`chainparams.cpp`, regtest section).

## What this changes in the plan

1. **The relay ceiling is arithmetic, not an unknown.** The relay ceiling is 56 tx/s per inbound
   peer and is arithmetic, not an unknown.
2. **No quorum chainparams patch is needed**, but the `-llmqtestparams` override is
   partial and the remaining parameters need one.
3. **the message-length raise is required** — plan item 1.4.
4. **`-logtimemicros` already exists** (`init.cpp:827`), so instrumentation patch 6 is
   mostly unnecessary.
5. **`mininode.py` already speaks the protocol**, so the generator is a token bucket over
   existing code rather than a protocol implementation.
