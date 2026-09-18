# Docs

Nine live documents, in three lifecycles — **living** (revised in place, always current),
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
| `architecture-plain-language.md` | **the same design without the code.** What it is and why it holds, for reading and for handing to someone else. Currently v2. |
| `build-plan.md` | **the schedule.** Phased components, effort, design certainty, the gates, and what is deferred with its tripwire. |
| `perf-constants.md` | **constants, read from source.** The single source of truth for any constant. Do not restate values elsewhere — cite this. |
| `perf-results.md` | **the measurement log**, chronological. Entries stand as measured; a correction banner flags conclusions later overturned. |
| `upstream-ledger.md` | **how this tree differs from upstream**, including local modifications that change behaviour. |
| `pre-decoupling-checklist.md` | **the pre-work record.** What was measured before the build plan existed, and what has been explicitly descoped and why. Superseded as *the plan* by `build-plan.md`. |
| `asset-cache-drag.md` | the per-ATMP asset-cache copy: measurement, fix, and follow-ups. |

## Two standing warnings

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
purpose. **Baseline 2026-09-18: 166** (design 119, build-plan 39, plain-language 6, ledger 2).
Run `./check-numbers.py --show 10` to see the worst, or `--max N` to fail in CI once a budget is
agreed.

## Other directories

- `outbound/` — PR descriptions, issue drafts, and messages. Text destined for elsewhere; may
  diverge from what was actually posted.
- `platform/` — **the second subject.** `architecture-decisions.md` is the contract platform's
  architecture, cited by the design's §17 and the plan's phase 5. It lives here rather than in
  `archive/` because it is live work that has not started, not work that stopped.
- `archive/` — superseded or parked; **every file carries a header giving why, what replaced it,
  when, and why it is kept.** `throughput-bottleneck.md` (headline overturned by F1; its five
  unique measurements were logged first), `pre-decoupling-checklist.md` (superseded as the plan;
  its descoping record moved into `build-plan.md`), `asset-cache-drag.md` (closed — F17, PR #481), `expensive-tx-test-design.md` (executed; results are in
  `perf-results.md`), `mempoolaccept-port-analysis.md` (parked: acceptance had ~3x
  headroom against the working target of the time; the v8 target is 520-2,083 tx/s, see
  `build-plan.md`), `architecture-decisions.md` (a different project — the contract platform,
  and cited by §17 and phase 5, so referenced rather than abandoned).
