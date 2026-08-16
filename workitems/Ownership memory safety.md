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

## Recommendation, from measurement rather than survey

Each stopgap below was built and run before being recommended. Method: build the
compiler, confirm the baseline (111 scenarios, 98 passed, 14 expected failures),
apply one candidate fix, rebuild, run the whole suite, record the result, revert.
Then apply all of them together and run again. Everything was reverted afterwards
— nothing under `src/` changed. The numbers below are observed, not projected.

### The triage's grouping claim does not hold: three defects, three fixes

The work item proposes the three silent-corruption defects "are probably one fix,
not three". They are not. Each was fixed **in isolation**, and each fixed its own
scenario with **zero regressions across the full suite**.

| Defect | Where the fix goes | Size | Result in isolation |
| --- | --- | --- | --- |
| `region-so-move` | `ir/flow.c:205`, in `flowScopeDealias` | 2 lines | XPASS, 98 pass, no regressions |
| `region-uninit-owning` | `ir/exp/assign.c:168` + `genllvm/genlexpr.c:818` + a flag bit | ~5 lines, 3 files | XPASS, 98 pass, no regressions |
| `region-owning-field` | `genllvm/genlalloc.c:62`, add the load | 2 lines | XPASS, 98 pass, no regressions |

The reasoning that grouped them is what misleads, and it is worth correcting
because it points at the wrong subsystem:

- **`region-so-move` is not codegen ignoring flow analysis.** The dealias list is
  *built by flow analysis* — `flowScopeDealias` (`ir/flow.c:198`), called from
  `blockFlow` (`ir/exp/block.c:291-321`), walks the scope's variables and records
  which ones to release on `BreakRetNode->dealias`. `genlDealiasNodes` only
  executes that list. Flow has `flowtempflags` in its hand at exactly that point
  and never reads it. The fix is `continue` on `VarMoved`, in flow, not codegen.
- **`region-uninit-owning` genuinely is codegen not asking** — but it cannot be
  fixed by reading `flowtempflags` from `genlStore`, because that word is a
  *running* state during the walk, not a per-program-point record. By codegen it
  says only "was initialized somewhere". Flow has to record the fact at the
  assignment site instead; `ir/exp/assign.c:162` already computes exactly this
  condition and then throws it away.
- **`region-owning-field` is the plain wrong-argument bug it looks like.** The
  predicted complication — that fixing the GEP "turns a leak into a release, and
  that release then has to be correct" — did not occur. Adding the load fixed the
  scenario and `region-success` still passed unchanged.

Worth recording for the redesign: `VarDclNode.flowflags`, the *permanent* flow
word (`ir/stmt/vardcl.h:20`), is zeroed in two places and never set or read
anywhere. `VarFlowInfo.flags` in `ir/flow.c:159`, commented "the preserved flow
flags", is likewise set to 0 and never used. **There is no durable flow record for
later phases to consult.** That absence, not any individual bug, is the structural
reason this class of defect keeps recurring.

### The coercion swap costs nothing — measured, and it is the best buy

Swapping `regionMatches(from->region, to->region)` to `(to->region, from->region)`
at `ir/types/reference.c:181`: **the entire suite is unchanged except that
`region-borrow-coerce` flips to XPASS.** No other scenario moved.

Two things the survey missed:

- **It is three lines, not one.** The same reversed call appears at
  `ir/types/reference.c:226` (`refvirtMatchesRef`) and `ir/types/arrayref.c:82`
  (`arrayRefMatchesRef`). Fixing only `refMatches` leaves virtual references and
  ref-to-arrayref coercion still backwards. Swapping all three: **still zero
  regressions.** Do all three or the hole stays open on two paths.
- **Leave `ir/exp/cast.c:176` alone.** It is a fourth call in the same argument
  order, but it asks a different question (`is` downcasting), where the regions
  are normally identical so `EqMatch` fires first. Swapping it blind would be
  guessing. Flag it for the redesign.

Honest limit on "blast radius": the repository contains **no Cone source outside
`test/cases/`** — 110 files, all of them the suite. `conesite/` ships HTML and a
prebuilt `.wasm`, not compilable sources. So the suite *is* the measurable corpus,
and "zero regressions" means zero across everything there is to measure. The
direction the swap newly rejects is precisely the unsound one, so unmeasured
breakage would have to be code that was already relying on the hole.

### Confidence without a test: the problem dissolves on contact

The work item says the dangerous half "cannot have a scenario until it is fixed".
True, and the fix supplies one immediately. Probe — a borrowed reference passed to
a `+rc-mut` parameter:

```
fn takesOwning(r +rc-mut i32) i32 { *r }
fn f() i32 {
  mut local = 7i32
  takesOwning(&mut local)     // hands a stack address to an owning parameter
}
```

Pristine compiler: **compiles clean, exit 0.** With the swap: `Error 1013:
Expression's type does not match declared parameter`. So there is a real
diagnostic to assert the moment the swap lands. The answer to "how would you gain
confidence without a test" is: don't — fix it first, then pin it, in that order,
in the same change. The scenario is about ten lines in a `reject` case.

### A seventh defect, unrecorded, that limits the third stopgap

`region-owning-field` pins a struct with an **`rc`** owning field. A struct with a
**`+so`** owning field still corrupts the heap — with the GEP fix applied *and* on
the pristine compiler, identically (`0xC0000374`, heap corruption):

```
struct HeldSo { r +so i32 }
fn f() {
  imm inner = +so 5
  imm outer = +so HeldSo[r: inner]     // 'inner' is never deactivated
  ...
}
```

The cause is not in `region` at all: **a type literal's field initializers do not
apply move semantics.** Passing a `@move` value as a call argument correctly
raises `ErrorMove` (1048); placing the same value in a struct literal raises
nothing, and the source variable stays live and stays on the dealias list. Hence
the double free. It is independently a **use-after-move hole** — reading the
moved-from variable afterwards compiles clean.

This is not recorded in this item or in [[Add test suite]]'s survey, and the
`move` group covers only moving *out of* an aggregate (`move-flow-aggregate`),
never *into* one. It belongs to `move`, and it should be **handed to the redesign
rather than patched**: making literals move changes which programs are legal,
which is a language decision, not a stopgap.

The consequence for scoping: the GEP fix is worth doing and covers the `rc` half,
but it is a **partial** fix and should be labelled as one.

### What to do

Land as one small, obviously-temporary change, ~10 lines total:

1. **Swap the region arguments at all three sites** (`reference.c:181` and `:226`,
   `arrayref.c:82`). Silent unsoundness, measured cost zero, and it converts an
   unassertable hole into an assertable one. Highest value on the page.
2. **Skip moved variables in `flowScopeDealias`.** Silent double free, 2 lines.
3. **Load the field in `genlDealiasFlds`.** Silent heap corruption, 2 lines,
   `rc` half only.
4. **Record first-assignment in flow and read it in `genlStore`.** Silent release
   of garbage. The most invasive of the four — it adds a flag bit to the IR — and
   the first to reconsider if the redesign starts soon.

All four together: **98 passed, 4 XPASS, no regressions.**

Where a hack would be worse than the defect:

- **Do not make the `VarMoved` skip path-sensitive.** The flag is a running state,
  so a variable moved on only one branch is skipped on every branch — that leaks.
  A leak is strictly better than a double free, and making it precise means
  per-program-point flow state, which is redesign work.
- **Do not fix the struct-literal move gap** as a stopgap (above).
- **Do not touch `cast.c:176`** without deciding what region downcasting means.
- **Leave the two loud failures.** Confirmed still loud: nested allocation fails
  module verification at compile time, and `?+rc-mut v` dies immediately with an
  access violation (`0xC0000005`). Neither can be mistaken for working code.

### Scenarios this implies — to write with the fix, not before

- Drop `xfail` from `region-so-move`, `region-uninit-owning`, `region-owning-field`
  and `region-borrow-coerce`; they become ordinary passing scenarios.
- **New `reject` scenario** for the dangerous coercion half, now that it has a
  diagnostic. It is `region`'s, alongside the other type-check scenarios.
- **New `xfail`** for the `+so` owning field, recording the half the GEP fix does
  not cover.
- **New `xfail` in `move`** for the struct-literal move gap, with the
  use-after-move probe — that is where the defect actually lives.

### What this says about the five xfails

The question of whether they survive the redesign largely evaporates: **four of
the five would be retired by this change itself**, leaving only
`region-nested-alloc`. And they behaved exactly as the item argues they would —
each flipped to XPASS and failed the suite the moment its defect was fixed, which
is how every measurement above was confirmed. They earned their keep this session.

`region-success` is the asset the item says it is, and it proved it here: it
passed unchanged under all four prototypes, which is what made "no regressions"
a statement about behavior rather than about compilation.

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
