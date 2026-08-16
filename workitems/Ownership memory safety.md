# Ownership memory safety

> **These are defects in provisional code.** Regions are only partly built and
> were never robustly designed; a redesign carrying substantially new ideas is
> intended, and getting that design right is the priority. Nothing here should be
> read as an argument for hardening the current implementation. Anything fixed
> before the redesign is a stopgap, and should look like one.

Six defects in how owning references are generated and coerced, found by building
the `region` group of the test suite. The evidence, with file and line, is under
"Found while building the groups" in [[Add test suite]]. It is recorded here
because it is expensive to rediscover, not because it is urgent to repair.

## The question this work item asks

Not *how do we fix these properly*. The implementation they live in is going to
be replaced, so a careful fix is effort spent on code with a known expiry date,
and a thorough one risks entrenching decisions the redesign wants to make freshly.

The question is: **which of these cost time before the redesign lands, and what
is the cheapest thing that stops them costing it?**

That splits the six cleanly, because they differ in how they fail rather than in
how deep they are.

## Triage

### Silent memory corruption — worth a stopgap

These produce wrong runtime behavior with no diagnostic and no crash at the point
of the mistake. They are the only ones that can burn an afternoon: a program
compiles, runs, and corrupts the heap, and nothing points at the compiler.

| Defect | Cause | Scenario |
| --- | --- | --- |
| A moved `+so` reference double-frees | `genlDealiasNodes` frees every variable whose type is an owning reference, with no notion of one whose value was moved out | `region-so-move` |
| A first assignment to an uninitialized `rc` variable releases garbage | `genlStore` dealiases the lval's previous value without consulting `VarInitialized` | `region-uninit-owning` |
| A region-allocated struct owning a reference corrupts the heap on release | `genlDealiasFlds` reaches the field with a struct GEP and hands that **address** to `genlRcCounter`/`genlDealiasOwn`, which want the reference the field **holds** | `region-owning-field` |

**These three are probably one fix, not three**, and establishing whether that is
true is the first thing worth doing. The first two are both *codegen not asking
what flow analysis already worked out* — flow correctly tracks deactivation and
correctly identifies which owning references are move types, and the dealias path
simply does not read it. The third looks like a plain wrong-argument bug, but
fixing the GEP turns a leak into a release, and that release then has to be
correct, which lands it back in the same territory.

A caveat that matters for scoping: a **stack** struct with an owning field is
currently safe only because its fields are never released at all. They leak. So
the third fix cannot be evaluated on its own.

### Silent unsoundness — measure, then decide

`refMatches` calls `regionMatches(from->region, to->region)`, arguments swapped
against the parameter names. One direction rejects an owning reference where a
borrowed one is wanted, which is obstructive and is pinned by
`region-borrow-coerce` as an `xfail`. The other **silently accepts a borrowed
reference where an owning one is wanted**, handing a stack address to a parameter
typed `+rc-mut`.

Swapping the arguments back is one line. What it costs is that programs which
compile today will stop, and some may be in `test/cases/`. **Measure that before
deciding** — if the blast radius is small, this is the cheapest real win on the
page, because it removes a trap that fails silently. If it is large, it is
redesign work rather than stopgap work, since what coercions exist between
regions is exactly what a redesign would settle.

Note the suite cannot hold the dangerous half. There is no diagnostic to assert
against until the compiler rejects the coercion, so a scenario for it can only be
written after the fix.

### Loud failures — leave them

| Defect | How it fails |
| --- | --- |
| A nested allocation | LLVM module verification fails, at compile time |
| Fallible allocation `?+rc-mut v` | Access violation, immediately |

Neither is silent and neither can be mistaken for working code, so neither wastes
debugging time — you find out at once. Fixing them buys convenience, not safety,
and the redesign will revisit both paths anyway. `region-nested-alloc` is pinned
as an `xfail`; the fallible-allocation crash is cross-listed to
[[Diagnose instead of crash]], which is where the systematic version of that
problem lives.

## What the scenarios are worth, given a redesign is coming

Five `xfail` scenarios sit in `test/cases/region/`. They cost nothing to keep and
they are worth keeping even though the surface may change, for two reasons: each
is a reduced, executable statement of a defect that would otherwise have to be
rediscovered from prose, and each will flip to XPASS and fail the suite the moment
the behavior changes — including as a side effect of the redesign, which is
exactly when you want to be told.

If the redesign changes the spelling of regions, they will need rewriting rather
than deleting. `region-success` is the one to protect: 37 established facts about
what allocation, reference counting and release actually do today, which is the
closest thing there is to a specification of the current behavior for the
redesign to diff against.

## Not in scope

Weak references and lock permissions do not exist; see [[Regions]] and
[[Permissions]]. `RaceSafe` and `IsLockless` are declared, populated, and read
nowhere, which belongs to [[Unenforced language rules]].
