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
`rc`, both written in Cone in the core package. A user can define a region the
same way — a struct declaring `is RegionRef`, whose `alloc`, `init`, `alias`,
`dealias` and `free` the compiler calls, and whose `alloc` may ask for the
value's **type record** (core's `TypeRecord`: its size, alignment and
finalizer) by taking one after the size — but no more of the protocol below is
built: no barriers, no traced references, no weak reference kind, no region
with global state, and no finalizing of a slice's elements when its region
frees it. Of the strategies that motivate the whole design, tracing GC
is unwritten, and the arena and the pool are written only as library values: the
`arena` package's `Arena`, a dynamic region allocated into by a call on the
value (`a.alloc(v)`), not by a `+` allocation through a region ref,
whose `alloc` is not handed the region value. It finalizes
each value through the finalizer in its type's record
(`mem.typeRecord[T]().finalize`) when it dies, newest first; nothing yet
pairs a reference with its arena's lifetime. Held in a local, an `Arena` is the
**scratch arena**: `alloc` returns a borrow of that local, so the borrow rules
are its whole safety, with their gaps (below, "Where each rule is enforced").
The `pool` package's `Pool[T]` is a generational pool: `add` returns a
`Ref[T]`, a slot's index and generation, which owns nothing and is checked
against the slot each time it is used, through the pool value — `get` answers
an `Option` of a borrow, `None` once the value is removed. The borrow `pool[r]`
returns is checked like the scratch arena's; the one inside `get`'s `Option`
is not (a borrow held inside another value carries no lifetime), and no
invariant lifetime yet pairs a `Ref` with its own pool, so one used with
another pool of its type is merely bounds- and generation-checked there.
A third region ref is a library package too: `rcweak`'s `rcw`, reference
counting whose header also counts weak references. A weak reference is no
reference kind but an ordinary struct, `Weak[T]`, holding the header's address
and reaching the value only by making a counted `+rcw-mut` owner, in an
`Option` (`upgrade`) or checked first (`alive`, `strong`). Its value dies at
its last strong owner, and its memory is freed at its last weak reference:
death and freeing separate, with the compiler told nothing new. The owner an
`upgrade` hands back in a `Some` is released when the `Option` goes, as what any
enum holds is.

The argument is in *Memory Managed Your Way* (`conesite/public/memory.html`) and
`c:/src/progling/content/post/gradual-memory-management.md`. The origin is
concrete: a 17ms frame budget for realtime 3D.

**The division of labour is one sentence**: *regions and lifetimes ensure
memory safety; permissions ensure data race safety.* Mechanically that is
**three independent axes on one reference type**: a region, a permission, and a
value type. This note is what each means, which
combinations are legal, and what each infects.
[references](../../compiler/c/doc/nodes/references.md) is the node that carries it,
[Flow Analysis](../../compiler/c/doc/phases/flow.md) is what enforces the parts that are enforced,
and [Generation](../../compiler/c/doc/phases/generation.md) is what it lowers to.

The reference manual gives this subject nine chapters. Read this first if you
want the shape before the detail.

*Provenance: read from source; the lowerings and the permission table were
measured.*

## Vocabulary — [Jon 26 Sep]

- **Region**: the part of memory managed by that region's management
  protocols. **Every point in memory is managed by exactly one region.** The
  **global region** is memory the linker sets up; the **local region** is a
  thread's stack, one per thread; every other region is memory owned and
  managed by the region that allocated it.
- **Regions nest.** Uniqueness holds one level at a time: each piece of memory
  has exactly one *immediate* manager, and managers stack — an `rc` allocation
  holds a `List`'s block, and inside that block the `List` manages its
  elements.
- **Static region**: its state and its lifetime are global; a module manages
  the state (a tracing collector's heap, say).
- **Dynamic region**: its state is a value, allocated dynamically on the heap
  or held locally (a thread's stack is itself carved from free memory), and the
  region lives as long as that value does — an arena, a pool, a collection.
- **Region ref**: the struct declaring the built-in trait `RegionRef` — the
  header a region puts in front of each value it manages, whose methods the
  compiler calls at each reference event. It is what `+rc` names; the region is
  the module (or, for a dynamic region, the value) behind it. Never "a region is
  a struct".
- **The dance**: the region decides *when* a value dies; the compiler, which
  alone knows the value's type, runs the death — the value's `final`, then its
  fields' finalizers, then the owners its fields hold, then `free`.

## Principles — [derived]

⚠ **The premise above is the author's and is quoted. These four are read from
source**, and what is unchecked is the claim that they are ruling positions
rather than the present arrangement.

1. **Three axes, independently chosen.** `+rc-mut Point` names a region, a
   permission and a value type, and each is a separate decision. ▸ **Forbids**
   the fused capability of Pony or the welded aliasing-mutability-lifetime of
   Rust. **This is the principle most "Cone cannot express X" claims dissolve
   against**, and it is why an error at one axis is fixable at that axis —
   locality [Expressiveness and Attention](expressiveness-and-attention.md)
   depends on.
2. **A region ref is an ordinary struct**, not a compiler concept: one declaring the
   built-in trait `RegionRef`, whose methods the compiler calls at each
   reference event and whose absent methods say what it does. ▸ **Settles**
   that a new strategy is library work, not compiler work.
3. **A permission is a set of capability bits**, not a keyword the compiler
   special-cases. What a permission permits is data. ▸ **Forbids** a fixed
   permission vocabulary, and **settles** that adding one is a table entry.
4. **Move-ness is derived, never declared, on a reference.** It falls out of the
   permission and the region. ▸ **Forbids** a `move` annotation on a reference
   type, and **settles** that move semantics can never disagree with the axes
   that produced them.

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
| `so` | `struct so is RegionRef, Move` in the core package, `packages/core/src/core.cone`: no fields, `alloc` and `free`, no `alias` | single owner frees |
| `rc` | `struct rc is RegionRef { cnt usize }` in the core package, with `init`, `alias` and `dealias` too | reference counting |
| `rcw` | `struct rcw is RegionRef { strong usize; weak usize }` in the `rcweak` package, `packages/rcweak/src/rcweak.cone`; its `free` gives back one weak count, and the last one frees | reference counting with weak references (`Weak[T]`, a struct) |
| user-defined | any struct declaring `is RegionRef` | whatever its methods do |

**`so` and `rc` are Cone source, not built into the compiler**, and nothing in
the compiler names either. Each method is optional, and an absent one's
operation does not happen: without `alias` a copy of a reference calls nothing;
`dealias` answers whether the owner that went was the last, and without it an
owner's going asks nothing; `free` gives the memory back. **One owner per value
is declared, not read off a missing method** [Jon 26 Sep]: a region ref
declaring the built-in trait `Move`, as `so` does, has its copies moved and
every owner's going is the value's death. One declaring neither `Move` nor
`alias` shares its references for nothing, and with no `dealias` either its
owners' going does nothing at all — the compiler never frees such a value, the
region owning death in its own loop: a tracing collector's or an arena's shape. **The region decides when a value dies; the compiler runs the death**,
because only it knows the value's type: the value's finalizer (its `final`,
then its finalizing fields'), then the owners its fields hold, then `free` —
the order a value on the stack is finalized in. A value moved out through its
sole owner leaves that owner **hollow**: its memory is still freed, but nothing
that moved is finalized there. The whole value moves, never a field of it:
nothing moves out of a field, so no value dies with a hole in it
(`doc/reference/refmove.html`). The compiler checks the methods' shapes where the struct is declared,
refusing only `Move` with `alias`, which contradicts itself; and the test corpus
declares regions of its own that get every call `rc` and `so` get
([What a region is](../../compiler/c/doc/nodes/module.md)).

**But the intended shape is much larger than that.** A region is meant to be a
*module* containing the region annotation type, the region's global state, and
its API — where the annotation is "effectively a special-purpose trait"
declaring bookkeeping fields plus a protocol of methods: `alloc`, `init`,
`_alias`, `_dealias`, `_free`, `_readBarrier`/`_writeBarrier`, `isAlive`,
`weak`, `drop` — and attributes such as `@move` and `traced` that the compiler
keys off. ⚠ **[differs: of that protocol `alloc`, `init`, `alias`, `dealias` and
`free` are built, spelled without the underscore; the annotation is a struct,
not a module; and what the compiler keys off is a trait, not an attribute:
`Move`, the one built so far]** A trait is a fact other code may ask about or
constrain on; an attribute is an instruction about representation or linkage
that nothing asks about [Jon 26 Sep], and a region ref's capabilities are the
first kind.

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

**Two different things share the word, and confusing them is easy.** A reference
*type* carries a permission, and so does a *declaration* — a field, a local, a
parameter or a global. They are written in different places, parsed by different
functions, admit different vocabularies, and answer different questions:

| | reference permission | declaration permission |
| --- | --- | --- |
| Written | inside a type: `&mut Point`, `+rc-ro T` | before a name: `mut x i32` |
| Parsed by | `parsePerm`, inside `parseType` | `parseDclPerm` |
| Vocabulary | all six below | `mut` and `imm`, nothing else |
| Answers | read, write, alias, share — and move-ness | may this storage's value change |
| Default | `ro` borrowed, `uni` owning, `opaq` for a function | `imm` on a variable, `mut` on a field |

**Why they differ is the reason the rest of this note exists.** A declaration
names one storage location that only it owns, so the aliasing and sharing
distinctions have nothing to bite on: what is left to choose is whether the value
may change. A reference is by definition a *second* way to reach something, which
is where the other axes start to matter.

Two consequences worth knowing:

- **A declaration permission is not part of a function's signature.**
  `fnSigMatches` compares each parameter's *type* and nothing else, so
  `fn f(mut n i32)` and `fn f(n i32)` are one signature and a caller cannot tell
  them apart. What a caller does see is the permission on a reference
  parameter's *type* — a different thing in the same sentence, which is what
  makes a parameter's own permission as private to the body as a local's.
- **They compose by taking the minimum, for the one bit that is enforced.**
  `iexpGetLvalInfo` walks an lval to its variable, adopting a reference's
  permission wherever one is crossed and downgrading on a field's. So a `mut`
  field is not writable through an `imm` value or a `&` reference, and an `imm`
  field is not writable through a `&mut` one. That is the write half of what
  `doc/reference/refstruct.html` calls *viewpoint adaptation*, and it is the half that
  exists; adaptation over the sharing bits is unimplemented because those bits
  are read nowhere.

The six below are the reference vocabulary. A declaration uses two of them.

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

**Only three of the seven bits are consulted today.** `MayWrite` gates assignment,
swap and a field write; `MayRead` gates a read through a reference — a
dereference, an index, or a field of a virtual reference — and feeds the
variance rule below; `MayAlias` decides move-ness. `MayAliasWrite`,
`RaceSafe`, `MayIntRefSum` and `IsLockless` are populated and read nowhere. That
is not a judgement on the design — it is that the concurrency half is unbuilt,
and those are the bits it would consult. One consequence is worth stating
outright: **`imm` and `ro` are behaviourally identical today**, differing only in
`RaceSafe` and `MayIntRefSum`. See [Safety](safety.md).

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
type**. `so` declares `is Move`, which makes it one, so every `+so` reference moves; `+rc` with the
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

**Covariance does not turn a move type behind a reference into a copy type.**
A read through the reference copies what it holds when that is a copy type, so
a borrow of a sole owner may not be seen as a borrow of a shared one:
`&+rc-mut T` from `&+rc T` would copy a second, writable owner out of a value
`uni` promised unique. The permission lattice still lets `uni` coerce down
where the owner itself is moved. A `+so` owner may be seen as `+so-mut` behind
a borrow, since both move and a move out through a borrow is refused.

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
[Generation](../../compiler/c/doc/phases/generation.md), "The allocation header".

## Where each rule is enforced

Worth having in one place, because it is spread across three phases and one
gap:

| Rule | Enforced by | Phase |
| --- | --- | --- |
| region must be a struct declaring `is RegionRef` | `refRegionCheck` | type check |
| a region's methods have the shapes the compiler calls, and `Move` is not contradicted by an `alias` | `regionRefCheck`, at the declaration | type check |
| a region allocated from has `alloc` | `regionAllocTypeCheck` | type check |
| requested permission vs. the source's | `permMatches` in `borrowTypeCheck` | type check |
| value-type variance | `refMatches` and friends | type check |
| a sole owner behind a borrow is not seen as a shared one | `refHeldMoveSeenAsCopy`, from `refMatches` and `arrayRefMatchesRef` | type check |
| region coercion direction | `regionMatches` | type check |
| move-ness infection | `refAdoptInfections` | type check |
| may not write through this reference | `assignlvalrtype`, `swapFlow` | **flow** |
| a moved-out value may not be used | `nameuseFlow` | **flow** |
| a borrow may not outlive what it points at | `assignlvalrtype` and `swapFlow` (one check, `assignBorrowLifetimeCheck`), `returnFlowEscape`, `fnCallFlowStoredBorrow` | **flow**, at three sites only |
| a call's returned borrow lives as long as the narrowest borrow it was handed | `fnCallFinalizeArgs`, on a reference node of the call's own — or on the borrowed elements of a tuple of its own, where the call returns several values | type check |
| aliasing of borrows | — | **nowhere** |
| freezing a borrow's source | — | **nowhere** |

## Hazards

- **Owning and borrowed references are the same machine value.** Nothing at
  runtime distinguishes them; everything that does is erased before generation.
- **`uni` is not `&mut`.** Reaching for it as "the mutable one" gets move
  semantics you did not ask for.
- **`&mut T` is invariant.** Coming from a language where mutability implies
  more permissive subtyping, this is backwards.
- **A borrow's lifetime is checked at three sites only.** Storing (by
  assignment, or by a swap in either direction), returning, and passing one beside a `&mut &T` argument. Capturing it, storing it in a
  field, or laundering it through a variable are all unchecked — see
  [Safety](safety.md).
- **The permission on a reference is not the permission on the binding.**
  `imm fixed = &mut target` is a writable target through an unrebindable name.
- **`&[]x` on a non-array is legal** and yields a one-element slice.

## What lives elsewhere

- The node that carries all this: [references](../../compiler/c/doc/nodes/references.md)
- Moves, counting and release: [Flow Analysis](../../compiler/c/doc/phases/flow.md)
- Layout, fat pointers and the allocation header: [Generation](../../compiler/c/doc/phases/generation.md)
- What is promised versus what is checked: [Safety](safety.md)
