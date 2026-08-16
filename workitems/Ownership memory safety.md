# Ownership memory safety

> **These are defects in provisional code.** Regions are only partly built and
> were never robustly designed; a redesign carrying substantially new ideas is
> intended, and getting that design right is the priority. Nothing here should be
> read as an argument for hardening the current implementation. Anything fixed
> before the redesign is a stopgap, and should look like one.

Six defects in how owning references are generated and coerced, found by building
the `region` group of the test suite. The evidence, with file and line, is under
"Found while building the groups" in [[Add test suite]].

**Five of the six are fixed.** Only fallible allocation remains of the original
list. Tracing them uncovered four further defects, three of them fixed.

## Status

| Defect | How it failed | State |
| --- | --- | --- |
| Region coercion reads backwards | Silent unsoundness | **Fixed** — `reference.c:181`, `:226`, `arrayref.c:82` |
| A moved `+so` reference double-frees | Silent corruption | **Fixed** — `flow.c` |
| First assignment to an uninitialized `rc` releases garbage | Silent corruption | **Fixed** — `assign.c` + `genlexpr.c` |
| A struct's `rc` owning field corrupts the heap on release | Silent corruption | **Fixed** — `genlalloc.c:62` |
| A nested allocation fails module verification | Loud, at compile time | **Fixed** — `genlallocref`'s phi |
| Fallible allocation `?+rc-mut v` | Loud, immediately | **Open** — see below |

Found while fixing the above:

| Defect | How it failed | State |
| --- | --- | --- |
| A type literal's field values are never moved or copied | Silent corruption, and a use-after-move | **Fixed** — `typeLitFlow` |
| A temporary's reference is counted as a second holder | Silent leak | **Fixed** — `flowHandleMoveOrCopy` |
| An array literal's elements are never moved or copied | Silent, same shape | **Fixed** — `arrayLitFlow` |
| An array literal cannot be generated unless its elements are constants | Loud, at compile time | **Open** — `array-nonconst-literal` |
| Every array-reference allocation takes the fill path | **Silent, wrong data** | **Open** — `genlalloc.c:227` |

The suite went from 111 scenarios / 98 passed / 14 xfail to **118 / 108 / 11**.

The last of those is the worst-behaved thing on this page and is one token:
`((ArrayNode*)allocatenode->vtexp)->dimens > 0` tests a `Nodes*` pointer, which is
never null there, instead of `->dimens->used > 0` as the same function does sixty
lines earlier. So `+[]rc-mut [11i32, 22i32, 33i32]` compiles, runs, and fills the
array with three copies of `11`. It belongs to array allocation rather than to
ownership, but it is recorded here because this is where it was found.

## The question this work item asks

Not *how do we fix these properly*. The implementation they live in is going to
be replaced, so a careful fix is effort spent on code with a known expiry date,
and a thorough one risks entrenching decisions the redesign wants to make freshly.

The question is: **which of these cost time before the redesign lands, and what
is the cheapest thing that stops them costing it?**

That splits the six by how they fail rather than by how deep they are. The three
silent-corruption defects and the soundness hole earn a stopgap, because a program
that compiles, runs and corrupts the heap can burn an afternoon with nothing
pointing at the compiler. The two loud ones do not: you find out at once, so
fixing them buys convenience rather than safety, and the redesign revisits both
paths anyway.

## What measuring the stopgaps showed

Each candidate was built, run against the whole suite, and reverted before any of
it was recommended. Three results are worth carrying into the redesign.

**The three silent-corruption defects were not one fix.** The original triage
guessed they shared a cause. Each turned out to be independent — fixed alone, with
zero regressions — and two landed in a different phase than predicted:

- **The `+so` move fix is in flow analysis, not code generation.** The dealias
  list is *built* by `flowScopeDealias` (`ir/flow.c:198`) and merely executed by
  `genlDealiasNodes`. Flow holds the move state at exactly that point and never
  read it.
- **The uninitialized-store fix could not read flow state from codegen.**
  `flowtempflags` is a running summary, not a per-program-point record; by code
  generation it says only "was initialized somewhere". Flow now marks the
  assignment site itself.
- **The owning-field fix was the plain wrong-argument bug it looked like.** The
  predicted complication — that fixing the GEP would turn a leak into a release
  that then had to be made correct — did not occur.

**There is no durable flow record for later phases.** `VarDclNode.flowflags`, the
*permanent* flow word (`ir/stmt/vardcl.h:20`), is zeroed in two places and never
set or read. `VarFlowInfo.flags` (`ir/flow.c:159`), commented "the preserved flow
flags", is likewise dead. That absence, rather than any single bug, is the
structural reason this class keeps recurring, and it is the thing the redesign
should fix properly.

**The coercion swap cost nothing, and it is three lines rather than one.** The
same reversed call sits at `reference.c:226` and `arrayref.c:82`; fixing only
`refMatches` would have left virtual references and ref-to-arrayref coercion
backwards. `ir/exp/cast.c:176` is a fourth call in the same order and was left
alone deliberately — it asks a different question (`is` downcasting), where the
regions are normally identical so `EqMatch` fires first. **Deciding what region
downcasting means belongs to the redesign.**

The one honest limit on "zero regressions": the repository contains no Cone source
outside `test/cases/`, so the suite is the whole measurable corpus.

### Known cost of the stopgaps

- The `VarMoved` skip is **path-insensitive**. The flag is the state at scope exit,
  so a value moved on only one branch is skipped on all of them — that leaks
  rather than double-frees. Making it precise means per-program-point flow state,
  which is redesign work.
- `FlagFirstAssign` adds a flag bit to the IR. It is the most entrenching of the
  four and the first to reconsider once the redesign starts.

## What a reference count means, and the two defects that followed

Tracing the `+so` owning field led out of this group entirely, into what the count
counts. The rule is that **the count is how many holders exist**:

- `+rc[2]` creates the object *and* the first reference to it. Born at 1.
- `imm a = +rc[2]` adds no holder. The temporary hands its reference to `a`, so
  the count stays 1.
- `imm b = a` does add one — `a` keeps its reference and `b` gets another — so 2.
- `+so` is the same shape with the second holder forbidden, so binding from an
  existing holder deactivates the source instead of counting it.

Two defects followed from the compiler not expressing that.

**`iexpIsMove` tests the type alone**, and `+rc-mut 7` and `a` have the same type.
A type-only test cannot tell a temporary handing over its reference from an
lvalue that keeps one, so `flowHandleMoveOrCopy` aliased both and every allocation
bound to a variable was born at 1, aliased to 2, released once, and **leaked**.
The suite could not see it: a `run` scenario observes stdout, not the heap. The
fix is one condition — alias only an lvalue read.

**A type literal was a leaf to flow analysis.** `TypeLitTag` sat in the same
`switch` bucket as `ULitTag` and `StringLitTag`, so its field values were never
visited: no move, no alias, no lifetime check. That is why a `+so` owning field
double-freed — the source variable was never deactivated — and it was
independently a **use-after-move hole**. A type literal is a `FnCallNode`, so it
now does what a call does with its arguments, unwrapping the `NamedValNode` that
wraps a value given by field name.

Neither turned out to need a language decision. Both are the type's existing rule
applied where it was not being applied.

**An array literal was a leaf too**, and closing it took one decision rather than
a design. The list form `[a, b]` gives every element a holder of its own, so each
value is moved or copied exactly as a call's argument is. The fill form
`[n; value]` makes n holders out of one value, and the two rules answer that
differently: a move value has one owner and cannot have n, so a fill of one is
**refused** — `ErrorBadFill`, unconditionally, because proving that n is 1 buys a
construct nobody writes at the cost of a rule that is harder to state. A counted
reference is repeated legitimately, so the count rises by n, or by n-1 from a
temporary. That generalizes the non-fill case, which is exactly n = 1.

The amount is a constant in the alias node, so a count known only at run time is
refused as well rather than counted wrongly. **Rewriting a fill into a loop before
generation is the general answer**, and it is recorded in [[Types. Array]] along
with the semantic question it raises.

## What remains

**The count a fill asks for cannot be asserted against generated code**, and the
reason is not in this group: `genlExpr` builds an array literal with
`LLVMConstArray`, so an element that is the result of an instruction — and an
allocation always is — produces a constant array with instruction operands, and
the module fails verification. It is true of an `i32` as much as of a reference,
which is why nothing had noticed: the `array` group only ever writes constant
elements. `array-nonconst-literal` records the cause and `region-fill-count` the
consequence.

**The two loud failures stay open deliberately.** Neither can be mistaken for
working code — nested allocation fails module verification at compile time, and
`?+rc-mut v` dies immediately with an access violation.

## What the scenarios are worth, given a redesign is coming

The `xfail` scenarios cost nothing to keep and are worth keeping even though the
surface may change: each is a reduced, executable statement of a defect that would
otherwise have to be rediscovered from prose, and each flips to XPASS and fails the
suite the moment the behavior changes — including as a side effect of the redesign,
which is exactly when you want to be told.

That is not a prediction any more. Four of the five did precisely this during the
stopgap work, and each flip is how the corresponding fix was confirmed. So did
both scenarios written afterwards to record newly found defects — one of which
was fixed within the hour, by the scenario telling us it had been.

If the redesign changes the spelling of regions, the remaining scenarios will need
rewriting rather than deleting. `region-success` is the one to protect: 37
established facts about what allocation, reference counting and release actually
do today, which is the closest thing there is to a specification of current
behavior for the redesign to diff against. It passed unchanged under every
prototype, which is what made "no regressions" a claim about behavior rather than
about compilation.

## Not in scope

Weak references and lock permissions do not exist; see [[Regions]] and
[[Permissions]]. `RaceSafe` and `IsLockless` are declared, populated, and read
nowhere, which belongs to [[Unenforced language rules]].
