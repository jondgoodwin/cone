`IfNode` has one field. Everything interesting is what that field's layout
implies, and what `match` lowers into it.

**At a glance.** `parseIf` and `parseMatch` both build it — `match` is
de-sugared here, not later, and its `IfNode` is marked (`FlagIfMatch`). Name
resolution walks conditions and arms uniformly. Type check coerces each condition
to `bool`, folds the arms into one type, and may rewrite the last condition into
an `else`; for a marked `if` it also judges what the cases cover (`ifMatchCover`).
Flow walks **both arms against one shared state**. Generation builds the basic
blocks and a phi.

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

**What a match or a bound `if` matches against is decided by its form**
(`MatchSubject`, `parseMatchSubject`). A *place* -- a name, a field of a place, a
dereference, an element at a name or a literal, with no call anywhere in it
(`parseMatchIsPlace`) -- is matched where it stands: the lowering declares no
variable for it and writes a copy of the place (`parseMatchPlaceCopy`) into every
test, so a pattern that only tests reads the tag in place and takes nothing.
Anything else -- a call, a borrow (`&r`, `&mut r`), a literal -- is evaluated once
into an anonymous variable, which every test and binding reads through one shared
name use. A reference is such a value: its copy is the reference. A match whose
arm binds a variable and ends in an `if` guard (`parseMatchHasBoundGuard`, read
ahead in the source text before the matched value is made) holds its value
whatever its form: the guard's binding reads the value before the arm is chosen,
and may take nothing from a place the arms after it test.

**A bound pattern match is de-sugared**: `if imm c Circle = value {…}` becomes the
`is` condition and a variable bound to a cast inserted at index 0 of the arm. For
a value that is held, a wrapping block declares the anonymous variable, the
condition and the cast share one name use of it, and **`parseIf` returns the
wrapping block, not the `IfNode`** (it returns the block for a place too, holding
only the `IfNode`). For a place, the condition tests a copy of the place and the arm
begins with an anonymous variable of its own, initialized from another copy of the
place, which the cast reads: the arm that runs is the one that takes the value,
copying it or moving it, and finalizes it as the arm ends. The condition and the cast
share the pattern's type node, and the variable takes its type from the cast
([cast](cast.md)).

`parseMatch` lowers the whole construct into a block plus one `IfNode`:
`case is T` → an `is` node, `case == v` (or any comparison operator) → that
operator's call with the scrutinee on the left, `case a ..< b` → `>= a and < b` (`case a .. b` → `>= a and <= b`),
`case imm x T` → a bound pattern, `case v` (a value alone) →
an `is` node flagged `FlagMatchValue` that type check turns into `v`'s variant
test or `== v` ([cast](cast.md)), `else` → `elseCond`. Patterns joined by `or` → a logical `or` of
their conditions; an `if g` after them → `cond and g` (the `and` flagged
`FlagMatchGuard`, so that a guard is told from the `and` of a range pattern). **A held value is one scrutinee node
pointer shared by every arm**, so a range's two calls and every `or` alternative hold
it too; **a place is a separate copy in each test.** The `IfNode` is marked `FlagIfMatch`.

**`match a, b` has one matchee for each value and one pattern for each in a case.**
Each matchee is a place or held in a variable of its own (`-match1`, … after the first,
which keeps the anonymous name), evaluated once each, left to right, in the block before
the `if`. A case's condition is the patterns of its columns, in order, joined by `and`
nodes flagged `FlagMatchRow` (left-nested, `(c0 and c1) and c2`), then a guard if there
is one. `_` is a column's pattern that takes any value: a literal `true` flagged
`FlagMatchAny` (`parseMatchWild`), so it type checks and generates as the constant it
looks like and costs nothing at run time. `_` in a match of one value, a case whose
patterns are fewer or more than the values, and a column that binds a variable
(`case imm x Some, …`; whether a column may bind is not decided) are `ErrorMatchColumn`,
reported at parse. At most `MatchMaxColumns` values.

**A pattern's first operand decides between a range, a value and a refusal.**
A pattern beginning with neither `is` nor a comparison operator reads one
operand (`parseOr`); a following `..` or `..<` makes it a range. A comparison
operator, `is` or `and` after it makes it a condition (`case n > 3`), and so does
a leading `not` (`case not b`). A condition is refused, `ErrorPatBare`: a value
alone is compared with the matched value, and the comparison without its left side
(`case >3`) and an `if` guard say a condition. The refused expression is finished
(`parseSimpleExprFrom`), its own `or` included, so it is reported once. Anything else is a value alone
(`case 1`, `case true`, `case K`, `case Circle`), the same at the start of a
case and after an `or`.

**A bound pattern to a variant declared as a type binds what it holds**, not the
variant: type check hoists a hidden local holding the converted value before the
variable, whose initializer becomes the local's contents ([cast](cast.md),
`castBindProject`). The desugaring here is the same for every variant.

**A bound pattern's guard binds the variable a second time.** The variable is
declared at the head of the arm, which the condition is outside, so `case imm x
T if g {…}` becomes the condition `is T and {imm x = [T]v; g}` — a block
declaring its own `x`, from a second conversion that shares the pattern's type
node (`FlagMatchBind`) — and the arm keeps its own `x` as before.

**What counts toward exhaustiveness is decided on a type checked match, by
`ifMatchCover`** (below): an `is` test of a variant of a closed enum, an `or` of them,
`v == true` and `v == false` on a `bool`, and `_`. A guarded case does not count (the guard
may fail). Any other value alone is an `==` call that counts for nothing.

## Name resolution

`ifNameRes` resolves every element of `condblk`, conditions and arms alike.
`elseCond` is a no-op arm. Nothing is bound or retagged at this level —
retagging happens inside the arms.

## Type check

**Pass 1**, per pair: check the condition; if it is an `is`, run
`ifExhaustCheck`; if it is not `elseCond`, coerce it to `bool` — which is where
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
two name uses of one declaration, or two reads of the same place -- the same variable,
the same field of the same place, a dereference of one, an element at the same name or
literal (`ifSameScrutinee`, `ifSamePlace`). An enum that *extends*
another is such a set and its `derived` holds the base's variants ahead of its own,
so a match on it must account for both; a match on the base still accounts for the
base's alone, which is what forbidding the substitution between the two buys. The
second
form is what a clone of the match presents — a generic instance's or a macro
expansion's — since cloning copies the shared node once per arm, and the last
is a match on a place. If they all
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
no `bool` coercion is attempted. That is how a `match` covering every variant
becomes a value-producing expression with no written `else`.

This is the check of an `if` chain written as an `if`, not of a `match`: a marked
`if` skips it, and `ifMatchCover` judges it once every condition is checked.

### `ifMatchCover`

Runs after the loop of pass 1 on an `IfNode` marked `FlagIfMatch`, unless a condition was
reported (`condErrors`: the cases are not judged then; an error in an arm's body does not
stop it). It reads each case as a **row** of cells, one for each value matched, found by
flattening the `FlagMatchRow` `and`s, with the guard (a `FlagMatchGuard` `and`) taken off and
noted. A cell (`matchClassify`) is one of:

- **wild** — `_`, or, in an `or`, any alternative that is;
- **a set** of the values of the column's type, which the first cell that is one gives the
  column: an `is` of a variant of a closed enum (`matchClosedEnum`: the test `ifExhaustCheck`
  used, a trait with no base carrying `HasTagField` or `SameSize`), which is the variant's index
  in `derived`; or `v == true` / `v == false` on a `bool`, the call type check made of
  `case true` and `case ==true` (its callee a use of the `==` it found, the matched value
  its first argument); an `or` is the union of its alternatives'; a cell of another
  type than the column's is "other";
- **other** — a comparison, a range, `==` against anything but a bool literal, a number, a
  string; anything this logic cannot count.

A row with an "other" cell, or with a guard, is not **counted**: it covers nothing and makes
no later case unreachable. The rows up to the first `else` are all that is read.

*Covered?* (`matchCovers`) splits the counted rows on the first column that is not wild in
all of them, by each value of its type, keeping the rows wild or naming that value, and
recurses on the rest of the columns; it is covered when every combination ends with a row
left. That is the one-level matrix check, the groundwork deep patterns would need. It stops
at the first uncovered combination, which `ErrorMatchNotExhaustive` names (a single column
names every variant missing).
*Unreachable?* (`matchUseless`) asks the same of a pure row against the counted rows before
it, over only the values the row takes: if every combination is taken already, the case is
`ErrorMatchUnreachable`, and so is an `else` after cases that cover everything.

A match with no `else` that is covered has its **last case rewritten to `elseCond`**, as
`ifExhaustCheck` did, so the value of a match needs no other. One that is not covered is
`ErrorMatchNotExhaustive` (once, replacing `ErrorNoElse` for a match), its type left
unknown as `ErrorNoElse` left it. A match whose cases were not judged falls through to
`ErrorNoElse` as an `if` does.

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
value is `undef`, which no path reaches. A value that is a small struct or array
is stored into a `%merge` slot by each arm and loaded at `endif` instead of
phi-ed (`genlMergeSlot`; [Generation](../phases/generation.md), "A small
aggregate is merged through a slot").

## Hazards

- **`endif` is created unconditionally**, so when every arm terminates it is
  left with no predecessors. The default build does not run the verifier, so it
  is emitted silently.
- **`ifExhaustCheck` rewrites `condblk` while `ifTypeCheck` is iterating it.**
  The target is fixed at the second-to-last slot, so today it can only be the
  condition currently being processed. Anything that made the target a non-final
  slot would corrupt the walk. (`ifMatchCover` rewrites after the walk.)
- **`ifMatchCover` reads the shape type check leaves a condition in**: an `is` node,
  `or` and `and` logic nodes, a literal, and the call of the `==` a comparison found
  (callee a name use of the declaration, the matched value first). A lowering that
  changes one of these makes the cell "other" and the match asks for an `else`; it does
  not mis-cover. A match counted wrongly would be the dangerous failure, so a cell is
  a set only for exactly those shapes.
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
