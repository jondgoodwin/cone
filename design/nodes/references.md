`RefNode` is one struct serving **seven tags** across two node groups — four
type nodes and four expression nodes (`RefTag` appears in both roles at
different times). Getting the family right is most of understanding Cone's
memory model.

**At a glance.** The parser builds a reference-shaped node without knowing
whether it is a type or a constructor. Name resolution decides by asking whether
the operand is a type. Type check builds the *result* type, records a lifetime,
and interns. Flow moves or copies an allocation's value and enforces the
lifetime at two consumers. Generation lowers a plain reference to a bare
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
| `typeinfo` | interned `RefTypeInfo` — the LLVM handles |
| `scope` | lifetime: 0 global, 1 parameter, 2+ local |

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

The group bits are the discriminator: type tags are `TypeGroup`, constructor
tags are `ExpGroup`. **The `scope` that matters is the one on a borrow's
*result* type**; the expression node's own is never read.

`RefTypeInfo` holds three LLVM handles: the reference's own type, the
`{region, perm, value}` allocation header, and a pointer to it.

## Constructors and interning

`newRefNode` seeds `borrowRef`, `roPerm`, `scope = 0` — and the comment says why
the zero matters: left uninitialized, the lifetime checks read allocator
garbage. `newRefNodeFull` additionally sets the three fields and runs
`refAdoptInfections`. `newBorrowMutRef` builds the pair by hand and
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

**A virtual reference may not be borrowed or allocated** — `ErrorBadTerm`,
"Coerce from a regular ref." There is nothing to construct *from*: the fat
pointer's second word is a vtable, selected either by scanning the trait's
implementations for the concrete source struct or by indexing the vtable list
with a runtime tag. Both need a source *reference type*; neither is available
from a bare lval.

Because the retag happens here, the four constructor tags have **no arms in
`inodeNameRes`** — they cannot exist before this point.

## Type check

### `borrowTypeCheck`

In order: re-associate `&v[i]` into `(&v)[i]` when the operand is an index, so a
type's own `` `&[]` `` method receives the borrowed receiver; check the operand;
**refuse a temporary** with its own message rather than "must be lval", because
every operand a borrow refuses is refused for that one reason; retag a
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
count); refuse an abstract or zero-size type; build the result type; then
`inodeTypeCheckAny` on it — **that line is load-bearing**, because it is what
routes to `refTypeCheck` and therefore what populates `typeinfo`, which
`genlallocref` dereferences unconditionally. Finally validate the region's
`_alloc(usize) *u8` and the permission's `init`.

A region is any struct with a suitable `_alloc`; `so` and `rc` are ordinary Cone
declarations in `corelibSource`, not compiler built-ins.

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
the tag is what selects the vtable at runtime.

`arrayRefMatchesRef` handles `&[T; n]` → `&[]T` and is never better than
`ConvSubtype` — a fat pointer must be built.

`ptrMatches` is fully invariant, but `itypeMatches` separately accepts a
reference as a `ConvSubtype` to a pointer, **ignoring region and permission
entirely**.

### `refAdoptInfections`

Where a reference type acquires `MoveType`: **when its permission lacks
`MayAlias`, or its region is itself a move type.** Of the six permissions only
`uni` lacks `MayAlias`, and `so` is `struct @move`. Since `+region` defaults to
`uni`, essentially every owning reference moves.

## Flow

`allocateFlow` loads and move-or-copies the initial value. `borrowFlow` is an
**empty function body** with a comment describing deactivation that was never
written — a borrow deactivates nothing and reads nothing.

**The `scope` a borrow recorded is enforced at two consumers, neither of them
the borrow site**: `assignlvalrtype` when a borrow is stored into a
longer-lived lval, and `returnFlowEscape` when one is returned. `fnCallArrIndex`
propagates scope into a borrowed element's type. Nothing checks a borrow passed
as an argument, stored in a field, or captured.

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
points into the *middle* of its allocation. `genlRcCounter` therefore reaches
the count by bitcasting to `usize*` and GEPing `-1`, and frees *that*.
`genlDealiasOwn` frees the value pointer directly, correct only because `so`'s
region struct is empty.

`BorrowTag` generates as nothing but `genlAddr(vtexp)`. There is **no region
`_free` hook** anywhere — a region controls allocation but not release; both
paths call libc `free`.

## Hazards

- **`refHash` and `arrayRefHash` hash `vtype`, which is permanently
  `unknownType` on a type node.** Every `RefTag` hashes to a function of tag and
  region alone. Correctness survives via linear probing; the table degenerates.

- **`arrayRefTypeCheck` never calls `refAdoptInfections`**, so a slice type
  written in source has different move semantics from the identical type built
  by an allocation. Same work item.
- **`ThreadBound` infection is unreachable** — it compares `perm` by pointer
  identity while the adjacent `MoveType` test unwraps through `permGetFlags`.
  Same work item.
- **A borrowed reference's inferred type has `typeinfo == NULL`.** The borrow
  path and the allocate path have different invariants for the same field.
  Anything reading `typeinfo` off an arbitrary reference type crashes on borrows
  only.
- **`genlRcCounter` assumes the counter sits exactly one `usize` before the
  value.** True only because `rc` has one field, the permission is zero-sized,
  and the value needs no alignment padding after it.
- **Coming from Rust:** `&mut T` is invariant and `&ro T` covariant; `uni` is
  not `&mut` but the *unique* permission, which is what makes owning references
  move; lifetimes are a block-nesting integer that is not part of type identity
  and is checked at two sites; and there is no borrow checker.

## What lives elsewhere

- The model these tags implement — the three axes, and what each permits: [References and Regions](../northstar/references-and-regions.md)
- Moves, counting, and what the count counts: [Flow Analysis](../phases/flow.md)
- The allocation header and pointer levels: [Generation](../phases/generation.md)
- What a borrow's type check establishes, in context: [Type Check Reasoning](../phases/type-check-reasoning.md)
