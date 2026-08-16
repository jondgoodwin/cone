# Ownership memory safety

Six defects in how owning references are generated and coerced. Each is a way the
compiler emits code that corrupts memory, or accepts a program it should reject.
They were found by building the `region` group of the test suite; the evidence,
with file and line, is under "Found while building the groups" in
[[Add test suite]].

This is the most serious body of work that survey produced, and it is separated
from the other defects for one reason: **these are miscompiles.** A program that
fails to compile wastes an afternoon. A program that compiles, runs, and
double-frees wastes a week, and does it in someone else's code.

## The soundness hole comes first

`refMatches` calls `regionMatches(from->region, to->region)`. The arguments are
swapped against the parameter names, and the effect runs both ways:

- An **owning reference is rejected** where a borrowed one is wanted. Merely
  obstructive, and pinned by `test/cases/region/region-borrow-coerce.cone` as an
  `xfail`, so fixing it will announce itself.
- A **borrowed reference is silently accepted** where an owning one is wanted. A
  stack address reaches a parameter typed `+rc-mut`; it compiles, it runs, and
  whatever the callee does with it — retain, release, store — operates on a
  pointer the region does not own.

The second has no diagnostic to assert against, so **the test suite cannot hold
it**. That is worth stating plainly: fixing this defect is not something a test
run will confirm, and a scenario for it can only be written once the compiler
rejects the coercion.

Swapping the arguments back is a one-line change. What it costs is that programs
which currently compile will stop, and some of those will be in `test/cases/` —
this work item exists because that consequence needs deciding, not because the
line is hard to find.

## The four codegen defects

Each has an `xfail` scenario in `test/cases/region/` that will fail the suite the
day it is fixed, so none of them can be fixed quietly.

| Defect | Cause | Scenario |
| --- | --- | --- |
| A moved `+so` reference double-frees | `genlDealiasNodes` frees every variable whose type is an owning reference, with no notion of one whose value was moved out. Flow analysis knows; codegen does not ask | `region-so-move` |
| A first assignment to an uninitialized `rc` variable releases garbage | `genlStore` dealiases the lval's previous value without consulting `VarInitialized` | `region-uninit-owning` |
| A region-allocated struct owning a reference corrupts the heap on release | `genlDealiasFlds` reaches the field with a struct GEP and hands that **address** to `genlRcCounter`/`genlDealiasOwn`, which want the reference the field **holds** | `region-owning-field` |
| A nested allocation fails LLVM module verification | | `region-nested-alloc` |

A fifth, fallible allocation `?+rc-mut v`, dies with an access violation and so
belongs to [[Diagnose instead of crash]] rather than here.

The first two share a shape worth naming: **codegen is not consulting what flow
analysis already worked out.** `move` established that flow analysis correctly
tracks deactivation, and `region-flow` establishes that it correctly identifies
which owning references are move types. The information exists at the point
`genlDealiasNodes` runs; it is simply not read. Whether the fix is to consult the
flags or to have flow analysis annotate the nodes it has decided about is the
design question.

The third is a plain bug with no decision attached, and is the cheapest of the
four.

## Why a stack struct with an owning field is not also broken

It leaks instead. Its fields are never released at all, which is why the defect
only shows up for region-allocated structs. Fixing the GEP will therefore turn a
leak into a release, and that release has to be correct — the two cannot be
separated.

## Not in scope

Weak references and lock permissions do not exist; see [[Regions]] and
[[Permissions]]. `RaceSafe` and `IsLockless` are declared, populated, and read
nowhere, which belongs to [[Unenforced language rules]].
