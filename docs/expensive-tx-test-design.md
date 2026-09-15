# Test design: does the acceptance ceiling survive expensive transactions?

**Status:** design, not yet built. For adversarial review before implementation.

## The question

Every throughput number measured so far used the cheapest transaction that exists: one
input, two outputs, 373 bytes, P2PKH, script verification at 0.002 ms per input with a
warm cache. On that corpus the node accepts at **~5,600 tx/s**, `rtm-msghand` pinned at
100.3% of one core, and validation is comfortably not the constraint.

RTM's head developer has proposed a design ("buspool") in which Smartnodes pre-attest
transactions so that ordinary nodes can take a shorter validation path. The stated benefit
is "so it can validate faster". Against the measured payment corpus that benefit is
worthless, because validation has an order of magnitude of headroom at the 2 MB design
point.

**But that conclusion is only as general as the corpus.** This tree exists to prototype a
smart contract layer. Contract execution is precisely the thing that makes per-transaction
validation expensive. If per-transaction validation cost rises by two orders of magnitude,
acceptance stops having headroom and becomes the binding constraint, and "validate once in
a quorum instead of once per node" becomes a real architectural saving rather than
accounting.

So the question is: **at what per-transaction validation cost does acceptance become the
bottleneck, and where do RTM's actual transaction types sit relative to that point?**

## Why not simply measure asset and future transactions

That was the first instinct and it is the weaker half of the design, for two reasons.

**They may not be expensive.** Reading the validation paths:

- `CheckFutureTx` (`evo/providertx.cpp:82`) is a payload deserialization, a version check
  and `CheckInputsHash` — one SHA256 over the inputs. Per byte it is arguably *cheaper*
  than a normal transaction.
- `CheckNewAssetTx` (`:125`) does name regex validation and **asset database lookups**:
  `GetAssetMetaData`, `GetAssetId`, `CheckIfAssetExists`. For a sub-asset it additionally
  does `CMessageSigner::VerifyMessage`, an ECDSA recovery. So its cost is dominated by
  LevelDB reads rather than computation.
- `CheckMintAssetTx` (`:344`) and `CheckUpdateAssetTx` similarly.

A test that only measures these answers "are RTM's current special types expensive?" — a
useful but narrow question — and it cannot answer the general one, because none of them is
a stand-in for contract execution.

**And the contract cost is hypothetical.** There is no VM in this tree. `src/script/` is
Bitcoin Script: no loops, no persistent state. Any number attributed to "contract
validation" would be invented.

## Design: two tracks

### Track 1 — scale the cost with a knob we control

Vary **inputs per transaction**. Each additional input adds one ECDSA verification plus one
UTXO lookup, both of which are exactly the per-transaction work that a shorter validation
path would remove. It is the one dial in this codebase that raises validation cost
predictably, without inventing a feature.

Corpora, each with the same total *transaction* count so acceptance rates are comparable:

| corpus | inputs | outputs | expected relative validation cost |
|---|---|---|---|
| I1 | 1 | 2 | 1× (baseline, already built as corpus v3) |
| I2 | 2 | 2 | ~2× |
| I5 | 5 | 2 | ~5× |
| I10 | 10 | 2 | ~10× |
| I25 | 25 | 2 | ~25× |

Measure for each: the acceptance ceiling (offered-rate sweep to saturation), `rtm-msghand`
CPU, and CPU per transaction. This produces a curve of **acceptance rate against
per-transaction validation cost**, which is the general answer. Reading off where it crosses
520 tx/s (the 2 MB decoupled design point) and 5,000 tx/s says how much more expensive a
transaction has to get before trí's premise becomes true.

The knob is not a perfect proxy for contract execution — input verification is
signature-bound where a VM would be interpreter-bound — but it is a real cost on the real
acceptance path, and it is honest about what it is.

### Track 2 — place RTM's actual types on that curve

Build a corpus of each special type and measure its acceptance ceiling, then locate it on
the Track 1 curve as an equivalent-input-count.

| corpus | type | notes |
|---|---|---|
| F | 7, future | `vExtraPayload` = `CFutureTx`; needs `IsFutureActive` |
| AN | 8, new asset | unique name per transaction; needs `IsAssetsActive` |
| AM | 10, mint asset | requires an existing asset to mint |
| AX | 0, asset transfer | normal type, `CAssetTransfer` in the output script |

This answers the narrow question and gives the Track 1 curve real anchors.

### Track 3 — does asset cost grow with the asset database?

`CheckNewAssetTx` consults the asset cache and database. A corpus of N new assets grows
that database by N entries, so per-transaction cost may rise during the run. Measure
acceptance rate over time within a single long AN run rather than only in aggregate. If it
degrades, that is a scaling property of the asset subsystem and a finding in its own right.

## Method

Reuses the existing rig (`test/perf/`): `fanout.py` for the UTXO set, `build_corpus.py`
for generation and signing, `generate.py` to offer at a target rate, `collect.py` for
per-second CPU and mempool sampling.

Changes needed:

- `build_corpus.py` gains an `--inputs` argument (Track 1) and a `--tx-type` argument with
  payload builders (Track 2).
- `wire.py` gains `CFutureTx`, `CNewAssetTx`, `CMintAssetTx` serialization and the
  `nVersion=3 | nType<<16` packing, plus `CAssetTransfer` output scripts.

Rig settings carried over from earlier work, each of which previously invalidated a
measurement:

- `checkmempool=0`. Regtest re-validates the whole mempool after every accepted
  transaction; leaving it on understated the ceiling by 60×.
- `maxtipage=999999999`, or peers sit in IBD and suppress transaction requests.
- `[regtest]` section in the node config, or port settings are ignored.
- Disconnect drains, not closes: the node discards a peer's unprocessed receive buffer on
  disconnect, so closing at the end of a run destroys the backlog that measures how far
  behind the node is.

## What each outcome means

| result | reading |
|---|---|
| acceptance stays near 5,600 tx/s across the input sweep | validation is not a constraint at any realistic cost; the buspool has no throughput case at any horizon |
| acceptance falls roughly as 1/cost, crossing 5,000 tx/s at a low multiple | trí is right about the direction; the argument becomes batching and relay, not whether |
| RTM's special types land near the cheap end | assets and futures do not motivate the design; only a future VM would |
| AN degrades within a run | the asset subsystem has its own scaling problem, independent of all of this |

## What this test cannot say

- Nothing about contract execution cost, because no VM exists. Track 1 measures
  signature-and-UTXO cost, which is a different shape of work.
- Nothing about cold-cache behaviour. Everything is one machine with a warm UTXO cache and
  no disk pressure. A real Smartnode is neither.
- Nothing about validation at block connect, which is a separate path from acceptance and
  was measured once (85,624 transactions in 1.1 s) under the same warm conditions.
