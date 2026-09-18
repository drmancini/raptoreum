<!-- archived: 2026-09-18
     why:      parked: acceptance is not the binding constraint (F7 against the v8 target)
     replaced: build-plan 5.x and findings.md F10 carry the current position on parallel acceptance
     keep:     its port-size estimate (1,000-1,500 lines) is the only sizing we have -->

# Preliminary analysis: adopting Dash's MemPoolAccept into RTM

> **PARKED (2026-09-16).** This port existed to enable parallel validation. Measurement has
> since shown acceptance sustains **~4,400-4,500 tx/s** against a v1 target of 1,500 — about
> 3x headroom — so parallel validation is not on the v1 path and neither is this port. The
> "~2.8x" figure below is an *estimate* derived from `-perfskipsigs` and has never been
> tested against a real parallel implementation. Revisit only for a higher-throughput v2.
> Current picture: `findings.md` for the values (F-7, F-10) and `build-plan.md` for where
> parallel acceptance now sits.

Scope: what it brings, what it touches, key risks, rough size. Based on reading RTM's
current acceptance path (`validation.cpp`) against Dash master's `MemPoolAccept`
(`dashpay/dash` `validation.cpp`). Preliminary — enough to scope and decide, not a port plan.

## Bottom line

**Feasible without a massive prerequisite, but not small.** The one refactor that would have
made this prohibitive — the modern `CChainState`/`ChainstateManager` chainstate object — RTM
**already has**. The real cost is (a) RTM's asset subsystem, which Dash has no equivalent of
and which is woven into RTM's ATMP, and (b) the validation-state split, which RTM lacks and
which Dash's version assumes. Both are manageable if the port deliberately *adapts to* RTM's
existing types rather than dragging in every upstream refactor.

## What it brings

- **A batch structure for parallel validation.** `AcceptMultipleTransactions` collects
  transactions into `Workspace`s and loops them — the natural point to gather `CScriptCheck`s
  across transactions and run one `CCheckQueue` batch. RTM's monolithic ATMP has no such seam.
- **Phase separation** (`PreChecks` / `PolicyScriptChecks` / `ConsensusScriptChecks` /
  finalize) with a per-transaction `Workspace`. Lets you parallelize the expensive phase and
  keep the lock-held phases serial, and makes **failure attribution** natural (each tx has its
  own state object).
- **Package acceptance** (parent+child atomic, CPFP) — RTM lacks it entirely.
- **Reduced divergence** on the hottest, most consensus-sensitive path — easier future
  backports.

## What RTM already has (reduces the port)

| dependency | status |
|---|---|
| `CChainState` / `ChainstateManager` (chainstate object) | **present** (`validation.h:615, 920`) — the big one |
| `CCoinsViewMemPool` | present (`txmempool.h:921`) |
| `ContextualCheckTransaction` | present (`validation.cpp:389`, old-style) |
| parallel script-check queue (`CCheckQueue`, `g_parallel_script_checks`) | present, used by `ConnectBlock` |

RTM refers to the chainstate via the `::ChainstateActive()` global rather than passing
`CChainState&` around, but the object exists — so this is a usage change, not a missing
subsystem.

## What RTM lacks (the cost drivers)

| gap | Dash uses | options |
|---|---|---|
| **split validation state** — RTM has only `CValidationState` (`consensus/validation.h:22`); Dash uses `TxValidationState` | throughout MemPoolAccept | (a) port the bitcoin#18191 state split — broad, touches everything; **(b) adapt MemPoolAccept to `CValidationState`** — contained to the port, recommended for a first pass |
| **mempool `Epoch`** (lockless traversal), package plumbing (`PackageValidationState`) | package path | only needed if adopting package acceptance; can be deferred |
| **`MempoolAcceptResult` / `PackageMempoolAcceptResult`** result types | new return types | keep a `bool + CValidationState` **compat wrapper** so call sites don't churn |
| **`m_chain_helper` (`CChainstateHelper`)** — Dash's special-tx/masternode integration seam | special-tx, InstantSend | RTM wires `CheckSpecialTx` directly; follow the same *pattern* for RTM's special-tx, and add asset handling Dash has no equivalent of |

## What it touches (the interface surface)

RTM's ATMP is `bool AcceptToMemoryPool(pool, state, tx, pfMissingInputs, bypass_limits, ...)`.
Real call sites (a **compat wrapper** keeps these unchanged):

- `net_processing.cpp:2242, 3199` — orphan handling, incoming tx messages
- `coinjoin/coinjoin-server.cpp:384, 519`, `coinjoin/coinjoin.cpp:358` — CoinJoin final tx + collateral
- `node/transaction.cpp:42` — RPC `sendrawtransaction` / broadcast
- `validation.cpp:512` — internal orphan reprocessing
- tests: `txvalidationcache_tests.cpp`, `txvalidation_tests.cpp`

Woven into RTM's ATMP that must be re-integrated into the new structure:

- **`CAssetsCache`** deep-copy + asset validation (`CheckNewAssetTx`, `CheckMintAssetTx`) —
  **no Dash equivalent**
- `CheckSpecialTx` (ProReg etc.), `existsProviderTxConflict`, `existsAssetTxConflict`
- InstantSend conflict check ("conflicts with locked TX", `validation.cpp:655`)
- CoinJoin DSTX handling
- `CalculateMemPoolAncestors`, the `CTxMemPoolEntry` construction

## Key risks

1. **Asset re-integration (highest).** RTM's asset subsystem is consensus-critical, woven
   into ATMP, and has **no Dash reference to copy from**. Getting it wrong breaks asset
   transactions silently. This is where the characterization tests earn their place — pin
   exactly what the old ATMP does with every asset op first.
2. **Validation-state adaptation.** `CValidationState` vs `TxValidationState` differ in how
   DoS scores and reject reasons are carried. Adapting MemPoolAccept to `CValidationState`
   risks subtle changes to *which* peers get banned / *what* reject codes are returned.
3. **Consensus parity.** Acceptance decides what enters the mempool and gets mined. Any
   behavioral drift is consensus-adjacent. Needs the functional suite green first (the
   `feature_llmq_is_cl_conflicts` flake must be resolved to have a clean baseline).
4. **CoinJoin / InstantSend integration.** RTM's versions may differ from Dash's; the
   collateral and final-tx acceptance paths need care.

## Rough size

For a **behavior-preserving first pass** that adapts to `CValidationState`, keeps a compat
wrapper (minimal call-site churn), re-integrates assets + RTM special-tx, and defers package
acceptance and the state split:

- MemPoolAccept class port/adapt: ~800–900 lines
- asset + special-tx re-integration: ~150–300 lines, high-care
- compat wrapper + result handling: ~50–100 lines
- characterization tests (prerequisite): substantial, separate
- **order of magnitude: ~1,000–1,500 lines changed**, most of the risk concentrated in the
  ~250 lines of asset/special-tx re-integration.

## Recommended staging

1. **Characterization tests of the current ATMP first** — pin accept/reject across the full
   surface (normal, multi-input, all asset ops, futures, special-tx, InstantSend, invalid
   paths). No-regret; the safety net for everything after.
2. **Behavior-preserving port** — adapt to `CValidationState`, compat wrapper, re-integrate
   assets. Goal: the new structure accepts/rejects identically (tests prove it). No
   parallelism yet.
3. **First parallel milestone** — batch the script checks in `AcceptMultipleTransactions`'s
   loop, measure against stock. This *is* the spike, folded into real code: confirms the
   ~2.8× before more investment.
4. **Optional later** — package acceptance, the validation-state split — only if they earn
   their keep; neither is required for the throughput goal.
