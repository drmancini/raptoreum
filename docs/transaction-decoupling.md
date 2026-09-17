# Transaction Decoupling on Raptoreum Core

**Version 7.** Supersedes v6. The header was left at "Version 3" through several
revisions; the content is what the revision number tracks, not this line.

**What this is.** Not an argument for decoupling. A description of what decoupling
*would be* in this tree, at the level of files, structures and states.

**Basis.** `ft/functional-test-suite` off upstream `master` == tag `2.0.3.01`. Every
claim about existing behaviour cites a file and line and was read, not recalled.
Proposed behaviour is labelled **Proposed**. Unverified claims are labelled
**Assumption**. Claims from v1 that turned out to be wrong are labelled **Corrected**
and kept, because the corrections are load-bearing.

**Terminology.** Off-chain transaction data is called the **body**, and the store that
holds it the **body store**. "Payload" is not used: `vExtraPayload` is the DIP2
special-transaction field throughout `src/evo` and `src/assets`.

## What changed since v1

- v1 claimed the dual-format boundary was deserialize-and-assemble. It is
  `AcceptBlock`. Below `ConnectBlock` v1 was right and the cost is near zero; above it
  the whole acceptance, candidacy and persistence layer is involved. See §2.
- v1 claimed no complete block is ever assembled. **False at the tip.** Every node that
  connects a block holds a materialised copy. This splits the design into two regimes
  and removes a mining attack. See §3 and §4.
- v1 proposed a block-version bit to select the format. There is no free bit, and the
  version is inside the hashed header. See §1.
- v1 claimed `nStatus` bit 128 is free. It is `BLOCK_CONFLICT_CHAINLOCK`. See §2.1.
- v1 claimed Smartnodes are forbidden from partial retention by the transaction-index
  requirement. **Investigated and withdrawn.** See §8.2.
- v2 named sync-against-retention as the remaining structural obstacle. **Version 3
  removes it** by owner decision: Smartnodes hold everything, miners hold what they need
  to mine, and retention mechanisms are deferred. See §8.
- v6 reasoned about the Smartnode attestation path from the cost of the cryptography.
  **Measured, the cryptography is 3 to 5 per cent of it** and the path is round-trip-bound
  rather than compute-bound. The conclusion — that it is far too slow to carry transaction
  volume — survives; the reasoning behind it does not. See §16.5.
- v6 treated the relay re-index as a consequence of raising the block size. **It is a
  prerequisite of decoupling at any block size, including 2 MB.** See §16.3 and §16.8.
- v6 listed InstantSend as "marginal" at the 2 MB design point on a modelled figure of
  ~800 tx/s network-wide. The measured figure is an order of magnitude lower. It is a
  blocker, not a margin. See §16.5 and §16.8.

---

## 0A. Read this first — standing corrections and the open fork

This document was written and revised across one working session and reviewed adversarially three
times. **Corrections were recorded where they were found, which is not always where the claim
was made.** These are the positions that stand, with the places they correct:

| Standing position | Corrects |
|---|---|
| The tip whole-block fallback is **necessary and not sufficient**; its argument is circular. Withholding is closed by the fallback **plus §14.1 rule 1** (relay only what you can assemble) **plus rule 2** (an announcer still owes delivery). | §0, §3, §3A, §9.2 |
| A miner must **serve** its own bodies. Relay-and-wait is not required, and publishing a block no longer discharges the obligation. | §9.1a, §14.10 |
| The state root moves **availability** onto the chain, not validity. Below the snapshot base a node has SPV-grade security; **the anchor is the ChainLock** of §7A.7. | §7, §7A.3, §13.A |
| Per-address asset balances are **never committed**; commit the asset records. Canonicality, not volume, is the hard part. | §7A.1, §7A.4, §7A.6, §8.2 |
| The retry bound is **per-block exponential backoff**. It must not key off accumulated work. | §2.3, §14.8, B3 |
| "I do not have that" is non-punitive **for history only**. An announcing peer keeps today's delivery obligation. | §8.4, §13.F, §14.1, §14.8a |
| Addressing is **positional**, `(height, index)`. §4's argument against positional requests is reversed by §5.2. | §0, §4, §5 |
| The signing-attempts process is **required and gates F**, not a known limitation. | §3A.4, §3A.6, §13.B |
| The fail-open safety walk is **downgraded** to near-unreachable. | C2, §14.8b |

### The open fork

**This document describes two designs and does not choose between them.**

- **The commitment format** (§1–§13). Blocks carry identifiers; bodies live outside. Buys capacity.
  Its first step is §12 Q1: **probe the acceptance layer before building anything on it.**
- **Build whole, split after** (§15). Blocks are unchanged on the wire; nodes split them for
  storage. Buys the storage reduction and none of the capacity, and removes the acceptance layer
  along with most of §14. Its first step is §15.4: **build the body store and the fetch protocol,
  which need no fork.**

**These are not contradictory instructions.** Under §15 the body store and fetch protocol do not
depend on the acceptance layer, so building them does not violate Q1's rule. **But the choice is
unmade**, and the rest of this document is written from the commitment design's point of view
because that is the order it was written in, not because the question is settled. §15 has also not
had the adversarial pass §14 gave the main design.

---

## 0. The model

**Proof of work still orders transactions and still decides the chain.** Nothing about
consensus changes. What changes is that a block records *what was ordered* rather than
*the data that was ordered*: the coinbase in full, and every other transaction as its
32-byte identifier.

The transactions themselves are held by the Smartnode tier and fetched by identifier.

Two commitments hold it together. The **merkle root**, which already exists and already
commits to identifiers, says what was ordered. A **state root**, which does not exist yet,
says what the result was, so a node can begin from a verified snapshot instead of replaying
history it does not have.

### Three roles, all of which this chain already has

| Role | Holds | Provides |
|---|---|---|
| **Miners and pools** | UTXO set, asset state, a recent window of bodies | Ordering and validity, under proof of work |
| **Smartnodes** | Everything, forever | Availability of history, alongside the finality and lock services they already run |
| **Ordinary nodes** | State, a recent window, and whatever else they choose | Independent validation; may prune freely |

Smartnodes are already identified, collateralised, addressable on-chain and organised into
BFT quorums. The design adds a duty to a tier that exists rather than inventing one.

### Two regimes

**At the tip**, complete blocks exist, because whoever connected one had to assemble it.
Existing relay and the existing whole-block fallback keep working. **On their own they are not
sufficient** — the fallback argument is circular (§3) and must be joined by §14.1 rule 1.

**Beyond the tip**, no complete block exists anywhere. Requests are by identifier, answered
by Smartnodes, whose addresses the chain already publishes.

### What is new, and it is only three things

1. A second block serialization, and a second "we have it" state on the block index (§1, §2).
2. A body store and a **position**-addressed fetch protocol (§5.2, which reverses §4).
3. A state commitment and snapshot sync (§7A).

### What the chain buys

**History becomes a protocol service instead of a favour.** Because commitments are kept
forever, every transaction that ever existed stays nameable from the chain alone. That is
what makes a retention guarantee, a challenge, a coverage metric or a repair loop
expressible at all — none of which can be built by a chain that deleted its own index.
See §11A.

### What is deliberately not here yet

Retention mechanics, sharding, challenges, incentives and repair. All deferred by owner
decision (§8), and all still available later precisely because the chain keeps the names.

### The bet

That **block acceptance can depend on a network fetch without destabilising chain
selection**. Every other chain that split its data avoided this, by delegating validation
elsewhere or by deleting the data (§11A). It is the one genuinely novel property here, and
therefore the thing to prove before anything else is built.

## 1. The block format

`consensus/merkle.cpp:66` — `BlockMerkleRoot` builds its leaves from `tx->GetHash()`.
The merkle root is already a commitment to transaction identifiers and nothing else.
This tree has no segwit, so that hash is the identifier with no witness variant.

Free consequences:

- Proof of work and header relay are untouched.
- `CPartialMerkleTree` produces inclusion proofs from identifiers alone.
  `evo/simplifiedmns.cpp:247-253` already does exactly this.
- A block hash means what it meant before, so `CBlockIndex`, chain selection and
  ChainLocks signing are unaffected.

**Corrected — the format selector cannot live in `nVersion`.** Raptoreum replaced BIP9
with its own update-voting scheme. `update/update.h:36-40` sets
`VERSIONBITS_TOP_MASK = 0xE0000000` and `VERSIONBITS_NUM_BITS = 29`;
`update/update.h:101` restricts an update to bits 0 through 28; `update/update.cpp:110`
counts any matching set bit as a yes-vote; `chainparams.cpp:209,213,388` already assign
bits 0, 1 and 2. `update/update.cpp:504-511` is the only writer of block `nVersion`, via
`miner.cpp:160`. There is no free bit below 29 and nothing above it that does not break
the top mask.

Independently, `nVersion` sits inside the hashed header (`primitives/block.h:37-38`), so
a format recorded there is fixed at mining time and a materialised block cannot be
re-serialised in full form without changing its hash.

**Proposed.** The selector is a serialization stream flag, on the pattern of
`SERIALIZE_TRANSACTION_NO_WITNESS`, negotiated between peers by protocol version or
service bit. The on-disk form is a node-local choice. The consequence is that the format
is not self-describing from the header and peers must agree before transfer.

**Corrected — bloom-filtered blocks need bodies.** v1 said `CMerkleBlock` works
unchanged. Only its identifier-set constructor does (`merkleblock.cpp:50-51`, used by
`gettxoutproof`). The bloom path at `merkleblock.cpp:53` calls
`filter->IsRelevantAndUpdate(*block.vtx[i])` over full scripts and outpoints, and
`net_processing.cpp:1654-1676` then sends the matched transactions. Serving
`MSG_FILTERED_BLOCK` requires bodies for the client's whole rescan range.

---

## 2. Block acceptance and the real boundary

### 2.1 What v1 got wrong

Below `ConnectBlock` v1 holds. The connect and disconnect graphs were traced in full:
`ProcessSpecialTxsInBlock`, `quorumBlockProcessor->ProcessBlock`,
`deterministicMNManager->ProcessBlock`, `CheckCbTxMerkleRoots`, the transaction loop,
the InstantSend conflict loop, `IsBlockValueValid`, `IsBlockPayeeValid`, the undo and
index writes, and the assets cache. All take a block by reference, none reads from disk,
none depends on byte offsets. Nothing in `src/assets/` reads a block from disk at all.

Above it, the boundary is `AcceptBlock`, which validates bodies, persists the block and
makes it a chain candidate before any connection happens:

- `validation.cpp:4332-4333` — `CheckBlock` and `ContextualCheckBlock`, both iterating
  bodies: `3885` block byte size, `3889-3893` coinbase placement, `3897`
  `CheckTransaction`, `3904` `GetLegacySigOpCount`, `4020` `IsFinalTx`, `4022`
  `ContextualCheckTransaction`, `4045` `vtx[0]->nType`
- `validation.cpp:4349` — `SaveBlockToDisk`, before connection
- `validation.cpp:3717-3723` — `ReceivedBlockTransactions` sets `nTx = vtx.size()`, sets
  `BLOCK_HAVE_DATA`, raises validity to `BLOCK_VALID_TRANSACTIONS`; `3742` inserts into
  `setBlockIndexCandidates`
- `validation.cpp:3127` — `FindMostWorkChain` treats absence of `BLOCK_HAVE_DATA` as
  missing data. That flag *is* "connectable".
- `validation.cpp:3026-3027` — `ConnectTip` treats a failed `ReadBlockFromDisk` as
  `AbortNode`
- `validation.cpp:5365-5413` — `CheckBlockIndex` asserts
  `HAVE_DATA` implies `nTx > 0` implies `VALID_TRANSACTIONS` implies candidacy

So a body-incomplete block either gets `BLOCK_HAVE_DATA`, becomes a candidate, is
selected, and shuts the node down when connection reads it back; or it is held out of
that flag, and roughly thirty call sites above `ConnectBlock` must learn a new state.

**Corrected.** `nStatus` bit 128 is `BLOCK_CONFLICT_CHAINLOCK` (`chain.h:132`), used at
fourteen sites and persisted as `VARINT` (`chain.h:365`), so existing databases already
have it set on chainlock-conflicting headers. **Proposed: bit 256.**

**Corrected.** `BLOCK_VALID_TRANSACTIONS` is defined at `chain.h:112-116` as transactions
valid, no duplicate identifiers, sigops, size and merkle root. Over commitments only the
merkle root, duplicates and count are checkable. The level cannot honestly be granted at
accept time, so the validity ladder gains a rung.

### 2.2 A trap

`CheckBlock` caches: `validation.cpp:3857` returns early on `block.fChecked` and `3911`
sets it. If a commitment-mode check marks an object that is later filled in place,
`ConnectBlock`'s own `CheckBlock` at `2099` short-circuits and the per-transaction and
sigop checks never run on the connect path. **Materialisation must produce a fresh
object.** `PartiallyDownloadedBlock::FillBlock` already calls `CheckBlock` after
assembly (`blockencodings.cpp:203`), which is the right place for it.

### 2.3 The give-up rule

A node must never mark a body-incomplete block invalid. Doing so would permanently
reject a chain other nodes accept. Holding it incomplete and retrying costs only
liveness. So the timeout governs retry cadence, never validity, and stays local policy.

---

## 3. The tip regime

**Corrected.** v1 said no complete block is ever assembled. At the tip that is false, and
the error cost the design twice.

`ConnectTip` requires a materialised block (`validation.cpp:3020-3033`), so every node
that connected it held one. `SendMessages` announces the tip from
`most_recent_compact_block` (`net_processing.cpp:4423-4438`), and `GETBLOCKTXN` is
answered from `most_recent_block` or from disk within `MAX_BLOCKTXN_DEPTH = 10`
(`net_processing.cpp:2953-2999`, `validation.h:118`).

**Proposed.** Keep the existing compact-block path and its whole-block fallback at the
tip. It costs nothing, it is a working one-round-trip path, and retiring it would make
the block's producer the only guaranteed holder of its bodies at announcement time.

> **INSUFFICIENT ALONE (§14, B1/B2).** "Every node that connected it can rebuild it" is
> **circular**: a body-starved node cannot connect the block, so cannot be a source, and the
> attacker chooses who connects first. The fallback must be joined by §14.1's relay rule —
> **never relay a commitment block you cannot assemble yourself.**

This is also the mining fix. See §9.

---

## 3A. What a Smartnode quorum attests to

> **DECIDED 2026-09-12 (owner): Option B. The attestation is a record, not a validity gate.**
> Block validity is unchanged, and §3A.6 states what that settles and what remains to build.

**The proposal (owner, 2026-09-11).** Proof of work mines the block, but a Smartnode quorum
must approve that the network holds the bodies of every included transaction before the
block is added. Absent that approval, the block is orphaned. This makes Smartnodes a
second tier of chain security.

### 3A.1 What such an attestation can actually carry

A quorum member can only attest that the data exists once it holds the bodies and has
assembled the block. **Assembling and validating a block is exactly what adopting it as
your tip means.** So a signature reading "I hold every body in this block" carries the same
information as one reading "this is my tip."

That second signature already exists. A ChainLock already means a quorum of Smartnodes each
independently validated and adopted that block, which under this design necessarily means
each held every body in it (§3, `TrySignChainTip` signs `::ChainActive().Tip()` only).

**So the proposal restated: make ChainLocks mandatory for block validity rather than
advisory.** It should be decided as that, not arrived at through a data-availability
argument, because withholding is defeated by §3 **together with §14.1 rule 1** — with relay gated on self-assembly
an unassemblable block never propagates as a most-work header. The fallback alone is circular
(§3).

### 3A.2 Option A — attestation as a gate

**Mechanism.** A block cannot contain a signature over itself, so the gate cannot be "wait
and see if a ChainLock arrives" — that would make validity depend on a timeout, and a
timeout is a consensus parameter that nodes with different network views will disagree
about. The constructible form is **the attestation rides in the next block**: a block at
height H is invalid unless it carries a valid quorum signature over height H−1, or H−k.
That is inspectable, needs no clock, and has precedent in Decred's ticket votes.

**What it buys.** Availability becomes an explicit, on-chain, signed fact by identified and
collateralised parties. Smartnodes become a genuine second security tier.

**What it costs.**

- **A liveness dependency in the block acceptance path.** Today, if the entire Smartnode set
  vanished, Raptoreum would keep producing blocks; ChainLocks and InstantSend would stop and
  confirmation would fall back to proof-of-work probabilities. Under Option A **the chain
  halts.** A miner cannot produce a valid block without a quorum signature over its parent.
- **A censorship surface.** A colluding quorum majority could refuse specific blocks.
  Proof of work no longer decides alone.
- **It promotes a known defect into a halt condition.** The signing-attempts process is
  deliberately skipped in this implementation (`llmq/quorums_chainlocks.cpp:268-271`) and
  fails when two blocks compete at one height. Today that delays finality. Under Option A it
  **stops the chain** until one side wins. This would have to be built first, not after.
- **A new consensus field** carrying the quorum signature, adding to §7.

### 3A.3 Option B — attestation as a record

**Mechanism.** The same signature, recorded, with no effect on validity. A block is valid or
not exactly as in §2.

**What it buys.** The thing an attestation genuinely provides and nothing else in this design
does: **identified, collateralised parties on record saying they hold this data.** That is
what turns retention from a hope into an obligation that can later be enforced, and it is
what Flow's collection guarantee does (§11A). You cannot penalise a party that never
promised anything, so the deferred retention layer of §8 will need this or something like it.

**What it costs.** Almost nothing. No liveness dependency, no veto, no halt condition, no
change to chain selection.

**What it does not buy.** Any additional availability, because withholding is already
defeated (§3).

### 3A.4 The comparison

| | Option A — gate | Option B — record |
|---|---|---|
| Availability at block time | No improvement over §3 | No improvement over §3 |
| Accountability for retention | Yes | Yes |
| Smartnodes as a security tier | Yes, with a veto | No |
| Chain halts if quorums stall | **Yes** | No |
| Censorship surface | **Yes** | No |
| Requires the signing-attempts fix first | **Yes, blocking** | No |
| New consensus field | Yes | No |
| Chain selection changes | Yes | No |

### 3A.5 Why B and not A

The two options answer different questions and only one is about data. Option A is a
decision to make Raptoreum a hybrid chain, where proof of work proposes and a collateralised
quorum disposes. Coherent and defensible, but it is not justified by the availability
problem, which §3 plus §14.1 rule 1 solves, and it buys a halt condition and a censorship surface that
do not exist today.

Option B gets the accountability in full at almost no cost, and it is a prerequisite for the
retention layer deferred in §8 either way. **A contains B**, so if the gate is ever wanted
later, this is its first half and nothing here forecloses it.

### 3A.6 What the decision settles, and what is still to build

**Settled.** Block validity is untouched (§2). No quorum veto, no halt condition, no
censorship surface, no change to chain selection, and no new consensus field — the coinbase
stays at the three additions in §7.

**Not settled, and upgraded since: the signing-attempts gap is a prerequisite, not a
limitation.** Option B means it can no longer halt the chain, which is what this decision
bought. But §3A.7 shows it is the *only* defence against the one attack that survives the relay
rule, so it moves from "worth fixing" to **required, and before the fetch protocol**.

### 3A.7 Manufactured quorum splits — existing mechanism, new reachability

**All of this is how the chain behaves today.** `TrySignChainTip` signs only the connected tip,
and refuses to sign twice at one height (`llmq/quorums_chainlocks.cpp:280-283`, the
`lastSignedHeight` check). The DIP8 signing-attempts process, which exists to let a quorum retry
with a fresh request id and converge, is deliberately skipped (`:268-271`). So when two blocks
compete at one height, members split their signatures, no ChainLock forms, and that height falls
back to proof-of-work finality.

**What decoupling changes is not the mechanism but who controls it.**

**The objection first, because it is a good one.** Selective delivery is available *today*: a miner
holding a whole block can hand it to part of the quorum when an honest sibling appears. So why is
this new?

Because today it is a **race**, not a choice. A node already on the sibling does not reorg to an
equal-work block, so the attacker must land its block on chosen members *before* the sibling
reaches them, against gossip it does not control. The window is short and uncontrolled.

**Decoupling separates "arrives" from "connectable".** The commitment reaches everyone instantly
because it is tiny, and connectability is gated on a body the attacker releases at a moment of its
choosing. There is no race to win and no window to hit. That is the difference of kind, and it is
what the promotion in §3A.6 rests on.

The sequence, and note that data availability is never denied:

1. Mine H. Deliver its bodies to a subset of the quorum; withhold from the rest.
2. The subset connects H and signs it. The remainder are still on the honest sibling H' and sign
   that.
3. Bodies now propagate normally — the subset serves them, gossip repairs availability within
   seconds. **This changes nothing**, because both halves of the quorum have already spent their
   signature for that height and there is no second round.
4. No ChainLock at that height.

**Why the obvious objection does not hold.** "The nodes that have the data will rebroadcast it"
is correct and irrelevant. The attack does not depend on denying data; it depends on the quorum
committing signatures before the data finishes spreading, and on the one-signature-per-height
rule latching the split permanently.

**Severity is bounded**, which is worth stating. The attacker gains no fork and no halt. One
height loses near-instant finality and falls back to proof of work, reopening a double-spend
window that ChainLocks normally close. It also requires catching a race rather than being
available on demand.

**Mitigation:** build the signing-attempts process. It is the mechanism whose entire purpose is
letting a quorum converge when two blocks compete at one height, which is exactly the condition
being manufactured.

**Not settled: "ChainLocks already do this" is nearly true and falls short in one place.**

A ChainLock is a **recovered BLS threshold signature**. By construction it identifies no
individual signer — that is what threshold signatures are for. So a ChainLock proves *a
quorum* adopted the block, and therefore that a quorum held its bodies, but it **names nobody
who can later be held to it.** Accountability needs a party.

The attributable information exists and is thrown away. `CSigShare`
(`llmq/quorums_signing_shares.h:40-56`) carries a `quorumMember` index alongside each share,
so every contribution is attributable while the signing session runs. Those shares live in
memory and are discarded once the recovered signature is assembled.

**So Option B reduces to: retain attributable attestations.** Two shapes:

- **(a) Persist the contributing shares** alongside the recovered signature. Cheapest, reuses
  everything, and the attributability is already in the wire format.
- **(b) A distinct availability message** that members sign and that is retained. More
  explicit, more code.

A point worth carrying into the retention design: a ChainLock share asserts *"this is my
tip"*, which implies **having** the bodies, not promising to **keep** them. Enforcement
eventually wants an explicit retention promise. That is a choice inside Option B and is
deferred with the rest of §8.

**When to build it.** It is a prerequisite of the deferred retention layer, not of
decoupling. Nothing in §13's components A through H is blocked on it. It should be built
before challenges exist, because a challenge can only penalise a party that promised
something.

---

## 4. The history regime

Outside the tip window no complete block exists anywhere. Requests must be by identifier
and cannot be positional.

`blockencodings.h:39` — `BlockTransactionsRequest` carries `std::vector<uint16_t>
indexes`: positions within one block, sent to the peer that announced it. It cannot name
anything past 65,535, and it means nothing to a peer that does not hold that block.

**Corrected — `getdata MSG_TX` is not reusable as v1 suggested.** What it serves today
(`net_processing.cpp:1745-1770`) is the relay map, whose entries expire after fifteen
minutes (`4616-4624`), or the mempool only if the peer sent `MEMPOOL` and the
transaction predates that request (`1762-1764`). **It never reads disk.** Serving
historical bodies is entirely new code, not a reuse.

The request scheduler is announcement-driven. `RequestObject`
(`net_processing.cpp:890-905`) queues an inventory item only for the peer that announced
it, and historical bodies are announced by nobody. There is no primitive for "ask this
peer for this hash". Transaction requests are also refused outright during initial block
download: `net_processing.cpp:2851-2853`, with `allowWhileInIBDObjs` containing only
`MSG_SPORK` (`2841`).

---

## 5. The body store

**It is not an index.** `index/txindex.h` is the closest existing shape, but
`CDiskTxPos` (`index/disktxpos.h`) is a byte offset *into a block file*, computed at
`index/txindex.cpp:210` from cumulative serialized transaction sizes. With identifiers on
disk those offsets point into identifier bytes. `TxIndex` does not merely go unused; it
breaks, and `GetTransaction`'s index branch (`validation.cpp:984-987`) must be
re-pointed at the body store.

**Corrected — v1 put the write on the wrong side of the thing it gates.**
`CTxMemPool::removeForBlock` (`txmempool.cpp:1103`) is called from
`validation.cpp:3074`, inside `ConnectTip`, after `ConnectBlock` has already succeeded.
Bodies are needed before that. Side-chain blocks are accepted and persisted
(`validation.cpp:4349`) and never connected at all, and `NewPoWValidBlock`
(`validation.cpp:4340`) announces the block to peers before `ConnectTip` runs.

**Proposed.** Bodies are written during acceptance, transactionally with the commitment
write, in `AcceptBlock` / `SaveBlockToDisk`.

> **NEVER write to the permanent store at mempool acceptance.** Unmined transactions
> pay no fee, so an attacker fills every node's disk at line rate for free until
> `AbortNode("Disk space is low!")` — on a Smartnode that is a PoSe ban two DKGs later. The
> 300 MB mempool cap bounds RAM, not the file. A cache is fine if it evicts with the mempool.
> §10.3's privacy regression is the second reason.

**Proposed layout.** A third flat-file series beside `blk*.dat` and `rev*.dat` over the
same `FlatFileSeq` machinery (`validation.cpp:200-202`), with a key-value map from
identifier to position. Adequate for the recent window; see §8 for why it is not
adequate for history.

### 5.1 Storage engine

**Not a key-value store for the bodies.** This tree already reached that conclusion: block
data lives in flat files and not in LevelDB, because LSM compaction rewrites large values
repeatedly as they migrate between levels. At a terabyte a year of immutable blobs the
write amplification is the dominant cost.

**Flat files in block order, plus a small index.** Values appended to a third file series;
a LevelDB map holding 32-byte keys against ~12-byte positions, where small values make LSM
behaviour harmless. This is the existing `blk*.dat` plus `TxIndex` pattern, and it is the
design the literature calls key-value separation (WiscKey). RocksDB's BlobDB is the
off-the-shelf version, with blob garbage collection already solved, at the cost of a large
new dependency and the locality below.

**The decisive property is assembly locality.** Bodies are written in block order, so
assembling a block is one contiguous read. Assembly is the hot path: it happens for every
block, on every node, on the critical path to connecting the tip. A key-ordered store
scatters one block's bodies across the whole dataset and turns that single read into
thousands of random ones. The identifier is a hash, so key order is random order.

### 5.2 Addressing: position, not hash — a reversal of §4

§4 argues against positional requests because a position means nothing to a peer that does
not hold the block. **Under §0's model that objection disappears.** Commitment blocks are
the chain, so every node holds every commitment, forever. A position becomes a universal
address.

**Proposed.** Requests are `(height, index)` or `(height, range)`, not bare identifiers.
Four consequences, all good:

- **Compact.** A height plus a varint index, against 32 bytes.
- **Verifiable.** The responder's body must hash to the identifier the chain already
  commits at that position. Nothing else needs checking.
- **Local.** The server reads contiguously, matching §5.1.
- **It demotes the identifier index to optional.** A serving node needs only a per-block
  offset table, not a global map over every transaction that ever existed. At 100 tx/s
  that map would be roughly 200 GB per year of pure random-access structure.

A bare-hash lookup path remains useful for "show me this transaction", and is served by
nodes that choose to maintain the optional identifier index — which is exactly what
`-txindex` is today, and exactly as optional.

Addressing therefore serves block assembly, light-client queries and storage challenges
alike, and the challenges of the deferred §8 sample by `(height, index)` in any case.

---

## 6. Reorg and disconnect

**Confirmed.** Undo data alone will not disconnect a block. `DisconnectBlock`
(`validation.cpp:1653`) asserts `vtxundo.size() + 1 == vtx.size()` at `1658`, iterates
each body's outputs and inputs from `1679`, dispatches on `tx.nType` and reads asset
payloads at `1746-1775`, and reaches `GetCommitmentsFromBlock`
(`llmq/quorums_blockprocessor.cpp:261`) through `evo/specialtx.cpp:177`.

Two refinements found since v1:

- `deterministicMNManager->UndoBlock` (`evo/deterministicmns.cpp:653-686`) needs only the
  block hash; it replays the evoDb diff.
- `DisconnectTip` (`validation.cpp:2917-2920`) pushes bodies into the disconnect pool for
  mempool re-add, so the window must cover full reorg depth for that reason as well.

The recent window is therefore one window, not two, and it can never be body-free at any
depth a node might disconnect.

**A second floor, unrelated to reorgs.** `NodeRoundVoting` (`update/update.cpp:157-185`)
reads `RoundSize()` previous blocks from disk, 720 on mainnet
(`chainparams.cpp:213`), asserting success at `164`, and scans their bodies for quorum
commitments. Its result gates `QUORUMS_200_8` and the assets activation
(`validation.cpp:1727,1746,1793`). This is consensus logic over bodies up to 720 blocks
deep, and it sets the real minimum for the tip window.

---

## 7. Coinbase commitments

`evo/cbtx.h` — `CCbTx`, `CURRENT_VERSION = 2`, additive serialization guarded on
`nVersion >= 2`. It went from v1 to v2 to add `merkleRootQuorums`, so a v3 follows a
shipped pattern.

**The coinbase reaches the header only as a hash.** It is the first transaction in the block,
so its hash is the first merkle leaf, and the header carries only the root. Three consequences,
and they are why this is the right place for new commitments:

- **The header stays 80 bytes** however much the coinbase grows. That is why this lineage
  already put `merkleRootMNList` and `merkleRootQuorums` here rather than inventing header
  fields.
- **It is still fully committed.** Change a byte, the coinbase hash changes, the merkle root
  changes, the header changes, the proof of work is void.
- **A light client can still read it**, with one extra step: the coinbase transaction plus a
  merkle path proving it is leaf zero. The machinery exists — `CSimplifiedMNListDiff` already
  ships a coinbase plus a partial merkle tree so a client can verify the Smartnode root against
  a header it trusts (`evo/simplifiedmns.cpp:242-253`). The state root verifies the same way,
  through the same code.

Of the five fields proposed in the scaling document, three survive.

**Body merkle root — drop. Confirmed redundant.** Order is fixed by the header root and
membership by each body hashing to its committed identifier. Nothing a second root
checks is not already checkable.

**Transaction count — drop.** It is the length of the committed vector.

**Aggregate fee total — keep ONLY with a consensus equality, else drop (§14).** Nothing binds
the field to reality. The coinbase bound is `GetValueOut() <= nFees + subsidy + specialTxFees`
(`validation.cpp:2540-2549`) and the Smartnode share derives from a block value including fees
(`:1128-1139`) — both need bodies. Without a rule `cbtx.totalFees == nFees + specialTxFees`
enforced in `ConnectBlock` the field is a **miner-chosen number**, and a light client verifying
against it verifies nothing.

**The original reasoning, as far as it goes.** A node holding the bodies
computes fees exactly as today; `ConnectBlock` accumulates them at
`validation.cpp:2200-2244`. The field lets a node that does *not* hold the bodies check
the coinbase. That is a light-client convenience, not a soundness requirement.

**Total body byte size — keep, and note the limitation.** It is the only pre-assembly
bound on body volume, but it is a claim by the miner. A node cannot verify it without
assembling, so the cap it enforces is advisory until then.

**UTXO set root — keep, and note it is separable.** It makes snapshot sync verifiable against a
ChainLocked root (§7A.7) rather than against a hardcoded hash and has nothing to do with decoupling. This tree has
`dumptxoutset` (`rpc/blockchain.cpp:2794`) and `SnapshotMetadata`
(`node/utxo_snapshot.h`) but no load path: no `loadtxoutset`, no `ActivateSnapshot`.

---

## 7A. The state root, explained

### 7A.1 Two different things a chain holds

**History** is the ordered list of transactions. Immutable, append-only, grows forever.

**State** is the current situation: who owns what, right now. It is small, it changes every
block, and it is a *pure function of history* — replay every block from genesis and you get
it, deterministically.

On this chain state is three stores:

| Store | Contents | Committed today? |
|---|---|---|
| Coins (`pcoinsTip`) | Every unspent output | **No** |
| Asset state (`passetsdb`) | Asset records; per-address balances exist only under `-assetindex` and are **not** committed (§7A.6) | **No** |
| Evo state (`evoDb`) | Deterministic Smartnode list, quorum state | **Yes** — `merkleRootMNList`, `merkleRootQuorums` |

### 7A.2 Why the existing merkle root is not enough

The block merkle root commits to **what was ordered**. The state root commits to **where
that left things**. Getting from the first to the second requires replaying every
transaction, which is precisely the work a node without history cannot do.

### 7A.3 Why it is necessary here specifically

Today a node syncs by downloading every block and replaying it. It computes state itself
and therefore trusts nobody. That is what being a full node means.

Under §8's tier model, a miner or ordinary node holds only a recent window. **It cannot
replay from genesis**, because the bodies are not on its disk and fetching all of them would
mean holding everything, which is the thing the tier exists to avoid.

So it must obtain state from someone else. Without a commitment, that means trusting them:
a hostile peer could hand over a coins set with an extra million RTM at an address of its
choosing, and the receiving node would have no way to tell.

**The state root moves the *availability* question onto the chain.** A node fetches a snapshot
from any source, computes the fingerprint of what it received, and compares it against a root
committed on-chain. **It does not make validity trustless.** A header chain proves work, and work
is rentable by the hour, so below the snapshot base this is SPV-grade. What makes the source
irrelevant is the **ChainLock anchor of §7A.7**, not the root alone.

> **CORRECTED (§14).** An earlier version said this moves trust off developers onto miners and
> treated that as the win. It is the win for *availability* and a **loss for validity**. A header
> chain proves work, not validity, and work is rentable by the hour. A node that cannot replay has
> **SPV security below its snapshot base**, where a replaying full node today cannot be fooled by
> any amount of hashrate. **The root alone is not an anchor — see §7A.7.**

**Without it the design has no beneficiaries.** A node that cannot get verified state must
replay from genesis, so it must hold all history, so only Smartnodes gain from decoupling —
and Smartnodes hold everything regardless. The state root is what gives decoupling anyone to
be useful to.

### 7A.4 What it actually is

**A hash over a set**, where the set is every state element: each unspent output, each asset
record.

The naive construction sorts every element and hashes them in order, reads the whole database,
and takes minutes. Impossible at a two-minute interval.

> **DO NOT COPY `hash_serialized_2` AS THE ELEMENT FORM** (`node/coinstats.cpp:29-42`). It omits
> `nType` and `vExtraPayload`, and it carries a precedence bug at `:32` —
> `nHeight*2 + fCoinBase ? 1u : 0u` evaluates to `1` for every coin — so it commits to **neither
> the height nor the coinbase flag**.

> **INVARIANT (§14).** The element is `outpoint || Coin::Serialize` verbatim. **Anything
> `CheckTxInputs` can read from a `Coin` must be inside the element.** This tree's `Coin` is not
> Bitcoin's: it carries `nType` and `vExtraPayload` (`coins.h:72-79`), where FutureTx locks live
> (`coins.cpp:127-130`), and `validateFutureCoin` enforces the lock from exactly those fields at
> spend time (`consensus/tx_verify.cpp:99-122`, from `CheckTxInputs` at `:362`). Omit them and a
> snapshot source can **strip a lock** so a locked coin spends early, or **add one** so a victim's
> coin is permanently unspendable — both with a matching root.

What is needed is a hash that **updates incrementally**: when a block spends 500 outputs and
creates 600, subtract 500 elements and add 600, in microseconds, without touching the rest.

That is a **multiset hash**. Each element maps to a large number; elements combine by
multiplication modulo a large prime. Multiplication commutes, so order does not matter —
correct, because a set has no order. Division undoes it, so removal is as cheap as addition.
This is MuHash, and it does not exist in this tree.

So concretely: **a running multiset hash over state elements, updated as each block is
applied and as each is disconnected, stamped into the coinbase.** Roughly 32 bytes per block,
and a few field operations per output created or spent.

#### From element to number

The digest is not what gets multiplied. It is a seed, and the expansion is the point.

```
element bytes            71 B    the outpoint and coin, serialised
  --SHA256-->            32 B    a digest, and only a seed
  --ChaCha20-->         384 B    keystream
  --read as integer-->  3072 b   the number that is multiplied   = h
```

**Why expand.** The construction is secure only if nobody can find two different sets of
elements whose products are equal. Multiplying 256-bit values would leave room to hunt for
multiplicative relations — choosing elements whose numbers share convenient factors and
combining them into a collision. A 3072-bit group removes that. Bitcoin's implementation uses
the modulus 2^3072 − 1103717, whose form (two-to-the-n minus a small constant) makes reduction
a shift and a small multiply-add rather than a division.

**The accumulator is 768 bytes, not 32.** The numerator and denominator are themselves
3072-bit numbers. Only the finalised root is small: multiply N by the inverse of D, SHA256 the
result, and 32 bytes go into the coinbase.

#### A worked block

A coinbase and two ordinary transactions. Hex and accumulator values are illustrative; the
counts, structure and costs are not.

```
BLOCK 1,000,000                     3 transactions (coinbase + 2)

  coinbase  4e1d9a..7b30   0 in,  3 out   miner / smartnode / founder
  tx A      a37f02..e21c   1 in,  2 out   payment + change
  tx B      b9c1e5..44a8   2 in,  1 out   consolidation

STATE DELTA
  spent   (elements removed)       3
  created (elements added)         6
  ------------------------------------
  modular multiplications          9
  modular inversions               1      <- once, at finalise only

ONE ELEMENT = outpoint || Coin::Serialize     <- the WHOLE coin
  txid           a37f02..e21c            32 B
  vout           00 00 00 01              4 B
  height+cb      VARINT(h*2+coinbase)     4 B
  amount         12500000    varint       5 B
  scriptPubKey   76a914..88ac            25 B
  nType          VARINT                   1 B  <- REQUIRED, see below
  vExtraPayload  FutureTx lock data       n B  <- REQUIRED, see below
  -------------------------------------------
  --> h, a 3072-bit number

RUNNING ACCUMULATOR              N = numerator        D = denominator
  carried in from 999,999        N0                   D0
  - spend  81bc44..09f1:1        N0                   D0*h1
  - spend  5d2077..c3e0:0        N0                   D0*h1*h2
  - spend  5d2077..c3e0:3        N0                   D0*h1*h2*h3
  + create 4e1d9a..7b30:0        N0*h4                "
  + create 4e1d9a..7b30:1        N0*h4*h5             "
  + create 4e1d9a..7b30:2        N0*h4*h5*h6          "
  + create a37f02..e21c:0        N0*h4..h7            "
  + create a37f02..e21c:1        N0*h4..h8            "
  + create b9c1e5..44a8:0        N0*h4..h9            "

FINALISE
  R     = N * D^-1  mod p                1 inversion, 1 multiplication
  root  = SHA256(R)                      32 bytes  -> CCbTx v3

VERIFY (any other node)
  applies the same 9 elements to its own N and D, finalises, compares 32 bytes.
  mismatch -> reject the block.

WHAT WAS NOT TOUCHED
  unspent outputs in the set             ~10,000,000
  of those, read                                   0
```

#### Cost, and why set size is irrelevant

**The accumulator is never iterated.** Per block the work is one modular multiplication per
element added and one per element removed. Ten million unspent outputs and millions of
addresses cost nothing, because none of them is touched. Only the delta is.

**Removal must not need an inverse.** A modular inversion is orders of magnitude dearer than
a multiplication, so a naive "divide to remove" makes disconnects and spends expensive. The
established construction keeps **two accumulators**: additions multiply into a numerator,
removals multiply into a denominator, and a single division happens only when the root is
finalised. Removal then costs exactly what insertion costs, and there is one inversion per
block rather than one per element.

Order-of-magnitude arithmetic, assuming roughly two inputs and two outputs per transaction
and a per-element multiplication in the low microseconds (**measure this before relying on
it**; the conclusion holds across a wide range of constants):

| Throughput | Tx per block | Multiplications | Per block |
|---|---|---|---|
| 10 tx/s | 1,200 | ~4,800 | ~10 ms |
| 100 tx/s | 12,000 | ~48,000 | ~0.1 s |
| 500 tx/s | 60,000 | ~240,000 | ~0.5 s |
| 2,600 tx/s | 312,000 | ~1.25M | ~2.5 s |

Against a 120-second interval, every row is comfortable. Multiplication is commutative and
associative, so the work parallelises trivially by splitting the elements across cores and
combining partial products — which matters only in the bottom row, where doing 2.5 seconds of
work inside `ConnectBlock` under `cs_main` would be its own problem.

**Asset state costs almost nothing.** It is thousands of asset records, not millions of balances:
per-address balances are **never committed** (§7A.6) and are already inside the coins multiset.

**The comparison that settles it** is against work already on the same path. Every input in a
block requires a signature verification, and that same input triggers exactly one accumulator
multiplication.

| Per input | Cost |
|---|---|
| Signature verification | ~60–100 µs |
| Accumulator update | ~2–4 µs |

So the accumulator adds a few percent to cryptography the node performs anyway. It is not a
new cost centre.

**The one genuinely expensive operation happens once, ever.** Seeding the accumulator over the
existing UTXO set at activation is a full pass. The arithmetic is smaller than it sounds — ten
million outputs at a few microseconds each is about thirty seconds — so it is dominated by
reading the whole coins database off disk, not by the multiplication. Once, at the activation
block, and never repeated.

### 7A.5 The commitment is a field; the accumulator is an object

The 32 bytes in the coinbase are only the published value. Behind them sits **a running
accumulator that is new node state, with the same standing as the coins set itself.**

It cannot be recomputed on demand — recomputation is the full-database scan the whole design
avoids — so it must be:

- **Persisted**, not derived at startup.
- **Updated on connect**: remove each spent element, add each created one.
- **Updated on disconnect**, exactly. This is where the multiset choice pays: division
  undoes multiplication, so a reorg is symmetric and exact. A sort-and-hash construction
  would have no inverse and would have to rebuild.
- **Flushed atomically with the coins cache.** If a crash lands between the two writes, the
  accumulator describes one height and the coins database another, the node computes a wrong
  root, and it forks. The accumulator has to ride in the same commit as the chainstate, not
  beside it.
- **Carried in the snapshot**, because a node starting from one must continue updating the
  accumulator and therefore needs its value, not just the state it summarises.

**So the honest count is one new field and three new objects:** the accumulator itself, a
canonical element serialisation for asset state (§7A.6), and a snapshot format covering all
three state stores.

### 7A.6 Asset state — smaller than it looks

> **CORRECTED (§14).** An earlier version said the volume is per-address-per-asset balances and
> called that the hard half. Wrong on both counts.

**Per-address balances must NEVER be committed.** `mapAssetAddressAmount` exists only under
`-assetindex` (`assets.cpp:322,349,374`), which **defaults off** (`validation.cpp:118`), and it
**clamps negatives to zero** (`:394-395`). Committing it would fork nodes **by configuration**.
Asset ownership already lives in the coins set as asset scripts with per-asset conservation
enforced on every non-mint transaction (`consensus/tx_verify.cpp:253-334`), so **balances are
already inside the coins multiset**.

**Commit `mapAsset` and `mapAssetId` in full** — one record per asset, thousands not millions.
Every `CAssetMetaData` field (`assets.h:40-56`) plus `CDatabaseAssetData.blockHeight/blockHash`
(`:107-109`). Omit `ownerAddress` and the attacker becomes owner, whose signature
`CheckMintAssetTx` then verifies (`evo/providertx.cpp:372`); omit `mintCount`/`maxMintCount` and
minting is uncapped or permanently blocked; omit `decimalPoint`/`updatable` and `validateAmount`
(`assets.cpp:611-617`) treats transfers differently.

**Proposed form:** `assetId || CDatabaseAssetData` and `name || assetId`, **with a domain tag
byte** so a coin element and an asset element can never collide.

**The hard part is canonicality, not volume.** Identical state must produce identical bytes on
every node or roots diverge and the chain forks.

### 7A.7 The snapshot anchor

The root of §7A.3 is necessary and **not sufficient**. Solana has run validators off snapshots
for years and does not solve this cryptographically either. It stacks three partial answers, and
the load-bearing one is not the hash.

| Leg | Solana | Here |
|---|---|---|
| Internal consistency | Recompute the account hash over what was loaded | Recompute the accumulator; one pass (§7A.4) |
| Consensus weight | What fraction of **stake** voted for that hash | **ChainLock over the snapshot base** — identified, collateralised quorum |
| Configured trust | `--known-validator`, `--only-known-rpc` | An operator-named source set |

**Why the middle leg cannot be proof of work.** Work behind a header is rentable by the hour;
collateral behind a signature is not.

**Proposed rules.**

1. The snapshot base must be **ChainLocked** and buried at least 720 blocks (§8.2).
2. The lock is verified against a quorum derived from `merkleRootMNList`, syncable from any peer
   via the simplified list diff and checked against headers.
3. Commit **coins count and serialised byte size** beside the root, and chunk the snapshot with
   per-chunk hashes, so a hostile source cannot stream hundreds of gigabytes before a mismatch is
   detectable (§14, C4).
4. An operator escape hatch naming snapshot sources.

Note the anchor here is the **ChainLock**, not accumulated work, so this design does not depend
on `nMinimumChainWork`, checkpoints or assume-valid being current. That is deliberate: it keeps
the anchor on collateral rather than on rented hashrate.

**The honest statement.** Quorum membership derives from a chain that bottoms out at a shipped
value, exactly as Solana's does. **Nobody escapes an anchor.** The question is how thin it is and
how many checks stack above it, and an identified set is a better place to rest it than rented
hashrate. **This is what §3A's Option B buys a second time** — the attestations chosen for
retention accountability are the signal this anchor needs.

The coins set is already a well-defined set of records: an outpoint maps to a coin. Nothing
needs deciding.

Asset state is not. `CAssetsDB` (`assets/assetsdb.h:52`) holds flat key-value rows — asset
metadata, asset ids, per-address amounts — with **no canonical ordering and no defined
element**. Before anything can be hashed, someone has to decide exactly what an element is
and exactly how it serialises.

That decision is consensus-critical in the strictest sense. Every node must produce the same
bytes for the same state, or they compute different roots, disagree about validity, and fork.
It is design work rather than porting, and it is the part of component A with no reference
to copy.

---

## 8. Node tiers (owner decision, 2026-09-11)

**Simplification adopted.** Retention, pruning of history and sharding are deferred until
the decoupling design itself is settled. Until then:

| Tier | Holds | Purpose |
|---|---|---|
| **Smartnode** | UTXO set, all commitments, **all bodies** | The history tier. Full replication across the set. |
| **Mining / pool** | UTXO set, commitments, bodies for a recent window | Select transactions, validate, produce blocks |
| **Ordinary full** | UTXO set, commitments, recent window | Validate; may prune freely |

### 8.1 What this dissolves

v2 named one remaining structural obstacle: historical fetch is a search, because nothing
says who holds what and the request scheduler cannot ask a peer for a hash it never
announced. **Full replication removes it.** The arithmetic parked in §8.6 bottoms out at
retention fraction 1, where it does not bind.

Holder discovery is answered by machinery that already exists and is already consensus
state. `CDeterministicMNList` carries every Smartnode's address on-chain
(`evo/deterministicmns.h:61-73`), so the chain already publishes a directory of
addressable servers, any one of which can answer any request. No new registry, no
assignment scheme, no advertisement.

What is left is a connection budget rather than a search: general outbound is capped at 8
(`net.h:85`), and `ThreadOpenSmartnodeConnections` (`net.cpp:2291`) returns early until
the chain is synced (`:2310`), which is exactly when body fetch is needed. Smaller
problem, and engineering rather than design.

### 8.2 What this makes mandatory

**The UTXO commitment root stops being optional.** A node holding only a recent window
cannot replay the chain from genesis, because replay needs every body. Its only route to a
validated tip is a snapshot verified against the chain itself. This tree has
`dumptxoutset` (`rpc/blockchain.cpp:2794`) and `SnapshotMetadata` (`node/utxo_snapshot.h`)
but no load path: no `loadtxoutset`, no `ActivateSnapshot`. **The mining and full tiers
above do not exist until that is built.** It moves from §7's "separable, worth doing" to a
hard prerequisite.

**A UTXO snapshot is not a chainstate snapshot on this chain, and that is the one place
this simplification breaks.** `dumptxoutset` (`rpc/blockchain.cpp:2794`) writes the coins
cursor and metadata only. RTM carries two further derived state stores:

- **evo state** (`evo/evodb.h:34`) — deterministic Smartnode list and quorum state. In
  good shape: it is already committed per block as `merkleRootMNList` and
  `merkleRootQuorums` (`evo/cbtx.h`), and `CSimplifiedMNList` (`evo/simplifiedmns.cpp`)
  already provides a compact, verifiable sync path.
- **asset state** (`assets/assetsdb.h:52`) — **committed nowhere.** There is no asset root
  in the coinbase or anywhere else, and it is not in the snapshot file.

Assets are consensus: create and mint are special transactions and activation is gated at
`validation.cpp:1727,1746,1793`. So a node started from a UTXO snapshot cannot obtain or
verify asset state, and therefore cannot validate blocks containing asset transactions.
Its only alternative is replay from genesis, which needs every body and defeats the tier.

**Consequence.** §7's "UTXO set root" must become a state root that also covers asset
state, or assets need a root of their own. Either way it is a new consensus commitment and
a new snapshot format, and it is a prerequisite for the mining and ordinary-full tiers
rather than a refinement of them.

**And the primitive for maintaining it does not exist in this tree.** The coinbase bytes
are trivial; producing the value every block is not.

- The only UTXO set hash here is `hash_serialized_2`, computed by cursoring the entire
  coins database (`node/coinstats.cpp:59-109`, `rpc/blockchain.cpp:1377-1417`). There is
  no incremental alternative: **no MuHash**, no rolling set hash of any kind. Per-block
  iteration of the coins DB is not viable.
- `CAssetsDB` (`assets/assetsdb.h:52`) is a plain `CDBWrapper` holding flat key-value
  records: asset metadata, asset ids, and per-address amounts. **No Merkle structure and
  no canonical ordering**, so there is nothing to take a root over.

**Proposed.** Backport an incremental multiset hash (MuHash, Bitcoin 0.21) and apply it to
both the coins set and asset state, which is expressible as a set of records. This is the
one place the design needs a primitive rather than a rearrangement, and it gates whether
the mining and full tiers can exist at all.

**The recent window has a floor of 720 blocks, not the reorg depth.** Per §6,
`NodeRoundVoting` (`update/update.cpp:157-185`) reads `RoundSize()` blocks of bodies with
an assert and its result gates consensus. At two-minute blocks that is about a day.
`DisconnectTip`'s disconnect pool, `VerifyDB`'s `-checkblocks` and `MAX_BLOCKTXN_DEPTH`
all sit well inside it.

**The body store becomes append-only.** No deletes, no compaction, no record-level
retention, no prune-accounting rework. The flat-file series proposed in §5 is sufficient.
The storage engine v2 called the largest piece of new code is deferred with the retention
question.

**The Smartnode transaction-index requirement becomes consistent rather than
contradictory.** §8.1 of v2 argued it could be dropped; under this model it does not need
to be, and the `CheckCanLock` refactor is no longer on the critical path.

### 8.3 One thing to note, not to solve now

Smartnodes do not need history for their own duties. InstantSend and ChainLocks work
inside the recent window and ProTx validation uses the coins view. Under this model a
Smartnode carries the full history as a **service to the network, not for its own
function**. What makes that rational is the incentive question, deferred with the rest.

### 8.4 Is deferral safe?

**Yes.** Four reasons, all structural rather than hopeful:

- **The permanent commitment list preserves the option.** Anything deferred can still be
  named later. A chain that deletes its index can never add retention machinery; this one
  can, at any point. See §11A.
- **The coinbase is additively versioned.** `CCbTx` went v1 to v2 already, so a fragment
  root or a coverage commitment is a cheap later addition (§7).
- **Most of what is deferred is not consensus.** Who holds what, discovery, challenges and
  repair are policy and protocol. Only the *reward* half touches consensus, by weighting
  the Smartnode payment queue.
- **Activation machinery exists.** Raptoreum's `UpdateManager` (`update/update.h`) is a
  working deployment mechanism with 26 unassigned bits.

**Three decisions must be taken now, because retrofitting them is expensive:**

1. **Choose a body-store format that can delete records, even if it never does.**
   Retrofitting deletion into an append-only store is a data migration across every
   Smartnode, not a code change.
2. **Make "I do not have that" non-punitive — FOR HISTORY ONLY.** Under partial retention a
   miss is normal, and a v1 that bans on miss makes partial retention unshippable without a
   second protocol change. **But split it by regime (§14.1).** Non-punitive applies to history
   addressed by `(height, index)`. A peer that *announced* a block and then declines to deliver
   keeps today's obligation and today's disconnect (`net_processing.cpp:4649-4673`). Two
   independent reviews found a global rule lets one peer announce every block, deliver none, at
   no cost, forever.
3. **Never prune commitments.** No optimisation that drops old identifier lists, however
   attractive. That list is the option value for everything parked in §8.6.

### 8.5 The risk deferral creates

**Full replication couples storage cost to consensus participation.** Under partial
retention, an operator under disk pressure drops data and keeps serving. Under full
replication, the same operator drops out — and takes ChainLocks signing, InstantSend and
quorum participation with it.

So the failure mode of "storage became a problem" is not a degraded archive. It is a
shrinking Smartnode set, which is a consensus-services problem wearing a storage costume.

**The signal to watch is throughput, not disk.** Operators do not warn you before they
stop renewing; the first visible symptom is quorum participation. Watching the transaction
rate gives years of notice. Watching the Smartnode count gives weeks.

#### Growth per Smartnode, and when to act

At roughly 400 bytes per transaction, full replication costs each Smartnode about
**12.6 GB per year per sustained transaction per second**.

| Sustained throughput | Per Smartnode, per year | Assessment |
|---|---|---|
| 10 tx/s | 126 GB | Irrelevant |
| 50 tx/s | 631 GB | Comfortable |
| 100 tx/s | 1.26 TB | A 4 TB box lasts ~3 years |
| 500 tx/s | 6.3 TB | Untenable |
| 2,600 tx/s | 32.8 TB | Tier empties |

Full replication holds to roughly **100 transactions per second sustained**. The headline
target in the scaling document is about 26 times past that, so deferral is safe for a
modest contract layer and unsafe for the one that document was written for. Which case
applies is not yet known, and §12 lists it as an open question for that reason.

**Trigger.** Partial retention is a protocol change plus an incentive change plus an
activation window with third parties involved: call it a year from decision to live, with
margin on top. **Start the work when sustained volume implies less than about two years of
headroom on a commodity box**, which is roughly where annual growth crosses 1 TB per node.
Not when disks fill.

### 8.6 Parked, and why it is kept

These stop binding under full replication and become live again the moment history
pruning returns. Recorded so they are not rediscovered.

- **Sync throughput under partial retention.** With retention fraction `f` per peer, 100
  objects in flight per peer (`net_processing.cpp:69`), a 60-second retry before another
  peer is tried (`:80`, enforced `:4776`) and 8 outbound, sustained fetch is
  `peers × 100 / (60 × (1/f − 1))`: about 4.4 transactions per second at `f = 0.25`,
  about 13 at `f = 0.5`, unbounded at `f = 1`.
- **Request plumbing.** `RequestObject` (`:890-905`) queues only for the announcing peer.
  Transaction requests are refused during initial block download (`:2851-2853`, with
  `allowWhileInIBDObjs` holding only `MSG_SPORK` at `:2841`). `g_already_asked_for` holds
  50,000 entries (`:438`).
- **The storage layout cannot express partial retention.** `PruneOneBlockFile`
  (`validation.cpp:4458-4489`) clears a whole file; `FindFilesToPrune` (`:4548-4600`)
  takes files oldest-first. Record-level delete and refill needs a different engine, and
  `CalculateCurrentUsage` (`:4449-4455`) and `CheckDiskSpace` (`:2727,2766`) would both
  need to learn about a third file series.
- **Repair.** Challenges detect loss; nothing causes a replacement to fetch it. Without a
  repair loop coverage can only decay.
- **Erasure coding puts back a field §7 deletes.** §7 drops the body merkle root because a
  body hashes to its committed identifier. A fragment hashes to nothing the chain knows,
  so fragment challenges need a fragment merkle root in the coinbase. The redundancy
  argument holds only while whole bodies are the unit of storage.

## 9. Mining and block production

Neither the scaling document nor v1 discussed mining.

### 9.1 Holds, hashes, publishes

Three different things, and only the third changes.

| | |
|---|---|
| **Holds** | Everything. To select transactions the miner must validate them, to build the coinbase it must total the fees, and to get identifiers at all it hashes the bodies. All of it comes from its own mempool. |
| **Hashes** | The 80-byte header, and nothing else. Transactions were never part of what proof of work covers, and the merkle root already commits to identifiers today. **The header is byte-identical** between the two designs. |
| **Publishes** | Header, coinbase in full, identifiers. The bodies are already on the network, gossiped as ordinary transactions before the block existed. |

**Mining hardware is untouched.** A rig never sees transaction data today either: stratum hands
it two halves of a coinbase with an extranonce between them plus a merkle branch to walk, and
the rig assembles, hashes and derives the root itself. Extra fields in the coinbase payload are
invisible to that arrangement. The work sits entirely in what the pool does after a share meets
the target, which is why this component is about template and submit handling.

**A miner can commit to identifiers whose bodies it does not have.** Nothing prevents it. The
block is then unconnectable by anyone including its producer, so it can never be built on. That is
the enforcement: not a prohibition, just a block that does not pay. **It is not "orphaned" in the
usual sense and it is not harmless** — nothing may mark it invalid, so it is never discarded the
way a stale block is, and at scale this is attack B3. See §14.8.

### 9.1a The transaction the miner created itself

The case that matters is a **non-coinbase transaction that only exists at assembly time** — a
pool fee payout sized to this block's fees, say. Nobody has relayed it, so nobody can assemble
the block.

**The common case is already covered.** Pool payouts normally live in the coinbase, which a
commitment block carries **in full** by construction. This chain's coinbase already pays miner,
Smartnode and founder, so coinbase-based payout needs nothing new.

**Where it bites, it creates a new obligation on miners — but it is serving, not waiting.**
The miner holds the body and can serve it: peers request, assemble, forward, and the next hop does
the same, so body and block propagate together with one extra round trip at the frontier. The cost
is latency, not failure. **Orphaning happens only if the miner will not or cannot serve** — it
publishes and goes offline, or refuses.

So the change is not a timing rule. **Today, publishing a block discharges the miner's obligation
completely. Under this design it does not**: publishing commitments is half of it, and the miner
stays on the hook for any body nobody else has until it has spread. Nothing in the protocol teaches
this, and pool software has never had to know it.

**Proposed escape valve: bounded prefill.** The mechanism already exists in this tree.
`CBlockHeaderAndShortTxIDs` carries `prefilledtxn`, a list of `(index, full transaction)` pairs
(`blockencodings.h:71-101`); today only the coinbase is prefilled and there is a `TODO` at
`blockencodings.cpp:24` noting the mempool could fill more. A commitment block can do the same:
a small number of transactions carried whole alongside the identifiers.

**It must be bounded by an explicit byte budget counted against the block limit.** Unbounded
prefill reopens unbounded block size, which is the thing this design exists to close. Bounded
prefill is therefore a **latency optimisation, not a necessity** — serving already covers the
assembly-time case (§14.10).

**Open:** whether to ship prefill at all. It buys propagation latency at the cost of a format
change and a new abuse surface to bound. Nothing depends on it, because serving covers the case.

### 9.2 The external interface

`CreateNewBlock` (`miner.cpp:134`) builds a full block in memory and calls
`TestBlockValidity` (`:262`), which runs `ConnectBlock` in check-only mode. The node has
the bodies, so that path is fine. The problem is outside this codebase:
`getblocktemplate` returns full transaction data per entry
(`rpc/mining.cpp:725-751`) and pool software assembles and serialises the block itself;
`submitblock` decodes a full block (`rpc/mining.cpp:923`). Every pool and stratum
implementation must emit the new form, and nothing in the node forces them to notice.

**The withholding advantage, and its fix.** If the whole-block path is retired, the
producer of a block is the only guaranteed holder of its bodies at announcement. Peers
cannot connect until assembly completes, while the producer mines on its own block.
Delayed or partial body release becomes a head start at no cost, which is a
selfish-mining primitive. **§14.1 rule 1 removes it** — with relay gated on self-assembly the block never propagates as a
most-work header, so honest miners keep extending the connectable tip. The tip fallback of §3 is
necessary but not sufficient on its own; its argument is circular.

---

## 10. Adversarial surface

### 10.1 Body request amplification

A request entry is 36 bytes and can pull a transaction of up to **1,000,000 bytes** — the
*consensus* limit (`consensus/tx_check.cpp:30`), not the 100,000-byte standardness cap — so
roughly **28,000x** amplification. `MAX_INV_SZ = 50,000` lets one message name 50 GB. **And §5.2's `(height, range)` addressing makes this worse, not
better**: a ten-byte range names thousands of bodies, so figures computed for 36-byte entries are
a floor. Incoming `getdata` is capped at `MAX_INV_SZ = 50,000` entries
(`net_processing.cpp:2863-2867`), so one maximal request can pull name on the order of fifty gigabytes of
scattered reads, against up to 125 inbound peers (`net.h:95`).

The only backpressure is the send buffer, `DEFAULT_MAXSENDBUFFER = 1000 KB`
(`net.h:108`), and the disk read happens before the push, so draining slowly keeps the
server seeking. `-maxuploadtarget` is enforced only in `ProcessGetBlockData`
(`net_processing.cpp:1614`) and the mempool handler (`:3667`), so body traffic falls
outside it entirely.

### 10.2 The honest traffic has the same profile

Retention refill under §8 produces the same scattered-read pattern as the attack. At
steady state the network does this to itself, so any defence must distinguish the two,
and identity is the only thing that can.

### 10.3 A privacy regression

Serving by identifier from a store written at mempool acceptance bypasses the rule at
`net_processing.cpp:1762-1764`, which exists to stop a peer probing whether a node has
accepted a given transaction. Anyone could then test acceptance the instant it happens.

---

## 11. What breaks, with locations

| Location | Issue |
|---|---|
| `update/update.h:36-40,101` | No free block-version bit; the format selector cannot live in the header |
| `chain.h:132` | Bit 128 is `BLOCK_CONFLICT_CHAINLOCK`, persisted; use 256 |
| `chain.h:112-116` | `BLOCK_VALID_TRANSACTIONS` cannot be granted over commitments |
| `validation.cpp:3857,3911,2099` | `fChecked` caching; materialisation must produce a fresh object |
| `validation.cpp:3127` | `BLOCK_HAVE_DATA` means connectable in `FindMostWorkChain` |
| `validation.cpp:3026` | `ConnectTip` treats a failed block read as `AbortNode` |
| `update/update.cpp:157-185` | Consensus vote count reads 720 blocks of bodies, asserting |
| `index/txindex.cpp:210` | Offsets point into a block file; `TxIndex` breaks |
| `validation.cpp:984-987` | `GetTransaction` index branch must be re-pointed |
| `net_processing.cpp:2851-2853` | Transaction requests refused during initial block download |
| `net_processing.cpp:890-905` | No primitive for requesting a hash a peer never announced |
| `net_processing.cpp:1745-1770` | `getdata MSG_TX` never reads disk; historical serving is new code |
| `merkleblock.cpp:53` | Bloom filtering needs bodies |
| `index/blockfilterindex.cpp:215-238` | Block filters need full scripts and undo data |
| `zmq/zmqpublishnotifier.cpp:229-237` | `rawblock` publishes a disk read; must materialise |
| `net.h:81` | `MAX_PROTOCOL_MESSAGE_LENGTH` is 3 MB |
| `blockencodings.h:42` | `uint16_t` index cannot name past 65,535 |
| `consensus/consensus.h:13-22`, `coins.cpp:285` | `MaxBlockSize` and everything derived from it |
| `validation.cpp:4449-4455,2727` | Prune accounting and disk checks ignore a third file series |
| `llmq/quorums_chainlocks.cpp` `Cleanup` | Walks all tracked transactions under `cs_main` and `mempool.cs`, every 30 seconds |
| `llmq/quorums_instantsend.cpp` | Negative lock lookups are never cached |
| `init.cpp:2143,2174-2232` | `-reindex` rebuilds by scanning block files; body files need a self-delimiting format |

One thing gets cheaper. `GetBlockTxs` (`llmq/quorums_chainlocks.cpp:441`) reads whole
blocks from disk to recover an identifier set. A commitment block *is* that set, given a
raw-commitment read API distinct from a materialising one.

---

## 10A. RPC surface

Most of it is unchanged. The changes cluster in four places, and only two of them are new
commands.

### 10A.1 Existing calls that gain a new failure mode

Anything returning transaction **contents** rather than identifiers can now fail on a node
holding only a recent window. The shape does not change; "not available here" becomes a valid
answer, and it should be an explicit, documented error rather than an empty result.

| Call | Why |
|---|---|
| `getblock` verbosity 2 (`rpc/blockchain.cpp:1203`) | Returns full transaction objects |
| `getrawtransaction` | Via `GetTransaction`, whose index branch is now optional (§5) |
| `getspecialtxes` | Returns special transactions from a block |
| `getblockstats` (`:2171`) | Computes fee and size statistics over bodies |
| REST block and transaction endpoints | Same paths |
| ZMQ `rawblock` (`zmq/zmqpublishnotifier.cpp:229-237`) | Publishes a disk read; must materialise |

### 10A.2 One family gets better

`gettxoutproof`, `verifytxoutproof` and `getblock` at verbosity 1 work over identifiers, and
**a commitment block is exactly an identifier list**. Inclusion proofs become cheap and
universally available, including on a node holding no bodies at all. Worth stating, because it
is a genuine improvement rather than a survival.

### 10A.3 Genuinely new

- **Load a snapshot.** `dumptxoutset` exists as a hidden RPC (`rpc/blockchain.cpp:2794`); there
  is no counterpart that loads one and verifies it against the committed root. Required for the
  tiers of §8 to exist at all. Its output format also has to grow to cover asset and evo state
  (§8.2), which is a compatibility break on a hidden call.
- **Fetch or repair a body range.** For an archive backfilling or an operator recovering a gap.
  The only new command with no existing analogue.

### 10A.4 Fields, not commands

Two things want reporting and neither needs its own RPC:

- **Body store status** — ranges held, disk used, gaps. Extend `getblockchaininfo`.
- **Whether a block is body-complete** — the new state of §2.1. A field in `getblock` output.

**Design position: prefer fields on existing calls.** A new RPC is API surface maintained
forever; a new field costs nothing to third parties that ignore it.

### 10A.5 Mining needs negotiation, not new calls

`getblocktemplate` and `submitblock` stay. The template needs a rule or capability string the
way segwit used `!segwit`, and submit must accept both serializations. That is parameters and
rules rather than new commands — and it is precisely the part external pool software has to be
taught (§9.2).

---

## 11A. Prior art, and what is actually novel here

Several production chains split block data from block commitments. **Every one of them
bought the split by removing the ordering layer's need to validate the data**, either by
delegating validation to another role or by deleting the data outright. This design keeps
that need while relocating the data, and that combination has no production precedent.

**Flow** is the closest structural match. Collection nodes cluster transactions and sign
collection guarantees; blocks carry an ordered list of collection hashes and *not* the
transactions. Consensus nodes work from the hash alone and do not inspect bodies during
normal operation. The price is a role split: consensus nodes never execute anything.
Validity is established afterwards by execution nodes and policed by verification nodes
with challenges.

**Ethereum blobs (EIP-4844)** put a KZG commitment in the execution layer while the data
travels a separate gossip network, is held by consensus clients rather than execution
clients, and is discarded after roughly 18 days. **PeerDAS (EIP-7594, live with Fusaka on
2025-12-03)** extends this by splitting blob data into 128 columns, each node holding and
serving a subset. That is the sharding model deferred in §8, running in production.

**Celestia** is the pure form: 2D Reed-Solomon erasure coding with namespaced Merkle
trees, light nodes sampling random chunks to gain confidence that data exists, recovery
possible from half the block. It orders and guarantees availability and executes nothing.

**Segwit**, inside this tree's own lineage, is the mild version: witness data is
separable and committed, and a node that does not fetch it cannot validate, only trust.
That is exactly the ordinary-full tier of §8.

### Proof-of-work precedent

Ethereum's blob work postdates the Merge, so it carries no information about proof of
work. Two proof-of-work chains do run without full history:

**Kaspa** prunes all block and transaction data older than roughly three days. It holds
the chain together with NiPoPoWs (compact proofs of proof of work) plus **MuHash and UTXO
commitments** — precisely the primitives §8.2 identifies as missing here. This is a
working proof-of-work reference implementation of the state-commitment hurdle, which
downgrades it from research to a port.

**Grin / Mimblewimble** goes further: cut-through removes spent outputs permanently, so a
new node needs only headers, the UTXO set and the transaction kernels.

**Both delete rather than relocate.** Neither guarantees the full transaction history is
retained anywhere, which is the owner constraint recorded in §8. So they answer "can a
proof-of-work chain stop holding history" and not "can it distribute history with a
guarantee."

**How Kaspa answers "I want to see an old transaction": it does not, at the protocol
level.** A standard node can serve about three days. Older data exists only on archival
nodes, which sit outside consensus, and an archival node **cannot be synced from the
peer-to-peer network at all** — it is seeded by `rsync` from another archive, because the
network no longer carries the data to serve. All headers are retained, so a transaction
handed over by an archive is still verifiable by merkle path. **Verification stays in the
protocol; availability leaves it.**

### The property this design has and the others do not

Keeping commitments forever means **the network always knows exactly what it is missing**.
Every transaction that ever existed remains nameable from the chain alone, at 32 bytes
each, even when no body is held.

Kaspa deleted the transaction lists along with the bodies, so a pruned node cannot
enumerate what an old block contained, cannot audit coverage, and cannot detect a hole.
Grin's cut-through is the same in stronger form: the data is provably gone.

**Naming is the precondition for every mechanism §8 defers.** A challenge, a retention
guarantee, a repair loop and a coverage metric all require the ability to ask for a
specific record. A chain that has deleted its own index can never do any of them.

### What this design is actually for

Consensus does not need the bodies. Kaspa demonstrates that with commitments and
proof-of-work proofs, and Grin demonstrates the stronger version. What needs the bodies is
everything downstream: explorers, an exchange crediting a deposit, a wallet rescan, an
audit, a dispute.

Kaspa answers those with `rsync` and trust in whoever runs the archive. **The purpose of
this design is to make history a protocol service with a guarantee rather than a favour**,
and the permanent commitment list is what makes a guarantee expressible at all.

### What is different here

In every example above, a node can compute the chain's state without the off-chain data.
Blobs are opaque to the EVM; Celestia executes nothing; Flow's consensus nodes delegate
execution. Data availability is therefore a **liveness and fraud-proof** concern.

Under this design the bodies *are* the transactions, and a node cannot compute the UTXO
set without them. Availability becomes a **validity** concern, which is why block
acceptance gains a network dependency (§2, §2.3) and why the incomplete state of §2.1 has
to exist at all. No production chain has that property.

Two ways out, both large. Adopt Flow's role split, so the ordering layer stops validating
and something else establishes validity. Or adopt **Mina's** answer and prove history away
with recursive proofs instead of distributing it, so nodes need neither bodies nor state.

## 12. Open questions

Ordered by what blocks the most.

**1. Is the acceptance-layer rework tractable?** §2. This is the bet of §0 stated as
engineering: block acceptance would gain a dependency on a network fetch, inside the state
machine that chooses the chain. It has been analysed here and never attempted. Nothing
should be built before this is probed, because it is the only part with no reference
implementation anywhere (§11A).

**2. What does a contract call look like on the wire?** Every volume figure assumes
payment-shaped transactions of a few hundred bytes. It decides the throughput, which
decides the storage trigger of §8.5, which decides whether the retention deferral is safe
or merely postponed.

**3. How is the state commitment built?** §8.2. The primitive is missing here and the
reference exists (Kaspa: MuHash plus UTXO commitments, §11A). Needs designing for both the
coins set and asset state, and it gates whether the mining and full tiers exist at all.

**4. Migration and activation.** Sketched only — §13.H lists the pieces and §14.5 gives the
activation attack; neither is a plan. The scaling document
proposes two-stage activation with a long live window and no rollback once commitment-only
blocks exist. That reasoning needs re-examining against the two-regime model of §0, which
may make a gentler path available, and against `UpdateManager` as the actual deployment
mechanism.

**5. How is body serving rate-limited and accounted?** §10. Smartnodes become public file
servers with roughly 28,000-fold amplification (§10.1) outside any upload accounting.

**6. Coordinating the external mining interface.** §9. Not an engineering problem, which
is why it decides schedules.

**Closed 2026-09-12 &mdash; gate or record?** §3A. Answered: record. Block validity is
unchanged. What remains is retaining attributable attestations, since a recovered threshold
signature names no one, and that belongs with the deferred retention layer rather than with
decoupling.

**Deferred by owner decision (§8), not open:** retention mechanics, sharding, challenges,
incentives, repair, erasure coding. Recoverable later because the chain keeps the names
(§11A).

---

## 13. Components to build

Ten. Two of them deliver value on their own and depend on nothing here.

### A. State commitment and snapshot sync — *prerequisite, independently useful*

- Incremental accumulator over the coins set (MuHash backport; Kaspa is the working
  proof-of-work reference, §11A). This tree has only `hash_serialized_2`, which cursors
  the whole database (`node/coinstats.cpp:59-109`).
- The same over asset state, which first has to be expressed as a canonical record set;
  `CAssetsDB` (`assets/assetsdb.h:52`) is flat key-value with no ordering.
- `CCbTx` v3 carrying the state root (§7).
- A snapshot format covering coins, asset state and evo state. `dumptxoutset` writes only
  the coins cursor.
- The load path: `loadtxoutset` / `ActivateSnapshot` / `PopulateAndValidateSnapshot`,
  verifying against the committed root. None of it exists here.

**Ships alone.** Verified fast sync is worth having with or without decoupling, and the
mining and ordinary tiers of §0 cannot exist without it.

### B. ChainLocks and InstantSend volume work — *independently useful*

- Rewrite `Cleanup` (`llmq/quorums_chainlocks.cpp`), which walks every tracked
  transaction calling `GetTransaction` under `cs_main` and `mempool.cs`, every 30 seconds.
- Negative caching for lock lookups (`llmq/quorums_instantsend.cpp`), where a miss costs
  up to three database reads and is never cached.
- **Build the DIP8 signing-attempts process** (`llmq/quorums_chainlocks.cpp:268-271`).
  **F depends on this** — §3A.6 and §3A.7: the fetch protocol is what makes a quorum split
  schedulable, and this is the only thing that lets a quorum converge afterwards.
- **Sign batches, not transactions** — §16.5. One threshold signature over a Merkle root of
  N transaction hashes. Cost is per session and per-session cost grows with quorum size, so
  this is the only change here that scales rather than moving a constant.
- **One session per transaction instead of inputs + 1** — a free 2× to 3×, measured, and
  independent of the batching work.
- **Give a saturated quorum a way to fail loudly.** Sessions are purged on timeout and
  nothing re-queues them, so above capacity InstantSend silently stops applying. Whatever
  the throughput fix, the absence of backpressure is its own defect.

**Ships alone.** Gates any throughput increase whether or not decoupling happens — and
§16.5 promotes it from "independently useful" to a blocker at the 2 MB design point, not
only at 10 MB.

### C. Block format and serialization

- Second serialization of `CBlock`: coinbase in full, the rest as bare identifiers.
- Stream flag, **not** a header bit (§1), with service-bit or protocol-version negotiation.
- A raw-commitment read API distinct from a materialising read, so §11's cheaper
  `GetBlockTxs` is actually reachable.
- Materialisation must produce a fresh object (the `fChecked` trap, §2.2).

### D. The acceptance layer — *the risk*

- Split "we have the block" into commitments-held and bodies-held; bit 256 (§2.1).
- A new validity rung, because `BLOCK_VALID_TRANSACTIONS` cannot be granted over
  commitments.
- Teach the roughly thirty sites above `ConnectBlock`: `FindMostWorkChain`,
  `ReceivedBlockTransactions`, candidate maintenance, `CheckBlockIndex`'s invariants,
  `ConnectTip`'s `AbortNode`, pruning, `VerifyDB`, crash replay.
- A durable incomplete-block state machine: survives restart and peer churn, retried on
  its own schedule, **never marked invalid** (§2.3).

**Probe this before building anything else.** It is the only component with no reference
implementation anywhere.

### E. The body store

- Third flat-file series, bodies appended **in block order**, not a key-value store
  (§5.1). Flat files preserve assembly locality; key order is random order.
- A per-block offset table, which is all a serving node needs under §5.2 addressing.
- The global identifier index becomes **optional**, for bare-hash lookup only. Re-point
  `GetTransaction`'s index branch at the body store; `TxIndex` becomes that optional index.
- Written during `AcceptBlock` / `SaveBlockToDisk`, transactional with the commitment
  write — **not** at `removeForBlock`, which runs after connection.
- A format that **can** delete records later, even though it never will at first (§8.4).
- Self-delimiting on disk, so `-reindex` can rebuild it.

### F. The fetch protocol

- Position-addressed request and response, `(height, index)` (§5.2). `getdata MSG_TX` is
  not reusable either way: it serves the relay map or a gated mempool read and **never
  touches disk** (§4).
- **"I do not have that" as a first-class, non-punitive answer — for history addressed by
  `(height, index)` only.** An *announcing* peer keeps today's delivery obligation and today's
  timeout-disconnect (§8.4 decision 2, §14.1 rule 2, §14.8a).
- Must work during initial block download, where transaction requests are currently
  refused outright.
- **Model body serving on the BLOCK path, not the transaction path (§14, B4).** These are
  already separated in the tree: `ProcessGetData` is `LOCKS_EXCLUDED(cs_main)`, takes the lock to
  drain non-block inventory, then **breaks and releases it** before calling `ProcessGetBlockData`
  (`net_processing.cpp:1718`, `:1879-1886`). Serving a block does not hold `cs_main` across a disk
  read; serving a transaction does. **Bodies are block-shaped work wearing transaction clothes**,
  so the instinct to hang them on `MSG_TX` puts them on exactly the wrong path and hands an
  attacker the lock that block connection, ChainLocks and the DKG handlers all need.
- **The store owns its own height index, so serving touches no chain state.** Resolving
  `(height, index)` through `ChainActive()` would need `cs_main`; a height-keyed index maintained
  by the body store at connect and disconnect needs only its own lock. **Self-verification is what
  makes the weak locking safe** — a server racing a reorg returns an answer the requester rejects
  by hash and re-requests, so it never needs a consistent view of the chain to be useful.
- Range requests, which are naturally batched. One transaction per message is untenable
  at volume (§10), and `(height, range)` makes a batch the default rather than an
  optimisation.
- Rate limiting and upload accounting, which today exclude this traffic entirely. A height
  range is a bounded ask, which makes accounting far easier than bare-hash fetch would.
- Smartnode targeting off the on-chain address list (§8.1).

### G. Block production

- Miner-side emission of commitment blocks.
- `getblocktemplate` and `submitblock` accepting both forms.
- **Coordination with external pool and stratum software**, which is not an engineering
  problem and is therefore the schedule risk (§9).

### H. Migration and activation

- A deployment bit through `UpdateManager`, which has 26 unassigned.
- The dual-format period and peer negotiation.
- The ecosystem deadline for wallets, explorers and exchanges.
- Re-examine the scaling document's no-rollback reasoning against the two-regime model of
  §0, which may make a gentler path available (§12, question 4).

### Not a component, but adjacent

**Retaining attributable attestations** (§3A.6). Small, blocked by nothing, and required
before any challenge or penalty can mean anything, because a recovered threshold signature
identifies no signer. Build it before the retention layer, not before decoupling.

### I. Safety hardening

The `HaveBodies` predicate and the audit of every path that may materialise a block; the relay
rule; the retry bound of §14.8; bounded snapshot transfer; stall rules for snapshot and body
fetch; the fail-closed flip of §14.8b. Small, and **it gates activation** rather than trailing it.

### J. Coverage telemetry

Per-peer miss rates and **per-range** coverage (§14.9), collected passively. Cheap, separable from
retention, and **required by the activation criterion of §14.5**, which currently has no source of
the data it says decides the cutover.

### Prerequisites in existing code

Not components, but they must ship before activation because changing them afterwards is itself a
hard fork: **intra-block asset state visibility** (B8) and **asset undo keyed per transaction**
(B9). Plus the signing-attempts process folded into B above.

And four constants or mechanisms that block testing rather than activation, all measured
rather than reasoned — see §16.6 for the numbers: the **relay cap re-index** (required at
2 MB, not only at 10 MB), **`maxmempool`**, **`MAX_PROTOCOL_MESSAGE_LENGTH`**, and
**template production**. The relay re-index is the one that is a design requirement rather
than a tuning change, because reconstruction from identifiers is only possible against a
pending set that has actually converged.

### Dependency shape

```
A ──────────────────────────────► (tiers exist)
B ──────────────────────────────► (volume survivable, quorum can converge)
J ──────────────────────────────► (coverage measurable)

C ──┬── D ──┐
    ├── E ──┴── F ──┬── I ── H
    └── G ──────────┘
```

A, B and J are independent of everything and of each other. **B gates F** (signing-attempts).
D is the risk. I gates H: nothing activates until the hardening is in.

---

## 14. Security review (2026-09-12)

Three independent adversarial passes on a different model, briefed to attack rather than
confirm, split across theft/forgery, consensus disruption, and availability/integrity. Plus one
pass on the migration window. Every code claim below was verified in the tree before recording.

> **SCOPE (owner, 2026-09-12).** Pre-existing Raptoreum network defects are **out of scope
> unless decoupling makes them reachable, or unless decoupling cannot ship without them fixed.**
> Several findings here are existing code — they are listed because this design turns an
> unreachable assert into a remote crash, a silent divergence into a permanent fork, or an
> optional index into a consensus input. Anything that stays a standalone RTM issue is noted in
> §14.6 and is not work for this project.

### 14.1 Three rules that close most of it

1. **Never relay a commitment block you cannot assemble yourself.** Restores relay atomicity: a
   withheld block never propagates as a most-work header. Closes B1, B2 and C8 at once. (B1 needs rule 2 as well: a header chain still reaches the
   attacker's own direct peers, and it is the announcer timeout that stops them being held.)
2. **Split the non-punitive rule by regime.** Non-punitive for history by `(height, index)`;
   a peer that *announced* a block keeps today's delivery obligation and today's disconnect.
   Found independently by two reviewers.
3. **One `HaveBodies(pindex)` predicate, and no network-triggered path may materialise a block
   without consulting it.** Closes the whole A1 crash class.

Plus one that costs almost nothing: **record miss rates and per-range coverage passively from
day one.** Ship the metric with the protocol, not with the retention layer. It is the only way
targeted erasure (C3) becomes visible before it completes, and it is what §14.5's activation
criterion requires.

### 14.2 Critical

| # | Attack | Mechanism | Fix |
|---|---|---|---|
| **A1** | **Remote crash of every windowed node, 37 bytes** | `ProcessGetBlockData` gates on `BLOCK_HAVE_DATA` and then `assert(!"cannot load block from disk")` (`net_processing.cpp:1640,1648`). A windowed node holds commitments for all history, bodies for 720 blocks. One `getdata MSG_BLOCK` for an older block kills it. Pool addresses are public. Same hazard on GETBLOCKTXN (`:2953-2999`), `MSG_FILTERED_BLOCK` (`:1650-1676`), ZMQ, `GetTransaction`. | Rule 3. Also: windowed nodes must not advertise `NODE_NETWORK`; reuse `NODE_NETWORK_LIMITED` semantics (`:1625-1637`). |
| **A2** | **ChainLock enforcement stalls or kills a node, zero hashrate** | `EnforceBestChainLock` marks competitors conflicting and **disconnects the victim's connectable tip** to move to the locked block, which the victim cannot connect without bodies. A failed disconnect hits `assert(false)` (`llmq/quorums_chainlocks.cpp:496-575`, `:537`). Unreachable today because having the header means having the block. | Gate marking and tip disconnect on `HaveBodies(B)`; fetch first, prioritised, from the quorum members whose shares prove they hold it. Convert the assert to a graceful abort. |
| **A3** | **Snapshot bootstrap is SPV security** | A header chain proves work, not validity. A node that cannot replay accepts any state whose root sits in a most-work chain. Recovery is worse: disconnecting below the base needs bodies it does not have, so it wedges. | §7A.7. |

### 14.3 High

| # | Attack | Mechanism | Fix |
|---|---|---|---|
| **B1** | **Body-availability partition of chain selection** | Relay header+commitments widely, bodies to a chosen subset. Everyone else holds it as most-work-but-unconnectable and retries forever. One block of ordinary PoW. | Rule 1. |
| **B2** | **Metered-body selfish mining, better than classic** | Publishing the header stops honest miners racing a competing block; withholding bodies stops them extending it. Both risks removed at once. A 10-30 s head start is 8-25% of the interval. | Rule 1. |
| **B3** | **Retry-queue poisoning** | Mine a block committing to bodies that exist nowhere (§9.1 permits it). Never invalid, never punished, so every node polls every Smartnode forever. Repeat to grow the load linearly. | **Per-block exponential backoff** and a cap on concurrently chased incomplete blocks. **Must not key off accumulated work** — see §14.8. |
| **B4** | **Seek-amplified fetch pinning `cs_main`** | ~28,000x by bytes; worse by IOPS — smallest body per distinct 128 MiB file, descending height, ranges straddling file boundaries. If served where `MSG_TX` is served, all under `cs_main` (`net_processing.cpp:1718`), blocking block connection, ChainLocks, and DKG contributions. | Serve bodies **outside `cs_main`** from the per-block offset table; per-connection budget in records and bytes; fold into `-maxuploadtarget`; charge per file opened. |
| **B5** | **Body serving becomes a free way to starve a Smartnode's DKG** | Missing a DKG contribution is punished today; what decoupling adds is an unattributable way to *cause* the miss. B4's traffic runs on the same lock as the DKG message handlers, so an attacker who overloads a Smartnode's body serving during its 8-minute contribution phase costs it a penalty, and each resulting ban now also removes one replica of history. | Rule for B4 — keep serving off `cs_main`; prioritise MN-to-MN DKG traffic. |
| **B5b** | **Storage as an economic weapon** | Entirely design-created: the burden only exists because Smartnodes must retain bodies. Filling every 2 MB block costs ~0.02 RTM in fees while imposing ~525 GB/year of permanent storage on **every** Smartnode. **An attacker can pull §8.5's 1 TB/year trigger for a few thousand RTM.** Nothing compensates the storage — rewards are round-robin with no service input. | A fee floor that scales with body bytes. The real answer is the deferred incentive half, which this moves up the priority list. |
| **B6** | **`NodeRoundVoting`'s 720-deep assert becomes reachable** | Reads 720 blocks of bodies with `assert(r)` (`update/update.cpp:164`) and gates consensus. Directly contradicts "a miss is normal", and the window floor is exactly 720 so the oldest needed body sits at the edge. | **Persist the per-block quorum-commitment fact at connect time.** The 720-deep rescan disappears entirely, which also removes §11's `GetBlockTxs` note. |
| **B7** | **Element form omits `nType` and `vExtraPayload`** | See §7A.4. Strip a FutureTx lock → early spend; add one → permanent unspendability. Both with a matching root. | §7A.4 invariant. |
| **B8** | **Asset supply cap bypass** *(exists today)* | Every mint in a block checks against **pre-block** state (`evo/specialtx.cpp:125-133`; state mutates later in `AddAssets`, `validation.cpp:2475`). N mints in one block all pass the cap. | Apply asset updates to a scratch cache inside the check loop. **Must ship before activation** — after, changing it is itself a hard fork. |
| **B9** | **Asset undo takes the wrong entry** *(exists today)* | `UndoMintAsset` lets the **last** matching `assetId` win (`assets.cpp:227-254`). Two mints in one block: disconnecting the first restores the wrong state. Silent divergence today; **permanent fork** once asset state is committed. | Key undo by `(assetId, tx index)`. Functional test: two mints, invalidate, reconsider, compare. |

### 14.4 Medium

- **C1 — fetch retry as peer-steering.** Being the first announcer captures a victim's fetch slot for 60 s per retry, free under a global non-punitive rule. *Fix: parallel multi-source tip fetch; re-request immediately on a miss; local per-peer reliability preference, not punishment.*
- **C2 — ChainLock suppression becomes schedulable** (§3A.7). The mechanism is unchanged from today; what is new is that an attacker can manufacture the competing-tips condition on demand by timing body delivery across the quorum, rather than waiting for two miners to collide. *Fix: build the signing-attempts process — see §3A.6, now a prerequisite.* (The safety walk's
fail-open was reported here too and has been **downgraded** — see §14.8b.)
- **C3 — slow-motion and targeted history loss.** Rewards are round-robin with no service input, misses are unpunished, so the rational operator keeps the window and deletes the rest. Targeted variant: run fast Smartnodes that serve everything except range R. Consensus never notices because the state root does not need R. *Fix: the passive coverage metric above; self-verification of the store at rest.*
- **C4 — what decoupling adds to a bootstrap eclipse.** Eclipse is an existing exposure; the design changes the payoff in both directions. **Blocked:** snapshot forgery, because the root sits behind the ChainLock anchor. **Added:** (i) hang-without-disconnect — the snapshot transfer and body scheduler have none of the stall rules block download has (`net_processing.cpp:4649-4673`); (ii) unbounded download — the snapshot's size is declared by the sender (`node/utxo_snapshot.h:22`) and the root is checkable only once everything has arrived; (iii) `ThreadOpenSmartnodeConnections` is inert until synced (`net.cpp:2310`), so the on-chain address list, the one address source an addrman eclipse cannot poison, is unused exactly when it would break the eclipse. *Fix: §7A.7 rule 3; give snapshot and body fetch the same stall-disconnect rules blocks have; use the verified MN list during bootstrap.*
- **C5 — NOTFOUND sybil.** With a tenth of the set answering NOTFOUND and a 60 s retry, one block in ten takes hours to assemble. *Fix: distinguish NOTFOUND (cheap, honest) from timeout (disconnect, as blocks do today).*
- **C6 — stale-block bodies become permanent.** Every equal-work sibling is persisted (`validation.cpp:4304-4326`). Prunable today; permanent on an append-only store. A 5% miner publishing late blocks adds ~26 GB/year per Smartnode. *Fix: §8.4 decision 1 must actually delete losing blocks' bodies once buried.*
- **C7 — opening transaction requests during IBD.** Today refused (`net_processing.cpp:2841-2853`). Opening the INV-driven path lets a peer push 100,000 announcements at a syncing node. *Fix: open only the `(height, index)` path, keyed to blocks being assembled.*
- **C8 — dual-format transition split.** A pool emitting commitment form without serving bodies is **indistinguishable from a withholder**. *Fix: rule 1; keep whole-block as the default relay form through the transition.*
- **C9 — reorg past the snapshot base wedges a windowed node**, which is a theft enabler against a merchant. *Fix: §7A.7 rule 1.*

### 14.5 The migration window

**The activation criterion cannot be measured, and the machinery cannot read it.** The scaling
document says B is decided by coverage data. But deployment here votes by block signalling —
hashrate (`MinerRoundVoting`) or mined quorum commitments (`NodeRoundVoting`) — and neither sees
data coverage. Coverage itself needs challenges, deferred by §8.

**The attack:** signal for B with hashrate. B activates, old-format blocks stop, and there is no
rollback. If coverage was poor, history is gone and nobody could have objected because the
measurement never existed. Near-unattributable — it looks like an honest activation gone wrong.

**Mitigation:** the passive coverage metric of §14.1 is separable from the retention layer and is
required by this criterion. It is a query and a counter, not a challenge economy.

Two smaller ones. **Old nodes depend on new nodes materialising for them** during the window,
which is an unpriced serving obligation and a resource attack surface. And **format negotiation
is a downgrade surface** — decline the commitment form and force a peer to serve full blocks.

### 14.6 Noticed but out of scope

Found during the review, **not decoupling work** and not on this project's list. Recorded so
they are not rediscovered, and because each is worth a separate upstream ticket.

- **`defaultAssumeValid` is inert on mainnet.** Written `"ox6fb0…"` with a letter `o`
  (`chainparams.cpp:221`); `SetHex` skips a `0x` prefix only when the first character is the
  digit zero, then stops at the first non-hex character, so it parses to all zeroes. Fail-safe —
  every node verifies every signature from genesis — but initial sync is slower than intended.
  Found independently by two reviewers. **This design does not depend on it** (§7A.7).
- **Peer discovery rests on two sources.** `pnSeed6_main` is empty (`chainparamsseeds.h:10-12`)
  and mainnet lists one DNS name plus one hardcoded IP (`chainparams.cpp:246-247`).
- **`nMinimumChainWork` and the top checkpoint are years stale** (block 421457 / 394273). Again,
  the anchor in §7A.7 is the ChainLock, so nothing here rests on these.

### 14.7 Confirmed safe

- **Option B holds.** A stalled quorum delays finality and does not halt the chain. Verified
  against `TrySignChainTip` and the spork-gated enforcement path.
- **An unassemblable block can never become anyone's tip.** `FindMostWorkChain` asserts
  `HaveTxsDownloaded()` (`validation.cpp:3119`) and candidacy requires `nChainTx`
  (`:3740-3742`). A withholder can **stall** a victim, never make it **adopt**.
- **Body forgery is blocked.** The transaction hash covers `vExtraPayload` for special
  transactions (`primitives/transaction.cpp:85-87`, `transaction.h:218-224`), and malleation
  changes the identifier, so a malleated body is simply the wrong body.
- **Accumulator collisions are blocked** given a canonical element form: elements pass through a
  random oracle before multiplication, so controlling element bytes controls only the oracle's
  input.
- **Smartnode impersonation is blocked** by ProRegTx address uniqueness (`evo/providertx.cpp:491-493`)
  and MNAUTH's signature over the operator key.
- **Reflection to third parties is blocked** — all serving is over the requester's own connection.
- **Duplicate coinbase identifiers are blocked** post-DIP3 by the enforced height in `CCbTx`.

---

## 14.8 The 51% case, and why the retry bound cannot use work

**Worst case:** a majority pool mines only blocks committing to identifiers with no transaction
behind them, and never supplies data because none exists.

**Unconnectable work is inert.** Chain selection follows the heaviest chain *it can connect*, not
the heaviest chain. A block whose data nobody has never becomes a candidate
(`validation.cpp:3119`, `:3127`, `:3740-3742`), so no work stacked on it moves any tip. Verified,
not assumed.

Honest miners never adopt it, never pause, and keep extending the connected tip. **And the
attacker earns nothing** — its blocks are in no one's chain, so its coinbase outputs are
worthless. It pays for majority hashrate and collects zero reward while the honest chain keeps
paying its own miners. The same hashrate spent on ordinary reorgs earns rewards *and* enables
double-spends, so this strategy is strictly worse for the attacker.

**The network slows, it does not stop.** The honest chain continues on the remaining hashrate, and
difficulty retargets every block here, so the interval recovers within a handful of blocks.

**"Orphaned" is the wrong word.** A stale block lost a race and is held complete. These never
entered the race, and because nothing may mark them invalid they are not discarded as a stale
block is. The accumulation is the real cost.

> **Therefore the retry bound must be per-block exponential backoff with a cap on concurrent
> chases, and must NOT key off accumulated work.** An earlier draft said to pursue only blocks
> with more work than the tip. Useless here: this attacker's chain always has more work. Work is
> exactly what it has in abundance.

## 14.8a Miss and timeout are different things

**The sybil interaction is worse than either half.** With a fraction of the Smartnode set
answering "not found", and the existing 60-second interval before another peer is tried
(`net_processing.cpp:80`, enforced `:4776`) against 100 objects in flight per peer (`:69`), a node
that picked a hostile source for a 12,000-transaction block waits 60 s per 100 bodies. At a tenth
of the set hostile that is roughly one block in ten taking hours, **on the critical path to the
tip**, with the attacker inside the set and collecting rewards throughout.

**The fix is to stop conflating two answers.** §8.4 decision 2 makes a miss non-punitive, which is
correct and necessary. But a **timeout is not a miss**. Blocks already distinguish these: a peer
that accepts a block request and does not deliver hits the download timeout and is disconnected
(`net_processing.cpp:4649-4673`). Body fetch must keep that distinction:

- **NOTFOUND** — cheap, honest, never punished, and **re-request elsewhere immediately**. No
  60-second wait; the peer told you it does not have it, so waiting buys nothing.
- **Timeout** — silence after accepting. Disconnect, exactly as blocks do today.
- **Source preference, not punishment** — a local per-peer throughput score, so slow sources stop
  being chosen. Preference keeps partial retention shippable in a way punishment would not.

## 14.8b Downgraded: the fail-open safety walk

The ChainLock safety walk treats a block as safe when it cannot recover its identifier set
(`llmq/quorums_chainlocks.cpp:321-324`). Reported as a live hazard; on examination it is **close to
unreachable** once §8.2's window exists.

The walk covers the tip and five blocks below. Identifier sets are held in memory for connected
blocks (`blockTxs`, populated on `BlockConnected`), and the disk fallback runs only after a
restart — where the 720-block window guarantees the bodies are present. So the null branch needs a
node missing bodies within six blocks of its own tip, which the window forecloses.

**Still flip it to fail closed.** It costs nothing and the direction is wrong on principle: a
member that cannot assemble a block in the walk window should decline to sign rather than assume.
But it does not belong among the serious findings, and it is recorded here so it is not
re-escalated.

## 14.9 Targeted erasure, in detail

**What the design already gives.** The target stays enumerable forever, so an attacker can make a
transaction unanswerable but never **unaskable**. Erasure produces a permanent, nameable,
verifiable hole — this position, these Smartnodes asked, none had it. A chain that pruned its
identifier lists cannot say that, and for a dispute a hole you can point at is itself evidence.

**Measure which ranges, not miss rates.** Honest loss and deliberate erasure look nothing alike:

| | Shape |
|---|---|
| Disk pressure | Oldest-first, contiguous from the start of history |
| File-granular pruning | Aligned to store file boundaries |
| Corruption | Scattered, small |
| Node still backfilling | Contiguous gap at one end, **shrinking** |
| **Deliberate erasure** | **Contiguous, mid-history, aligned to nothing, identical across one operator's nodes, stable** |

**The attacker cannot escape the signature.** To reliably erase a range you must reliably not
serve it, and reliable behaviour is what makes a pattern. Randomising to hide means sometimes
serving it, which means it survives.

**Timing is the whole value.** An alarm when replication of a range drops below a threshold is
actionable, because someone can re-seed. One that fires at zero is an obituary. That, not
monitoring hygiene, is why the coverage metric ships with the protocol rather than with retention.

**And it exposes a gap the design never answers: how a new Smartnode obtains history.** If the
answer is fetching through the body protocol, the attacker's fast nodes are the preferred source
by construction and the gap propagates to every new Smartnode — **the attack rides onboarding
rather than fighting attrition**. If the answer is copying a datadir, that is the position this
design exists to reject, and one corrupt source propagates silently because nothing verifies a
store at rest. Needs multi-source fetch and verification at rest.

## 14.10 Publishing no longer discharges the miner's obligation

§9.1a said a pool must relay and then wait for propagation or it orphans its own block. **Too
strong.** The miner holds the body and can serve it: peers request, assemble, forward, and the
next hop does the same, so body and block propagate together with one extra round trip at the
frontier. The cost is latency, not failure.

Orphaning happens only if the miner **will not or cannot serve** — publishes and goes offline, or
refuses. **So relay-and-wait is not required; serving is.**

Which is the cleaner statement of the whole change. Today, publishing a block discharges the
miner's obligation completely. Under this design it does not: publishing commitments is half of
it, and the miner stays on the hook for any body nobody else has until it has spread. That also
demotes bounded prefill from a necessity to a latency optimisation.

---

## 15. Alternative: build whole, split after

**Blocks are constructed, relayed, validated and connected exactly as today**, carrying their
transactions. Each node then splits the validated block locally into its identifier list and its
bodies, keeping the list forever and putting the bodies in the store where they can be pruned and
fetched back. Decoupling becomes a **storage transformation**, not a wire change.

### 15.1 What it removes

**The entire acceptance layer** — §2, and the one component with no reference implementation
anywhere. No second serialisation, no negotiated flag, no split of have-the-block into two facts,
no new validity rung, no durable incomplete state, none of the thirty call sites.

**With it, most of §14.** No withholding, because a block is complete or it does not propagate. No
partition of chain selection, no metered-body selfish mining, no manufactured quorum split (§3A.7),
no retry-queue poisoning, no assembly-time transaction problem (§9.1a), and no remote crash from a
block whose bodies are absent. **Mining is untouched**, so §9.2's coordination risk — the schedule
threat — disappears.

**And it may not be a consensus change at all.** Splitting a validated block for storage is the
same class of local decision as pruning: no activation, no dual-format window, no ecosystem
deadline, no irreversible cutover.

### 15.2 What it does not give you

**Capacity.** The block on the wire still carries full transactions, so the size cap still bounds
how many fit. The original claim was 32 bytes per transaction *inside* the block, turning a 2 MB
cap into ~60,000 transactions rather than ~5,000. This variant keeps storage small and leaves the
ceiling where it is.

Getting capacity means raising the cap, and that is the real trade. At the tip the wire cost is
nearly identical either way, because compact blocks already move short identifiers and bodies
already travelled as ordinary relay. The difference is what a hostile peer can make you download:
**a 10 MB block of commitments is cheap to reject; a 125 MB block of transactions is not.** That is
the one genuine argument the commitment format has, and it only bites at caps far above today's.

### 15.3 One property weakens

In the commitment design the identifier list **is** the block, so every node necessarily has it and
the network provably knows what it is missing. Here, keeping the list after pruning bodies is a
local choice. Fixable by making leaves-retained the standard pruning mode, but it moves from
**guaranteed to conventional** — and the naming property is what §11A's whole retention argument
rests on.

### 15.4 What it does to the plan

Four of the ten components in §13 largely dissolve: block format, acceptance layer, mining, migration.
What remains is the state root and snapshot sync, the lock and instant-send volume work, the body
store, the fetch protocol, and the telemetry — always the parts with known shapes, two of which
already pay for themselves.

> **Sequencing this suggests.** Build the storage transformation and the fetch protocol, which are
> safe and need no fork, and find out whether contract load actually pushes against the block cap.
> If it never does, you are finished. If it does, the commitment format is still available and the
> body store, fetch protocol and state root are already running — which were its prerequisites
> anyway. Nothing is wasted either way.

---

## 16. Measured inputs (2026-09-13, extended 2026-09-15)

Everything above reasoned about throughput qualitatively. These numbers are measured on
a real node — Raptoreum Core 2.0.4.1, `develop` plus the functional-test series, on a
12-core Ryzen 9 with the load generator on a separate machine. Full method and evidence
in `perf-results.md`.

### 16.1 What the node can already do

| | |
|---|---|
| mempool acceptance ceiling | **~4,400-4,500 tx/s sustained** (per-second peaks ~5,200 early, decaying as the mempool grows; an earlier ~5,600 figure divided by a fixed duration while acceptance ran on past the offer) |
| sustained 10 minutes at 5,000 offered | 4,884 tx/s, no eviction |
| holding a 3,000,000-transaction backlog | **4.07 GB** mempool, 4.54 GB RSS |
| cost per 373-byte transaction | 1,471 bytes accounted, 1,641 resident |

The acceptance side is not the problem, and it is roughly 120× what a 2 MB block every
two minutes can carry. The ceiling is architectural rather than hardware: `rtm-msghand`
pins at 100.3% of a single core and 22 of the box's 24 threads cannot help.

### 16.2 The sizing this puts on the block format

At 5,000 tx/s a two-minute interval accumulates **600,000 transactions**. A 2 MB block
removes 5,350 of them — 0.9% of arrivals. So a decoupled block must commit to 600,000
identifiers, which at 32 bytes each is **19.2 MB of commitments per block**, propagating
every two minutes. §1's block format has to carry that, and §15's build-whole-split-after
variant has to move the bodies as well.

### 16.3 The finding that changes a decision

Transaction relay is capped per peer at

```cpp
INVENTORY_BROADCAST_MAX_PER_1MB_BLOCK * MaxBlockSize() / 1000000
```

per trickle — **indexed to block size**. Measured: 74.7 tx/s per inbound peer at 2 MB,
230.3 tx/s at 8 MB, with the announcement burst exactly 280 at 2 MB. Over a block
interval that delivers ~9,000 transactions per peer against the ~5,350 a full block
holds: deliberately proportioned, with margin.

**Decoupling invalidates the proportion.** The formula treats block size as a proxy for
how many transactions need to propagate, and that is only true while blocks carry
bodies. A decoupled block committing to 600,000 transactions is 19.2 MB, so relay scales
to roughly 700 tx/s — while those 600,000 bodies still have to reach every peer at
5,000 tx/s. The block shrinks twelvefold and the propagation requirement does not shrink
at all.

This matters because the whole design rests on peers already holding the bodies a block
commits to. Reconstruction from identifiers is only possible against a shared pending
set, and the relay cap is what determines whether that set converges. Under decoupling
the constant must be re-indexed to something that still tracks transaction volume —
committed transaction count — rather than to the size of a block that no longer carries
them. That is a required change, not an optimisation.

**More peers do not help.** `CompareDepthAndScore` is a single global ordering over the
mempool, evaluated the same way on every peer link, so each link announces its own prefix
of the same sequence. The subsets peers hold are nested, not complementary. Measured: the
union across all peers contributed nothing over the best single peer. A node missing a
transaction cannot route around it, because every peer it could ask is missing the same
one. The fix is necessarily the rate constant or the ordering, and can never be topology.

**And it is not deferrable to the 10 MB stage.** The cap is indexed to block *bytes*, and
under decoupling block bytes are 32 × transaction count whatever the block size limit is.
So a 2 MB decoupled block commits to ~62,500 transactions — a throughput of ~520 tx/s —
while the relay cap stays at the 2 MB figure of ~75 tx/s. Coverage falls from about 170%
today to about 14%. Keeping the block size at 2 MB is a sound staging choice for every
other reason, but it does not let the relay constant stay as it is.

**Fee ordering does not rescue this, though it rescues compact blocks.** §13 of
`perf-results.md` measures a peer holding 1.5% of the hub's mempool filling an entire
block with nothing to fetch, because relay announces in fee order and the miner selects in
fee order, so the slice a peer has been told about *is* the slice the block takes. That
alignment is exactly what decoupling removes: a commitment block does not take the top
slice by fee, it commits to everything that arrived up to the payload cap. There is no top
slice for relay to align with, every node needs every transaction, and convergence has to
be full. Relay rate must be greater than or equal to arrival rate, with no shortcut.

### 16.4 Two node defects found on the way

Neither is caused by decoupling; both get worse under it, because both scale with
mempool size and decoupling exists to make the mempool large.

- `CChainLocksHandler::Cleanup()` walks one entry per accepted transaction calling
  `GetTransaction()` on each, holding `cs_main` **and** `mempool.cs`, every 30 seconds,
  on every node rather than only smartnodes. Measured linear at ~0.5–0.6 µs per entry:
  **1.9 seconds of total stall every 30 seconds at 3 million entries.**
- The socket thread burns a full core spinning on a socket it has paused, so falling
  behind costs an extra core precisely when there is least to spare.

### 16.5 The Smartnode attestation path, measured

§3A reasoned about what a quorum attestation can carry. This is what producing one costs.
Measured end to end on regtest with real Smartnodes forming a real quorum; full method in
`perf-results.md` §15 and §16.

**Correction to v6.** v6 costed this from the cryptography — 9.41 ms of threshold recovery
per signing session — and concluded the path was far too slow. The conclusion holds. The
reasoning does not: recovery is **3 to 5 per cent** of the measured cost.

| quorum | locked | locks/s | per-node sessions/s | ms per session |
|---|---|---|---|---|
| 5 of 3 | 3,000 / 3,000 | 74.2 | 55.4 | 18.1 |
| 9 of 6 | 2,924 / 3,000 | 41.2 | 23.0 | 43.5 |
| 13 of 8 | 2,773 / 3,000 | 32.5 | 13.9 | 71.9 |

Three properties, each of which any attestation-based design inherits:

**Cost is linear in quorum size, not threshold.** The fit is ≈ 6.7 × quorum size − 15 ms
across all three points. The live InstantSend quorum is **50** — `UpdateLLMQParams` resolves
`LLMQ_50_60` to `llmq50_60` above 600 Smartnodes (`chainparams.cpp:1101`), not the size-3
test quorum. That is an order of magnitude past anything measured here.

**It is round-trip-bound, not compute-bound.** At 5 of 3 the signing thread spends 9.1 ms
of CPU against 18.1 ms of wall clock — idle half the time, waiting on the
announce/inventory/request/share exchange that `CSigSharesManager::SendMessages` ships at
most once per 100 ms. Parallelising BLS verification addresses the half that is already
fast.

**Full coverage holds only to about 60 tx/s at the smallest quorum tested.** Paced offers
at 5 of 3: 20, 40 and 60 tx/s all lock 100%, 80 tx/s locks 98.5%, 100 tx/s locks 87.8%. The
achieved rate pins at ~76 locks/s and does not rise, so past the ceiling the excess is lost
rather than delayed. That ceiling agrees with the burst measurement, and it is at a quorum a
tenth the size of the live one.

**Saturation loses work rather than queueing it.** Above capacity, sessions are purged 60
seconds after their last new share (`quorums_signing_shares.cpp:1317-1357`), and nothing
retries them: `pendingRetryTxs` is only ever populated with the *children of a transaction
that just locked* (`quorums_instantsend.cpp:1239-1245`), so a transaction whose own session
timed out is never re-queued. Measured: a run with 237 timed-out sessions and 85 unlocked
transactions logged **zero** `retrying to lock` lines. The only way back is
`BlockConnected`'s retroactive path (`quorums_instantsend.cpp:1176`), which re-signs once
the transaction is mined — by which point the lock is worthless. There is no error, no
backpressure, and no degraded-but-working mode.

**And the unit is wrong.** InstantSend signs each input separately and then the lock
(`quorums_instantsend.cpp:548-561`, `:724`). Measured: 5,845 distinct signing sessions for
2,915 locked one-input transactions — sessions = inputs + 1, confirmed. A two-input
transaction costs three threshold signatures.

#### What this settles about the buspool proposal

A design in which Smartnodes pre-attest transactions so they can take a shorter validation
path is optimising the wrong side of the wrong bottleneck. §16.1 shows validation is not a
constraint — acceptance runs at ~4,400 tx/s sustained. The attestation path runs at a small fraction
of that and gets slower as the quorum grows. It **adds** work to the slowest thread in the
system in order to **remove** work from a path that has an order of magnitude of headroom
at the 2 MB design point. And it leaves relay untouched: bodies still have to reach every
node whether or not a quorum signed them first.

#### What this settles about InstantSend as it exists today

This is a finding about the current chain, not about decoupling.

If threshold signing caps where these numbers say it does, **InstantSend as built cannot
cover transactions at any throughput near the design point.** Either most transactions go
unlocked — which changes what `IsTxSafeForMining`'s wait check is actually doing, since it
falls through to the ten-minute age rule — or InstantSend has to sign batches rather than
transactions.

Batching is the only fix here that scales, and for a sharper reason than "fewer
signatures". Cost is **per session**, and per-session cost grows with quorum size. One
threshold signature over a Merkle root of N transaction hashes divides the whole cost by N
— the round trips as well as the cryptography. Every other available fix moves a constant:
one session per transaction instead of inputs + 1 is a free 2× to 3×, and worth doing, but
it does not change the shape.

That is also the salvageable form of the buspool instinct. "Smartnodes attest before
mining" is the right shape; the purpose is finality and double-spend protection rather than
a validation shortcut, and the unit has to be a batch.

### 16.6 Prerequisites in existing code, found by measurement

None of these is a design question. Each is a constant or a mechanism that must change
before anything can be tested, and each would have been an expensive discovery on testnet.

| | current | why it blocks |
|---|---|---|
| `MAX_PROTOCOL_MESSAGE_LENGTH` | 3 MB | a 10 MB commitment block cannot cross the wire at all |
| `maxmempool` default | 300 MB | ~876 MB needed per two-minute interval at 5,000 tx/s; 41 seconds of headroom at the default |
| `getblocktemplate` | full `ConnectBlock` per call | seconds of `cs_main` per poll at 600,000 commitments, in direct competition with acceptance |
| `MAX_BLOCKFILE_SIZE` | 128 MiB | `FindBlockPos` loops forever on a block larger than one block file — silent, unbounded allocation until the OOM killer intervenes (`perf-results.md` §12) |

The block-file loop is avoided by the separate body store, since commitment blocks stay far
under 128 MiB. It is recorded here so that no variant drifts back towards writing bodies
into block files, where it detonates immediately and fatally.

Mempool growth is also lumpy rather than smooth — hash-table doubling means a node at 90%
of the cap crosses it in one step — so the default is a footgun at any throughput increase,
decoupling or not.

Template production is the one that becomes a design requirement rather than an
optimisation: under decoupling, commitments must not be revalidated as bodies. Stratum V2's
Template Distribution Protocol is the right direction — the node pushes a template with the
coinbase and a Merkle path, with no polling and no JSON marshalling of 600,000 identifiers
— subject to the `CCbTx` complication in §7.

### 16.7 What the staging choice actually costs

Holding the block size at 2 MB isolates the problems well. It does not sidestep the relay
one.

| | 2 MB decoupled | 10 MB decoupled |
|---|---|---|
| throughput implied | ~520 tx/s | ~2,600 tx/s |
| relay re-index (§16.3) | **required** | **required** |
| InstantSend coverage (§16.5) | **blocker** | **blocker, needs batching** |
| `maxmempool` | ~91 MB/interval, inside the default but tight | ~455 MB/interval, must raise |
| `MAX_PROTOCOL_MESSAGE_LENGTH` | under 3 MB, fine | must raise |
| template production | tolerable | must replace |
| message handler (~4,400 tx/s sustained) | 10× headroom | 2× headroom |
| body propagation | ~23 MB/block, ~195 KB/s, trivial | ~115 MB/block |

So the 2 MB stage has **two** blockers, not one. v6 recorded InstantSend as marginal at
this point against a modelled ~800 tx/s network-wide; the measured figure is an order of
magnitude below that, at a quorum a tenth the live size. The rest of the 10 MB list is then
a known set of prerequisites rather than a set of discoveries.
