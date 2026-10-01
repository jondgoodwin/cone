`RefNode` is one struct serving **seven tags** across two node groups — four
type nodes and four expression nodes (`RefTag` appears in both roles at
different times). Getting the family right is most of understanding Cone's
memory model.

**At a glance.** The parser builds a reference-shaped node for `&`, `&[]`, `&<`
and `+`, `+[]`, `+<` without knowing whether it is a type or a constructor.
Name resolution decides by asking whether the operand is a type. A managed
reference *type*, `Rc[mut, Node]`, is not built by the parser at all: it is a
bracketed call until type check lowers it into this node ("The managed
reference type", below). Type check builds the *result* type, records a
lifetime, and interns. Flow moves or copies an allocation's value and enforces the
lifetime at three consumers. Generation lowers a plain reference to a bare
pointer and two others to fat pointers.

*Provenance: read from source; the LLVM shapes and the allocation header were
measured against emitted IR.*

## Shape

| Field | Meaning |
| --- | --- |
| `region` | `borrowRef` singleton, or a region struct |
| `perm` | permission — usually a `NameUseNode` wrapper built by `newPermUseNode`, not the bare `PermNode`. See Hazards |
| `vtexp` | **the pointed-at type** on a type node; **the value expression** on an expression node |
| `vtype` | unused on a type node; the **constructed reference type** on an expression node |
| `typeinfo` | interned `RefTypeInfo` — the LLVM handles. **Interned by what the reference refers to**, so reference types that agree at run time share one record and generation memoizes the LLVM type on it |
| `scope` | lifetime: 0 global, 1 parameter, 2+ local |

⚠ **A cloned reference re-interns, in `cloneRefNode`.** Cloning is how a trait's
method becomes an implementer's and a generic's becomes an instance's, and both
repoint `Self` — so the copy refers to something the original did not, while the
`memcpy` brings the original's `typeinfo` across. The copy also carries the
original's `TypeChecked` mark, so `refTypeCheck` never revisits it to normalize.
Sharing the record made the memoized LLVM type answer for the original's pointee:
whichever implementer generated first named it for all of them, so a trait
default's clones took one another's receiver and `fn f(m &Trait)` took an
implementer's type. `trait_inherited_defaults` pins both.

**There is no lifetime field.** `LifetimeNode` exists in `ir/types/lifetime.c`
and `lifeMatches` is called from nowhere; only `'static` is ever built. Lifetime
is entirely that `uint16_t`.

### The seven tags

| Tag | Group | `vtexp` | `vtype` |
| --- | --- | --- | --- |
| `RefTag` | type | pointed-at type | unused |
| `VirtRefTag` | type | trait or struct | unused |
| `ArrayRefTag` | type | **element** type | unused |
| `ArrayDerefTag` | type | element type | unused |
| `BorrowTag` | exp | the lval | built `RefTag` node |
| `ArrayBorrowTag` | exp | the lval | built `ArrayRefTag` node |
| `AllocateTag` | exp | initial value | built `RefTag`, or an `Option` call under `FlagQues` |
| `ArrayAllocTag` | exp | initial value | built `ArrayRefTag` node |

The group is the discriminator: type tags are `TypeGroup`, constructor tags
are `ExpGroup`. **The `scope` that matters is the one on a borrow's
*result* type**; the expression node's own is never read.

`RefTypeInfo` holds three LLVM handles: the reference's own type, the
`{region, perm, value}` allocation header, and a pointer to it.

## Constructors and interning

`newRefNode` seeds `borrowRef`, `roPerm`, `scope = 0` — and the comment says why
the zero matters: left uninitialized, the lifetime checks read allocator
garbage. `newRefNodeFull` additionally sets the three fields and runs
`refAdoptInfections`. `borrowMutRef` and `borrowAuto` build the pair by hand
for a borrow the compiler injects — an operator's `&mut` receiver, a folded
method's field, an array coerced to a slice argument — and record the lval's
scope on it, since the node they build is never type checked; `borrowMutRef`
short-circuits `&*p` to `p`.

**Interning collapses permissions.** `refTypeCheck` and `arrayRefTypeCheck` end
with `typetblFind`, whose equality test is `itypeIsRunSame` — "same *machine*
representation", which differs from `itypeIsSame` in one arm: `case PermTag:
return 1`. So `&mut i32`, `&ro i32`, `&imm i32` and `&uni i32` share **one**
entry. That is correct because a permission lowers to a zero-field struct and
occupies no bytes.

**Neither identity test compares `scope`**, so lifetime is not part of type
identity — and `refFindSuper` drops it entirely.

## Parse

`parseAmper` handles `&`, `&[]`, `&<`; `parsePlus` handles `+`, `+[]`, `+<`.
The differences are worth knowing:

- **`&` has no region syntax** — `region` stays at the `borrowRef` default.
  **`+` requires a region annotation** and errors without one.
- `&` leaves an absent permission as `unknownType`, deferred to type check.
  `+` defaults to `uni` **at parse time**.
- `&` has two escapes `+` does not: a `fn` operand, where the presence of a body
  decides closure-versus-signature; and a `,` or `)` operand, where `vtexp` is
  left `unknownType` for later `Self` inference in a parameter position.
- Both reach the **whole suffixed term**, at the same precedence every other
  prefix operator has: `&x.a` references the field, `&x[4]` the element. Binding
  only the prefixed term instead would make `&x.a` mean `(&x).a` — typed as the
  field but returning the field's address, which generation cannot catch.

`+` is the allocation, `+Rc-mut Node[1]`, and the owning array reference type,
`+[]So i32`. A single or virtual managed reference type is written
`Rc[mut, Node]`, below. `parsePlus` still builds a `RefTag` or `VirtRefTag`
type from `+Rc-mut Node` when the operand is a type, and marks it
`plusSpelled`; type check refuses it (`refRefusePlusType`,
`ErrorPlusRefType`), naming the bracket spelling. The one exception is a match
pattern's root, `case imm c +Rc-mut Circle`: `castPatternName` reads the root
at parse time, before anything knows `Rc` is a region rather than a generic
variant, so `castPatternMark` clears the mark there and the `+` spelling
stands until patterns are given their own.

## The managed reference type, `Rc[mut, Node]`

The region names the reference, and its brackets hold an optional permission,
then the value type: `Rc[Node]`, `Rc[mut, Node]`, `Gc[imm, Leaf]`. Left out,
the permission is `uni`. It is ordinary generic syntax, so it parses as an
`FnCallNode` flagged `FlagIndex` (`parseSuffix`), and it stays one until type
check lowers it into a `RefNode`. Everything from type check on sees only the
`RefNode`.

- **Parse.** A static permission is a keyword (`PermToken`), so it is no term;
  `parseIndexArg` takes one inside `[...]` as the `newPermUseNode` it names.
  What a permission may be an argument of is type check's to judge. A generic
  region needs no more: `parseSuffix` already reads a second bracket group,
  `R[A][mut, Node]`.
- **Name resolution.** `isTypeNode` must answer yes for it, or every
  type-or-value vote reads it as a value: `&Rc[Node]` would be a borrow,
  `?Rc[Node]` an error. `itypeIsManagedRefType` is that arm of `inodeIsType`:
  the head names a region, and every argument is a type (a permission is one)
  or, in a template, a type parameter still to be substituted. That last
  condition is what tells the type from the region's own literal: core's
  `Rc[1usize]` builds Rc's header, and its argument is a value. The head is a
  region when its `is` list names `RegionRef` **as written**
  (`regionStructWritesRegionRef`): the question is asked before the region's
  declaration need have been resolved, when `traits` is still empty and the
  list is where the parser left it, the first name in `basetrait` and the rest
  as mixin fields. A generic region's first bracket group is its own type
  arguments (`itypeManagedRefRegion`), so `Pooled[i32]` alone is the region and
  `Pooled[i32][mut, Node]` the reference. A head that is a type parameter,
  `R[mut, T]`, is a provisional type (`inodeIsProvisionalType`).
- **Type check.** `fnCallTypeCheck` lowers it first among the shapes it
  recognizes by syntax (`fnCallLowerManagedRef`), ahead of the struct-literal
  pass, which would otherwise take the head for a literal's struct. The value
  type, the last argument, is checked first, since whether it is an **open
  trait** (`TraitType` without `HasTagField`) decides the shape: `VirtRefTag`
  for one, `RefTag` for anything else. An enum is a trait to the compiler too,
  and a thin owning reference to one exists (`So[Option[...]]`), which is why
  the test is the open trait's. Then the `RefNode` replaces the call and is
  checked as any reference type is. The arguments are refused as
  `ErrorRefTypeArgs` (other than one or two, or a value where the type goes),
  `ErrorRefTypePerm` (a first of two that is no permission, or a last that
  is one); a permission given to a head that is no region is
  `ErrorPermNotRegion` (`fnCallRefusePermArg`), which also keeps a generic
  from taking a permission as a type argument. In the permission slot a struct
  stands as the unbuilt dynamic permissions do (`refThreadBinds`), which the
  traced region's rule on a permission taking room judges.
- **Before type check, elsewhere.** Three readers meet the unlowered form and
  see through it as they do through a `RefNode`, to the last argument: generic
  inference (`genericInferType`, which also captures a region type parameter
  from the argument's region), a field's fold (`foldSourceDcl`), and a
  global's fold peek (`modFoldPeek`, which peeks the head rather than resolving
  it).
- **Diagnostics** spell a managed reference as it is written, permission
  always given: `Rc[mut, Point]` (`itypeSpellCat`, `regionTracedTypeName`).

## Name resolution

`refNameRes` and `arrayRefNameRes` answer the one question the parser could
not — is `vtexp` a type or a value?

```c
if (!isTypeNode(node->vtexp)) {
    node->tag = node->region == (INode*)borrowRef ? BorrowTag : AllocateTag;
}
```

A value means this is a *constructor*, and the region picks which. The array
forms produce `ArrayBorrowTag`/`ArrayAllocTag`.

**In a generic's template the answer is provisional.** `&T` asks the question
of a use of a generic parameter, which is not a type (`nameUseGroup` puts
`GenVarDclTag` in the meta group), so the template holds a borrow. Templates
are never type checked, so nothing reads that; `cloneRefNode` decides again once
the operand is the type argument, flipping a borrow or allocate whose original
operand was not a type and whose clone is back to `RefTag`/`ArrayRefTag`.
`cloneStarNode` does the same for `*T`. The inner clone runs first, which is
what carries `&&T`.

**A virtual reference may not be borrowed or allocated** — `ErrorBadTerm`,
"Coerce from a regular ref." There is nothing to construct *from*: the fat
pointer's second word is a vtable, selected either by scanning the trait's
implementations for the concrete source struct or, from a reference to an enum, by
the runtime tag — which indexes the vtable list where the tag values are the
variants' positions in it, and compares against each variant's value where they are
not (see [Generation](../phases/generation.md), "Vtables"). Both need a source
*reference type*; neither is available from a bare lval.

Because the retag happens here, the four constructor tags have **no arms in
`inodeNameRes`** — they cannot exist before this point.

## Type check

**A reference type must name what it refers to.** `refTypeCheck` and
`arrayRefTypeCheck` each compare `vtexp` against `unknownType` **by pointer** —
`errorType` shares the tag and means "already reported" — then raise
`ErrorNoRefType` and assign `errorType` so nothing downstream reports it again.
`parseAmper` leaves `vtexp` unknown on purpose, because a reference in a
parameter position may have its pointee inferred, and a method's `Self`
inference (`parseFnSig`, or `parseFnSigSettle` for an anonymous function's) is
the only thing that ever fills one in. Every other spelling — `fn f(s &)`, `&&`,
`*&`, `&[]`, `(&, i32)`, `&fn(&) i32`, a non-`self` parameter inside a method — would otherwise
reach `genlType` still unknown and hand `LLVMPointerType` a NULL element type.
Every enclosing type reaches these two guards through `itypeTypeCheck` on its
own pointee, which is what makes two checks cover all of them.

### `borrowTypeCheck`

In order: re-associate `&v[i]` into `(&v)[i]` when the operand is an index, so a
type's own `` `&[]` `` method receives the borrowed receiver — but not when its
list is type arguments (`fnCallHasTypeArgs`): `&half[i64]` borrows the generic's
instance, and an index of `&half` by a type means nothing; check the operand;
**refuse a temporary** with its own message rather than "must be lval", because
every operand a borrow refuses is refused for that one reason; **refuse an
inline function** (`ErrorInlineRef`), which generation gives no symbol, so a
reference to it would point at nothing — the anonymous `&fn(…) inline {…}`
form arrives as a name use of the lifted declaration and is refused the same
way, and the reference type is still built so nothing downstream reports it
again; retag a
whole-value `&[]` that dispatches to a method so the checks below are the ones a
hand-written `&mut value` gets; auto-deref a suffixed borrow through a
reference; extract lval, permission and scope with `iexpGetLvalInfo`; infer the
value type; check the requested permission with `permMatches`; build the result
`RefNode` carrying `borrowRef` and the lval's scope.

Two details worth keeping: an unspecified permission becomes `ro` for a concrete
type and `opaq` otherwise; and `&[]` of a **non-array** is deliberately a
one-element slice.

### `allocateTypeCheck`

Default the permission to `uni`; check the value (an array allocation routes
through `arrayLitTypeCheckDimExp`, the only path to a **runtime** element
count; any other initial value, a string literal or an array variable, must be
typed a fixed-size array, and `genlallocref` takes the count from that type's
dimension rather than from the node); refuse an abstract or zero-size type; build the result type; then
`inodeTypeCheckAny` on it — **that line is load-bearing**, because it is what
routes to `refTypeCheck` and therefore what populates `typeinfo`, which
`genlallocref` dereferences unconditionally. Finally check the region declares
an `alloc` at all (its shape was checked at the region's declaration) and
validate the permission's `init`.

A region ref is a struct declaring `is RegionRef`, which `refTypeCheck`,
`arrayRefTypeCheck` and `refvirtTypeCheck` each require of an owning
reference's region (`refRegionCheck`, `ErrorNotRegion`); `So` and `Rc` are
ordinary Cone declarations in the core package, `packages/core/src/core.cone`,
not compiler built-ins ([What a region is](module.md)). A region slot naming
something other than a type (a function, say) is reported twice, as no type and
as no struct, and `refRegionCheck` then puts the error type in the slot, since the
reference type is still hashed and asked whether it moves; an allocation asks
before that, so `refAdoptInfections` and `regionDcl` pass over a slot that is no
type.

### Matching

`regionMatches` is the shared gate and is **one-directional**: identical
regions match, anything coerces *to* `borrowRef`, and two different regions
never coerce to each other.

`permMatches` returns only `EqMatch` or `NoMatch`: `uni` coerces down to `ro`,
`mut`, `imm`, `mut1`; anything readable coerces up to `ro`; `opaq` accepts
everything.

**`refMatches` keys value-type variance on the target's permission flags** —
this is the part most worth internalizing:

| Target permission flags | Variance |
| --- | --- |
| neither (`opaq`) | covariant |
| `MayRead` (`ro`, `imm`) | covariant |
| `MayWrite` | contravariant |
| both (`mut`, `uni`, `mut1`) | **invariant** |

So `&mut T` is invariant in `T` while `&ro T` is covariant.

**Covariance stops at a reference held behind one that would stop moving.**
`refHeldMoveSeenAsCopy`, asked by the covariant arm of `refMatches` and of
`arrayRefMatchesRef`, refuses a value type that is a reference whose own type
moves seen as one that copies: `&+Rc-mut T` (or `-imm`, `-ro`, `-opaq`, `-mut1`)
from `&+Rc T`, and a slice of them. A read through the outer reference copies
a copy type out, so the view would make a second owner of a value `uni`
promised unique. `permMatches` is untouched, and a move of the owner itself
still coerces `uni` down. A `+So` owner keeps its view as `+So-mut`, since
every `+So` reference moves and a move out through a borrow is refused in flow.

`refvirtMatchesRef` builds a fat pointer, so it refuses `Monomorph` outright and
applies **no** value-type variance. Same-struct requires `HasTagField`, since
the tag is what selects the vtable at runtime. A reference to an enum converted
to a different trait needs the tag for the same reason: `structVirtRefMatches`
registers each variant's implementation, not the enum's, and the conversion
selects among them by the tag. A reference to an open trait converts to no other
trait ([struct](struct.md), "A reference to an enum converts").

`arrayRefMatchesRef` handles `&[T; n]` → `&[]T` and is never better than
`ConvSubtype` — a fat pointer must be built.

`ptrMatches` is fully invariant, but `itypeMatches` separately accepts a
reference as a `ConvSubtype` to a pointer, **ignoring region and permission
entirely**.

### `refAdoptInfections`

Where a reference type acquires `MoveType`: **when its permission lacks
`MayAlias`, or its region is itself a move type.** Of the six permissions only
`uni` lacks `MayAlias`, and a region ref is a move type by declaring `is Move`,
as `So` does ([struct](struct.md), "Move and Copy"). Since `+region` defaults to
`uni`, every owning reference written without a permission moves; one with an
aliasable permission into a region ref declaring neither `Move` nor `aliasRef`
copies, and the copy calls nothing.

It no longer sets a thread-bound flag. That flag was settled as a type was laid
out, from the permission by identity with `mut` and `ro` and from the flags of
the pointee node, which for a pointee written by name was the name use's, never
the type's; the region had no say, and nothing read it.

### `refThreadBinds`: the thread check's rule for one reference

Whether a reference may cross to another thread, as far as the reference itself
says; what it points at is asked separately by the walk (`itypeThreadBound`, in
`itype.c`, beside `itypeCarriesBorrow`, whose fixed point it shares). In order:

| The reference | Answer |
| --- | --- |
| to a function (`&fn`) | crosses, whatever it points at: code every thread shares |
| a borrow, any permission | `RefBindsBorrow`: its lifetime is checked in one thread alone |
| into a region declaring `Traced` | `RefBindsTraced`: the collector is single threaded |
| an owner that cannot be aliased: `uni`, or any owner of a `Move` region (`So`) | crosses if what it points at does: it moves, taking its value |
| any other owner whose permission is not `RaceSafe` (`mut`, `ro`, `mut1`, a struct in the permission slot) | `RefBindsPerm` |
| any other owner whose region does not declare `ThreadSafe` (`Rc`) | `RefBindsShared` |
| otherwise (`+Arc-imm`, `+Arc-opaq`) | crosses if what it points at does |

The walk adds a raw pointer (always bound: nothing checks its target), a
reference to an open trait (its implementers are not all known), and a struct
declaring `Sendable` (taken at its word, but bound where one of its type
arguments is). `genericTypeIs` grants `Sendable` to what the walk finds unbound;
an unmet `T is Sendable` is `ErrorNotSendable`, whose message names the culprit
and its path (`itypeThreadBoundWhy`). See [Generics](generic.md), Constraints.

## Flow

`allocateFlow` loads and move-or-copies the initial value. `borrowFlow` asks
only that the borrowed place was not moved out: it walks the place to the
variable at its root, which `nameuseFlowBorrowed` refuses if moved out or
hollowed, as a read is refused. Unlike a read, a variable never initialized may
be borrowed, so that a method taking it `&mut` can fill it in. A reference the place is reached through is
loaded as a value, not read through, and an index is read. A borrow
deactivates nothing and records nothing, so no aliasing of borrows is tracked.

A reference's permission is enforced at the access, not at the borrow.
`flowLoadThroughRef` asks it for `MayRead` wherever a value is read through a
reference — `derefFlow`, `fnCallArrIndexFlow`, and `fnCallFldAccessFlow` for a
virtual reference, which has no dereference injected — and `ErrorNoRead`
refuses the read; `opaq` is the permission that fails it. `assignlvalrtype` and
`swapFlow` ask `iexpGetLvalInfo` for `MayWrite` on the write side. Holding,
copying, comparing for identity (`===`) and passing a reference never read
through it, so an `opaq` reference does all of those. `==` and the orderings
do read through — type check injects a dereference on both operands
([fncall](fncall.md), "A comparison on a reference") — so on an `opaq`
reference they are `ErrorNoRead`.

**The `scope` a borrow recorded is enforced at three consumers, none of them
the borrow site**: `assignBorrowLifetimeCheck` when a borrow is stored into a
longer-lived lval — by `assignlvalrtype` for an assignment, and by `swapFlow`
once in each direction for a swap, which stores both ways — `returnFlowEscape` when one is returned, and
`fnCallFlowStoredBorrow` when one is passed to a call beside a `&mut &T`
argument that points at a longer-lived place. Each reads `RefTag`, `ArrayRefTag`
and `VirtRefTag` alike: a virtual reference is a borrowed reference carrying a
vtable, and a slice borrows as a single reference does. Three sites propagate
scope into a reference type they build: `fnCallArrIndex` into a borrowed
element's; `fnCallFinalizeArgs` into a call's result, which takes the narrowest
scope among the borrowed arguments on a `RefNode` of the call's own, the
declared return type being shared by every call site and unable to carry it;
and `iexpCoerce` into the `CastNode` it injects for a borrow coerced to another
reference type — which is how a virtual reference is built at all, and how one
is widened to a base trait's reference — the type coerced to being a declared
node, interned and shared by everything written with it, so the scope goes on a
copy of it belonging to that coercion. A call returning several
values gets a `TupleNode` of its own on the same terms, each borrowed element
carrying that scope, because a multi-value assignment checks every element
against its own lval. A borrow the compiler
injects records its lval's scope where it is built (`borrowMutRef`,
`borrowAuto`, `borrowUniReborrow` for a `&uni` lent as a shareable
borrow, and `borrowOwnerLend` for a sole owner lent as a `&uni`), so it reaches a call as the written borrow would; and
`iexpGetLvalInfo` gives a dereferenced borrow expression or call result the
scope on that reference's own type, since no variable holds it — a reference
held in a variable keeps the variable's scope, because the variable's type
carries at most its initializer's lifetime (`varDclTypeCheck` scopes a declared
borrowed-reference type as an inferred one is), not that of a borrow assigned
to it later. Nothing checks a borrow stored in a field or captured.

## Generation

| Tag | LLVM |
| --- | --- |
| `RefTag` | `ptr` — **identical for borrowed and owning** |
| `VirtRefTag` | named `{ ptr, ptr }`: the object, then its vtable |
| `ArrayRefTag` | anonymous `{ ptr, usize }`, count at index 1 |

`genlRefTypeSetup` returns immediately for a borrow — a borrowed reference has
no allocation header. Otherwise it builds `%refstruct = { region, perm, value }`,
once, when an allocation or a region header first asks for it.
Measured: `{ %rc, %void, i32 }` where `%rc = { i64 }` and `%void = {}`.

**`genlallocref` returns the pointer to `ValueField`**, so an owning reference
points into the *middle* of its allocation. The region's methods other than
`alloc` and `init` are handed the header instead, which `genlRegionHeader`
reaches by stepping back the value's offset in `%refstruct` — 8 bytes for `Rc`,
none for `So` — so nothing assumes a region's size. The region's `free`, where
it has one, is what gives the memory back; the compiler calls no `free` of its
own ([What a region is](module.md)).

**An owning virtual reference is released as any owner is, through its
vtable's type record.** `itypeNeedsFinal`, `flowIsOwningType` and `flowIsRcRef`
answer for `VirtRefTag` as for `RefTag`, so flow schedules its death, its
release before a store, and an `aliasRef` for each counted copy. The concrete type
is erased, so the two things a death needs are read at run time from the last
slot of the vtable, a pointer to the implementer's core `TypeRecord`
(`genlVirtRecord`): the value dies through the record's `finalize`
(`genlVirtFinalize`), and the header sits before the value at the region and
permission's size rounded up to the record's `align` (`genlVirtHeader`), which
`genlOwnerHeader` hands to `aliasRef`, `dealiasRef` and `free` in place of
`genlRegionHeader`'s static offset. A `So` header is empty, so no alignment is
read for one. **A conversion into an owning virtual reference carries its
operand's owner** (`flowCastCarries`): unlike any other conversion, which makes
a new value, flow walks through it as through a recast, so a plain owner
converted is moved into the virtual reference, or counted for a counted region,
and a local returned converted is exempt from its scope's release.

`BorrowTag` generates as nothing but `genlAddr(vtexp)`.

## Hazards

- **Sendable is also safe to read from several threads.** An `+Arc-imm` owner
  crosses when its pointee does, so a type granted or declaring `Sendable` must
  also bear being read through `&` from several threads at once. Nothing Cone
  can write mutates through `imm` except an atomic value, so the grant holds; a
  type declaring `Sendable` over raw pointers promises it too (Rust's `Send`
  and `Sync` are one question here).
- **A borrowed reference's inferred type has `typeinfo == NULL`.** The borrow
  path and the allocate path have different invariants for the same field.
  Anything reading `typeinfo` off an arbitrary reference type crashes on borrows
  only.
- **A virtual reference type has no `typeinfo`.** `genlRegionHeader` reads the
  header's offset from it, so anything reaching a region method through an
  owning virtual reference goes through `genlOwnerHeader`, which reads the
  alignment from the vtable's record instead.
- **Coming from Rust:** `&mut T` is invariant and `&ro T` covariant; `uni` is
  not `&mut` but the *unique* permission, which is what makes owning references
  move; lifetimes are a block-nesting integer that is not part of type identity
  and is checked at three sites; and there is no borrow checker.

## What lives elsewhere

- The model these tags implement — the three axes, and what each permits: [References and Regions](../../../../doc/design/references-and-regions.md)
- Moves, counting, and what the count counts: [Flow Analysis](../phases/flow.md)
- The allocation header and pointer levels: [Generation](../phases/generation.md)
- What a borrow's type check establishes, in context: [Type Check Reasoning](../phases/type-check-reasoning.md)
