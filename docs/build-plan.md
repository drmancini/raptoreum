<!-- lifecycle: living — revised in place, always current
     owns:      the schedule: what gets built, in what order, effort, design certainty, the gates
     not mine:  the design (transaction-decoupling.md), values (findings.md), measurements (perf-results.md)
     known debt: this file carries dated progress notes and measured figures, which its own
                class forbids. They belong in the log and in findings; see check-numbers.py. -->

# Build plan — transaction decoupling

**Version 1, 17 September 2026.** Owns the **schedule**: what gets built, in what order, how
long, and which parts are unknown. The design itself lives in `transaction-decoupling.md` (v8);
the measurements behind every figure are in `perf-results.md`; constants are cited from
`perf-constants.md`. Where this document and §13 of the design disagree about order or effort,
this one wins.

## The design point

Commitments fill a 2-8 MB block every two minutes (D-4).

| | 2 MB of commitments | 8 MB of commitments | from |
|---|---|---|---|
| identifiers per block | 62,500 | 250,000 | K-1, 32 B each |
| throughput | **520 tx/s** | **2,083 tx/s** | D-4 |
| body bytes per block | 23 MB | 93 MB | F-30 × the count |
| body bandwidth | ~195 kB/s | ~780 kB/s | derived |
| vs today (2 MB of bodies) | 11.6× | 46× | K-1, F-30 |

**Peaks and bursts, not sustained load** (RTM team, relayed by the owner 2026-09-17). Capacity
is therefore sized for the burst and storage for the average — which is what keeps retention and
the state root out of this plan. Both assumptions carry tripwires; see the last section.

**One fork** (D-14). The format, the 8 MB `MaxBlockSize` (K-1) and the `MAX_PROTOCOL_MESSAGE_LENGTH` raise (K-7) all
ride the same activation, with 2 MB mined by policy afterwards (D-14). Splitting them would make the
higher target a second hard fork and a second ecosystem event for no benefit.

**One genuine unknown.** The acceptance layer. No production chain has a block whose acceptance
depends on a network fetch, so there is nothing to copy and phase 0 exists to price it.

**Effort.** 54-90 weeks of components; roughly 12-20 months of solo calendar time with phase 3
interleaved. Estimates include tests and were revised upward twice under adversarial review.

---

## Phase 0 — de-risk before committing · 5-7 weeks

Nothing in phase 1 starts before 0.1 returns a verdict.

> **0.1's kill criterion, written down before it runs.** The acceptance layer is **not
> tractable**, and §15 becomes the design, if making any probe scenario safe requires
> **relaxing an invariant that today catches database corruption** — `CheckBlockIndex`'s
> `HAVE_DATA ⇒ nTx > 0 ⇒ VALID_TRANSACTIONS ⇒ candidacy` chain, or `ConnectTip`'s guarantee
> that a selected block is readable — rather than adding a state alongside it. Weakening one of
> those trades a liveness problem for a silent-corruption problem, which is a worse design than
> not decoupling.
>
> Two softer signals, neither a kill on its own: needing more than one status bit and one
> validity rung in the block index is a **cost** signal, to be re-estimated rather than
> abandoned; and failing to reach "converges once bodies arrive" within two weeks is evidence
> about the layer rather than about the probe.
>
> **Pass** is all scenarios reaching: never asserts, never marks the block invalid, never wedges,
> and always converges once the bodies arrive.

| # | Component | Effort | Design |
|---|---|---|---|
| 0.1 | **Acceptance probe. Phase A DONE 2026-09-18** — the flag exists (`-perfwithholdbody` / `-perfwithholdheight`, failing `ReadBlockFromDisk`'s index overload, which 19 call sites reach) and the blast radius is measured: `VerifyDB` calls a missing body **database corruption** and refuses to start, P2P serving **aborts the process**, `ConnectTip` hits `AbortNode`, RPC degrades gracefully. No kill — each needs a predicate consulted, not an invariant relaxed. `DisconnectTip` is the one open path and the likeliest place for the criterion to fire. Phase B (the split plus the remaining scenarios) is the 3-5 d item. A debug-only flag that withholds bodies for a chosen block, plus a scenario suite: restart while incomplete, peer churn mid-fetch, a competing tip at equal work, a ChainLock arriving for an incomplete block, a reorg across one, `-reindex`. Runs on today's serialization, needs no fork. **Must carry a written kill criterion.** | 3-5 d | straightforward |
| 0.2 | **Characterisation tests.** Pin what the node accepts and rejects across normal and multi-input payments, every asset op, futures, every special-tx type, InstantSend interactions, and every rejection path. Tier 1 — strictly before decoupling. Runs on the rig host; mario's Python 3.13 has no `asyncore`. | 2-3 w | straightforward |
| 0.3 | **Two cheap measurements.** `spork show` and `getblock <tip> 2` on the mainnet node — sporks 2, 3 and 19 are compiled off and whether they are live decides four priced items below. And fix `bench_inputs.py` (size-scaled fee, non-overlapping UTXOs, ≤675-input ceiling) to calibrate 1.2's work unit. | 1-2 d | straightforward |
| 0.4 | **Smartnode swarm bring-up.** ProTx registrations, collateral, DKG and quorum formation on the 12-node WAN swarm — never attempted. Needs a test-only patch: `-llmqtestparams` overrides only size and threshold, leaving `dkgBadVotesThreshold`, `signingActiveQuorumCount`, `recoveryMembers` and `keepOldConnections` at three-member values. Turns every existing figure from an upper bound into a measurement. | 2-3 w | fiddly, known |

## Phase 1 — consensus surface, one fork · 15-31 weeks

| # | Component | Effort | Design |
|---|---|---|---|
| 1.1 | **Block format.** Second `CBlock` serialization — coinbase in full, every other entry a bare 32-byte identifier. Selector is a stream flag negotiated by service bit, never a header bit. Raw-commitment read API distinct from a materialising one; materialisation produces a fresh object so `ConnectBlock`'s own `CheckBlock` cannot short-circuit on `fChecked`. | 1-2 w | straightforward |
| 1.2 | **Block resource budget** (§1A). Sigop cap re-based to committed count, a per-transaction work cap, and a consensus body-byte cap — one joint decision, because the sigop cap is also what bounds worst-case hashing. **0.3 measured the unit: an accurate count over *spent* scripts, since legacy counting charges an 800-input bare-multisig spend 2 sigops for 2.35 s of work; the constant is ~130 µs per sigop.** | 2-3 w | design-sensitive |
| 1.3 | **Acceptance layer.** Having the identifiers and having the bodies become two facts: a new status bit (a persisted block-index format change), a new validity rung, and ~30 sites above `ConnectBlock` — chain selection, candidate maintenance, `CheckBlockIndex`'s invariants, `NewPoWValidBlock`, pruning, `VerifyDB`, crash replay, `-reindex`, and the `InvalidateBlock` / `MarkConflictingBlock` / `EnforceBestChainLock` interactions. Plus a durable retry queue with per-block exponential backoff that never keys off accumulated work. | **3-6 mo** | **unknown** |
| 1.4 | **Fork envelope** (D-14). The 8 MB cap (K-1) and the message-length raise (K-7) ride this activation; 2 MB by policy. | in 4.6 | straightforward |

**What cold connect costs, and why the cache is load-bearing** (measured 0.3). A full commitment
block of *ordinary* two-in/two-out payments, when the bodies were never seen at acceptance:
~16-20 s single-thread at 2 MB and ~65-78 s at 8 MB (F-16); ~7 s and ~28 s on a 4-thread box; ~4-5 s and
~17-20 s on the Smartnode spec of X-5. **Warm — validated at acceptance — the same block is
~0.8 s** (F-15b, F-16). A 20-25× swing, which makes three things follow: 2.3's relay cap re-index is what keeps
the cache hitting, not just what makes reconstruction possible; `-maxsigcachesize` in 4.5 is
consensus-adjacent sizing rather than tuning, since K-4's 524,288 entries is only 2.1 blocks at 8 MB;
and the cold column is a third independent argument for mining 2 MB by policy (D-14), beside relay and
storage.

**Why 1.2 is not optional** (K-2, F-13). `MaxBlockSigOps() = MaxBlockSize()/50` is 40,000 at 2 MB, and
`GetLegacySigOpCount` counts sigops per output (F-13), so a two-output payment is 2. A 2 MB commitment
block naming 62,500 payments carries 125,000 sigops and is **invalid** (K-2) — the cap admits about
20,000, i.e. **167 tx/s, not 520** (K-2) — and the miner silently stops filling. It is the relay cap
bug in a second place: a consensus limit indexed to block bytes, which stop tracking what the
block commits to.

## Phase 2 — data plane · 7-11 weeks

| # | Component | Effort | Design |
|---|---|---|---|
| 2.1 | **Body store.** Third flat-file series in block order beside `blk*.dat` and `rev*.dat`, with a per-block offset table — block order because assembly is the hot path. Written during `AcceptBlock` transactionally with the commitment write, **never** at mempool acceptance. Self-delimiting for `-reindex`, delete-capable, and it drops losing siblings' bodies once buried. | 2-3 w | straightforward |
| 2.2 | **Fetch protocol.** `(height, index/range)` **plus `(blockhash, index/range)`**, since a position is ambiguous off the active chain. Served off `cs_main` on the block path from the store's own height index; NOTFOUND ≠ timeout; works during initial block download; range-batched; responses chunked; per-connection budgets folded into upload accounting; multi-source. | 4-6 w | rate-limit policy and multi-source scheduling unknown |
| 2.3 | **Relay cap re-index.** `InvBroadcastMax()` reads a throughput target instead of `MaxBlockSize()`. Mandatory at any target — the shipped cap delivers 50-65 tx/s per peer; 50,000 was measured to converge 1,500 tx/s to sixteen peers. | 1 d | straightforward |
| 2.4 | **New-smartnode onboarding.** Multi-source body fetch plus verification of the store at rest, so one corrupt or deliberately erasing source cannot propagate its hole to every node that joins later. | 1-2 w | straightforward |

**2.2 is the tip critical path, not a history service.** A worst-case `BLOCKTXN` at the 2 MB
design point is 62,500 × F-30 = 23 MB (F-33), a sender above K-7's limit is disconnected, and nothing in the
compact-block path chunks (K-10). Any node ~8,400 transactions (~16 s) behind therefore loses compact
reconstruction, and the whole-block fallback carries no bodies under this format.

## Phase 3 — existing-code prerequisites · 4-5 weeks, independent

Interleave anywhere. 3.1 gates 2.2; the asset fixes want to land before activation regardless.

| # | Component | Effort | Design |
|---|---|---|---|
| 3.1 | **DIP8 signing-attempts process.** Deliberately skipped today, so competing blocks at one height split the quorum's signatures and no ChainLock forms. Decoupling makes that condition *schedulable* rather than a race, because identifiers arrive instantly and body release is the attacker's choice. **Gates 2.2.** | 1-2 w | straightforward |
| 3.2 | **Persist the round-voting count at connect.** `NodeRoundVoting` reads 720 blocks of bodies with an assert and its result gates consensus (R-17 retracts the claim that this sets the window floor). Persisting the per-block fact removes the rescan — and with it the only stated reason the window floor is 720, which then needs a new justification. | 3-5 d | straightforward |
| 3.3 | **ChainLocks `Cleanup` rewrite.** Replace the O(mempool) `GetTransaction` walk under `cs_main` + `mempool.cs` every 30 s — 1.9 s of stall at 3M entries, though R-25 says it is not what caps throughput — with event-driven pruning. | 1 w | straightforward |
| 3.4b | **Asset-cache follow-ups** — recovered from `archive/asset-cache-drag.md`, where they were the only copy: bound or evict the global in-memory cache, which grows without eviction from the `LoadAssets` floor; elide the copy for asset transactions too, since they still pay it in full; and narrow the residual surface, which after #481 is asset-typed transactions only. | 1 w | straightforward |
| 3.4 | **Asset fixes.** Intra-block mint visibility, undo keyed by `(assetId, tx index)`, and the distribution-type check that accepts any value. No longer fork-critical now asset state is uncommitted, still real divergences. | 1 w | straightforward |
| 3.5b | **Parallel-acceptance sizing**, recovered from `archive/mempoolaccept-port-analysis.md`: the behaviour-preserving Dash `MemPoolAccept` port alone is **1,000-1,500 lines**, which is the only sizing we have for phase 5's parallel work. | — | reference |
| 3.5 | **Already proposed upstream.** Per-ATMP asset-cache deep copy (#481) and the socket-handler busy loop (#480), both open against `develop`. | — | submitted |
| 3.6 | **Stale anchors.** `nMinimumChainWork` at block 421457, a `defaultAssumeValid` written with a letter `o` so it parses to zero, a top checkpoint at 394273. Low priority, but they are the floor under any eclipse argument. | 1 d | straightforward |

## Phase 4 — hardening, interfaces, activation · 13-18 weeks

| # | Component | Effort | Design |
|---|---|---|---|
| 4.1 | **Safety hardening.** One `HaveBodies` predicate plus an audit of every path that may materialise a block — which alone closes the 37-byte remote crash where the serving path checks "we have this block" then asserts on a failed read. Also: relay only what you can assemble; windowed nodes stop advertising `NODE_NETWORK`; `EnforceBestChainLock` gated on bodies with its `assert(false)` made graceful; the ChainLock safety walk flipped to fail closed; stall rules for body fetch; the serving privacy regression; and the paths that break without bodies — ZMQ `rawblock`, bloom-filtered blocks, block filters. **Gates 4.6.** | 3-4 w | straightforward |
| 4.2 | **Coverage telemetry.** Per-peer miss rates and per-*range* coverage, passive, shipped with the protocol. Honest loss is oldest-first, file-aligned, scattered, or a shrinking gap; deliberate erasure is contiguous, mid-history, aligned to nothing and stable. | 1 w | straightforward |
| 4.3 | **Index-build paths.** Every index is built by replay today. The asset index is a pure aggregate over unspent asset outputs, so it gets a build-from-coins pass — the `-reindex`-only route is dead on a windowed node. Wallet rescan becomes range-driven, and anything returning transaction contents returns an explicit "not held here". | 1 w | straightforward |
| 4.4 | **Mining and template.** Commitment-mode `getblocktemplate`: cache the template, drop the full `ConnectBlock` per poll (seconds of `cs_main` and tens of MB of JSON at 62,500 names). `submitblock` accepts both serializations. Mining hardware is untouched — the 80-byte header is byte-identical. | 3-4 w | in-node known; **external pool coordination is the schedule risk** |
| 4.5 | **Constants and policy.** `maxmempool` burst-sized (≥459 MB once K-12's gate is counted, against K-8's default), `-maxsigcachesize`, `BLOCKTXN` chunking, compact-block index widening (~7 lines — and 62,501 *fits* `uint16_t`, which is the trap), the relay in-flight constants, and a fee floor scaling with body bytes. | 1-2 w | straightforward |
| 4.6 | **Migration and activation.** One `UpdateManager` bit carrying format, cap and message length. Dual-format window, old nodes' unpriced serving obligation, the downgrade surface, the no-rollback question, and an activation criterion that reads 4.2's data rather than hashrate signalling, which cannot see coverage at all. | 4-6 w | partly unknown |

## Phase 5 — attested transaction class · 10-18 weeks, trí-gated

Kept live rather than deferred (owner, 2026-09-17). The throughput case for attestation is dead;
the structural one is not — a Spark job's result is not verifiable by anyone, so for that
transaction class a quorum signature is the only validity rule available. See §17 of the design.

| # | Component | Effort | Design |
|---|---|---|---|
| 5.1 | **Attestation ceiling, measured properly.** Quorum of K-13's live size with async threshold recovery (F-21) and the 100 ms `SendMessages` cadence addressed. Every existing figure comes from 3-13 member quorums. Needs 0.4. | 1-2 w | straightforward after 0.4 |
| 5.2 | **Batched attestation.** One threshold signature over a Merkle root of N items, partial-conflict semantics, the mining-gate interaction. The gate for everything attestation-based: one lock costs ≥1.15 ms of BLS (F-22) against ~118 µs of ECDSA saved (F-11), so break-even is 10-20 transactions per signature. Shared with InstantSend batching — build once. | 3-5 w | partly unknown |
| 5.3 | **Admission-only shortcut.** Acceptance skips script checks for attested transactions and does not cache; `ConnectBlock` verifies cold on the parallel queue. Consensus untouched, trust boundary is the one islocks already impose, a lying quorum cannot fork. The baseline any consensus version must beat. | 2-3 w | straightforward |
| 5.4 | **Attested transaction type.** A new `nType` whose validity rule is a quorum threshold signature over a declared result, plus the rules bounding what it may change. `ContextualCheckTransaction` whitelists types, so it is a fork either way. Blocked on three answers from trí: what is attested, what it may change, and whether ordinary payments are in scope. | 4-8 w | **unknown, scope not ours** |

## Measured as not needed — the descoping record

Recovered from `pre-decoupling-checklist.md` when it was archived. Each was once on the required
list and was disproven by measurement, so each is here to stop it being rediscovered.

| item | why it is off the list |
|---|---|
| **parallel validation / MemPoolAccept port** as a v1 blocker | acceptance is not the binding constraint (F-7); and the naive version is *slower* than stock (F-10) |
| **relay ordering rewrite** (the O(backlog) heap) | `-perfinvnosort` moved throughput <1% at the operating point; the "~40% of msghand" figure came from a 300k backlog at cap 50k-100k, a regime the target never reaches |
| **`getblocktemplate`**'s full `ConnectBlock` per poll | decoupling dissolves the hard part — a commitment block's bodies were validated at acceptance. Revisit trigger: sustained mining at scale (and see 4.4) |
| **message-size raises** as separate work | the send loop already chunks at `MAX_INV_SZ`; no splitting needed. The `MAX_PROTOCOL_MESSAGE_LENGTH` raise is still required, as part of 1.4 |
| **`dbcache` / `maxsigcachesize`** as acceptance fixes | swept at saturation with no effect across 13× and 16× raises, application confirmed in the node log; the ceiling is real compute. **Note:** `-maxsigcachesize` *is* required, for a different reason — K-4 holds only 2.1 blocks at 8 MB (4.5) |

## Gates

Four, and each carries real information:

- **0.1 → 1.3.** The probe kills or confirms the acceptance layer in a week instead of two months.
- **0.4 → 5.1.** No quorum measurement is meaningful until smartnodes run on the swarm.
- **3.1 → 2.2.** The fetch protocol is what makes a quorum split schedulable, so the process that
  lets a quorum converge has to exist first.
- **4.1 → 4.6.** Hardening gates activation rather than trailing it.

## Deferred, each with a tripwire

| deferred | brought back by |
|---|---|
| **Retention, sharding, challenges, repair** | sustained load above F-32's 80 tx/s, or growth crossing 1 TB per node per year (K-14) |
| **State root and snapshot sync** — and §7A.6's canonical asset element form with it, which was the hardest unsolved design problem | replay bootstrap becoming impractical, or the Smartnode tier thinning under storage pressure |
| **Attributable attestation shares** | ships with retention — a challenge can only penalise a party that promised something, and a recovered threshold signature names nobody |
| **Relay structural work and parallel acceptance — as blockers** | bursts that stop draining. A 60 s burst at 2,083 tx/s leaves ~71,000 transactions of backlog, clearing in ~80 s at the measured 900 tx/s of delivery |

> **InstantSend batching (5.2) was on that list and should not have been.** Under D-7 the design
> assumes InstantSend on, and mempool signing at the design point then needs roughly 27× the measured
> capacity (F-18) — so 5.2 is a **prerequisite of the design point**, not a deferral. Deferring it
> would only be coherent under a decision to ship without InstantSend, which nobody has taken.
> It keeps its number because that is where its design work sits.
| **Bounded prefill** | nothing depends on it; a latency optimisation |

## Assumptions this plan rests on

**X-1 — the target is peak, not sustained.** At the 512 GB Smartnode specified in RTM's contract
paper, the practical sustained ceiling is ~10-30 tx/s depending on replacement cycle (K-14, F-32). Tripping it
brings back retention first, then the state root.

**X-2 — Smartnodes hold the entire history.** This is what keeps replay bootstrap available and
the state root optional.

**Caveat on both.** `vExtraPayload` runs to K-11's 10,000 B, so F-30's 373-byte
body figure behind every storage and bandwidth number here could be off by up to 27× once the
contract layer's new transaction type is specified.

> **SCOPE, standing (owner, 2026-09-18).** Mainnet's current spork configuration is
> **deliberate and temporary**: InstantSend is switched off on purpose and **will be switched
> on**, and the quorum layer is broken and **will be fixed before decoupling**. Neither is this
> project's scope. **Design for sporks 2, 3 and 19 all on, mempool signing enabled, and healthy
> quorums** — the measurement below records what is *currently observable*, not what to design
> against, and nothing in this plan may be descoped on the strength of it.

**Answered by 0.3 on 2026-09-18** (mainnet spork state read from a production node; see
`perf-results.md`). Sporks **2 and 19 are ON, 3 is OFF**, and InstantSend mempool signing is off
because spork 2 carries a timestamp rather than `0`. Three costs are therefore **not measurable on
mainnet today** — the ChainLock safety walk is skipped, there is no ten-minute mining gate, and
no islocks are produced outside the retroactive path — but per the scope note above, **all three
are real for the design** and stay fully priced:

| cost | why it binds under the designed configuration |
|---|---|
| **every ChainLock-signing smartnode must converge** | with spork 3 on, `TrySignChainTip` refuses to sign a tip holding any transaction that is not islocked and is younger than 600 s, and `txAge` is 0 when the txid is absent entirely. Mainnet rotates a 200/400-member quorum, so in practice that is every smartnode. |
| **`maxmempool` sized with the mining gate** | ≥459 MB at the design point once `IsTxSafeForMining` holds unlocked transactions for K-12's ten minutes, against K-8's default |
| **per-node islock BLS verification** | `ProcessPendingInstantSendLocks` gates on `IsInstantSendEnabled` only, with no smartnode check — ≥1.15 ms per message, 0.6 of a core at 520 tx/s and 2.4 cores at 2,083, on the single `rtm-isman` thread |

**So 5.2 (batched attestation) is a prerequisite of the design point, not a phase-5 option.**
Mempool signing at the design point needs roughly 27× the measured capacity (F-18), so the batching that
5.2 builds is what makes InstantSend survivable anywhere on D-4's range. It stays
numbered in phase 5 because that is where its design work sits, but it is required, not
conditional.

0.3 also observed that ChainLocks have not formed in ~14 months (best chainlock 1,122,354 against
a tip of 1,432,200). **That is the known broken quorum layer, it is out of scope here, and RTM
will fix it before decoupling.** It is recorded only because it is why the quorum-dependent
figures above cannot be measured on mainnet today — which is what 0.4's swarm bring-up exists to
work around.
