**The premise is that there is no perfect memory management strategy.** Tracing
GC and reference counting are flexible and nearly invisible, and are usually the
slowest in throughput and the least predictable in responsiveness. Arena and
pool can be 10-20x faster. Single-owner is often faster still. Each trades
throughput, responsiveness, safety, convenience, memory consumption, runtime
size and type restrictions differently — so **a language that picks one strategy
for you fits some programs and misfits others.**

**The aim** — the author's term for it is **gradual memory management** — is to
let the programmer choose *per object*, and to **mix strategies within one
program**: high-performance strategies where the data structures are amenable,
slower and more flexible ones to fill the gaps. Each region is an importable
library package implementing one strategy, named at the allocation site, with
borrowed references used to shed the overhead wherever region oversight is not
needed. Safety is preserved across all of it.

**The distance** is large and worth stating plainly. Two regions ship, `so` and
`rc`, both written as Cone text inside the compiler. **There is no way for a
user to define a region**, no `region` keyword, and none of the protocol below
beyond `_alloc` and `init`. The strategies that motivate the whole design —
arena, pool, tracing GC — are unwritten.

The argument is in *Memory Managed Your Way* (`conesite/public/memory.html`) and
`ProgLing/plingsite/content/post/gradual-memory-management.md`. The origin is
concrete: a 17ms frame budget for realtime 3D.

**The division of labour is one sentence**: *regions and lifetimes ensure
memory safety; permissions ensure data race safety.* Mechanically that is
**three independent axes on one reference type**: a region, a permission, and a
value type. This note is what each means, which
combinations are legal, and what each infects.
[references](../nodes/references.md) is the node that carries it,
[Flow Analysis](../phases/flow.md) is what enforces the parts that are enforced,
and [Generation](../phases/generation.md) is what it lowers to.

The reference manual gives this subject nine chapters. Read this first if you
want the shape before the detail.

*Provenance: read from source; the lowerings and the permission table were
measured.*

## Key principles

1. **Three axes, independently chosen.** `+rc-mut Point` names a region, a
   permission and a value type, and each is a separate decision.
2. **A region is an ordinary struct**, not a compiler concept. Anything with a
   suitable `_alloc` is one.
3. **A permission is a set of capability bits**, not a keyword the compiler
   special-cases. What a permission permits is data.
4. **Move-ness is derived, never declared, on a reference.** It falls out of the
   permission and the region.

## The axes

### Region — which memory management strategy this object uses

A region is **a library package implementing a strategy**, not a compiler
concept. It is named at each allocation site, so the choice is per-object and
lexically visible:

```cone
imm person = +so Person["Tako"]     // single-owner: freed when the owner drops
imm shared = +rc Person["Tako"]     // counted: freed at zero
```

| Region | Is | Strategy |
| --- | --- | --- |
| `borrowRef` | a sentinel node, not a struct — the default for `&` | none; a borrow owns nothing |
| `so` | `struct @move so` in `corelibSource`, no fields | single owner frees |
| `rc` | `struct rc { cnt usize }` in `corelibSource` | reference counting |
| user-defined | any struct with `_alloc(usize) *u8` and an optional `init()` | whatever it implements |

**`so` and `rc` are Cone source, not built into the compiler.**
`regionAllocTypeCheck` validates the `_alloc` signature and nothing else, so a
third struct with an `_alloc` is declarable today and the test corpus declares
one.

**But the intended shape is much larger than that.** A region is meant to be a
*module* containing the region annotation type, the region's global state, and
its API — where the annotation is "effectively a special-purpose trait"
declaring bookkeeping fields plus a protocol of methods: `_alloc`, `_init`,
`_alias`, `_dealias`, `_free`, `_readBarrier`/`_writeBarrier`, `isAlive`,
`weak`, `drop` — and attributes such as `@move` and `traced` that the compiler
keys off.

**The compiler's intended role is choreography, not ownership**: "it is the
compiler's job to choreograph how operations on references invoke
programmer-defined methods in the regions and permissions they specify." A
reading of the source that treats region handling as a fixed compiler feature —
or as an optimization the compiler chooses — inverts the design. **The
programmer picks the strategy; the compiler makes it safe and cheap.**

**Borrowed references are meant to be the common case, and they are how the
overhead comes back off.** "The more your program uses borrowed references,
instead of region references, the faster and more flexible it becomes." They are
also what makes a library region-polymorphic: a borrow has *forgotten* which
region its object came from, so it can be passed to any package. A region-managed
reference costs whatever its strategy costs; converting most uses to borrows
removes that cost, and because a borrow can point *into* a larger structure it
also enables the inlined, cache-friendly layouts that motivate choosing a
strategy in the first place. That is why region coercion is one-directional:
anything coerces **to** `borrowRef`, and never between two owning regions.

### Permission — what may be done through this reference

Six permissions, each a bit set:

| | `MayRead` | `MayWrite` | `MayAlias` | `MayAliasWrite` | `RaceSafe` | `MayIntRefSum` | `IsLockless` |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `uni` | ● | ● | | | ● | ● | ● |
| `mut` | ● | ● | ● | ● | | | ● |
| `imm` | ● | | ● | | ● | ● | ● |
| `ro` | ● | | ● | | | | ● |
| `mut1` | ● | ● | ● | | | ● | ● |
| `opaq` | | | | | | | |

**`uni` is the interesting one, and it is not `&mut`.** It permits reading and
writing but **not aliasing** — and lacking `MayAlias` is precisely what makes a
reference carrying it a move type. It is the default for `+region` allocation,
which is why owning references move by default.

`opaq` permits nothing, which makes it the safe default where the value type is
not concrete.

**That table is the implementation's vocabulary, and it is not the author's.**
The design writing names `uni`, `imm`, `mut`, `mutex`, `mutex1`, `atomic`,
`const` and `opaq` — where `const` is what the code calls `ro`, and the whole
runtime *lock* permission family (`mutex`, `mutex1`, `atomic`) is unimplemented.
Do not assume `mut1` in the code is the writing's `mutex1`: it carries
`IsLockless`, so it is probably not. **Say which vocabulary you are using.**

The permissions are meant to be the *race-safe strategies* — the programmer
picks a reference's constraint by annotating it. `uni` is described as "the
universal donor", a solo mutable traveller able to move between scopes and
threads; `opaq` as the universal receiver. Transitions are irreversible under a
move, but **temporary and reversible when done by borrowing** — which is how a
`uni` reference is recovered after being lent out.

**Only two of the seven bits are consulted today.** `MayWrite` gates assignment
and swap; `MayAlias` decides move-ness. `MayRead` is never used as an access
check, and `MayAliasWrite`, `RaceSafe`, `MayIntRefSum` and `IsLockless` are
populated and read nowhere. See [Safety](safety.md).

The coercion lattice (`permMatches`) is small: `uni` coerces down to `ro`,
`mut`, `imm` or `mut1`; anything readable coerces up to `ro`; `opaq` accepts
everything. It returns only "yes" or "no" — there is no partial ordering with a
conversion.

### Value type — what is pointed at

Ordinary, with one consequence worth stating: **a reference answers its own size
and never consults its target**, which is what makes recursive types
expressible. `struct S { next &S }` is legal because `&S` never asks `S`
anything.

## What composes out of the axes

**Move-ness.** `refAdoptInfections` is the whole rule: a reference is a move
type **when its permission lacks `MayAlias`, or its region is itself a move
type**. `so` is `struct @move`, so every `+so` reference moves; `+rc` with the
default `uni` moves too, on a region that counts. That one sentence explains why
`+rc x` moves while `+rc-mut x` copies.

**Variance is keyed on the permission**, not on the reference kind:

| Target permission | Variance in the value type |
| --- | --- |
| `MayRead` only (`ro`, `imm`) | covariant |
| neither bit (`opaq`) | covariant |
| `MayWrite` only | contravariant |
| both (`mut`, `uni`, `mut1`) | **invariant** |

So `&mut T` is invariant in `T` and `&ro T` is covariant — the opposite of the
intuition that a mutable reference is "more capable" and therefore more
permissive.

**Lifetime is a `uint16_t` scope depth** on the reference's *type*: 0 global, 1
parameter, 2+ a local. It is not a type parameter, not a constraint variable,
and not part of type identity — `refIsSame` ignores it and `refFindSuper` drops
it.

**That integer is a placeholder for a much larger design.** The intent is an
encoding of source variable, *invariance group*, and relative scope, forming a
partial order whose comparison can yield "no valid comparison is possible" —
built to carry first-class-region pairing as well as simple nesting. The
mechanism is credited to Cyclone's restricted-alias pointers plus the insight of
annotating a borrow with a *lifetime* rather than with an arena.

## The four kinds of reference, and what each is for

| Written | Kind | Runtime |
| --- | --- | --- |
| `&T` | borrowed — points at something someone else owns | `T*` |
| `+region T` | owning — the region releases it | `T*`, pointing **past** a header |
| `&[]T` | slice — a borrowed run of elements | `{T*, usize}` |
| `&<Trait` | virtual — dispatches through a vtable | `{i8*, Vtable*}` |

Two of the four are fat pointers, and **the first two are indistinguishable at
runtime** — region and permission are entirely compile-time. That is the single
fact most likely to mislead when reading generated IR.

An owning reference points at the payload of a `{region, perm, value}` header,
so the allocation base is *behind* the pointer. See
[Generation](../phases/generation.md), "The allocation header".

## Where each rule is enforced

Worth having in one place, because it is spread across three phases and one
gap:

| Rule | Enforced by | Phase |
| --- | --- | --- |
| region must be a struct with `_alloc` | `regionAllocTypeCheck` | type check |
| requested permission vs. the source's | `permMatches` in `borrowTypeCheck` | type check |
| value-type variance | `refMatches` and friends | type check |
| region coercion direction | `regionMatches` | type check |
| move-ness infection | `refAdoptInfections` | type check |
| may not write through this reference | `assignlvalrtype`, `swapFlow` | **flow** |
| a moved-out value may not be used | `nameuseFlow` | **flow** |
| a borrow may not outlive what it points at | `assignlvalrtype`, `returnFlowEscape` | **flow**, at two sites only |
| aliasing of borrows | — | **nowhere** |
| freezing a borrow's source | — | **nowhere** |

## Hazards

- **Owning and borrowed references are the same machine value.** Nothing at
  runtime distinguishes them; everything that does is erased before generation.
- **`uni` is not `&mut`.** Reaching for it as "the mutable one" gets move
  semantics you did not ask for.
- **`&mut T` is invariant.** Coming from a language where mutability implies
  more permissive subtyping, this is backwards.
- **A borrow's lifetime is checked at two sites only.** Storing and returning.
  Passing one as an argument, capturing it, or laundering it through a variable
  are all unchecked — see [Safety](safety.md).
- **The permission on a reference is not the permission on the binding.**
  `imm fixed = &mut target` is a writable target through an unrebindable name.
- **`&[]x` on a non-array is legal** and yields a one-element slice.

## What lives elsewhere

- The node that carries all this: [references](../nodes/references.md)
- Moves, counting and release: [Flow Analysis](../phases/flow.md)
- Layout, fat pointers and the allocation header: [Generation](../phases/generation.md)
- What is promised versus what is checked: [Safety](safety.md)
