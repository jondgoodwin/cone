`IfNode` has one field. Everything interesting is what that field's layout
implies, and what `match` lowers into it.

**At a glance.** `parseIf` and `parseMatch` both build it — `match` is
de-sugared here, not later. Name resolution walks conditions and arms
uniformly. Type check coerces each condition to `Bool`, folds the arms into one
type, and may rewrite the last condition into an `else`. Flow walks **both arms
against one shared state**. Generation builds the basic blocks and a phi.

*Provenance: read from source.*

## Shape

```c
typedef struct IfNode { IExpNodeHdr; Nodes *condblk; } IfNode;
```

**`condblk` is a flat, even-length alternating list** — `[cond₀, blk₀, cond₁,
blk₁, …]`. Every consumer walks it with `for (nodesFor(...)) { …cond…;
++nodesp; --cnt; …block… }`, where the manual step inside the body makes the
stride 2.

**`elseCond` is a sentinel** marking a condition slot as unconditional. It is a
single global `AbsenceNode`, compared by **pointer identity**, never by tag.
Unlike the three type sentinels — which are also `AbsenceNode`s but retagged
`UnknownTag` — `elseCond` keeps `AbsenceTag`, which is in the expression group,
so `isExpNode(elseCond)` is true. It survives cloning by identity, because
`cloneNode`'s `AbsenceTag` arm returns the node unchanged.

`vtype` stays `unknownType` for a statement-position `if`, and that is exactly
the signal generation uses to skip building a phi.

## Parse

`if` / `elif` / `else` append pairs; `else if` is folded into `elif`.

**A bound pattern match is de-sugared into a wrapping block**: `if imm c &Circle
= value {…}` becomes a block declaring an anonymous variable for the value, an
`is` condition over a shared name use, and a variable bound to a cast inserted
at index 0 of the arm. **`parseIf` returns the wrapping block, not the
`IfNode`.**

`parseMatch` lowers the whole construct into a block plus one `IfNode`:
`case is T` → an `is` node, `case == v` → an `==` call, `case imm x T` → a bound
pattern, `case <expr>` → the expression, `else` → `elseCond`. **Every arm shares
one scrutinee node pointer**, which is what makes the exhaustiveness check below
work.

## Name resolution

`ifNameRes` resolves every element of `condblk`, conditions and arms alike.
`elseCond` is a no-op arm. Nothing is bound or retagged at this level —
retagging happens inside the arms.

## Type check

**Pass 1**, per pair: check the condition; if it is an `is`, run
`ifExhaustCheck`; if it is not `elseCond`, coerce it to `Bool` — which is where
an implicit `.isTrue` reaches a conditional. An `elseCond` sets `hasElse`, and
must be last. Then check the arm against `expectType` and fold its type into the
type in common.

**After the loop:** a `noCareType` expectation returns immediately, leaving
`vtype` `unknownType` — the statement-position `if`. No `else` in a
value-producing `if` is `ErrorNoElse`. A specific expectation becomes `vtype`
directly, since every arm was already coerced.

**Pass 2 — the re-coercion pass.** Only when a common *supertype* was inferred
rather than an exact match, because the arms checked before it was known were
coerced to the wrong target. It coerces **each arm block's last statement in
place**, not the block — generation requires the arm to stay a block node and
cannot have a cast wrapped around it. This runs before flow, so the `blockret`
flow later injects wraps the already-coerced node.

### `ifExhaustCheck`

Given an `is` condition on a **closed** variant set — a trait with no base of
its own, carrying `HasTagField` or `SameSize` — it checks whether every entry of
`derived` is matched by some arm, comparing scrutinees by **pointer identity**
(which is why `parseMatch` shares one node across arms). If they all are, and
one of the variant tests is the **last** condition, it **overwrites that
condition with `elseCond`**.

Because `ifTypeCheck` calls this *before* testing for `elseCond`, the rewrite
takes effect for the very condition being processed: `hasElse` becomes true and
no `Bool` coercion is attempted. That is how a `match` covering every variant
becomes a value-producing expression with no written `else`.

`ifRemoveReturns`, called from `returnTypeCheck`, strips the redundant `return`
from each arm of `return if … { return a } else { b }`, recursively.

## Flow

`ifFlow` walks each condition, then each arm block — **all against the same
mutable `FlowState` and the same global variable stack, with no fork and no
join.** A move in one arm is seen by the other and by everything after. Each
arm's `blockFlow` does bracket its own locals correctly; what is shared is the
move state of *outer* variables.

This is the deliberate conservative approximation. `flowScopeDealias`'s own
comment says a value moved on one branch is skipped on all of them — which
leaks rather than double-frees.

## Generation

`genlIf` creates `endif` first, then per pair an `ifnext` (or `endif` for the
last) and an `ifblk`, and a conditional branch. **For `elseCond` no block is
created** — the else body is emitted into whatever `ifnext` the previous
iteration left the builder at.

After each arm, if its last statement is not a jump, branch to `endif` and
record a phi incoming — using `LLVMGetInsertBlock`, not `ifblk`, because the arm
may have split the block. The phi is built only when something was recorded.

## Hazards

- **`endif` is created unconditionally**, so when every arm terminates it is
  left with no predecessors. The default build does not run the verifier, so it
  is emitted silently.
- **A branch that ends in `return` cannot participate in a value-producing
  `if`** — it fails earlier as "Branch's expression type inconsistent with other
  branches", and the diagnostic does not say that is the reason.
- **`ifExhaustCheck` rewrites `condblk` while `ifTypeCheck` is iterating it.**
  The target is fixed at the second-to-last slot, so today it can only be the
  condition currently being processed. Anything that made the target a non-final
  slot would corrupt the walk.
- **Exhaustiveness matches the scrutinee by pointer identity.** Any lowering
  that replaces one arm's scrutinee with a copy or a coercion silently breaks
  detection — and it surfaces as `ErrorNoElse`, not as a defect.
- **`ErrorNoElse` returns with `vtype` still `unknownType`.**
- **`ifFlow` has no join**, so conditional moves are not tracked per path.
- **`ifRemoveReturns` calls `nodesLast` without an emptiness guard.**

## What lives elsewhere

- Branch folding, the type in common, and the second pass: [Type Check Reasoning](../phases/type-check-reasoning.md), "Unifying branches"
- Where `derived` and `HasTagField` come from: [struct](struct.md)
- Why flow is path-insensitive: [Flow Analysis](../phases/flow.md)
- The `is` node itself: [cast](cast.md)
- `match` de-sugaring: [Parse](../phases/parse.md)
