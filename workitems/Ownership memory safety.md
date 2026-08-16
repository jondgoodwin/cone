# Ownership memory safety

> **These are defects in provisional code.** Regions are only partly built and
> were never robustly designed; a redesign carrying substantially new ideas is
> intended, and getting that design right is the priority. Nothing here should be
> read as an argument for hardening the current implementation. Anything fixed
> before the redesign is a stopgap, and should look like one.

Six defects in how owning references are generated and coerced, found by building
the `region` group of the test suite. The evidence, with file and line, is under
"Found while building the groups" in [[Add test suite]].

**Four are now fixed by stopgaps.** Two remain, plus one more the stopgap work
uncovered. This item stays open for those three.

## Status

| Defect | How it failed | State |
| --- | --- | --- |
| Region coercion reads backwards | Silent unsoundness | **Fixed** — `reference.c:181`, `:226`, `arrayref.c:82` |
| A moved `+so` reference double-frees | Silent corruption | **Fixed** — `flow.c:205` |
| First assignment to an uninitialized `rc` releases garbage | Silent corruption | **Fixed** — `assign.c:170` + `genlexpr.c:819` |
| A struct's `rc` owning field corrupts the heap on release | Silent corruption | **Fixed** — `genlalloc.c:62` |
| A struct's `+so` owning field double-frees | Silent corruption | **Open** — `region-owning-field-so` |
| A nested allocation | Loud, at compile time | **Open, deliberately** — `region-nested-alloc` |
| Fallible allocation `?+rc-mut v` | Loud, immediately | **Open, deliberately** — [[Diagnose instead of crash]] |

The suite went from 111 scenarios / 98 passed / 14 xfail to **114 / 103 / 12**.

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

## What remains

**A `+so` owning field still double-frees**, and the cause is not in this group: a
type literal's field initializers do not apply move semantics, so naming a
variable in a literal never deactivates it. Passing the same value as a call
argument does deactivate it. That gap is independently a **use-after-move hole** —
reading the moved-from variable afterwards compiles clean — and it is recorded as
`move-flow-literal` in the `move` group, with `region-owning-field-so` recording
the consequence here.

**Closing it is a language decision, not a codegen fix**: it settles whether a
literal moves or copies its initializers. It is handed to the redesign rather than
patched.

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
stopgap work, and each flip is how the corresponding fix was confirmed.

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
