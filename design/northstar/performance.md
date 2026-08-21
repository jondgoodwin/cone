**The aim is not "the compiler optimizes well".** Native compilation through
LLVM is stated as *necessary but not sufficient*. The stated top design goal is
to make it **easier for knowledgeable programmers to leverage high-performance
strategies** — because high-performance programs grow complex faster than CPUs
get faster, so the wins come from architecting for performance, not from the
back end being clever.

**The distance** is that the levers the design is built around are mostly not
built yet: arenas and pools do not exist, there is no thread layer, and the
array primitives for data-oriented layout are incomplete. What *is* built is the
machinery that makes those levers cheap to add and free to not use.

The argument is in *Optimal Performance* (`conesite/public/fast.html`).

## The strategies the language means to enable

These are the levers, in the author's framing — what a programmer reaches for,
not what the compiler does behind their back.

**Prevention, not profiling.** The stated philosophy inverts the usual
"premature optimization" advice: profile-and-fix works for hot spots, but
"struggles when inefficient code patterns are repetitively diffused throughout
the code base. Thousands of tiny paper cuts sprinkled in many different places
are likely to be largely 'invisible' to performance profilers." The remedy is
architectural — *Don't Pessimize Prematurely*. The four levers named are memory
management, data copying, indexing, and data-oriented design.

**Memory technique is where the orders of magnitude are.** Not instruction
selection, not inlining, not the pass list — how data is laid out and how it is
allocated. Everything below is a way of giving the programmer control over that,
and it is the reason the compiler's own optimization is treated as table stakes
rather than as the story.

**Cache locality.** Memory fetch is roughly 100x slower than L1, so layout
dominates. The language gives explicit control over the physical size, layout
and representation of objects, so data processed together is located together.
Concretely that means smaller objects — encoded data, and offsets in place of
full pointers — sequential rather than random collection access, and isolating
mutable data so read-only blocks can be shared. Borrowed references matter here
beyond safety: because a borrow can point *into* a larger structure, data can be
inlined rather than scattered behind indirections.

**Choosing a memory strategy per object.** Arena and pool allocation can be
10-20x faster than general-purpose allocate-and-free; single-owner is often
faster than either. A program using only ref-counting or tracing GC can improve
throughput by moving some allocations to a cheaper region. See
[References and Regions](references-and-regions.md).

**Demoting owning references to borrows.** Region-managed references carry
whatever their strategy costs; converting most uses to borrows removes that
overhead entirely — and enables the inlined layouts above.

**Latency, not just throughput.** Region choice is also how stop-the-world
pauses are avoided, which matters for the real-time and interactive programs the
language is aimed at.

**Concurrency as a performance vector.** Lock and synchronization costs are the
thing to minimize. Static permissions carry safety guarantees with **no runtime
lock cost**, so immutable and mutable data can be exchanged between threads
losslessly; actors communicate through FIFO queues, which synchronize more
cheaply than locks or concurrent data structures. None of this exists yet.

## What makes those levers affordable

Five choices in the language, each made so that a cost is either visible or
absent. This is the half that is built.

**1. Safety is a compile-time argument, so it has no runtime.** Permissions,
regions, lifetimes and move-ness are checked and then discarded. This is the
central bet: the whole apparatus that makes Cone safe leaves *no trace* in the
emitted code. A `&T` and a `+rc T` are the same machine value; a permission
lowers to a zero-field struct.

**2. Nothing allocates unless you write an allocation.** There is no garbage
collector, no hidden boxing, no implicit copy of a large value. A region is
named at every allocation site — `+rc`, `+so` — so allocation is a lexical
event, not an inference.

**3. Ownership is declared, so release is static.** Because a region and a
permission say who owns a value, the compiler can decide *at compile time*
where each release goes and emit it as ordinary code. There is no runtime
ownership metadata, no drop flags, no unwinding.

**4. Generics are monomorphized.** A generic is a template; each distinct type
argument produces a separate function with concrete types. No boxing, no vtable,
no type erasure — at the cost of code size.

**5. Abstraction is opt-in at the point of use.** Dynamic dispatch happens when
you write `&<Trait`, and only then. A trait used statically costs nothing; the
fat pointer and the indirect call appear exactly where the source asks for
them.

## What is free, and why the language can promise it

| Construct | Free because |
| --- | --- |
| **permissions** | bet 1 — erased; `%void = type {}`, zero bytes in the header, nothing at a use site |
| **regions**, as a property of a reference | bet 1 — `&T` and `+rc T` are both `T*` |
| **lifetimes** | bet 1 — a compile-time scope depth, never emitted |
| **move semantics** | bet 1 — a type flag; moving is not a runtime operation |
| **a trait used statically** | bet 5 — a direct call; default methods are cloned into the implementer |
| **a two-variant union with a pointer payload** | no struct is emitted at all — the value *is* the pointer, null is the empty variant |
| **zero-size types** | `void` and an empty struct are `%void = {}` |
| **`inline` functions** | inlined by the generator itself — no call, no symbol. This is how a region's `_alloc` becomes a direct `malloc` at each allocation site |

The first four are one claim restated: **the static safety apparatus is
free.** That is the bet the language is making, and it is the one worth
checking if it ever stops being true.

## What costs, and where the source shows it

| Construct | Cost | Visible as |
| --- | --- | --- |
| **`+rc` reference** | one `usize` in the header; an increment per new holder, a decrement and zero-test per release | the `+rc` at the allocation |
| **`+so` reference** | no header bytes; a `free` at release | the `+so` |
| **slice `&[]T`** | two words, passed by value | the `[]` |
| **virtual reference `&<Trait`** | two words; an indirect call through a loaded slot | the `<` |
| **array or slice index** | a compare and branch per dimension | the `[i]` |
| **raw pointer index** | nothing — unchecked, deliberately | the `*` |
| **same-size union** | every variant padded to the largest | the `union` keyword |
| **generic instantiation** | code size — one function per type argument | the `[T]` |

**Every row has a mark in the source.** That is bet 2 and bet 5 doing their
work: there is no construct in the table whose cost is invisible at the point
you write it.

The two that surprise people are the fat pointers — a slice and a virtual
reference are twice the size of a plain reference and are passed by value, so a
function taking `&[]T` moves two words per call.

## How the compiler cashes this in

Each of these is possible *because* of a language bet, not despite it.

**Erasure lets reference types share machine representations.** Because
permissions have no runtime footprint, the type table interns on "identical
machine representation" — so `&mut i32`, `&ro i32` and `&uni i32` collapse to
one entry with one LLVM type and one allocation-header struct. The safety
distinctions cost nothing to carry because they are gone by then.

**Static ownership lets release be ordinary code.** Flow analysis decides at
compile time which variables are released where and writes it as a list of IR
nodes; generation replays it. There is no runtime drop machinery to be fast or
slow — the question does not arise.

**Monomorphization lets generic code optimize like hand-written code.** An
instance has concrete types, so LLVM inlines and specializes it the same way it
would a monomorphic function. The compiler memoizes instances so each distinct
type argument is built once.

**Explicit allocation lets the allocator inline.** `_alloc` is an ordinary
`inline` method on a region struct, so the generator splices it in and the
emitted code calls `malloc` directly with a constant size.

**Values are memory-backed, then promoted.** Every local and parameter is an
alloca with a store — because Cone lets you assign to a parameter and borrow
from it, so it needs an address. Generation puts every alloca in the entry block
precisely so `PromoteMemoryToRegister` can undo the ones that did not need it.
**Unoptimized IR therefore looks far worse than the result**; read `.ir`, not
`.preir`, when judging cost.

## What is not optimized today

Stated so nobody assumes otherwise:

- **A string literal emits a fresh global per occurrence.** No interning, and
  constant merging is not in the pass list.
- **An array fill literal is unrolled**, except on the region-allocated path.
- **Bounds checks are not elided** by the front end; whatever LLVM proves is
  what goes.
- **The pass list is short** — mem2reg, reassociate, GVN, CFG simplification,
  and function inlining. The compiler is not trying to out-optimize LLVM, only
  to hand it IR it can optimize.

## Hazards

- **Judging cost from `.preir` is misleading** — the alloca-everything strategy
  dominates it, and mem2reg removes most of it.
- **"It compiled to a pointer" does not mean it is a borrow.** Owning and
  borrowed references are the same machine value; only the emitted release code
  tells them apart.
- **A permission that is free at runtime is not free in expressiveness.** `uni`
  costs nothing to emit and changes move semantics.
- **Monomorphization is code size**, and a generic instantiated at ten types is
  ten functions.

## What lives elsewhere

- Strategy choice, and what a region is for: [References and Regions](references-and-regions.md)
- Why permissions and regions can be erased: [References and Regions](references-and-regions.md)
- Which safety properties actually hold: [Safety](safety.md)
- The exact lowerings and the allocation header: [Generation](../phases/generation.md)
- How fast `conec` itself runs: [Compiler Performance](../compiler/performance.md)
