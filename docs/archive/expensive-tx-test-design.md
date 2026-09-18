<!-- archived: 2026-09-18
     why:      executed; it was a test design and the tests have run
     replaced: perf-results.md §17 and the 2026-09-18 entries carry the results
     keep:     it states what each outcome would have meant, which is worth keeping as a worked example of
               pre-registering a measurement -->

# Test design: what does a shorter validation path actually buy?

**Status:** design, revised after adversarial review. Review findings were verified against
source before being accepted; where a claim below corrects an earlier one, the earlier one
is named.

## The question, restated

The original framing was: "validation is not a bottleneck, so does that survive more
expensive transactions?" That framing was built on a mistake, and the mistake is worth
stating because it changes the answer.

**Correction.** Earlier work reported script verification at 0.002 ms per input and
concluded that making validation cheaper could not raise the acceptance ceiling. The 0.002
ms figure is the **block-connect** cost (`perf-results.md` §10), where the signature cache
is warm because `AcceptToMemoryPool` verified those same signatures moments earlier. ATMP
itself never gets that hit: the cache nonce is generated per process
(`script/sigcache.cpp:36-38`), so a signature arriving over the wire is always new, and
`AcceptToMemoryPool` calls `CheckInputs` with no `pvChecks` vector
(`validation.cpp:796`), which means `CScriptCheck::operator()` runs **inline on the calling
thread** (`validation.cpp:1471-1476`) rather than being queued.

The profile in `perf-results.md` §6 says the same thing and was misread. In the *healthy*
baseline — same corpus, ~5,000 tx/s, node at ~114% CPU — `secp256k1_fe_mul_inner` is
32.39% of samples and `secp256k1_fe_sqr_inner` 25.58%. **Those two symbols alone are 58% of
the process**, on a node whose ceiling is one pinned `rtm-msghand` core, and they undercount
ECDSA because the scalar and group symbols are not in that excerpt.

So signature verification is not a rounding error on the acceptance path. It is most of it.
A shorter validation path would raise the acceptance ceiling.

**That makes the real question sharper, not softer:** how much does removing per-input
verification actually buy at acceptance, and is that saving available without a trust
assumption? Because the same work is already parallelisable — `CCheckQueue` exists and
`CheckInputs` already knows how to use it (`validation.cpp:1472-1474`), it is simply not
used from ATMP, on a box where 22 of 24 threads are idle.

## Track 0 — the measurement that decides it

Two builds, one corpus, one rate sweep each. Nothing else changes: same bytes, same UTXO
set, same mempool accounting, same fees, same ancestor structure.

| arm | change | what it measures |
|---|---|---|
| **stock** | none | today's ceiling (~4,400-4,500 tx/s sustained; see `throughput-bottleneck.md`) |
| **skip** | test-only flag that makes `CachingTransactionSignatureChecker::VerifySignature` (`sigcache.cpp:85-97`) return true without calling the base verifier | the ceiling with **all** per-input signature work removed |
| **parallel** | ATMP passes a `pvChecks` vector and drains it through `CCheckQueue`, as `ConnectBlock` does | the same saving with **no** trust assumption |

The **skip** arm is an upper bound on what any attestation scheme can save at acceptance —
it is strictly more generous than a real shorter path, which still has to verify the
attestation itself. The **parallel** arm is the honest comparator: if it reaches the same
place, then the buspool's throughput case is not "attestation versus nothing", it is
"attestation versus a thread pool the node already contains".

This is three runs on an existing corpus and one patch. It answers the question the whole
argument turns on, and everything below is secondary to it.

**Where the cost goes if it is skipped.** If acceptance stops verifying signatures, block
connect no longer finds a warm cache. §10's 1.1 s for 85,624 transactions is a warm number;
cold it is roughly 85,624 × 2 inputs × ~59 µs ≈ 10 s of single-threaded verification,
divided by the script threads. At a decoupled 600,000-transaction block that is on the
order of a minute per 120-second interval. So attestation **moves** the cost unless block
validation trusts the attestation too — which is a consensus change, not an optimisation,
and needs to be stated as one.

## Track 1 — the cost curve

Scale per-transaction validation cost and measure the ceiling against it.

**Corrected:** the baseline corpus v3 is **2-in-2-out**, not 1-in-2-out
(`build_corpus.py:28-36` pairs UTXOs; mean payload 373 bytes). The earlier design called it
I1 and treated it as the 1× reference. It is I2 and already carries two verifications.

The knob should be **inside the node**, not in the corpus: a test-only
`-perfverifyrepeat=K` that runs the script check K times, with K=0 meaning skip. That gives
the entire curve on one fixed corpus, with bytes, fees, UTXO count, mempool accounting and
ancestor structure all held constant — none of which is true if the knob is
inputs-per-transaction.

Inputs-per-transaction is kept as a secondary arm, because it is a real workload rather
than an artificial one, but it is no longer the primary knob, and its confounds must be
reported rather than assumed away:

- Each input adds ~148 bytes as well as ~59 µs, so byte throughput at the ceiling is flat
  to rising in N. Against a 2 MB *body* block, validation headroom **grows** with inputs.
  Results must be reported in tx/s, MB/s and inputs/s, and the design point being crossed
  must be named in its own unit.
- Ancestor limits force the N-input corpora to be single-generation:
  `DEFAULT_ANCESTOR_LIMIT` is 25 (`validation.h:85`), and at 2 outputs per transaction an
  I25 second-generation transaction has 182 in-mempool ancestors. **All** corpora,
  including a newly built I1 and I2, must be generation-0 from one fan-out, or the baseline
  pays ancestor bookkeeping the others do not.
- Fees must be per kilobyte, not per transaction. `build_corpus.py:81` defaults to 10,000
  satoshi flat and the lineage fee skews most transactions near the floor; `minRelayTxFee`
  is 1,000 sat/kB (`validation.h:83`), so a ~3.8 kB I25 transaction needs ≥3,800. Below
  that the node rejects and `collect.py` reads the rejection as a lower ceiling.
- The legacy sighash re-serializes the transaction per input, so sighash cost is O(N²) —
  small at these sizes but it bends the "expected linear cost" assumption.

## Track 2 — where RTM's own types sit

Constructible in Python, with corrections to the earlier reading:

- **Future (7).** All payload fields are plain (`providertx.h:204-229`), no signature.
  Active on regtest from block 1 — `nFutureForkBlock = 1` (`chainparams.cpp:706`).
- **New asset (8).** Root assets carry no `vchSig` (`providertx.h:277-283`, serialized only
  when `!isRoot`), so no wallet involvement. Asset ID is the txid, known at build time.
  One new-asset per name in the mempool (`txmempool.cpp:465`).
- **Mint asset (10).** **Corrected:** the earlier design called this database-bound. It is
  **signature-bound** — `CMessageSigner::VerifyMessage` does an ECDSA `RecoverCompact`
  (`providertx.cpp:369-373`), unconditionally, because type 0 is the only accepted
  distribution type. The same applies to update asset. Reproducible in Python with
  coincurve's recoverable signing, over `MakeSignString` (`providertx.cpp:807-820`) hashed
  with the `"DarkCoin Signed Message:\n"` prefix. Constraint: one mint per assetId in the
  mempool (`txmempool.cpp:470`), so an N-transaction mint corpus needs N **confirmed**
  assets.
- **Asset transfer (0).** **Corrected:** asset outputs carry zero RTM, so every transfer
  needs an RTM input for the fee — minimum 2-in 2-out. The earlier one-line description
  omitted the input side.

Assets are gated on a *voted* deployment, not a height: `IsAssetsActive` is
`IsActive(EUpdate::ROUND_VOTING)` (`update.cpp:282-284`), registered on regtest at
`chainparams.cpp:711-712` and active by about block 300 with the default miner. The fan-out
mines past that anyway, but the rig must assert it rather than assume it.

Fee is zero unless `SPORK_22_SPECIAL_TX_FEE` is active (`assets.cpp:33-38`, default off),
confirmed, so hand-built transactions with `fee=0` are accepted even though the RPCs refuse.

## Track 3 — the assets cache copy

**This replaces the earlier Track 3, which was measuring something that does not happen.**
The asset database does not grow during an unmined run: `AddAssets` is reached only from
`UpdateCoins` at block connect (`validation.cpp:1371`).

What does happen is worse. `validation.cpp:668` executes

```cpp
CAssetsCache assetsCache = *passetsCache.get();
```

for **every** transaction of every type, before any dispatch on `nType`. The copy
constructor copies three `std::map`s and four `std::set`s (`assets.h:150-195`), and
`mapAsset`/`mapAssetId` grow in-process as assets are confirmed, with `LoadAssets`
pre-loading up to 2,500 entries at startup (`assets.h:25`, `assetsdb.cpp:100-147`).

Every acceptance measurement so far was taken on a regtest chain with **zero** assets, so
this cost was exactly zero. On a chain with assets it is a per-transaction cost proportional
to the number of assets, paid by payments as much as by asset transactions.

The measurement: confirm K assets for K in {0, 100, 1,000, 2,500, 10,000}, then measure the
**payment** corpus ceiling at each K — once without restarting (maps uncapped) and once
after a restart (2,500 cap), with and without `-assetindex`. If this bites, RTM's mainnet
acceptance ceiling is far below the rig figure and the reason is neither validation nor the
message handler.

**Measured 2026-09-16 — this bites, and hard.** On a rig chain carrying 3,500 confirmed
assets (mainnet holds 3,439): unfixed **695 tx/s**, fixed **4,460 tx/s**, with an unfixed
zero-asset control at 4,502 tx/s. The copy costs **6.5x throughput**. Fixed in
`validation.cpp` by constructing the working copy only for the three asset tx types; see
`docs/asset-cache-drag.md`.

## Rig settings

Carried over, each of which previously invalidated a measurement:

- `checkmempool=0` — regtest re-validates the whole mempool per accepted transaction;
  leaving it on understated the ceiling by 60×.
- `maxmempool=8000` — **added back**; the earlier revision dropped it. At the 300 MB
  default a large corpus overflows, `GetMinFee` rises, and growth-based counting breaks.
- `maxtipage=999999999` — or peers sit in IBD and suppress transaction requests.
- `[regtest]` section in the node config, or port settings are ignored.
- Drain, do not disconnect: the node discards a peer's unprocessed receive buffer on
  disconnect, destroying the backlog that measures how far behind it is.
- One fresh process per run, asserted from the single `Using N MiB ... signature cache`
  log line (`sigcache.cpp:81`), so no cross-run cache carry-over.
- Verify the UTXO cache regime rather than asserting it: `CCoinsViewCache::Flush()` clears
  the cache (`coins.cpp:237-241`), and any restart between fan-out and run starts cold.
  Check the `leveldb::` share in an `rtm-msghand` profile.

## What each outcome means

| result | reading |
|---|---|
| **skip** arm ≈ **stock** | verification is not the ceiling after all; the buspool has no throughput case and the §6 profile needs re-reading |
| **skip** ≈ **parallel** ≫ **stock** | the saving is real but available with no trust change; the buspool is competing with a thread pool the node already has |
| **skip** ≫ **parallel** > **stock** | attestation buys something parallelism cannot; the size of the gap is the buspool's actual case |
| Track 3 degrades with asset count | RTM has an acceptance ceiling on mainnet that no measurement here has seen |

## What this cannot say

- Nothing about contract execution, because no VM exists in this tree. The in-node repeat
  knob measures signature-shaped work scaled up, not interpreter-shaped work.
- Nothing about cold-cache behaviour on real hardware. Every number is one machine with a
  warm UTXO cache; a Smartnode is weaker and colder.
- Nothing about mainnet standardness: regtest sets `fRequireStandard=false`
  (`init.cpp:1701`), so `IsStandardTx` and `AreInputsStandard` never run. Cheap, but it
  makes every number here optimistic for mainnet.
