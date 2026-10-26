`IfNode` has one field. Everything interesting is what that field's layout
implies, and what `match` lowers into it.

**At a glance.** `parseIf` and `parseMatch` both build it — `match` is
de-sugared here, not later, and its `IfNode` is marked (`FlagIfMatch`). Name
resolution walks conditions and arms uniformly. Type check coerces each condition
to `bool`, folds the arms into one type, and may rewrite the last condition into
an `else`; for a marked `if` it also judges what the cases cover (`ifMatchCover`).
Flow walks **both arms against one shared state**. Generation builds the basic
blocks and a phi, and a marked `if` that tests one integer, character or tag is built as one `switch`.

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

`if` / `else if` / `else` append pairs.

**What a match or a bound `if` matches against is decided by its form**
(`MatchSubject`, `parseMatchSubject`). A *place* -- a name, a field of a place, a
dereference, an element at a name or a literal, with no call anywhere in it
(`parseMatchIsPlace`) -- is matched where it stands: the lowering declares no
variable for it and writes a copy of the place (`parseMatchPlaceCopy`) into every
test, so a pattern that only tests reads the tag in place and takes nothing.
Anything else -- a call, a borrow (`&r`, `&mut r`), a literal -- is evaluated once
into an anonymous variable, which every test and binding reads through one shared
name use. A reference is such a value: its copy is the reference. A guard's
binding reads the value before the arm is chosen, so it takes nothing from a place
or a held value the arms after it test (see [cast](cast.md), "What a binding takes from a place").

**A bound pattern match is de-sugared**: `if let Circle(c) = value {…}` becomes the
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
operator's call with the scrutinee on the left, `case a ..< b` → `>= a && < b` (`case a ..= b` → `>= a && <= b`),
`case let T(x)` → a bound pattern, `case v` (a value alone) →
an `is` node flagged `FlagMatchValue` that type check turns into `v`'s variant
test or `== v` ([cast](cast.md)), `else` → `elseCond`. Patterns joined by `|` → a logical `||` of
their conditions; an `if g` after them → `cond && g` (the `&&` flagged
`FlagMatchGuard`, so that a guard is told from the `&&` of a range pattern). **A held value is one scrutinee node
pointer shared by every arm**, so a range's two calls and every `||` alternative hold
it too; **a place is a separate copy in each test.** The `IfNode` is marked `FlagIfMatch`.

**`match a, b` has one matchee for each value and one pattern for each in a case.**
Each matchee is a place or held in a variable of its own (`-match1`, … after the first,
which keeps the anonymous name), evaluated once each, left to right, in the block before
the `if`. A case's condition is the patterns of its columns, in order, joined by `&&`
nodes flagged `FlagMatchRow` (left-nested, `(c0 && c1) && c2`), then a guard if there
is one. `_` is a column's pattern that takes any value: a literal `true` flagged
`FlagMatchAny` (`parseMatchWild`), so it type checks and generates as the constant it
looks like and costs nothing at run time. `_` in a match of one value, a case whose
patterns are fewer or more than the values, and a column that binds a variable
(`case let Some(x), …`; whether a column may bind is not decided) are `ErrorMatchColumn`,
reported at parse. At most `MatchMaxColumns` values.

**An `if` or `match` whose value has no place to give its type lets the arms that are
untyped number literals follow the others.** `ifLiteralArmsFollow` runs after the
arms are joined: they were left out of the join, and take the number type the others met at,
or are joined as ever when it is none (see [literals](literals.md)).

**A pattern's first operand decides between a range, a value and a refusal.**
A pattern beginning with neither `is` nor a comparison operator reads one
operand (`parseOr`); a following `..=` or `..<` makes it a range (a bare `..` is refused, `ErrorRangeBare`, and read as `..=`). A comparison
operator, `is` or `&&` after it makes it a condition (`case n > 3`), and so does
a leading `!` (`case !b`). A condition is refused, `ErrorPatBare`: a value
alone is compared with the matched value, and the comparison without its left side
(`case >3`) and an `if` guard say a condition. The refused expression is finished
(`parseSimpleExprFrom`), its own `||` included, so it is reported once. Anything else is a value alone
(`case 1`, `case true`, `case K`, `case Circle`), the same at the start of a
case and after a `|`.

**A binding that outlives an early exit, `let Ok(n) = r else {...};`, is the same bound
pattern declared where it stands** (`parseLetElse`, from `parseLetPattern`, which the `PermToken`
case of `parseExprBlock` calls when `lexNextOpensPattern` says a variant pattern follows the `let`;
a function's pattern is read as a plain local and reaches `parseLetElse` when `else` follows its
initializer). It makes, in the
enclosing block: the hidden variable that holds a value that is not a place (named
`-held1`, a name of its own because several of these can stand in one block), an
`IfNode` flagged `FlagIfLetElse` whose condition is the `is` test, whose block is empty
and whose `else` is the one written, then the arm's hidden variable (`-arm2`) if the value
is a place, and the declared variable with the pattern's conversion as its value. Type
check refuses an `else` that can go on (`ifBlockFallsOut`, `ErrorLetElse`): every path
through it must leave, by ending in a jump (`ifBlockJumps`) or in an `if` or `match` with
an `else` each of whose blocks leaves, through any plain block a `match` or a bound `if`
is wrapped in; the error is put at the spot a path goes on (an `if` with no `else`, a last
statement, a loop, an empty block) and says which. A plain block that itself ends in the
jump, `else { { return 7; } }`, is refused as a stand-in until generation ends the path at
such a `return` (scenario `enum_typecheck_let_else_block`). Because the `else` leaves, the
`if`'s path out is the test holding, so the narrowing after it ([narrow](narrow.md)) is the
variant: an `else` that leaves only through an inner `if` is no path out either, and keeps
the narrowing kills it made, as a branch that continues a loop does. The hidden names are recognized as a match's own
(`nameIsMatchHolder`) wherever flow asks whether a variable is one.

**A function's pattern, `using`, is the call of that function with the matched value put in
as its first argument** (`parseUsingCall` reads the call after `using`, a suffix term that must be a
call with parentheses; `parseUsingMatchee` inserts the value at index 0 of its arguments, so a method's
comes after the receiver). It is read in three places.

- *A test*, `case using isOdd()`, is a pattern of `parseMatchPattern`: the call is the condition, like any
  other call a case may hold, so it can be joined by `|`, be a column's pattern and take a guard. A
  call giving an Option is `ErrorUsingForm` (`ifTypeCheck`), not a condition that is not a bool.
- *A binding in a match*, `case let rest using prefix("GET ")`, must run the call once and read its result
  twice (the test and the binding), which one condition of the flat chain cannot do. So the chain goes on
  in the `else` of the cases before it, which holds the call's result in a variable and the cases after, as
  an `else if let Some(x) = call` does (`parseBoundMatch`, `usingarm`): `[…, elseCond, { let -using1 =
  prefix(matchee, "GET "); if -using1 is Some { let rest = [Some]-using1; … } else if … }]`. The cases
  after it are an `IfNode` of their own, marked `FlagIfMatchTail`, which `parseMatch` goes on adding to;
  it is not a match for `ifMatchCover`, which judges the first `IfNode`, whose last arm is that `else`. The
  variable is named `-using` and a number, one of a match's own names (`nameIsMatchHolder`).
  `varDclTypeCheck` asks the variable of that name for an Option, `ErrorUsingForm`, and gives it the error
  type otherwise, which `castIsTypeCheck` leaves unreported.
- *A binding in an `if`, an `else if` and a let-else*, `let rest using prefix("GET ") = line`, reads the call
  where a type would stand (`parseBindVarDcl`; `parseVarDcl` with `ParseMayUsing`, and `ParseState.usingdcl`
  to say it did), and the value after `=` becomes its first argument: `let Some(rest) = prefix(line, "GET ")`,
  held and bound as any bound `if` is. A declaration of this kind with no `else` is `ErrorUsingForm`.

The variable takes `Some` (`parseUsingSome`), so the binding is what a variant declared as a type binds: the
contents. A match with a `using` anywhere in it (`parseUsingSeen`, saved around each match) must end in
`else`, `ErrorMatchUsing` at parse, and its `IfNode` is marked `FlagIfMatchUsing` so that `ifMatchCover` does
not call that `else` unreachable when the other cases cover every value.

**A bound pattern to a variant declared as a type binds what it holds**, not the
variant: type check hoists a hidden local holding the converted value before the
variable, whose initializer becomes the local's contents ([cast](cast.md),
`castBindProject`). The desugaring here is the same for every variant.

**A bound pattern's guard binds the variable a second time.** The variable is
declared at the head of the arm, which the condition is outside, so `case let x
T if g {…}` becomes the condition `is T and {let x = [T]v; g}` — a block
declaring its own `x`, from a second conversion that shares the pattern's type
node (`FlagMatchBind`) — and the arm keeps its own `x` as before.

**What counts toward exhaustiveness is decided on a type checked match, by
`ifMatchCover`** (below): an `is` test of a variant of a closed enum, a `|` of them,
`v == true` and `v == false` on a `bool`, and `_`. A guarded case does not count (the guard
may fail). A number or character alone (`v == 5`) is counted only to find one named twice, never
to cover the match; any other value alone is an `==` call that counts for nothing.

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
so has no say in its type: `let x = if c {1} else {return 7}` and
`let x = if c {1} else {panic("no")}` are both `i32`.

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
flattening the `FlagMatchRow` `&&`s, with the guard (a `FlagMatchGuard` `&&`) taken off and
noted. A cell (`matchClassify`) is one of:

- **wild** — `_`, or, in a `||`, any alternative that is;
- **a set** of the values of the column's type, which the first cell that is one gives the
  column: an `is` of a variant of a closed enum (`matchClosedEnum`: the test `ifExhaustCheck`
  used, a trait with no base carrying `HasTagField` or `SameSize`), which is the variant's index
  in `derived`, or of a trait pattern ([cast](cast.md), "Trait patterns"), which is the indexes
  of the variants that comply (`castIsVariants`), so a trait case after the variants it names,
  or a variant after the trait, is unreachable as a variant named twice is; or `v == true` / `v == false` on a `bool`, the call type check made of
  `case true` and `case ==true` (its callee a use of the `==` it found, the matched value
  its first argument); or `v == 5` on an integer or `char` with a literal (`matchIntTest`).
  A column of these counts the literals the cases name (`matchCollectInts`, a pass before the
  cells are made) plus one more value for every other number, which only `_` or `else` takes;
  so they find a number named twice but never cover a match, and the message for an uncovered
  column of numbers is still the one that asks for `else`. A `||` is the union of its
  alternatives'; a cell of another type than the column's is "other";
- **other** — a comparison, a range, `==` against anything but a bool, integer or character
  literal, a string; anything this logic cannot count.

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
`ErrorMatchUnreachable`, and so is an `else` after cases that cover everything, unless the match
is marked `FlagIfMatchUsing`, which must end in an `else` whatever its other cases cover. The `else` that
holds the cases after a binding by `using` is reported as the case it is.

A match with no `else` that is covered has its **last case rewritten to `elseCond`**, as
`ifExhaustCheck` did, so the value of a match needs no other. One that is not covered is
`ErrorMatchNotExhaustive` (once, replacing `ErrorNoElse` for a match), its type left
unknown as `ErrorNoElse` left it. A match whose cases were not judged falls through to
`ErrorNoElse` as an `if` does.

### Flow typing

`ifTypeCheck` also carries what is known of the variant each name holds ([narrow](narrow.md)):
it restores the environment going into each condition (the earlier conditions false), assumes the
condition true for its block and false for the next, and after the arms joins the environments
of those that do not jump with the path that went by every condition when no `else` ends the
`if` (a match whose cases cover counts as having one). It anchors the hidden local a shared
reference's narrowing is held by to the block (`narrowAnchorArm`, `narrowInsertHeads`) or to
the `if` as a statement of its block (`narrowAnchorAfter`).

`ifRemoveReturns`, called from `returnTypeCheck`, strips the redundant `return`
from each arm of `return if … { return a } else { b }`, recursively.

## Flow

`ifFlow` walks the first condition, then each arm from the state the
conditions before it leave, a later condition and each arm one level deeper
(`flowDepth`). Each arm's changes to *outer* variables' flags are taken and
rolled back (`flowVarPathTake`, `flowVarRollback`), and the arms that did not
jump away, and a missing `else`, are joined (`flowVarJoin`): a flag any of them
set is set after the `if`, so a value moved in both arms compiles, and one moved
in one arm may not be used after it. An arm that ends in a `return`, a `break`
or a `continue` jumped away: what a `break` or `continue` changed went to its
loop, which joins it where the loop ends ([Flow Analysis](../phases/flow.md),
"What ends a path"). Whether each variable holds its value at its releases,
where that differs by arm, is the path walk's to decide, and a drop flag's at
run time ([Flow Analysis](../phases/flow.md), "Drop flags"). `ifFlow` leaves
`FlowState.jumped` set when every arm jumped away.

## Generation

`genlIf` creates `endif` first, then per pair an `ifnext` (or `endif` for the
last) and an `ifblk`, and a conditional branch. **For `elseCond` no block is
created** — the else body is emitted into whatever `ifnext` the previous
iteration left the builder at.

**A temporary a condition makes dies once the condition is computed**, before
the conditional branch (`genlTempsEnd`): a later condition runs on some paths
only, so it cannot wait for the statement's end, and a `while`, which arrives
as `if !cond { break }`, finalizes its own each pass ([Generation](../phases/generation.md),
"Temporaries").

**A marked `if` (`FlagIfMatch`) whose leading arms test one integer, character or
enum tag is one LLVM `switch`** (`genlIfSwitch`), made before the chain, so flow,
borrows, narrowing and exhaustiveness have all worked on the `if` nodes as written.
`genlSwitchTest` reads each arm's condition as it was type checked and
takes it if it is: a `==` of the matched value with a literal of its type or a named constant
(the built-in integer `==`); `v >= lo && v <= hi` or `v < hi`, the range pattern, spelled out as a label
for each value (at most `SwitchMaxRange`, 128); an `is` of a variant, or of the variants a
trait pattern names (`CastNode.variants`), a label for each tag; or a `|` of those. The matched value
is the expression the first test reads, generated once before the `switch`
(a place is read once instead of once for each test; a call was already held in a variable), and for a tag it
is the enum's tag field, read through the reference if the matched value is one
(`genlTagLoad`, which `genlIsVariant` uses too). The plan stops at the first arm that is
`elseCond`, is not such a test (a guard — the `&&` flagged `FlagMatchGuard` —, a `match a, b` row, another
comparison, a call, which a function's pattern is, a string, a float), tests a different kind of value than the first, holds a
nullable-pointer variant or a virtual reference, or would take the switch past `SwitchMaxLabels` (512) labels;
fewer than `SwitchMinArms` (2) arms stay a chain. A value an earlier arm already has keeps that arm
(LLVM refuses a label twice). Each arm's block is generated as the chain's are (`genlIfArm`): a binding is
the first statements of the block, not part of the test. The `switch`'s default is the `ifnext` where
the chain carries on from the first arm not taken, so an `else`, which a match that covers an enum has
as its last case after `ifMatchCover`, is the default. A case that tests with a function's pattern is a call,
so it and the cases after it are that chain; one that binds with it is the `else` that holds the rest of the
match (Parse), so the cases before it are the `switch` and the rest is its default. The only difference from the chain is that the matched
value is read once, which no arm could have changed before it was chosen. A debug build's `simplifycfg`
makes a `switch` of a chain of `==` or of tag tests too, but not of ranges, and a chain it cannot make
one of costs a compare for each arm; the plan makes the `switch` before any pass, in every build.

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
  `||` and `&&` logic nodes, a literal, and the call of the `==` a comparison found
  (callee a name use of the declaration, the matched value first). A lowering that
  changes one of these makes the cell "other" and the match asks for an `else`; it does
  not mis-cover. A match counted wrongly would be the dangerous failure, so a cell is
  a set only for exactly those shapes.
- **Exhaustiveness recognizes a scrutinee only as the shared node or as a use
  of the lowering's variable.** A lowering that wraps one arm's scrutinee in a
  coercion or a deref silently breaks detection — and it surfaces as
  `ErrorNoElse`, not as a defect.
- **`ErrorNoElse` returns with `vtype` still `unknownType`.**
- **`ifRemoveReturns` calls `nodesLast` without an emptiness guard.**

## What lives elsewhere

- Branch folding, the type in common, and the second pass: [Type Check Reasoning](../phases/type-check-reasoning.md), "Unifying branches"
- Where `derived` and `HasTagField` come from: [struct](struct.md)
- How flow joins the arms, and follows each path for drops: [Flow Analysis](../phases/flow.md)
- The `is` node itself: [cast](cast.md)
- `match` de-sugaring: [Parse](../phases/parse.md)
