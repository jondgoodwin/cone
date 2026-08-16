# Diagnose instead of crash

> **Done.** Every site is fixed, and the compiler now reports each of them
> instead of dying. Investigating them turned up two more crashes on *valid*
> programs — an empty block and an uninstantiated generic — plus a third that had
> nothing to do with an error path at all: a trait implementer declared after the
> first function whose signature names the trait got a null function pointer in
> its vtable. `--checktree`, which this item was to extend, turned out to be a
> no-op; it is implemented, and the runner now passes it on every compile rather
> than only the ones that succeed. Suite: **118 scenarios / 110 passed / 9 xfail**
> to **119 / 111 / 9**.
>
> Two entries left this item rather than being fixed here. Fallible allocation
> moved to [[Regions]], which owns the design it needs. Cross-module private
> access was fixed here after all, because the language answer it was waiting for
> is already published; the corresponding entry in [[Unenforced language rules]]
> is closed.

Places where the compiler works out that a program is wrong, reports it or
returns an error, and then dereferences the NULL its own error path left behind.
Found by building the test suite; the evidence is under "Found while building the
groups" in [[Add test suite]], shape B.

## Why these were grouped, and why they were urgent out of proportion to their size

**The test suite structurally could not hold any of them.** A scenario asserts
something about a compile: its exit status, its diagnostics, its output. An
access violation produces an exit status outside the `ErrorCode` taxonomy and
nothing else, so there was no assertion to make. It could not even be marked
`xfail`, because `xfail` asserts that a case *fails*, and a process death fails
nothing.

Every other defect the survey found is either fixed, pinned by an `xfail`, or
excluded with a written reason that a future reader will find. These were
excluded with a reason and **nothing watched them**. That was the argument for
fixing them as a group rather than as they were encountered.

Each fix landed with the scenario that fails without it (R6.3) — and for these,
the scenario could not be written first. Each was written after the fix and then
run against the pre-fix binary to confirm it really fails there.

## Status

| Construct | Where the NULL came from | State |
| --- | --- | --- |
| `struct {`, `trait {`, `union {` with no name | `parseStruct` reported `ErrorNoIdent` and returned NULL; the caller dereferenced it | **Fixed** — `parsetype.c`, `module.c` |
| `each x in <anything not a range>` | `parseEach` filled its wrapping block only on the range path, so the loop *and its body* were discarded in silence and the empty block killed the type check | **Fixed** — `parsefnflow.c`, `block.c` |
| `[2, 3; 0]` — a two-dimension array literal | `arrayLitTypeCheckDimExp` returned after the error without assigning `arrlit->vtype`, which `newArrayNode` never initialized either | **Fixed** — `arraylit.c`, `array.c` |
| A wrong number of generic type arguments | `genericMemoize` returned NULL; `genericSubstitute` type-checked it | **Fixed** — `generic.c` |
| A non-type generic argument | Same | **Fixed** — `generic.c` |
| `fn f[]()` — an empty type parameter list | Not a parser crash: `genlGlobalSyms` walked the generic's null `memonodes` | **Fixed** — `genllvm.c`, and diagnosed by `parsefnflow.c` |
| Narrowing to a *structurally* conforming target | `genlIsType` looked the vtable up through the concrete type's declared base trait, which a structural conformer does not have | **Fixed** — `genlexpr.c` |
| Cross-module private access | Name resolution accepted `mod::_privateName`; codegen never generated a symbol, so the call site used a null `llvmvar` | **Fixed** — `nameuse.c`, `import.c` |
| Fallible allocation `?+rc-mut v` | Needs a rebuilt lowering, not a repair | **Moved** — see [[Regions]] |

### Found while fixing these

| Construct | What happened | State |
| --- | --- | --- |
| An empty block, `{}` | Legal Cone. `blockTypeCheck` called `inodeTypeCheck` with the null "last statement" of a block that has none | **Fixed** — `block.c` |
| A generic nothing instantiates | Legal Cone. `genlGlobalSyms` and `genlGlobalImpl` walked the null `memonodes` a generic starts with | **Fixed** — `genllvm.c` |
| A trait implementer declared after the trait's first use in a signature | Legal Cone, and nothing to do with an error path. A vtable is built the first time a type naming it is generated, which happens *during* the symbol pass, so an implementer declared later in the file had no symbol yet and its vtable slot took a null function pointer | **Fixed** — `genltype.c` |
| Four constructors left `vtype` uninitialized | `newNameUseNode`, `newMemberUseNode`, `newAssignNode`, `newIsNode`. Arena memory is not zeroed, so these held whatever was there — which is why the array-literal crash faulted at an arbitrary address rather than at zero, and why the invariant below could not be checked before they were fixed | **Fixed** — `nameuse.c`, `assign.c`, `cast.c` |

## The shared question, and what was done about it

Two remedies were available and they were not exclusive. Both were built.

**Per site**, the error path now leaves a well-formed node, so analysis continues
and reports more than one problem. `corelib.c` gains a third singleton alongside
`unknownType` and `noCareType`:

```c
errorType = (INode*)newAbsenceNode();
errorType->tag = UnknownTag;
```

Told apart from the other two by identity, as they are from each other. It means
**already reported as bad**, where `unknownType` means *not inferred yet* — the
distinction matters, because the first must silence follow-on diagnostics and the
second must not. `itypeMatches` and `iexpMultiInfer` are where it does the
silencing, and `newErrorNode` (`void.c`) builds the expression-shaped counterpart
for a site that has to replace a whole subtree rather than retype one node.

**Once**, the invariant was made checkable. `--checktree` had a command-line
option, a help line, and no implementation — `opt->check_tree` was set and never
read. `ir/checktree.c` implements it: after analysis, every expression node
reachable from the program must carry a value type and every block must carry a
statement list. It runs whether or not errors were reported, because error paths
are exactly where the holes come from, and `test/run.py` now passes it on
`reject`, `recover` and `warn` scenarios as well as `compile` and `run` — a
scenario that provokes a diagnostic is the one that exercises this, and a clean
compile is the one that cannot. A violation is `ErrorBadTree`, whose message says
"Compiler defect" because it accuses the compiler rather than the program.

The walk does not descend into type declarations. A type may refer to itself
through a reference, so that graph has cycles, and following them would need
visited-set bookkeeping to buy nothing.

On CLAUDE.md's "Keep type safety explicit; do not hide invalid IR states with
unchecked casts or placeholder values": the rule targets silent papering-over.
A named, tested, first-class error marker is the opposite of hiding, and is what
satisfies the rule's intent.

## Related

`ErrorGenErr` is **coverable**, and the reason recorded here for its not being so
was wrong twice over. It said the only construct raising it is a nested
allocation, which kills the compiler afterwards; nested allocation is fixed and
`region-nested-alloc` is a passing `run` scenario. But `ErrorGenErr` is not a
construct's diagnostic at all — every site that raises it is an LLVM
infrastructure failure: an unusable target triple, a module that fails
verification, an output file that cannot be written. The first of those is
reachable from the command line, `conec --triple bogus-triple-xyz prog.cone`,
which exits `ExitOpts`. That belongs to the `driver` group, which follows no
manual chapter because it tests the command line rather than the language. It is
left for whoever next extends that group; it is not this item's.

The two diagnostics that remain uncovered for want of a construct are listed by
`python test/run.py --coverage`.
