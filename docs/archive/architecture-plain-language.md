<!-- archived: 2026-09-18
     why:      it restates every number in prose by construction, so it cannot obey the
               citation rule the rest of the folder now runs on — and it had already drifted,
               carrying "12.6 GB per year" (400 B) beside "six terabytes" (373 B) in adjacent
               paragraphs. A living prose copy of a living technical document is two clocks.
     replaced: transaction-decoupling.md (the design) and findings.md (every value, with its
               regime). If a plain-language version is needed for an audience, generate a fresh
               one from those rather than maintaining a parallel copy.
     keep:     it is the clearest statement of the design's shape that exists, and §§1-2 in
               particular — the idea in one page, and the principles — are worth reading before
               the technical document. Read it as a 2026-09-17 snapshot, not as current. -->

> **ARCHIVED 2026-09-18, and it is a snapshot.** Its numbers predate `findings.md`; where it
> disagrees with the design or with findings, it is wrong. Kept because §§1-2 are the best short
> explanation of what decoupling is.

# Transaction Decoupling on Raptoreum

## An architecture in plain language

**Version 2 — 17 September 2026.** Companion to the technical design (v8) and to `build-plan.md`.
The technical document cites files and line numbers; this one explains what the design is and why
it holds. Nothing here is softer than the technical version — every claim below was checked
against the code — but the code is not shown.

**Status:** target architecture. The fork between the two candidate designs is **closed** (§7).
The schedule is not here; it is in `build-plan.md`.

**What changed since version 1.** The commitment format is now the design rather than one of two
options. A whole section is new — the block's resource budget, because a block of names bounds
neither work nor bytes, and one consensus limit caps the design at a third of its target until it
is re-based. Retention returned to scope for several hours and then left again on stated RTM team
input, which also made the state root optional. Six claims from version 1 are **deleted** rather
than corrected, and they are listed in §7 so nobody re-derives them.

---

## 1. The idea in one page

A block today carries its transactions inside it. A transaction is a few hundred bytes, a block
is capped at a couple of megabytes, and so a block holds a few thousand transactions.

Under decoupling, a block carries **only the name of each transaction** — its 32-byte hash —
plus the coinbase in full. The transactions themselves, which this document calls **bodies**, are
stored separately and fetched when needed. The same two-megabyte block names sixty thousand
transactions rather than five thousand, and an eight-megabyte block names a quarter of a million.

Nothing about consensus changes. Proof of work still orders transactions and still decides the
chain. Mining hardware sees no difference, because rigs only ever hashed an 80-byte header and
that header is byte-for-byte identical. What changes is what a block *records*: what was ordered,
rather than the data that was ordered.

Three things hold it together.

1. **The merkle root**, which already exists, commits to the list of names. It is already built
   from transaction hashes and nothing else, so it works over a block of names with no change.
2. **The hash itself** binds a body to its name. A body is correct exactly when it hashes to the
   name the chain committed. No index, registry or signature is needed to prove a body is the
   right one; anyone can check it.
3. **A budget**, which is new in this version, bounds what a block may commit *to*. A block of
   names is small however much work and however many bytes it implies, and that gap is where the
   design's sharpest hazard lives (§4).

Who holds the bodies? The **Smartnode tier**, which already exists, is already identified and
collateralised, and already publishes its addresses on the chain. The design gives a duty to a
tier that exists rather than inventing one.

**The target.** A throughput whose commitments fill a 2-8 MB block every two minutes: 520 to
2,083 transactions per second. Those are peaks and bursts rather than sustained load, which is
what lets the Smartnode tier keep the whole history and therefore what keeps two large pieces of
machinery out of scope (§3.1, §3.4).

---

## 2. Principles

**Consensus is untouched.** No change to proof of work, difficulty, header relay or how the chain
is chosen. The block hash means exactly what it meant before. Everything that runs once a block
is connected — script verification, special transactions, assets, undo data — takes a complete
block and cannot tell where it came from.

**Capacity is sized for the burst; storage for the average.** These are different numbers and
conflating them is what briefly forced retention into scope. A block must remain valid and must
propagate at the peak rate. Disks fill at the average one.

**History is kept, not deleted.** Chains that split data from commitments have done it two ways:
delegate validation to some other role, or throw the data away after a few days. This design does
neither. The names are kept by every node and the bodies by the Smartnode tier. That is what makes
history a *protocol service* rather than a favour from whoever happens to run an archive.

One exception, and it is deliberate: **losing sibling blocks' bodies are dropped once buried.**
Every node persists equal-work siblings, and a small miner publishing late would otherwise add
tens of gigabytes a year to every Smartnode forever.

**The network always knows what it is missing.** Because every name is kept, every transaction
that ever existed can be asked for from the chain alone, even when no body is held. A pruning
chain cannot say "this position, these nodes asked, none had it." This one can, and every
retention guarantee or repair mechanism that might be added later depends on that property.

**A body-incomplete block is never invalid, but it is not chased forever either.** Marking it
invalid would permanently reject a chain other nodes accept. Chasing it indefinitely is an attack.
Validity and fetching are separate questions, and only the second has a budget.

**Two regimes, one boundary.** At the tip, complete blocks exist, because whoever connected one
had to assemble it. In history, no complete block exists anywhere and never has since the day it
was mined. The boundary between them is the recent window.

---

## 3. Architecture

### 3.1 Who holds what

| Role | Holds | Provides |
|---|---|---|
| **Miners and pools** | Current state, plus bodies for the recent window | Ordering and validity, under proof of work |
| **Smartnodes** | Everything, forever | Availability of history, alongside the finality and lock services they already run |
| **Ordinary nodes** | Current state, the recent window, and whatever else they choose | Independent validation; may prune freely |

**What full replication costs.** Each Smartnode's disk grows by about 12.6 GB per year for every
*sustained* transaction per second. At the stated hardware target — 4,000 to 6,000 Smartnodes with
8 cores, 16 GB and a 512 GB SSD — the practical sustained ceiling is somewhere between ten and
thirty transactions per second, depending on how often operators replace disks.

Read the design target as *sustained* and that arithmetic breaks: 520 tx/s is six terabytes a year
per node, and 2,083 is nearly twenty-five. On RTM team input the figures are peaks and bursts, so
full replication stands — but the margin is a stated assumption rather than a comfortable one, and
the trigger below is the thing to watch.

**The disguised risk.** Storage cost is coupled to consensus participation. An operator short of
disk does not degrade into a partial archive; it drops out entirely, and takes ChainLocks signing,
InstantSend and quorum participation with it. So the failure mode of "storage became a problem" is
a thinning Smartnode set. **Watch throughput, which gives years of warning, not disk complaints,
which give weeks.** Begin partial-retention work when sustained volume implies less than two years
of headroom — roughly where growth crosses a terabyte a year per node. And note an attacker can
pull that trigger cheaply unless bodies are bounded and priced (§4, §5.4).

### 3.2 The block

A commitment block is the same structure as today with a second serialisation: coinbase in full,
every other entry a bare 32-byte name.

The format cannot be signalled in the block header. Raptoreum's update-voting scheme owns the
version bits, and the version field is inside the hashed header anyway, so a format recorded there
would be fixed at mining time. Instead the format is a **stream flag negotiated between peers**,
on the pattern segwit used. Peers must agree on a format before transferring blocks, and the
on-disk form is each node's own choice.

One rule belongs with the format and does most of the security work in this document:

> **Never relay a commitment block you cannot assemble yourself.**

A block of bare names is cheap to forward, which is exactly why forwarding one you cannot connect
is dangerous: it spreads as the heaviest-work header while nobody downstream can build on it.
Gating relay on self-assembly restores the property whole-block relay had for free.

### 3.3 The body store

Bodies live in flat files, written in block order, beside the existing block and undo files. Not in
a key-value database: the database would rewrite large values repeatedly as it compacts, at a
terabyte a year of immutable data, for no benefit.

Block order matters because **assembling a block is the hot path** — every block, every node, on
the critical path to connecting the tip. In block order, assembly is one contiguous read. In hash
order it would be thousands of random reads scattered across the whole dataset, because a hash is a
random number.

**Bodies are addressed by position.** A request names a block height and an index within the block,
or a range. That is compact, the response is self-verifying (the body must hash to the name at that
position), the server reads contiguously, and — the largest gain — no node needs a global map from
every transaction that ever existed to a file offset. A per-block offset table replaces it. Lookup
by bare hash stays available on nodes that choose to keep the transaction index, exactly as optional
as it is today.

**But a position is not universal, and version 1 said it was.** A competing branch at the same
height commits to different names, so during a reorg, or while following a branch never seen before,
a request must name the **block hash** rather than the height. Height addressing remains the default
for the active chain and for history, where it is unambiguous.

Three rules about writing, each learned the hard way in review:

- Bodies are written **when a block is accepted**, transactionally with the block itself. Not later
  when transactions leave the mempool — side-chain blocks never reach that point, and the block is
  announced to peers before it runs.
- **Never at mempool acceptance.** Unmined transactions pay no fee, so an attacker could fill every
  node's disk for free until the node aborts — which on a Smartnode is a ban two quorum rounds later.
- The format **must be able to delete records**, even though it will barely do so at first.
  Retrofitting deletion is a data migration across every Smartnode.

### 3.4 State, and why the state root is now optional

**History** is the list of transactions: immutable, growing forever. **State** is the current
situation — who owns what, right now. State is small, it changes every block, and it is a pure
function of history: replay everything from genesis and you get it.

A node that holds only a recent window cannot replay. Version 1 concluded from this that the design
needs a **state root** — a commitment to the result, so a node can start from a snapshot it can
check instead of trusting whoever handed it over. That conclusion was load-bearing while the target
was read as sustained throughput, because replay then costs days per year of chain history and
terabytes of download.

It is not load-bearing now. With the Smartnode tier holding everything and the sustained average
modest, **a new node can replay from the tier and then prune.** Snapshot sync becomes a convenience
and the state root becomes optional.

What survives as a reason to build it anyway: it is a per-block check that two nodes have not
silently diverged, which is worth something in a design whose acceptance layer is new. What does not
survive is the idea that it makes quorum attestation auditable — see §6.

**Deferring it also defers the hardest unsolved problem in the design.** Committing asset state
would require deciding exactly what an asset element is and exactly how it serialises, identically
on every node, forever, with nothing to copy. Asset state therefore stays exactly as it is today:
balances already live in the unspent output set, and the asset records are committed nowhere.

**Tripwires that bring the state root back**, in order: replay bootstrap becoming impractical, or
the Smartnode tier thinning so that "history is always available from a Smartnode" stops being true.

### 3.5 Where the new commitments live

The coinbase. It reaches the header only as a hash — it is the first merkle leaf — so the header
stays 80 bytes however much the coinbase grows, and every byte of it is still committed. This
lineage already put the Smartnode and quorum roots there for the same reason.

Of the five fields once proposed, **none is now required.** A body merkle root is redundant, since
each body hashes to its committed name. A transaction count is the length of the list. An aggregate
fee total is worth keeping only if consensus enforces that it equals the real total; without that
rule it is a number the miner chose. A total body byte size was kept in version 1 as an advisory
bound — but a declared bound buys nothing, because bodies are self-authenticating and a node can
simply stop fetching when the real total exceeds the limit. And the state root is optional (§3.4).

That leaves the coinbase unchanged in the first version of this design, which is a genuine
simplification over version 1.

---

## 4. The block's resource budget

**This section is new, and it is the sharpest thing found since version 1.**

A block of names is small however much it implies. Three limits therefore have to be re-based, and
they interact, so they are one decision rather than three.

**The signature-operation cap blocks the design outright until it is re-based.** The cap is derived
from block size — forty thousand operations at two megabytes — and an ordinary two-output payment
counts two. A two-megabyte block of names for sixty-two thousand payments therefore claims a hundred
and twenty-five thousand operations and is **invalid**. The cap admits about twenty thousand
transactions: **167 per second against a 520 per second target.** The miner does not fail loudly; it
simply stops filling the block.

This is the same bug as the transaction-relay cap, which is also derived from block size. Both were
correct while a block carried bodies, because block size was then a good proxy for how much work and
how much data a block implied. Decoupling severs the proxy, and every limit indexed to it silently
comes to mean something else.

**But the signature cap is also what bounds worst-case hashing, so it cannot simply be scaled up.**
This chain has no segwit, so signing a transaction with many inputs re-serialises the whole
transaction once per input, and the legacy multi-signature opcode re-hashes once per key tried. The
cap is what keeps that bounded — today's worst case is around four gigabytes of hashing, on the order
of ten seconds. Scale the cap with committed count and the worst case scales with it; scale it
generously and there is no bound at all.

**Body bytes need a cap of their own.** Sixty-two thousand names at the hundred-kilobyte
per-transaction limit is over six gigabytes of bodies for one block, and at the default relay fee an
attacker buys that for a few dozen coins. A fee floor helps against ordinary users and not at all
against a miner. Under full replication that is a permanent storage bill imposed on every Smartnode,
so the bound has to be part of consensus.

So the budget is three limits chosen together: a block signature budget re-based to committed count,
a per-transaction work cap that keeps the quadratic term contained, and a consensus cap on committed
body bytes — with the values picked so that a block's worst case is no worse than today's.

---

## 5. Key processes

### 5.1 Producing a block

The miner does what it does today: selects transactions, validates them, totals the fees, builds the
coinbase, and hashes the header. It already holds every body it commits to, because it hashed them to
get the names. It then publishes the header, the coinbase in full, and the list of names. The bodies
are already on the network — gossiped as ordinary transactions before the block existed.

**Publishing no longer discharges the miner's obligation.** Today, publishing a block is the end of
the miner's job. Under decoupling it is half of it: the miner stays on the hook for any body nobody
else has until it has spread. The case where this bites is a transaction that only exists at assembly
time — a pool fee payout sized to this block's fees, say. Pool payouts normally live in the coinbase,
which is carried in full, so the common case needs nothing new. Where it does bite, the fix is not to
wait but to **serve**: peers request the body, assemble, forward, and the next hop does the same. The
cost is latency, not failure. A block is orphaned only if its miner will not or cannot serve.

Nothing in the protocol teaches this, and pool software has never had to know it, which is why
coordinating the external mining interface is a schedule risk rather than an engineering one.

### 5.2 Accepting a block

This is where the design lives and where the cost is concentrated.

Today a node either has a block or it does not, and "has it" means "can connect it." Under decoupling,
having the **names** and having the **bodies** become two separate facts, and a block can sit between
them indefinitely. Everything below block connection needs no change. Everything above it — chain
selection, the download scheduler, candidate maintenance, consistency checks, pruning, database
verification, crash replay, roughly thirty places — must learn a state that did not exist:
**names valid, bodies incomplete.**

```
Header valid  →  Names valid  →  Bodies incomplete  →  Bodies complete  →  Connected
 (proof of work)  (merkle root, no    (durable, retried,      (the new flag)     (unchanged)
                   duplicates; cheap   never marked invalid)
                   to reject)
```

**Never invalid, and never chased forever.** Taken literally, "never invalid" is an attack: mine a
block naming bodies that exist nowhere and every node polls every Smartnode indefinitely. The block
stays valid-but-unconnectable, which costs a cheap index entry; *fetching* is bounded by per-block
exponential backoff and a cap on how many incomplete blocks are chased at once. That bound must not
be "chase only blocks with more work than my tip", because the attacker who matters — a large pool
mining nothing but unresolvable names — always has more work.

**Why this holds at 51%.** Chain selection follows the heaviest chain *it can connect*, not the
heaviest chain. A block whose data nobody has never becomes a candidate, so no amount of work stacked
on it moves anyone's tip. Honest miners never adopt it, never pause, and keep building. The attacker
earns nothing, because its coinbase outputs are in nobody's chain. The network slows on the remaining
hashrate and difficulty retargets within a few blocks. It does not stop.

### 5.3 Fetching bodies

At the tip, most bodies are already in the mempool and assembly is what compact blocks do today. If
assembly fails, the existing fallback — ask a peer for the whole block — still works.

That fallback is necessary and **not sufficient on its own**. "Every node that connected it can
rebuild it" is circular: a node starved of bodies cannot connect the block, so cannot be a source, and
the attacker chooses who connects first. The relay rule of §3.2 is what closes the gap.

**And at this scale the fetch protocol is a tip path, not a history service.** A block of sixty-two
thousand names implies more than twenty megabytes of bodies, which exceeds the largest message the
protocol allows, and nothing in the compact-block path splits a reply. So any node more than a few
thousand transactions behind — about a quarter of a minute — cannot use compact reconstruction at all,
and the whole-block fallback carries no bodies under this format. Position-addressed fetch is
therefore on the critical path to connecting the tip, which is where its latency budget and its rate
limits have to be designed.

In history, a request names a position, a Smartnode returns the body from contiguous storage, and the
requester hashes it and compares to the name it already holds. No trust in the server. Requests are
batched by range as the default shape, which makes rate accounting a bounded question.

**Serving must run off the main lock.** Today, serving a *block* releases the node's main lock before
reading disk; serving a *transaction* holds it. Bodies are block-shaped work wearing transaction
clothes. Put them on the transaction path and an attacker requesting scattered bodies pins the lock
that block connection, ChainLocks and quorum contributions all need. The body store keeps its own
height index so serving touches no chain state, and self-verification is what makes that safe: a
server racing a reorg returns an answer the requester rejects by hash and asks again.

**A miss is not a timeout.** "I do not have that" is cheap and honest, never punished, and should
trigger an immediate re-request elsewhere rather than the existing sixty-second wait. Silence after
accepting a request is a timeout, and should disconnect exactly as block download does today. Conflate
the two and a tenth of the Smartnode set answering "not found" makes one block in ten take hours to
assemble, on the critical path to the tip, with the attacker collecting rewards throughout.

### 5.4 Reorganising

Disconnecting a block needs its full bodies, not just undo data: the disconnect path walks every
transaction to unwind indexes and pushes bodies back into the mempool. So the recent window serves
reorgs and everything else at once.

Version 1 said the window's floor is 720 blocks, set by the round-voting mechanism that reads a full
round of bodies to count quorum commitments. **That reason is being removed** — persisting the count at
connect time deletes the rescan — so the floor now needs a new justification. ChainLock depth is the
obvious candidate and the decision is open.

### 5.5 Bootstrapping a node

With the Smartnode tier holding everything, the ordinary path is the familiar one: fetch the chain,
replay it, validate everything from genesis, then prune to a window. A node that does this trusts
nobody. It is slower than a snapshot and it is the reason no state commitment is required.

If snapshot sync is ever built (§3.4), it carries its own rules: a ChainLocked base at least a day
back, a quorum derived from the chain's own Smartnode list rather than from whoever offers the
snapshot, a transfer bounded before it starts, and a recomputation of the fingerprint rather than
trust in it. And it is honest to say that below its base such a node has light-client security,
because a header chain proves work, and work is rentable by the hour.

### 5.6 Looking up history

Any Smartnode can answer, because every Smartnode holds everything and the chain publishes their
addresses. A wallet rescanning, an exchange crediting a deposit, an explorer, an auditor asks by
position and verifies by hash. Inclusion proofs actually get *cheaper*: a commitment block is exactly
an identifier list, so proving a transaction was in a block works on a node holding no bodies at all.

Two consequences worth stating. Anything returning transaction *contents* gains a new legitimate
answer on a windowed node — "not held here" — and that must be an explicit error rather than an empty
result, so a caller can tell an absent transaction from an unheld one. And **every index in the node
is built by replay today.** Asset balances per address are a pure aggregate over unspent outputs, so
they can be rebuilt from the current state in one pass; a wallet rescan over old blocks cannot, and
must fetch the range it needs or run on a node that has it.

---

## 6. Quorum attestation, and the dual validation path

Kept live rather than parked, because trí holds to it and the strongest reason for it is not the one
that was measured.

**The throughput case is dead.** Removing signature verification triples acceptance — it is two
thirds of the work — but acceptance is the path with the most headroom, roughly ten times the 520 per
second design point, while relay and the signature cap bind first. Attestation also *adds* work to the
slowest thread in the system to remove work from one that is not short.

**The structural case is different in kind.** A Spark job's result is not verifiable by anybody: a
miner cannot re-execute it, and Spark's execution model is non-deterministic by design, so no second
party can reproduce it either. For a transaction that carries an off-chain computation result, "validate
it yourself" is not expensive but impossible, and a quorum signature is the only validity rule available.
If that is what the dual path is for, it is necessary rather than optimising — and no measurement here
can argue with it.

**Two things that were said in earlier revisions and are wrong.**

A committed state root does **not** make a false attestation detectable. If every node trusts the
attestation, they all apply the same transaction and the root matches: it commits to *agreement*, not to
validity, and the verifying minority is the side that forks off.

And skipping verification at acceptance does not merely move the cost to block connection. Connection
pays it cold, but cold verification at connection runs on the node's parallel check queue — thousands
queued, one wait — which is exactly the shape acceptance cannot have, because acceptance handles one
transaction at a time and the synchronisation costs more than the work. Getting that saving locally,
without any trust change, means batching checks across transactions inside acceptance. That is real
engineering, and it is the honest comparator for any attestation proposal.

**The decision that bounds the risk.** What may an attested transaction change? If contract-layer state
only, a lying quorum corrupts contract state and cannot steal coins. If balances or asset mints, a
quorum majority is theft, under an honest-majority assumption about a committee whose collateral is worth
a few million dollars. That is the whole security surface of the feature.

**And whatever it turns out to be, it has to be batched.** Verifying one quorum signature costs roughly
ten times what verifying an ordinary two-input transaction's signatures costs. Per transaction,
attestation is worse than doing the work. Break-even is somewhere between ten and twenty transactions
per signature.

---

## 7. Decisions on record

| Date | Decision |
|---|---|
| 11 Sep 2026 | Smartnodes hold everything; miners and ordinary nodes hold a recent window. Retention mechanics, sharding, challenges, incentives and repair are deferred. **Re-confirmed 17 Sep** on RTM team input that the target is peaks and bursts — with the tripwire of §3.1 kept live, because read as sustained load this decision does not survive its own arithmetic. |
| 12 Sep 2026 | The Smartnode attestation is a **record, not a validity gate**. Making ChainLocks mandatory for validity would turn Raptoreum into a hybrid chain with a halt condition and a censorship surface that do not exist today. What remains is retaining the attributable signature shares, and that ships with retention. |
| 13 Sep 2026 | **The commitment format is the design**, provisionally. |
| 17 Sep 2026 | **The fork is closed.** The target is stated as a throughput whose commitments fill a 2-8 MB block, which a design carrying bodies in the block cannot express at all. Build-whole-split-after is retained solely as the fallback if the acceptance layer proves intractable. |
| 17 Sep 2026 | **The state root is optional**, with tripwires, and the canonical asset element form is deferred with it. Asset state stays as it is. |
| 17 Sep 2026 | **The dual validation path stays live** rather than deferred, gated on three answers about what is attested and what an attested transaction may change. |

**Three decisions taken now because retrofitting them is a migration, not a code change:** a body-store
format that can delete records; a non-punitive answer for a historical miss; and never pruning the
commitments.

**Six claims from version 1 are deleted rather than corrected.** Carrying a dead claim is its own
failure, so they are listed once here and nowhere else: that the block size cap is what limits
throughput (relay, InstantSend and the signature cap all bind first); that a position is a universal
address (it is not, off the active chain); that the window floor is set by round-voting (that reason is
being removed); that the state commitment is a port rather than research (the hash is a port, the
persisted reorg-symmetric accumulator is not); that the design does not rest on the shipped chain
anchors (it does, since quorum membership is derived against headers, and those anchors are stale); and
that nothing is deleted (losing siblings' bodies are).

---

## 8. What is still open

In order of what blocks the most.

1. **Whether the acceptance-layer rework is tractable.** Analysed, never attempted, no precedent
   anywhere. A probe exists to answer this in a week rather than two months, and it needs a written
   kill criterion.
2. **What a contract call looks like on the wire.** It creates a new kind of transaction, its payload
   can be twenty-seven times the size assumed by every storage figure here, and it decides whether the
   burst assumption holds.
3. **What an attested transaction may change** (§6) — the question that bounds the dual path's risk.
4. **The values in the block's resource budget** (§4), which need the input-cost curve measured rather
   than reasoned about.
5. **The recent window's floor**, now that its stated reason is being removed.
6. **Migration:** rollback or not, how long both formats coexist, and what wallets, explorers and
   exchanges must do by when.
7. **Rate limiting and accounting for body serving**, which today's upload accounting does not cover.
8. ~~Whether InstantSend and ChainLocks are actually live on mainnet.~~ **Answered, and it does
   not change the design.** Mainnet currently has InstantSend switched off deliberately and a
   broken quorum layer; both will be fixed before decoupling and neither is this project's scope.
   The design assumes InstantSend on and quorums healthy, so every cost that depends on them
   stays priced — including that a ChainLock-signing smartnode must hold every transaction in the
   last six blocks, which in practice means every smartnode must converge.
9. **Coordinating the external mining interface** — not an engineering problem, which is why it decides
   schedules.
