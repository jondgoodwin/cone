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
the signal generation uses to skip building a phi. An `if` typed `void` builds
none either: every arm is void, as in `if c {f();} else {}`, which is how the
last expression of a macro's body is typed (a macro expands with no expected
type, so its last `if` needs an `else`, and core's `assertDebug` writes an empty
one). Before 27 Sep 2026 such an `if` built a phi of `void` with null operands,
which failed module verification.

## Parse

`if` / `elif` / `else` append pairs; `else if` is folded into `elif`.

**A bound pattern match is de-sugared into a wrapping block**: `if imm c &Circle
= value {…}` becomes a block declaring an anonymous variable for the value, an
`is` condition over a shared name use, and a variable bound to a cast inserted
at index 0 of the arm. The condition and the cast share the pattern's type node,
and the variable takes its type from the cast ([cast](cast.md)). **`parseIf`
returns the wrapping block, not the `IfNode`.**

`parseMatch` lowers the whole construct into a block plus one `IfNode`:
`case is T` → an `is` node, `case == v` (or any comparison operator) → that
operator's call with the scrutinee on the left, `case a .. b` → `>= a and < b`
(`<=` for `...`), `case imm x T` → a bound pattern, `case v` (a value alone) →
an `is` node flagged `FlagMatchValue` that type check turns into `v`'s variant
test or `== v` ([cast](cast.md)), `else` → `elseCond`. Patterns joined by `or` → a logical `or` of
their conditions; an `if g` after them → `cond and g`. **Every arm shares one
scrutinee node pointer**, a use of the variable the lowering declared to hold
the matched value, so a range's two calls and every `or` alternative hold it too.

**A pattern's first operand decides between a range, a value and a refusal.**
A pattern beginning with neither `is` nor a comparison operator reads one
operand (`parseOr`); a following `..` or `...` makes it a range. A comparison
operator, `is` or `and` after it makes it a condition (`case n > 3`), and so does
a leading `not` (`case not b`). A condition is refused, `ErrorPatBare`: as a
value alone it would be compared with the matched value, and whether it should
be is not decided. The refused expression is finished (`parseSimpleExprFrom`),
its own `or` included, so it is reported once. Anything else is a value alone
(`case 1`, `case true`, `case K`, `case Circle`), the same at the start of a
case and after an `or`.

**A bound pattern's guard binds the variable a second time.** The variable is
declared at the head of the arm, which the condition is outside, so `case imm x
T if g {…}` becomes the condition `is T and {imm x = [T]v; g}` — a block
declaring its own `x`, from a second conversion that shares the pattern's type
node (`FlagMatchBind`) — and the arm keeps its own `x` as before.

**Only a bare `is` condition counts toward exhaustiveness.** A guarded arm's
condition, and an `or` of `is` tests, are logic nodes, so `ifExhaustCheck`
never sees them. For a guard that is the rule: the guard may fail. For `or` it is
a gap: `case is A or is B` accounts for neither variant, and a match that relies
on it needs an `else`. A variant named alone, `case A`, is its `is` test once
checked and counts; any other value alone is an `==` call and counts for
nothing, so `case true` and `case false` on a `Bool` need an `else` to be a
value, as `case ==true` and `case ==false` do.

## Name resolution

`ifNameRes` resolves every element of `condblk`, conditions and arms alike.
`elseCond` is a no-op arm. Nothing is bound or retagged at this level —
retagging happens inside the arms.

## Type check

**Pass 1**, per pair: check the condition; if it is an `is`, run
`ifExhaustCheck`; if it is not `elseCond`, coerce it to `Bool` — which is where
an implicit `.isTrue` reaches a conditional. An `elseCond` sets `hasElse`, and
must be last. Then check the arm against `expectType` and fold its type into the
type in common — **unless the arm jumps away** (`ifBlockJumps`: it ends in a
`return`, `break` or `continue`, a call returning `Never` among them, since
`blockTypeCheck` makes a `return` of one). Such an arm gives the `if` no value,
so has no say in its type: `imm x = if c {1} else {return 7}` and
`imm x = if c {1} else {panic("no")}` are both `i32`.

**After the loop:** a `noCareType` expectation returns immediately, leaving
`vtype` `unknownType` — the statement-position `if`. No `else` in a
value-producing `if` is `ErrorNoElse`. A specific expectation becomes `vtype`
directly, since every arm was already coerced.

**Pass 2 — the re-coercion pass.** Only when a common *supertype* was inferred
rather than an exact match, because the arms checked before it was known were
coerced to the wrong target. It coerces **each arm block's last statement in
place**, an arm that jumps away excepted, not the block — generation requires
the arm to stay a block node and
cannot have a cast wrapped around it. This runs before flow, so the `blockret`
flow later injects wraps the already-coerced node.

### `ifExhaustCheck`

Given an `is` condition on a **closed** variant set — an enum with no base of
its own, carrying `HasTagField` or `SameSize` — it checks whether every entry of
`derived` is matched by some arm testing **the same scrutinee**: the same node,
or two name uses of one declaration (`ifSameScrutinee`). An enum that *extends*
another is such a set and its `derived` holds the base's variants ahead of its own,
so a match on it must account for both; a match on the base still accounts for the
base's alone, which is what forbidding the substitution between the two buys. The
second
form is what a clone of the match presents — a generic instance's or a macro
expansion's — since cloning copies the shared node once per arm. If they all
are, and one of the variant tests is the **last** condition, it **overwrites that
condition with `elseCond`**. An arm after the one being checked may name its
variant bare and not be bound to it yet (`castPatternPending`), or be a value
alone not yet decided between a variant and `==` (`FlagMatchValue`), so it
counts as no match; the check runs again as each arm is checked, and the last
one sees every pattern bound. An arm whose pattern named nothing to narrow to has been
reported and was left unchecked, so it may not be a type at all; it counts as no
match too.

Because `ifTypeCheck` calls this *before* testing for `elseCond`, the rewrite
takes effect for the very condition being processed: `hasElse` becomes true and
no `Bool` coercion is attempted. That is how a `match` covering every variant
becomes a value-producing expression with no written `else`.

`ifRemoveReturns`, called from `returnTypeCheck`, strips the redundant `return`
from each arm of `return if … { return a } else { b }`, recursively.

## Flow

`ifFlow` walks the first condition, then each arm from the state the
conditions before it leave, a later condition and each arm one level deeper
(`flowDepth`). Each arm's changes to *outer* variables' flags are taken and
rolled back (`flowVarPathTake`, `flowVarRollback`), and the arms that did not
return, and a missing `else`, are joined (`flowVarJoin`): a flag any of them set
is set after the `if`, so a value moved in both arms compiles, and one moved in
one arm may not be used after it. Whether each variable holds its value at its
releases, where that differs by arm, is the path walk's to decide, and a drop
flag's at run time ([Flow Analysis](../phases/flow.md), "Drop flags").
`ifFlow` leaves `FlowState.jumped` set when every arm returned.

## Generation

`genlIf` creates `endif` first, then per pair an `ifnext` (or `endif` for the
last) and an `ifblk`, and a conditional branch. **For `elseCond` no block is
created** — the else body is emitted into whatever `ifnext` the previous
iteration left the builder at.

**A temporary a condition makes dies once the condition is computed**, before
the conditional branch (`genlTempsEnd`): a later condition runs on some paths
only, so it cannot wait for the statement's end, and a `while`, which arrives
as `if not cond { break }`, finalizes its own each pass ([Generation](../phases/generation.md),
"Temporaries").

After each arm, if its last statement is not a jump, branch to `endif` and
record a phi incoming — using `LLVMGetInsertBlock`, not `ifblk`, because the arm
may have split the block. The phi is built only when something was recorded;
where a value-producing `if` recorded nothing — every arm jumped away — its
value is `undef`, which no path reaches.

## Hazards

- **`endif` is created unconditionally**, so when every arm terminates it is
  left with no predecessors. The default build does not run the verifier, so it
  is emitted silently.
- **`ifExhaustCheck` rewrites `condblk` while `ifTypeCheck` is iterating it.**
  The target is fixed at the second-to-last slot, so today it can only be the
  condition currently being processed. Anything that made the target a non-final
  slot would corrupt the walk.
- **Exhaustiveness recognizes a scrutinee only as the shared node or as a use
  of the lowering's variable.** A lowering that wraps one arm's scrutinee in a
  coercion or a deref silently breaks detection — and it surfaces as
  `ErrorNoElse`, not as a defect.
- **`ErrorNoElse` returns with `vtype` still `unknownType`.**
- **An arm ending in `break` or `continue` is joined**: only a `return` counts
  as leaving (`jumped`), since the main walk walks a loop once and has no state
  to hand a jump. So a value moved on an arm that breaks counts as moved after
  the `if`, as it did before the join.
- **`ifRemoveReturns` calls `nodesLast` without an emptiness guard.**

## What lives elsewhere

- Branch folding, the type in common, and the second pass: [Type Check Reasoning](../phases/type-check-reasoning.md), "Unifying branches"
- Where `derived` and `HasTagField` come from: [struct](struct.md)
- How flow joins the arms, and follows each path for drops: [Flow Analysis](../phases/flow.md)
- The `is` node itself: [cast](cast.md)
- `match` de-sugaring: [Parse](../phases/parse.md)
