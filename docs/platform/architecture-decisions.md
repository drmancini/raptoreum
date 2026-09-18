<!-- lifecycle: living — a different subject, not archived
     owns:      the contract-platform architecture: runtime, consensus, state layer, peg
     not mine:  decoupling, which is this folder's other subject
     note:      cited by transaction-decoupling §17 and build-plan phase 5, so it is referenced
                rather than abandoned. It is the first file of the second subject; when the
                platform work starts in earnest it gets the same treatment decoupling has. -->

# Raptoreum Contract Platform — Architecture Decisions and Rationale

**Status:** proposal / discussion draft
**Purpose:** the reasoning behind the choices assumed in the companion *Delivery
Roadmap*. Written to be argued with — every decision below states what would change
it.

---

## 0. Summary of decisions

| # | Decision | Confidence |
|---|---|---|
| 1 | Drop Apache Spark as the contract runtime | High |
| 2 | Drop payment decoupling; keep payments on PoW + ChainLocks | High |
| 3 | Contract state lives on a Smartnode-operated state layer | High |
| 4 | Deterministic WASM for execution | High |
| 5 | Tenderdash for consensus | Medium |
| 6 | Keep the ABCI process seam | High |
| 7 | Take GroveDB, do **not** take rs-drive wholesale | Medium |
| 8 | Inbound-only peg in v1 | High |
| 9 | Dash-lineage over Cosmos SDK | **Low — needs resolution in Phase 0** |

---

## 1. What Raptoreum currently proposes

For context, the design being replaced:

- **Execution host:** Smartnodes, not miners. Contracts stored and executed by the
  collateralised masternode tier inherited from the Dash fork.
- **Runtime:** Apache Spark, with contracts written in Java, Python and R.
  Explicitly positioned as steering away from EVM-based solutions, with developer
  familiarity and cheaper audits as the justification.
- **Sidechain:** "transaction decoupling" — relocating transactions off the main
  chain to a Smartnode-operated sidechain which handles them and stores raw
  transaction data, with notarisation back to the PoW chain.

Already shipped and working: the asset layer and futures (time- or height-locked
transactions). These are real and are marketed as non-contract DeFi. The contract
VM and the sidechain are not shipped.

The contracts paper dates to 2021. Current documentation still uses the future
tense. Treat the roadmap as unexecuted after five years.

---

## 2. Decision: drop Apache Spark

**Four independent disqualifiers. Any one is sufficient.**

### 2.1 Determinism

Consensus requires bit-identical replay. Spark's execution model — lazy DAG
evaluation, dynamic partitioning, shuffle, speculative re-execution of straggler
tasks — is explicitly built around *not* caring about scheduling order. That is
correct design for batch analytics and a category error for consensus. You are
selecting a runtime whose core optimisation is precisely what replicated execution
forbids.

Layering Java, Python and R on top adds thread scheduling, floating point, hash
iteration order, locale, wall clock and unrestricted IO. None replays identically.

### 2.2 The pitch is self-defeating

This is the structural argument, and it is worth stating carefully because it is the
one that cannot be engineered around:

1. To be trustless, execution must be deterministic.
2. To make Java/Python/R deterministic, you must restrict them to a sandboxed
   subset — no threads, no IO, no ambient time, fixed-point or software float,
   deterministic collections, bounded memory, instruction-counted metering.
3. At that point you have a new restricted language that happens to use Java
   syntax. Existing Java code cannot be ported to it. Existing audit tooling does
   not apply.

The entire justification for rejecting EVM and WASM was developer familiarity and
cheaper audits. Both evaporate under the constraint the design must satisfy in order
to function.

### 2.3 Scale mismatch

Spark exists to process gigabytes across a cluster. Contract calls touch kilobytes
and perform arithmetic. JVM plus Spark context startup is seconds and hundreds of
megabytes; the overhead exceeds the computation by orders of magnitude.

### 2.4 Metering and attack surface

No instruction accounting, therefore no gas equivalent, therefore no DoS pricing.
You cannot charge for what you cannot count.

Separately: JVM + Spark + the Hadoop dependency tree, running as a
consensus-critical component on every validator. Java deserialisation gadgets and
Log4Shell emerged from exactly this ecosystem. Poor choice for a load-bearing
security component.

### 2.5 The alternative branch

Not enforcing determinism — one node executes, the quorum attests — is internally
coherent, but the result is not a smart contract platform. It is a decentralised
compute oracle. Real product category, much smaller claims. Worth building
deliberately if that is the intent; not worth arriving at by accident.

### 2.6 Where Spark could legitimately live

Beside the VM, never on the consensus path: an optional off-chain compute service
invoked by contracts, executed by a quorum, result attested by threshold signature
and *clearly labelled* as carrying an honest-majority trust assumption rather than
consensus verification. Heavy data jobs no chain can verify anyway — analytics over
asset histories, model inference. Adjacent to Chainlink Functions. This would
actually be differentiated.

**What would change this decision:** nothing short of a determinism proof for Spark
that does not reduce it to a restricted subset. No such thing exists.

---

## 3. Decision: drop payment decoupling

### 3.1 What decoupling was supposed to achieve

Three claimed benefits:

1. **Throughput** — settlement rate decoupled from the ~2-minute PoW block interval;
   a BFT sidechain commits every second or two.
2. **Main chain footprint** — raw transaction data on the sidechain, commitments
   only on the main chain; slower growth, smaller UTXO set, cheaper full nodes.
3. **Independent scaling of security and capacity** — PoW produces security, the
   sidechain produces throughput. This is the modular blockchain thesis and it is
   sound in general.

### 3.2 Why they do not apply here

**Benefits 1 and 2 answer problems Raptoreum does not have.** RTM blocks are not
full. There is no congestion, no fee market, no block space contention. Scaling work
pays off when demand exceeds supply; here the ratio is inverted and has been for
years.

**Fast confirmation is already solved.** ChainLocks give near-instant
irreversibility and the InstantSend pattern gives sub-second confirmation, both
without moving any transaction anywhere. Raptoreum inherited this machinery. Dash's
core blocks are 2.5 minutes and Dash built Platform anyway — latency was solved years
before Platform and was not the driver.

**Chain size has cheaper answers.** Pruning, assumeutxo-style fast sync, UTXO
snapshots. None require a second consensus system or place user funds under quorum
custody.

### 3.3 What decoupling actually costs

- **Quorum custody of the general money supply.** Transactions that currently settle
  under proof-of-work would settle under an honest-majority assumption about a
  committee whose collateral is worth a few million dollars.
- **Existential liveness dependency.** If settlement moves off the main chain and the
  Smartnode layer stalls, the network's core function stops. Dash structurally
  avoided this by keeping payments on core.
- **A mandatory general two-way peg** — the hardest engineering component, and the
  one Dash deferred past launch.
- **Cross-chain validator set coherence.** The sidechain validator set derives from
  the main chain masternode list; main chain reorgs retroactively change that set.
  Reorg handling across two consensus systems with different finality models is
  where subtle, fund-losing bugs live.
- **Data availability.** If the main chain stops storing raw transaction data,
  withholding by the Smartnode set becomes an attack with no described recovery path.

### 3.4 The honest reframing

The real motivation was never throughput. Contracts need somewhere to keep state and
process calls at a rate a 2-minute PoW chain cannot support. **Decoupling is contract
infrastructure sold as a scaling feature.** That is why the two items are always
announced together, and it is why the sidechain cannot be evaluated on scaling merits
— on those merits alone it does not clear the bar.

### 3.5 The separation that makes the project tractable

Two distinct things were bundled under one word:

| | |
|---|---|
| **Payment decoupling** | ordinary RTM transfers settle on the Smartnode layer instead of the PoW chain |
| **Contract state residency** | contracts get a mutable, provable store maintained by the Smartnode layer |

Raptoreum's documentation describes the first. The contract platform only needs the
second. Separating them removes every failure mode in 3.3 except a narrowed version
of the peg.

**Residual coupling, stated precisely:** contracts that hold or move RTM still need
a peg. You do not escape peg design, you shrink it — from the settlement path for
every transaction on the network to an opt-in bridge with explicitly scoped risk.
Dash's asset lock / asset unlock transactions are that narrow version.

There is a further dial: contracts could reference main-chain assets by commitment
and settle back on-chain, keeping only logic and intermediate state off-chain. That
shrinks custody further at the cost of latency and expressiveness.

**What would change this decision:** sustained main-chain congestion. If RTM blocks
were full, benefit 1 becomes real and the calculus shifts.

---

## 4. Decision: contract state on a Smartnode-operated state layer

### 4.1 Why a UTXO chain cannot hold contract state

UTXOs are immutable, consumed once, and addressable only by outpoint. Contracts
need mutable, persistent, randomly-addressable, *provable* state — key-value with
secondary indexes and Merkle proofs, so a light client can verify a query it did not
compute. That is a fundamentally different data structure, and no amount of faster
block production produces it.

Dash built GroveDB and Drive specifically because Dash Core cannot represent this.
Raptoreum's asset layer already strains against the same limit, which is why assets
and futures are fixed-schema constructs rather than general state.

### 4.2 The secondary constraint: bandwidth

Every contract call carries calldata and produces state deltas. Even at instant
finality, if all of it must land in main-chain transactions you bloat the chain the
project simultaneously wants to shrink. The constraint is bytes per unit time, and
block time changes little about it.

### 4.3 What the architecture class buys, and its limit

"Committee of collateralised nodes executes, threshold-signs a state commitment,
anchors to a base chain" is a proven pattern — the Internet Computer runs it in
production, Dash Platform is a narrower version, Cosmos app-chains and Avalanche
subnets are neighbours. The objection is never "you cannot build a contract platform
on a masternode quorum." You can.

What makes those work is that **every node in the committee re-executes and gets
bit-identical output**. The threshold signature attests to a result each signer
independently verified, not to a result one node computed and the others
rubber-stamped. That distinction is the entire security value, and it is why
section 2 matters.

---

## 5. Decision: deterministic WASM

### 5.1 Why WASM

It delivers the thing Spark was chosen for, and delivers it correctly:

- Multiple source languages — Rust, C, C++, Go via TinyGo, AssemblyScript
- Determinism enforceable at the bytecode level, not by developer discipline
- Instruction-level metering by bytecode injection
- Mature toolchain and real audit tooling
- Industry-standard for this problem for roughly the entire period Spark has been
  proposed

**The original objection to EVM was legitimate. The conclusion drawn from it was
wrong.** WASM is the answer that objection pointed at.

### 5.2 Honest scoping of the language claim

In practice, the mature contract toolchain is Rust-first. WASM as a compilation
target supports more languages than that, but "write contracts in the language you
already know" is narrower than the general WASM pitch implies. Better than
Solidity-only; not an open field. State this plainly rather than repeating the Spark
mistake of overselling language accessibility.

### 5.3 CosmWasm vs. embedding Wasmtime directly

An earlier characterisation of CosmWasm as "off the shelf" was too generous.

CosmWasm is written against the Cosmos SDK's module system, storage abstractions and
gas accounting. Dropping it into a non-Cosmos ABCI application means implementing its
storage, API and querier traits against GroveDB and reconciling its gas model with a
separate credit system. **Months of adaptation, not weeks.**

Its genuine advantages are worth taking seriously regardless of the decision:

- Float opcodes rejected at upload time during bytecode validation, not discouraged
- Gas metering injected into bytecode at instantiation
- An actor model for inter-contract calls — contracts return messages that are
  dispatched afterward, which structurally eliminates the reentrancy bug class

**Recommendation:** embed Wasmtime directly and write a thinner contract host, but
copy CosmWasm's design decisions — particularly upload-time validation and the actor
model. More work up front, less impedance mismatch, no dependency on another chain's
architecture. For a project already committed to maintaining two upstream forks,
avoiding a third coupling is worth the cost.

**Unless** decision 9 resolves toward Cosmos SDK, in which case take CosmWasm intact.

---

## 6. Decision: Tenderdash for consensus

### 6.1 Alternatives considered

**CometBFT** — the maintained upstream. Larger ecosystem, more eyes, better
documentation. But no threshold signatures: light clients verify N individual
signatures per commit. And a staking module you would disable and work around. You
would end up reimplementing the L1 validator-set binding that Tenderdash already has.

**HotStuff variants** (Diem lineage, Aptos, Sui) — linear message complexity where
Tendermint is quadratic, pipelined, better throughput. Genuinely better consensus
algorithms. But you integrate a research-grade codebase and build the L1 binding
from scratch.

**Roll your own** — no.

### 6.2 Why Tenderdash wins

Not consensus quality. Integration. Two things are already wired:

- **BLS threshold commits** — one signature verifiable against one quorum public key,
  rather than N signatures. This is what makes light-client verification cheap.
- **Validator set imported from a masternode list with rotation**, rather than an
  internal staking module. Sybil resistance outsourced to L1 collateral.

That is precisely the integration you would otherwise write, and it is where the
subtle failures live.

### 6.3 Costs accepted

- A fork maintained by one small team, tracking upstream CometBFT with a lag. A real
  long-term liability, accepted because the alternative is building the Core binding
  yourself.
- Quadratic message complexity caps the validator set at roughly 100–150. Acceptable
  — Dash's platform quorum is that size by design.

---

## 7. Decision: keep the ABCI seam

ABCI is an interface specification, not a component. The question is whether
consensus and the state machine remain separate processes with a defined contract
between them.

The thesis: ordering opaque bytes is generic and hard; interpreting them is
application-specific and comparatively easy. Consensus handles networking, gossip,
proposal and voting, then hands the application an ordered byte stream. The
application answers `CheckTx`, `PrepareProposal`, `ProcessProposal`, `FinalizeBlock`
and `Commit`, and `Commit` returns an app hash that enters the next block header.

**Alternative:** embed the state machine in the consensus binary, as Ethereum clients
do. Faster — no socket serialisation per call — but welds you to one consensus engine
permanently, in one language.

**Keep the seam.** For a project maintaining forks for years, it means the Rust state
machine and the Go consensus engine evolve independently, and swapping consensus
later does not touch the application.

*Implementation note:* ABCI 2.0 is the current generation. Tenderdash tracks its own
variant. Confirm which surface you are coding against before designing around it.

---

## 8. Decision: GroveDB yes, rs-drive no

This is where "just port Dash Platform" is weakest, because Drive encodes Dash's
*product*, not only its infrastructure.

**GroveDB** — a hierarchy of Merkle AVL trees over RocksDB where every key-value pair
has a cryptographic path to a single root hash, and any query returns a proof.
Genuinely generic, few chain-specific assumptions. Alternatives exist (NOMT, Verkle
tries, plain Merkle Patricia) but none provide hierarchical trees with
secondary-index proofs out of the box, and that is the hard part. **Take it.**

**rs-drive** — document schemas, data contracts, identities, credit accounting, DPNS
assumptions. It is a document database with an identity system, built for DashPay.
For a WASM contract platform most of it is unused or actively in the way: contracts
want flat key-value under a contract address, not a document store with declarative
indexes.

**Assessment:** taking Drive wholesale means inheriting a data model designed for a
different product and bending contracts to fit it. The leaner path is GroveDB plus a
purpose-built ABCI application — state root management, fee accounting, WASM host
bindings — on the order of 10–15k lines against proven storage, versus adapting 100k+
lines of another project's product logic.

---

## 9. Decision: inbound-only peg in v1

Dash launched Platform with withdrawals disabled and shipped them later. That
deferral is informative: the withdrawal path is the hard part, and even a
well-funded team with a dedicated CTO chose not to ship it at launch.

Getting value out safely requires a path resistant to a malicious threshold signer
set, with fraud detection or a challenge period, plus correct handling of validator
set turnover mid-withdrawal.

Copying the deferral is free risk reduction. Withdrawals become a separate project
with a separate audit.

---

## 10. Open decision: Dash lineage vs. Cosmos SDK

**This is the weakest assumption in the entire plan and should not be inherited by
default.**

The reasoning so far assumes Dash-lineage components because Raptoreum is a Dash
fork. That is a genetic argument, not an engineering one.

**The Cosmos option:** Cosmos SDK + CometBFT + CosmWasm. Whole stack,
ecosystem-supported, CosmWasm designed for it, no adaptation layer. You would write a
custom module binding the validator set to Raptoreum's Smartnode list and the peg to
asset-lock transactions. This is the mainstream answer and probably the fastest route
to a working contract platform.

| | Dash lineage | Cosmos SDK |
|---|---|---|
| Light client verification | Threshold sig, cheap | N signatures, heavier |
| L1 validator set binding | Already exists | Build it |
| Ecosystem and maintenance | One small team | Large, active |
| Contract layer | Build the host | CosmWasm intact |
| Documentation and tooling | Thin | Extensive |

**Substrate** was also considered: WASM runtime and contracts pallet built in,
upgradeability without hard forks, but opinionated toward its own consensus and
account model. Bending it to a PoW-anchored masternode validator set fights the
framework. Not recommended.

**The Core-side work in Phase 1 is required either way.** What differs is whether the
layer above is a fork of one small team's product or a widely maintained framework.
On a four-year horizon with a small team, ecosystem support may matter more than the
elegance of threshold-signed commits.

**Resolve this in Phase 0 by building the spike both ways.** It is cheap there and
expensive later.

---

## 11. Constraints no architecture resolves

Recorded here so that they are not mistaken for engineering problems.

**Security budget.** Platform security equals the cost of acquiring enough Smartnode
collateral to capture a quorum, denominated in RTM. On successful delivery, that cost
sits below the value the platform would need to secure to be worth using. The fix
requires a much higher valuation; the valuation is downstream of the platform
working. The dependency is circular.

**Funding.** 6–8 engineers for four years is roughly $6–12M. No treasury mechanism
exists.

**Differentiation.** The architecture recommended here is, approximately, Dash
Platform plus a WASM VM. It would work. It is not distinctive. The strategic question
— build a fork of an existing platform, or build the application on one that already
has a security budget — sits upstream of everything in this document and is not
answered by it.

---

## Appendix — reference points

- Dash Evolution: first mentioned around 2015–16, mainnet beta 29 July 2024. Roughly
  nine years elapsed, three under a dedicated CTO, with a funded treasury and a
  full-time core team — and shipped with withdrawals disabled.
- Dash Core, GroveDB and the Dash Platform monorepo are MIT licensed. Licensing is
  not a constraint on any decision in this document.
- Raptoreum Core sits roughly at the Dash 0.16/0.17 era; Dash Core has since moved
  through v18, v19, v20, v21 and beyond.
- Raptoreum's shipped asset layer and futures are real and working. They are the
  evidence for section 4.1 — fixed-schema constructs are what a UTXO chain can
  support without a state layer.
