How the compiler stays fast, and which of its design choices exist for that
reason.

This is about `conec`'s own speed. What a *Cone program* costs at runtime is
[the language's cost model](../northstar/performance.md).

*Provenance: read from source. The timer instrumentation is the way to measure
any claim here.*

These are **stated design commitments with measurements behind them**, not
accidents — made following the same "don't pessimize prematurely" argument the
language itself is built on. The measured result: the front end runs at roughly
250 Kloc/sec, and **LLVM accounts for about 99.3% of total compile time**. A
refactor toward a constraint solver, an immutable IR, or generic-based analysis
layers would contradict a written commitment, so make the case before making the
change.

## Key principles

0. **No solvers, and no abstraction layers in semantic analysis.** Type
   inference and borrow checking use cascading conditionals; semantic analysis
   traverses the full IR only three times, and none of it runs through core
   library templates or generics.
1. **Allocate and never free.** The process is short-lived, so the arena trades
   memory for the absence of ownership bookkeeping.
2. **Compare pointers, not contents.** Names and reference types are interned so
   that equality is an address comparison.
3. **Do each piece of work once, and remember it.** Declarations, generic
   instances and LLVM types are all memoized.
4. **Do only the work demanded.** Type check is demand-driven; an unreferenced
   declaration is still visited, but nothing is analyzed twice.

## The arena

`memAllocBlk` is a bump allocator that **never frees and never zeroes**. There
is no `free`, no reference counting of compiler structures, and no destructor
anywhere in the front end.

That is the right trade for a compiler: the process exits when the work is done,
so returning memory buys nothing, and not tracking ownership removes an entire
category of code and bug from every node constructor.

**It has one cost, and it is paid repeatedly.** Because nothing is zeroed, a
field a constructor forgets holds arena garbage rather than NULL — which reads
as a plausible pointer rather than crashing at address zero. Several defects
have had exactly this shape. The discipline is in
[IR Nodes](../nodes/_index.md): a constructor initializes every field it
declares.

## Interning

**Names.** `nametblFind` returns one immovable `Name*` per unique string, in a
djb2-hashed open-addressed table that doubles at 50% load. After the lexer,
**string comparison never happens again** — every name equality in the compiler
is a pointer comparison, including the unqualified name lookup, which is a
single dereference of `Name.node`.

The same table doubles as the scoping mechanism: `Name.node` is the current
binding, so entering a scope plugs values in and leaving restores them. There is
no scope chain to walk. See [Name Resolution](../phases/name-resolution.md).

**Types.** `typetblFind` interns reference types, keyed on `itypeIsRunSame` —
"identical machine representation" rather than identical spelling. Since
permissions are erased at runtime, `&mut i32`, `&ro i32` and `&uni i32` collapse
to **one** entry sharing one LLVM type and one allocation-header struct.

`refHash` and `arrayRefHash` hash `vtexp`, the type pointed at, which is the
field `refIsSame` compares — so the table spreads. Hashing `vtype` instead would
put every `RefTag` in one bucket, since `vtype` is permanently `unknownType` on a
reference type node; linear probing would keep the lookup correct and turn it
into a scan.

## Memoization

| What | Keyed on | Where |
| --- | --- | --- |
| a declaration's analysis | the `TypeChecked` mark | `inodeTypeCheck` |
| a generic instance | the type arguments, compared with `itypeIsSame` | `genericMemoize` |
| a named type's LLVM type | the `llvmtype` field | `genlType` |
| a reference type's LLVM type | the interned `typeinfo` | `genlType` |

**The `TypeChecked` mark is not primarily an optimization** — type check lowers
and replaces nodes, so a second walk corrupts the declaration. It is a
correctness requirement that happens also to make the work linear.

**Generic memoization is registered before the instance is checked**, which is
what lets a generic that recurses at the same type arguments terminate: the
inner call hits the half-built instance rather than cloning again.

## Measuring it

`shared/timer.h` defines the phases the compiler times itself on: `LoadTimer`,
`LexTimer`, `ParseTimer`, `SemTimer`, `GenTimer`, `VerifyTimer`, `OptTimer`,
`CodeGenTimer`. That split is the first place to look — it separates the front
end from LLVM's own optimization and code generation, which usually dominate.

For anything finer, instrument and compile the corpus:
[Measuring](../diagnostics/measuring.md).

## What is not optimized, and deliberately

- **No incremental compilation.** Every compile is from scratch; the memo tables
  live and die with the process.
- **No parallelism.** The demand-driven walk is inherently sequential, and the
  global name-table hook stack could not survive concurrent walks.
- **`ir.h` aggregates every node header**, so touching one rebuilds everything.
  Accepted in exchange for not maintaining an include graph.
- **The LLVM pass list is short** — the compiler is not trying to out-optimize
  LLVM, only to hand it IR it can optimize.

## Hazards

- **The arena's silence is the hazard.** An uninitialized field does not crash;
  it reads as a plausible value.
- **Interning means shared mutable structure.** Two spellings of a
  runtime-identical reference type share one `RefTypeInfo`. Writing through one
  writes through both — which is intended, and is why that struct holds only
  LLVM handles.
- **A memo hit returns a node still under construction** in the generic case.
  That is load-bearing for recursion, and it means an instance may be observed
  before it is fully checked.
- **`--verify` and `--checktree` both cost time** and are off by default.
  Neither is a reason to skip them when changing generation.

## What lives elsewhere

- What a Cone construct costs at runtime: [Performance](../northstar/performance.md)
- How the source is organized: [Architecture](architecture.md)
- The constructor discipline the arena demands: [IR Nodes](../nodes/_index.md)
- Demand-driven scheduling and the marks: [Type Check](../phases/type-check.md)
