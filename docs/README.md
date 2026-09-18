# Docs

Six live documents, in three lifecycles — **living** (revised in place, always current),
**append-only** (the measurement log, never edited except by a forward-pointing correction), and
**frozen** (written once for an audience elsewhere: `outbound/`). `archive/` holds what stopped
being living, and every file there says why and what replaced it.

**One number, one home.** A measured value lives in `perf-results.md` and is named in
`findings.md`; a constant lives in `findings.md` read from source. Anywhere else, cite the ID.
A value restated in prose is how two documents come to disagree, and it is how this folder
already contradicted itself four times. Everything else is outbound text or archive. If a fact appears in two
places it will eventually disagree with itself, so each doc below owns its subject and the
others reference it rather than restating.

## Live

| doc | owns |
|---|---|
| `findings.md` | **what we know.** Every number and decision this project relies on, one line each, with the regime it was measured in and a status. **Living docs cite an ID (F12, C7, A1, D4, R9); they do not restate a value.** |
| `transaction-decoupling.md` | **the design.** What we are building and why, at the level of files, structures and states. Currently v8. |
| `build-plan.md` | **the schedule.** Phased components, effort, design certainty, the gates, and what is deferred with its tripwire. |
| `perf-constants.md` | **where a constant lives in the source** — the exhaustive symbol-and-file audit, plus the LLMQ and spork tables in full. `findings.md` is authoritative for a *value*; this says where to find it. |
| `perf-results.md` | **the measurement log**, chronological. Entries stand as measured; a correction banner flags conclusions later overturned. |
| `upstream-ledger.md` | **how this tree differs from upstream**, including local modifications that change behaviour. |

## Three standing warnings

**This branch is not shipped RTM.** `perf/throughput-rig` carries committed local modifications
that change behaviour — `MAX_DIP0001_BLOCK_SIZE` is 8 MB here against 2 MB upstream, which changes
block capacity and the relay cap derived from it, plus the test-only `-perf*` flags. Reading a rig
value as a shipped value has already produced one wrong conclusion. Run
`git diff upstream/develop -- src/` before quoting anything as upstream behaviour, and see
`upstream-ledger.md`. (An earlier version of this warning said "uncommitted"; the 8 MB change is
committed as `e55f029d6` and `git status` is clean, which makes it easier to miss, not harder.)

**Line numbers in these documents are hints, not addresses.** Every `file:line` anchor was read
on this branch and drifts with any commit touching that file — the acceptance probe alone moved
everything past `validation.cpp:111`. Resolve a citation by its **symbol**, not its line.

**Measure at the operating point, not only at saturation.** Two conclusions in this project
were confidently wrong because a subsystem's capacity was measured while a shared thread was
saturated by something else, and because rates were computed over a fixed duration when the
work ran past it. Both are recorded in `archive/throughput-bottleneck.md`, whose own first pass made all four.

## The lint

`check-numbers.py` reports numbers restated in living documents instead of cited from
`findings.md`. It is a budget to shrink, not a list of bugs — some derivations are shown on
purpose. **Baseline 2026-09-18: 158** — design 103, build-plan 47, ledger 4, platform 4. It fell from 204
when the design's §16 was rewritten: a measurement section had grown inside a design document,
and replacing its tables with ID citations removed 46 restated numbers in one pass. That is the
mechanism working as intended — the lint found the largest duplication in the folder. The first pass reported 166; the cross-check showed the regex was blind to bare
seconds, bare bytes, MiB, `×` multipliers, unitless consensus counts, `years`, and every
spelled-out quantity ("a few thousand", "six terabytes"), which is most of how the
plain-language companion is written. A lint that flatters you is worse than no lint.
**The budget is now enforced, not observed:** `./check-numbers.py --max 117` is the gate, set at
the number the folder actually carries. A new restated number fails it, which forces the choice
the rule exists for — cite the ID, or raise the budget on purpose. `--show 10` prints the worst
offenders.

The floor is not zero and should not be: a worked example, an arithmetic derivation shown on
purpose, and the design point stated in the document that owns the schedule are all content
rather than duplication. `build-plan.md` is exempted for durations, since a schedule that cannot
state weeks is not a schedule.

## Citing the source

A line number is not an address: any commit touching a cited file moves every anchor below it,
and the acceptance probe alone moved everything past `validation.cpp:111` by 5 lines. So a
citation **carries its symbol**:

```
`validation.cpp:MAX_STANDARD_TX_SIZE`          a symbol
`validation.cpp:scriptExecutionCache.insert`   a specific use, where a symbol appears often
`llmq/quorums_chainlocks.cpp:TrySignChainTip`  a path-qualified file
```

`check-anchors.py` resolves those against the tree and fails if one has been renamed or removed.
A key naming a function resolves to its **definition** — in this codebase a definition starts at
column 0 and calls are indented — so `CheckBlock` keys cleanly despite 32 occurrences. Where a
symbol is genuinely used in many places, key the use: `scriptExecutionCache.insert`,
`VARINT(obj.nStatus)`.

**29 citations are keyed and verifying; 165 are still `file:line` and unverifiable** — that is
the migration backlog, and `--list` prints it. New citations use the keyed form, and the
acceptance layer's are done first because phase B of plan item 0.1 edits exactly those
functions.

**Auto-rewriting the old ones was tried and abandoned**, which is worth recording: inferring the
symbol from nearby prose picks *a* symbol rather than *the* one meant, and it proposed moving
`validation.cpp:3026` to `:5485`. A heuristic that repoints a citation confidently is worse than
a stale number, because staleness is at least visible. Each unkeyed citation needs a human who
knows which line the sentence was about.

## Other directories

- `outbound/` — PR descriptions, issue drafts, and messages. Text destined for elsewhere; may
  diverge from what was actually posted.
- `platform/` — **the second subject.** `architecture-decisions.md` is the contract platform's
  architecture, cited by the design's §17 and the plan's phase 5. It lives here rather than in
  `archive/` because it is live work that has not started, not work that stopped.
- `archive/` — superseded or parked; **every file carries a header giving why, what replaced it,
  when, and why it is kept.** `throughput-bottleneck.md` (headline overturned by F1; its five
  unique measurements were logged first), `pre-decoupling-checklist.md` (superseded as the plan;
  its descoping record moved into `build-plan.md`), `asset-cache-drag.md` (closed — F-17, PR #481), `architecture-plain-language.md` (a prose copy of a living document is two clocks; its §§1-2 are still the best short explanation of the design), `expensive-tx-test-design.md` (executed; results are in
  `perf-results.md`), `mempoolaccept-port-analysis.md` (parked: acceptance had ~3x
  headroom against the working target of the time; the v8 target is 520-2,083 tx/s, see
  `build-plan.md`), `architecture-decisions.md` (a different project — the contract platform,
  and cited by §17 and phase 5, so referenced rather than abandoned).
