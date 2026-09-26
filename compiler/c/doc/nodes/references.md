`RefNode` is one struct serving **seven tags** across two node groups — four
type nodes and four expression nodes (`RefTag` appears in both roles at
different times). Getting the family right is most of understanding Cone's
memory model.

**At a glance.** The parser builds a reference-shaped node without knowing
whether it is a type or a constructor. Name resolution decides by asking whether
the operand is a type. Type check builds the *result* type, records a lifetime,
and interns. Flow moves or copies an allocation's value and enforces the
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
parameter position may have its pointee inferred, and `parseFnSig` is the only
thing that ever fills one in. Every other spelling — `fn f(s &)`, `&&`, `*&`,
`&[]`, `(&, i32)`, a non-`self` parameter inside a method — would otherwise
reach `genlType` still unknown and hand `LLVMPointerType` a NULL element type.
Every enclosing type reaches these two guards through `itypeTypeCheck` on its
own pointee, which is what makes two checks cover all of them.

### `borrowTypeCheck`

In order: re-associate `&v[i]` into `(&v)[i]` when the operand is an index, so a
type's own `` `&[]` `` method receives the borrowed receiver; check the operand;
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
reference's region (`refRegionCheck`, `ErrorNotRegion`); `so` and `rc` are
ordinary Cone declarations in the core package, `packages/core/src/core.cone`,
not compiler built-ins ([What a region is](module.md)).

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
as `so` does ([struct](struct.md), "Move and Copy"). Since `+region` defaults to
`uni`, every owning reference written without a permission moves; one with an
aliasable permission into a region ref declaring neither `Move` nor `alias`
copies, and the copy calls nothing.

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
the borrow site**: `assignlvalrtype` when a borrow is stored into a
longer-lived lval, `returnFlowEscape` when one is returned, and
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
`borrowAuto`), so it reaches a call as the written borrow would; and
`iexpGetLvalInfo` gives a dereferenced borrow expression or call result the
scope on that reference's own type, since no variable holds it — a reference
held in a variable keeps the variable's scope, because a declared type carries
none. Nothing checks a borrow stored in a field or captured.

## Generation

| Tag | LLVM |
| --- | --- |
| `RefTag` | `T*` — **identical for borrowed and owning** |
| `VirtRefTag` | named `{ i8*, Vtable* }` |
| `ArrayRefTag` | anonymous `{ T*, usize }`, count at index 1 |

`genlRefTypeSetup` returns immediately for a borrow — a borrowed reference has
no allocation header. Otherwise it builds `%refstruct = { region, perm, value }`.
Measured: `{ %rc, %void, i32 }` where `%rc = { i64 }` and `%void = {}`.

**`genlallocref` returns the pointer to `ValueField`**, so an owning reference
points into the *middle* of its allocation. The region's methods other than
`alloc` and `init` are handed the header instead, which `genlRegionHeader`
reaches by stepping back the value's offset in `%refstruct` — 8 bytes for `rc`,
none for `so` — so nothing assumes a region's size. The region's `free`, where
it has one, is what gives the memory back; the compiler calls no `free` of its
own ([What a region is](module.md)).

`BorrowTag` generates as nothing but `genlAddr(vtexp)`.

## Hazards

- **Nothing consumes `ThreadBound`.** `refAdoptInfections` sets it, and struct
  and array types propagate it up from their fields and elements, but no check
  anywhere reads it. The infection is computed and inert.
- **A borrowed reference's inferred type has `typeinfo == NULL`.** The borrow
  path and the allocate path have different invariants for the same field.
  Anything reading `typeinfo` off an arbitrary reference type crashes on borrows
  only.
- **Coming from Rust:** `&mut T` is invariant and `&ro T` covariant; `uni` is
  not `&mut` but the *unique* permission, which is what makes owning references
  move; lifetimes are a block-nesting integer that is not part of type identity
  and is checked at three sites; and there is no borrow checker.

## What lives elsewhere

- The model these tags implement — the three axes, and what each permits: [References and Regions](../../../../doc/design/references-and-regions.md)
- Moves, counting, and what the count counts: [Flow Analysis](../phases/flow.md)
- The allocation header and pointer levels: [Generation](../phases/generation.md)
- What a borrow's type check establishes, in context: [Type Check Reasoning](../phases/type-check-reasoning.md)
