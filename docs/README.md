# Docs

Seven live documents. Everything else is outbound text or archive. If a fact appears in two
places it will eventually disagree with itself, so each doc below owns its subject and the
others reference it rather than restating.

## Live

| doc | owns |
|---|---|
| `transaction-decoupling.md` | **the design.** What we are building and why. |
| `throughput-bottleneck.md` | **what limits throughput**, measured, and the errors that produced earlier wrong answers. |
| `perf-constants.md` | **constants, read from source.** The single source of truth for any constant. Do not restate values elsewhere — cite this. |
| `perf-results.md` | **the measurement log**, chronological. Entries stand as measured; a correction banner flags conclusions later overturned. |
| `upstream-ledger.md` | **how this tree differs from upstream**, including local modifications that change behaviour. |
| `pre-decoupling-checklist.md` | **the plan.** What must happen before/around decoupling, and what has been explicitly descoped. |
| `asset-cache-drag.md` | the per-ATMP asset-cache copy: measurement, fix, and follow-ups. |

## Two standing warnings

**The rig tree is not shipped RTM.** It carries local, uncommitted modifications that change
behaviour — `MAX_DIP0001_BLOCK_SIZE` is 8 MB here against 2 MB upstream, which changes block
capacity and the relay cap derived from it. Reading a rig value as a shipped value has already
produced one wrong conclusion. Run `git diff upstream/develop -- src/` before quoting anything
as upstream behaviour, and see `upstream-ledger.md`.

**Measure at the operating point, not only at saturation.** Two conclusions in this project
were confidently wrong because a subsystem's capacity was measured while a shared thread was
saturated by something else, and because rates were computed over a fixed duration when the
work ran past it. Both are recorded in `throughput-bottleneck.md`.

## Other directories

- `outbound/` — PR descriptions, issue drafts, and messages. Text destined for elsewhere; may
  diverge from what was actually posted.
- `archive/` — superseded or parked. `expensive-tx-test-design.md` (executed; results are in
  `perf-results.md`), `mempoolaccept-port-analysis.md` (parked: acceptance has ~3x headroom at
  the v1 target), `architecture-decisions.md` (a different project — the contract platform).
