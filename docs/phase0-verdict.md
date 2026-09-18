<!--
lifecycle: frozen on the commit decision
owns: what phase 0 asked, what it answered, and the estimate that follows
do not edit after the decision is taken; a later change of mind gets its own record
-->

# Phase 0 verdict — is transaction decoupling worth committing to?

**Date:** 2026-09-18. **Question phase 0 existed to answer:** whether to commit months of work to
transaction decoupling, and on what estimate. Not to build anything.

**Answer: yes, commit.** The acceptance layer — the one component with no reference implementation
anywhere — is tractable, and its cost is now named rather than guessed. The estimate for it comes
down from 3-6 months to 8-16 weeks. One risk remains that is **not ours to resolve** and needs a
decision from RTM before phase 5 can be sized at all.

---

## The four questions, and what came back

### 1. Can a node hold a block it cannot complete, without corrupting itself?

**Yes, and most of it already exists.** The behaviour that looked hardest — hold an incomplete block
without condemning it, and pick it up when the rest arrives — is already implemented, because
pruning needs the same thing. `FindMostWorkChain` treats absent data as a third outcome beside valid
and invalid: it declines the chain without setting a failure bit and re-arms it through
`m_blocks_unlinked`, in its own words "if the block arrives in the future we can try adding to
setBlockIndexCandidates again" (F-25f).

The probe held a commitment-only block, restarted on the validated tip, and converged when the body
arrived — with **no change to chain selection at all**.

**The cost, against the written kill criterion:** one **per-block status bit** and one **validity
rung** (F-25k). The criterion forbade *relaxing* an invariant that catches corruption and permitted
*adding a state alongside* one. A global flag would have been a relaxation; a per-block bit is a
state alongside. `BLOCK_HAVE_BODIES` at bit 256 costs **zero relaxed assertions** — one added, one
recovery rule, two guards narrowed in a way that is inert on every state an unmodified node can
reach (F-38).

**And one thing the bit cannot do alone:** with the commitment block stored honestly, every "do I
need this block?" answers *yes, I have it*, and the node never fetches the bodies. Measured: zero
requests, stuck at the fork indefinitely (F-40). **The bit and the fetch layer are one deliverable.**

### 2. What does a body-less block honestly certify?

Settled by **source audit, not by probe** — and that distinction is the single most important
methodological point in this phase. The probe validates a block's bodies and then hides them, so it
can test what the *index* does and cannot test what *acceptance* would do for transactions never
seen. Two confident conclusions were retracted before that limitation was recognised as the shape of
the experiment rather than a caveat on it.

Of thirteen checks between `AcceptBlockHeader` and `ConnectBlock`, the commitment-checkable ones are
identified (F-43) — proof of work, the header-chain rules, **merkle root**, malleation, size,
first-tx-is-coinbase, the whole coinbase `CheckTransaction` including the **founder payment**, and
the DIP3 coinbase type. That is where the rung sits.

**Exactly three rules have no connect-time home** (F-44), because `ConnectBlock` re-invokes
`CheckBlock` but deliberately not `ContextualCheckBlock`:

1. `nLockTime` finality
2. transaction type and version
3. **`bad-txns-oversize`**, the per-transaction size cap (K-6) — the one that matters most, since it
   is simultaneously §1A's body-byte multiplier and the bound on quadratic sighash cost, and its only
   connect-time counterpart is an order of magnitude looser

**Encoding decided (D-16): no new ordinal.** Grant `BLOCK_VALID_TRANSACTIONS` at the rung as
commitment-valid and relocate those three rules to connect. This avoids renumbering three persisted
values and migrating every index entry on disk, and it is what makes 1.3's estimate hold.

**Two traps found on the way**, both consensus-relevant: `fChecked` already caches a
**height-dependent** verdict — `CheckBlock` enforces the founder payment, whose only enforcement
point in the codebase is that function, from a height `ProcessNewBlock` supplies as `Tip()+1`
computed before it takes `cs_main` (F-45). And the merkle malleation check compares hashes at **even
indices only**, so a duplicate identifier at an odd boundary satisfies everything the merkle tree
checks — which makes identifier uniqueness a strictly necessary rung rule, not a belt-and-braces one
(F-43b).

### 3. What does a block cost to validate, and do the caps price it?

**No, the caps do not price it.** `GetLegacySigOpCount` charges the sigops in outputs a transaction
*creates* plus its scriptSigs; `GetP2SHSigOpCount` adds spend-side cost for **P2SH prevouts only**.
A bare multisig spend is charged **nothing**: 800 such inputs took over two seconds to validate and
were charged two sigops.

So §1A's work unit must be an accurate count over every **spent** scriptPubKey, at roughly 130 µs
per sigop plus about 40% from the size term at the 100 kB ceiling.

Separately, the block sigop cap is `MaxBlockSize()/50` — 40,000 at 2 MB (K-2) — which admits about
**20,000 ordinary payments**, so it caps a commitment block long before its byte limit does. It must
be re-based to committed count **in the fork**, jointly with the per-transaction work cap, so
worst-case block hashing stays at or under today's figure.

### 4. What do quorums and locks cost on a real network?

The 12-host WAN swarm was brought up with ten live smartnodes; quorums form, ChainLocks sign,
InstantSend locks (F-49). The figure: **a DKG phase needs about 15 seconds over real latency**
(F-48), against the ~1 second that unpaced regtest mining allows — which is invisible locally,
because the phase derives from tip height and `SleepBeforePhase` no-ops under `MineBlocksOnDemand`.

**And the result that matters most in this phase, which is not about decoupling at all:**

> At 300 tx/s, `rtm-sigshares` — the single thread that signs, recovers *and* verifies — sits at
> **93.7% and 97.0%** on the smartnodes measured. That is **below stage 1's 520 tx/s target** (K-2's
> staging), on a quorum of eight; mainnet's is 200 of 400 with the same per-transaction work on every
> member (F-53).

The failure mode is **not** graceful degradation. Identical load with InstantSend off and on: both
arms accepted every transaction offered with zero rejections, but the mempool went from ~7,000 to
**183,273 transactions / 239 MB** and kept climbing (F-54); blocks starved for ten minutes and then
ran 13-15% short (F-55); and **ChainLocks stopped forming entirely** — the feature that converts
probabilistic finality into an assertion is the *first* thing lost under load (F-56).

---

## The estimate that follows

| item | before phase 0 | after |
|---|---|---|
| 1.3 acceptance layer | 3-6 months, **unknown** | **8-16 weeks, partly known** |
| the rung | open design question | **decided** (D-16), no persisted renumber |
| phase 5 components 5.3, 5.4 | in the build | **out** until trí answers (6-11 w) |
| 5.1 attestation ceiling | separate item | fold into 5.2's design spike |
| 3.4b asset-cache follow-ups | in the build | defer — on no path to the target |

Net: roughly **8-14 weeks of components removed**, and 5.2 grows by an amount only its design can
price. The calendar does not honestly shrink until RTM answers the question below.

---

## What is still open

**One decision, and it is not ours.** Un-batched InstantSend cannot reach the design point, and
batching it is not an engineering task: a threshold signature needs every member signing the *same*
message, while peers hold divergent mempools — so batch composition must be agreed across those
mempools *before* signing, which means a proposer-per-slot protocol and imports the
manufactured-split problem into InstantSend. **Dash never built this.** It is currently scheduled
*after* activation while being described as a prerequisite of the design point.

Three options, and RTM owns the choice:

1. **5.2 rides the fork** — months onto the critical path, for a protocol with no reference
   implementation.
2. **5.2 trails the fork** — the design point ships with InstantSend at single-digit coverage and
   ten-minute delays on everything else.
3. **InstantSend stays off at activation** — which deletes 5.2, the mempool sizing, the per-node
   verification cost and the ChainLock convergence requirement in one stroke.

One fact narrows it: the retroactive signing path fires **regardless** of the mempool-signing spork,
so mainnet's current timestamped spork 2 does **not** avoid this load. Only spork 2 fully off does
(F-57).

**Also open, and ours:** F-18's InstantSend ceiling at a quorum of fifty cannot be answered by the
swarm — ten smartnodes forming an eight-member quorum can never speak to it, and `-llmqtestparams`
overrides only size and threshold. It needs a 50-smartnode loopback regtest, which is how the
original figure was measured.

---

## What phase 0 got wrong, and why it matters

Four conclusions were recorded as settled and then overturned, three of them by an adversarial review
on a different model. The pattern is worth stating because it predicts where the next error will be:

- **A probe that passes does not prove the mechanism you think it does.** Twice, the measurements
  were correct and the *attribution* was wrong.
- **Arguing from a comment rather than the code.** The claim that `ConnectBlock` re-checks what the
  rung certifies came from an enum comment that omits the relevant function.
- **A test that appears to exercise a rule while exercising nothing.** Three instances in one day: a
  founder-payment mutation below the height where the rule applies, a harness whose peer was
  disconnected after the first row, and `pruneblockchain` reporting success while pruning nothing
  (F-69).

Every one of those produced output that read like a finding about the node. The defence that worked
was the same each time: an independent reviewer on a different model, briefed to attack rather than
confirm.

---

## Recommendation

Commit to the build. Start phase 1 with the block format and the resource-budget re-base, which
depend on nothing that is still open. Put the InstantSend measurement in front of trí this week, and
let the answer size phase 5. Carry the remainder of 0.2 — asset, futures and special-transaction
characterisation — alongside phase 3, which is the work it protects; the part that protects 1.3 is
done.
