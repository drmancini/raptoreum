# Functional test suite — nine pull requests

Replaces the single PR #470. Each targets `develop` and builds on the one before it,
so they must be merged in order. Until an earlier one merges, a later PR's diff on
GitHub shows the earlier work as well; that is expected for a stacked series.

Numbers quoted as "suite at this point" are measured, not estimated: a sweep built all
nine branches from clean and ran `test_runner.py -j12` under Python 3.11.9 at each one,
and stages 8 and 9 were re-measured after the fixes described there. That sweep ran
without `pyzmq` installed, so the three ZeroMQ tests skip in the per-stage numbers. With
`pyzmq` present all three pass; see "Known intermittent failures" at the end for the
full-suite figures with them enabled.

The count falls monotonically: 86 failures at stage 4, then 8, 5, 5, 0, 0. Where an
intermediate stage still fails, the cause is named in that PR and the fix is in a later
one, which is inherent to splitting a single change into nine reviewable pieces.

---

## 1 — contrib: add raptoreum_hash, a GhostRider module for the test framework

Branch `ft/01-ghostrider-hash` · 1 commit · 4 files, +208

The functional test framework builds and mines blocks in Python. Without a hash
function that matches consensus it cannot produce a header the node will accept, so
every test that mines a block by hand is unrunnable.

This adds `contrib/ghostrider-py`, a small CPython extension that binds Core's own
GhostRider implementation rather than reimplementing it. There is exactly one hash
function in the tree and the tests now use it.

No `src/` change. Nothing outside `contrib/` is touched.

The commit body explains the `PY_SSIZE_T_CLEAN` requirement: the `y#` argument format
needs that macro defined before `Python.h`, Python 3.10 through 3.12 raise
`SystemError` at call time without it, and 3.13 relaxed the rule. A module built and
smoke-tested only on 3.13 therefore looks correct while failing on every version the
rest of the suite can run on.

**Suite at this point:** still cannot run. The runner gates on a config key the build
does not emit; that is fixed in PR 4.

---

## 2 — test: remove tests that no longer describe this node

Branch `ft/02-prune-dead-tests` · 3 commits · 9 files, +1 −706

Three kinds of dead weight, one commit each:

- Seven stale copies of tests that were renamed upstream. Both names exist in the
  tree; the old copies were never updated and test nothing current.
- The governance test. Smartnodes do not run governance on Raptoreum; it is a Dash
  relic.
- The block reward reallocation test, which encodes Dash's reallocation schedule
  rather than this chain's.

Deletions only, apart from one line in `test_runner.py`.

**Suite at this point:** still cannot run, same gate as above.

---

## 3 — regtest: register test-only quorum types

Branch `ft/03-regtest-quorums` · 1 commit · 4 files, +90 −8

Two problems stop a regtest chain from ever forming a quorum.

The test quorum types are not registered on regtest, so nothing can sign. And
`UpdateLLMQParams` rescales quorum parameters to the smartnode count on every block,
which overwrites whatever size a test chose through `-llmqtestparams` and re-registers
production-sized types that a handful of local nodes can never fill.

This registers the test types on regtest and makes `UpdateLLMQParams` return early
there. Scaling to smartnode count remains a mainnet and testnet concern, untouched.

This is the only consensus-adjacent code in the series, and it is inside a regtest
branch. Mainnet and testnet behaviour is unchanged.

**Suite at this point:** unit tests pass; the functional suite still cannot run.

---

## 4 — test: make the functional suite runnable

Branch `ft/04-test-harness` · 3 commits · 11 files, +762 −195

The commit that turns the suite on.

- `test_runner.py` gated the whole run on a configuration key the build never emits.
  With the key absent it exited zero having run nothing, which reads as success. This
  removes the gate.
- `authproxy.py` and `wallet_util.py` are taken from upstream rather than carrying
  local drift.
- The framework is adapted to Raptoreum: chain parameters, the extra RPC surface, and
  the block and address helpers.

**Suite at this point:** 34 passed, 86 failed, 2 skipped.

That failure count is the point of this PR, not a regression. Before it the suite
reported success while running nothing. The 86 failures are pre-existing and become
visible here; PR 5 repairs them.

---

## 5 — test: repair the suite

Branch `ft/05-repair-suite` · 21 commits · 83 files, +1788 −1322

The bulk of the work, grouped by subject so each commit can be read on its own:
subsidy and founder fee assumptions, activation read from the update system rather
than BIP9, peer sync driven by the mocked clock, HD wallet assumptions, soft-fork
tests rewritten around rules that are always on, and then the rpc, wallet, feature,
mempool, p2p, tool, quorum, smartnode and evo tests in turn.

Five small `src/` fixes are included, each in its own commit alongside the test that
proves it:

| fix | file |
|---|---|
| separate the message caption from the message | `src/noui.cpp` |
| validate `-llmq-qvvec-sync` at startup | `src/init.cpp` |
| register the `-asmap` argument again | `src/init.cpp` |
| emit the `ischange` field `getaddressinfo` documents | `src/wallet/rpcwallet.cpp` |
| correct the "specified data directory" error message | `src/util/system.cpp` |

One test fix in this PR is a portability fix rather than a repair.
`wallet_multiwallet` asserted Boost's wording for a directory it cannot create, and
that wording changed: 1.77 raises `create_directory: Not a directory`, 1.84 raises
`create_directories: File exists`. `depends/packages/boost.mk` pins 1.84, so the
assertion failed for anyone who built the way the tree says to. It now matches either.
Verified passing against both versions.

Two assertions in `interface_bitcoin_cli.py` are commented out rather than weakened,
each with a comment saying why. One is restored in PR 8 once the code it depends on is
fixed. The other concerns `-getinfo` fields this client does not emit and is a
permanent divergence.

**Suite at this point:** 111 passed, 8 failed, 3 skipped.

The eight are known and none is caused by this PR. `rpc_deriveaddresses` (twice, plain
and `--usecli`) and `rpc_named_arguments` need code fixes that land in PR 8.
`wallet_createwallet` (twice) is named by upstream's runner index but the file itself
has never existed in this fork; PR 7 adds it. `feature_block`,
`feature_llmq_connections` and `rpc_masternode` each appear at this stage only and at
no other, which is contention at twelve parallel jobs.

---

## 6 — rpc, llmq and wallet fixes found by the suite

Branch `ft/06-rpc-and-llmq-fixes` · 6 commits · 14 files, +593 −57

Six defects, each with the test that catches it:

- `listsinceblock` did not filter conflicted transactions by depth.
- `smartnode payments` ignored its `count` argument.
- `logging` listed category aliases rather than the real categories.
- `getrpcinfo` did not return the `logpath` field it documents.
- Connections to quorums that fall outside the active window were never closed.
- The asset RPCs took `cs_main` before `cs_wallet`, the opposite order to the rest of
  the wallet, which is a lock inversion.

**Suite at this point:** 117 passed, 5 failed, 3 skipped: the same
`rpc_deriveaddresses`, `rpc_named_arguments` and `wallet_createwallet` entries, all
resolved by PR 7 and PR 8. The three single-stage flakes above do not recur.

---

## 7 — test: cover what nothing reached

Branch `ft/07-new-coverage` · 10 commits · 27 files, +1945 −18

New tests, no behaviour changes:

- Asset RPCs, what they accept and what they refuse.
- Future transactions.
- The founder fee.
- The query RPCs no test reached at all.
- Upstream tests the fork never took.
- `wallet_createwallet.py`, which was simply missing.

Three repairs to existing tests are included because they are coverage gaps rather
than failures: `feature_addressindex` picked an output at random, several tests
half-covered the RPC they named, and `create_block` now takes the node so hand-built
blocks are valid rather than accidentally so.

One of those repairs is worth calling out. `rpc_scantxoutset` pays its three scan
targets to the wallet's own addresses, so the thirteen sends that follow could select
and spend those very outputs, leaving the totals short by some subset of 0.001, 0.002
and 0.004. Measured at one failure in twenty-four runs. The outputs are now locked as
they are created, and their presence in the UTXO set is asserted before the wallet is
deleted, so a recurrence names its cause rather than surfacing as a wrong total forty
lines later. Ninety-six runs clean afterwards.

**Suite at this point:** 132 passed, 5 failed, 3 skipped. `rpc_deriveaddresses`
(twice) and `rpc_named_arguments` wait on PR 8. `wallet_address_queries` waits on the
`importelectrumwallet` fix in PR 8. `feature_llmq_is_cl_conflicts` appears at this
stage only.

---

## 8 — wallet and rpc fixes found by the new coverage

Branch `ft/08-wallet-and-rpc-fixes` · 5 commits · 11 files, +220 −33

- `echo` could not echo. Its `RPCHelpMan` declares an empty argument list and the body
  validated against it, so every call with an argument threw the help text, while the
  dispatch table registers `arg0` through `arg9`. The registration and the help
  contradicted each other. This restores upstream's body. The assertion PR 5 commented
  out is restored in the same commit.
- `deriveaddresses` did not accept a `[begin,end]` range.
- `importprivkey` discarded an existing label.
- `-fallbackfee=0` paid the floor instead of being honoured.
- `importelectrumwallet` read the extension with `find_last_of(".")` over the whole
  argument, so a path under a directory called `2.0.3` or `.raptoreum` supplied one and
  a file with no extension at all was reported as having the wrong one. It now asks
  `fs::path` for the extension, as the same function already does forty lines further
  down. This is the only site in the tree that reads an extension this way. The test
  creates the dotted directory itself rather than relying on the build path, so the
  case fails on the old code everywhere rather than only where the tree happens to sit
  under a dot.

**Suite at this point:** 139 passed, 0 failed, 3 skipped. With `pyzmq` installed,
142 passed, 0 failed, nothing skipped.

---

## 9 — test: run by default what upstream runs by default

Branch `ft/09-run-by-default` · 1 commit · 1 file, +21 −22

Aligns `test_runner.py`'s default set with upstream's, and removes the one genuine
duplicate entry (`p2p_unrequested_blocks.py`). Entries that look duplicated are the
same script under different flags, `--usecli`, `--ipv4` and `--spork21`, and are kept.

**Suite at this point:** 146 passed, 0 failed, 3 skipped.

With `pyzmq` installed the suite is 149 tests with nothing skipped. A clean run is
147 passed and 2 failed, both of them intermittent and neither introduced by this
series. They are described below.


---

## Known intermittent failures

Two tests fail occasionally under `-j12` and pass reliably on their own. Both predate
this series; the suite simply could not run before it, so neither had ever been
observed. They are recorded here rather than papered over.

### `feature_block.py` — the framework's disconnect is not synchronous

Fails in `disconnect_p2ps` with "peers still connected 20s after disconnect". Ten
sequential runs on an idle box pass.

It is not slowness. The node's own log shows it dropped the peer and then accepted a new
one six milliseconds later, and the replacement stayed connected until shutdown, so the
node never saw its socket close. The cause is the mininode framework's lifecycle:
`peer_disconnect()` only sets a flag, and the single global `NetworkThread` is the only
thing that acts on it, while its loop condition is `while mininode_socket_map:` and so
exits the moment the map is momentarily empty. Dropping a misbehaving peer empties it.
With no thread alive to service the flag, the socket is never closed and the wait can
only time out.

The fix belongs in `test_node.disconnect_p2ps`, closing from the test thread under
`mininode_lock` when the network thread is not running. It is deliberately not in this
series: proving a rare race is gone needs many full-suite runs, and the change is to
shared framework code that every other test depends on.

### `feature_llmq_is_cl_conflicts.py` — a node abort, cause not yet known

The node exits with `-6`, which is `SIGABRT`, so something inside the node aborts rather
than a test timing out. It passes three of three on its own. The failing run's directory
was cleaned up despite the crash, so the assertion text was not captured, and no claim
is made here about what it was. It is listed so that whoever sees it next knows it has
been seen before and that reproducing it means looping the test under load with
`--nocleanup`.
