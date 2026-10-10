`RefNode` is one struct serving **seven tags** across two node groups — four
type nodes and three expression nodes (`RefTag` appears in both roles at
different times). Getting the family right is most of understanding Cone's
memory model.

**At a glance.** The parser builds a reference-shaped node for `&`, `&[]`, `&<`
and `+`, `+<` without knowing whether it is a type or a constructor.
Name resolution decides by asking whether the operand is a type. A managed
reference *type*, `Rc[mut, Node]`, is not built by the parser at all: it is a
bracketed call until type check lowers it into this node ("The managed
reference type", below), and an allocation, `new Rc[mut, Node](1)`, is a
construction until type check makes it an `AllocateTag` node
("Allocation"). Type check builds the *result* type, records a
lifetime, and interns. Flow moves or copies an allocation's value and enforces the
lifetime at two consumers, and the loan walk the rest. Generation lowers a plain reference to a bare
pointer, and a virtual reference, a slice and a reference whose target carries its length
("A target that carries its length", below) to fat pointers.

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
| `scope` | lifetime: 0 global, 1 the caller band (what a borrowed parameter points at), 2+ a block of the function, 2 its top block (where its parameters' own storage lives too) |
| `lifename` | the lifetime a function's signature or a struct's field writes on this borrowed reference type (`&'a T`), interned (`staticLifeName` for `'static`); NULL for the unnamed one, and on every type outside those, a generic's instance's own types among them (`lifeErased`), but for the argument of a type parameter the generic bounds, whose every lifetime is the bound's `'+T` (`lifeRenamed`). An invariant one (`'=a`, a key: `lifeIsInvariant`) is kept everywhere it is written, a body's types too, and on a call's result as the brand the call gave it |
| `bound` | a virtual reference's bound, `&<Trait + 'a` (`parseAmper`): what the value it points at holds lasts `'a`. The type holds `'a` beside `lifename` (`lifeGather`) and implies `'a >= lifename` (`lifeImplied`); a coercion's scoped copy keeps it (`iexpScopedBorrowType`); NULL on every other reference, and erased with `lifename` (`lifeErased`). Read by the loan walk where a value is returned or stored as it (`pwBoundHolds`), and, `'static`, where one is a parameter (`pwStaticArgs`). Ignored by type identity, as `lifename` is |

⚠ **A cloned reference re-interns, in `cloneRefNode`.** Cloning is how a trait's
method becomes an implementer's and a generic's becomes an instance's, and both
repoint `Self` — so the copy refers to something the original did not, while the
`memcpy` brings the original's `typeinfo` across. The copy also carries the
original's `TypeChecked` mark, so `refTypeCheck` never revisits it to normalize.
Sharing the record made the memoized LLVM type answer for the original's pointee:
whichever implementer generated first named it for all of them, so a trait
default's clones took one another's receiver and `fn f(m &Trait)` took an
implementer's type. `trait_inherited_defaults` pins both.

**A lifetime is two fields, and they answer different questions.** `scope` is
the band, compared as a number wherever a borrow is returned, stored or handed
to a call. `lifename` is read only on a signature's own types — a parameter's
declared type (the copy `varDclTypeCheck` scopes keeps it) and the result —
and on a struct's fields, by `lifeCarry`, `lifeFlows` and their neighbours
(`ir/types/lifetime.h`), which say what of each argument a result or a store
may draw on; within the band, two names are told apart by the loan walk's
caller loans, never by `scope`. A name is never compared across two
signatures, so a copy of a callee's type that reaches a caller (a call's
result) carries a name nothing reads there. A struct's lifetimes, named on a
use of it (`Cursor['a]`), are no reference's: they are the use's
`NameUseNode.lifeuse`, read by the same functions.

**An invariant `lifename` is a third thing: a brand.** A reference of one,
`&'=a T`, is a key: its `scope` is 0 and means nothing, it is no borrow to
`itypeCarriesBorrow` or `iexpIsBorrowType`, it is never the same type as a
borrow (`refIsSame`, `refMatches`), and nothing reaches through it but its
arena's `[]`. Its name *is* compared across signatures, by binding: a call
replaces the callee's names in its result with the brands its arguments gave
them ([Type Check Reasoning](../phases/type-check-reasoning.md), "Invariant
lifetimes: brands").

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

**Every array reference is borrowed.** There is no owning array reference: a
managed reference is thin, virtual, or fat with a length ("A target that
carries its length", below), an owned runtime-sized array is a `List`,
and a shared one is `Rc[List[T]]`. An `ArrayRefTag`'s region is always
`borrowRef`. A slice of bytes and a borrow of `str` are two types of one
layout, `{ptr, usize}`, and convert to each other by `as`.

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

`parseAmper` handles `&`, `&[]`, `&<`; `parsePlus` handles `+`, `+<`, and
refuses `+[]` (`ErrorOwnedArrayRef`, at the token, naming `List` and `&Array[T]`),
reading the rest through as the thin form so that nothing after it is
reported; the lexer keeps `PlusArrayRefToken` only for that.
The differences are worth knowing:

- **`&` has no region syntax** — `region` stays at the `borrowRef` default.
  **`+` requires a region annotation** and errors without one.
- `&` leaves an absent permission as `unknownType`, deferred to type check.
  `+` defaults to `uni` **at parse time**.
- `&` may name a lifetime right after its token, before the permission
  (`&'a mut T`, `&'a Array[u8]`), only while `ParseState.lifesig` is set (the types
  of a signature's parameters and result, a type's arguments there included),
  where it sets `lifenamed` on that signature, or `ParseState.lifestruct` (a
  struct's field's type), where it is noted for the struct to declare
  (`lifeStructDeclare`). Anywhere else the lifetime is `ErrorLifetimePlace`,
  and read past (`parseLifeNamed`) -- but for an invariant one, which a body
  may name too (a cast making a key, a variable's type, a generic's type
  argument), type check holding it to the signature's (`lifeBrandKnown`).
  An invariant lifetime on `&[]` or `&<` is `ErrorLifetimeInvariant`: a key is
  a plain reference. On `&Array[T]`, which the parser reads as a plain reference,
  `refNameRes` refuses it once it has made the node the slice.
- `&` has two escapes `+` does not: a `fn` operand, where the presence of a body
  decides closure-versus-signature; and a `,` or `)` operand, where `vtexp` is
  left `unknownType` for later `Self` inference in a parameter position.
- Both reach the **whole suffixed term**, at the same precedence every other
  prefix operator has: `&x.a` references the field, `&x[4]` the element. Binding
  only the prefixed term instead would make `&x.a` mean `(&x).a` — typed as the
  field but returning the field's address, which generation cannot catch.

An allocation is written `new Rc[mut, Node](1)` or, with a finished value,
`new Rc[i32](5)` (below, "Allocation"). The `+` spelling, `+Rc-mut 5`, is
refused whatever its value. `parsePlus` builds every `+` node marked
`plusSpelled`. With a value operand it becomes an `AllocateTag` at name
resolution, and `allocateTypeCheck` refuses it (`ErrorPlusAlloc`, naming the
`new` or `trynew` form, with `(...)` for a construction and `(value)` for any
other value, a type with no name of its own written `T`). With a type operand it
is a `RefTag` or `VirtRefTag` type, which type check refuses
(`refRefusePlusType`, `ErrorPlusRefType`), naming the bracket spelling. The
one exception is a match pattern's root, `case imm c +Rc-mut Circle`:
`castPatternName` reads the root at parse time, before anything knows `Rc` is
a region rather than a generic variant, so `castPatternMark` clears the mark
there and the `+` spelling stands until patterns are given their own.

## The managed reference type, `Rc[mut, Node]`

The region names the reference, and its brackets hold an optional permission,
then the value type: `Rc[Node]`, `Rc[mut, Node]`, `Gc[imm, Leaf]`. Left out,
the permission is `uni` (`imm` for a value type declaring `Immutable`; "A type
that never changes", below). Lowering leaves it as `unknownType` for
`refTypeCheck` to settle, since it is the value type's declaration that
decides. It is ordinary generic syntax, so it parses as an
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
  stands as a lock permission, `Arc[Mutex, T]`, which `refTypeCheck` judges
  (`refLockCheck`, below, "Lock permissions").
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
form is always borrowed, so `arrayRefNameRes` produces `ArrayBorrowTag`.

**In a generic's template the answer is provisional.** `&T` asks the question
of a use of a generic parameter, which is not a type (`nameUseGroup` puts
`GenVarDclTag` in the meta group), so the template holds a borrow. Templates
are never type checked, so nothing reads that; `cloneRefNode` decides again once
the operand is the type argument, flipping a borrow or allocate whose original
operand was not a type and whose clone is back to `RefTag`/`ArrayRefTag`.
`cloneStarNode` does the same for `*T`. The inner clone runs first, which is
what carries `&&T`.

**A virtual reference may not be borrowed or allocated** — `ErrorBadTerm`,
"Coerce from a regular ref.", and for the retired allocation `+<Rc-mut
Rect[3]`, `ErrorPlusAlloc`, naming `new Rc[mut, Rect](...)` and the coercion.
There is nothing to construct *from*: the fat
pointer's second word is a vtable, selected either by scanning the trait's
implementations for the concrete source struct or, from a reference to an enum, by
the runtime tag — which indexes the vtable list where the tag values are the
variants' positions in it, and compares against each variant's value where they are
not (see [Generation](../phases/generation.md), "Vtables"). Both need a source
*reference type*; neither is available from a bare lval.

Because the retag happens here, the three constructor tags have **no arms in
`inodeNameRes`** — they cannot exist before this point.

## Type check

**A reference never demands its target's layout.** Its size is its kind's, so
while a layout of a type holding values by value is in flight,
`refTargetTypeCheck` resolves the target — `refTypeCheck`'s, `refvirtTypeCheck`'s,
`arrayRefTypeCheck`'s, `ptrTypeCheck`'s and a managed reference type's
(`fnCallLowerManagedRef`) — without laying it out, and what reads its layout (a
key's borrow, a vtable) waits until no layout is in flight. Its region and
permission are its kind and are checked with it. [Type
Check](../phases/type-check.md), "A reference does not demand its target".

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
within a local's declaration, make the temporary the operand's place is rooted
in a hidden local, which the declaration keeps if the borrow extends it and
otherwise refuses ([vardcl](vardcl.md), "Temporaries an initializer extends");
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

### Allocation: `new Rc[mut, Node](1)` and `trynew`

`new` with a managed reference type, written out or through an alias of one
(`new Node(1)` for `alias Node = Gc[mut, NodeValue]`), or with a type
parameter whose argument is one, allocates. `typeLitNewCheck` checks the
type, and finding a `RefTag` hands it to `typeLitNewAllocate`, which builds
the `AllocateTag` node: its region and permission the reference type's, its
`vtexp` the value `typeLitAllocValue` makes of the parentheses, or, for an
array allocated with contents after `<-`, the literal of those contents,
which `contentsLower` hands it (`typeLitNewFilled`, `FlagAllocFill`: filled in
place by generation, not evaluated first; [fncall](fncall.md), "The list
after `<-`"); and its `vtype`
the reference type as written, so that a rule judged of
that type (a traced value behind an `Rc`, say) is reported once, where it was
written. `trynew` adds `FlagQues` and types the node `Option[R[perm, T]]`
through the `Option` its construction carried from name resolution; on a type
that is no managed reference it is `ErrorTryNewValue`. A borrowed reference
reached through an alias, and a virtual one, are `ErrorNewType`: no region
holds the one, and the other's value is a trait's.

**The parentheses hold the value's init arguments or one finished value**
(`typeLitAllocValue`), told apart by the value type:

- **A struct** that is neither a trait, an enum nor a variant has inits. When
  exactly one argument is given, not by name, it is type checked as the
  construction would check it (expecting the first field's type when the
  struct declares no init, nothing otherwise), and if its type is the struct
  itself, `itypeIsSame` after aliases, with no coercion, it is the finished
  value and becomes `vtexp` as it is. Otherwise `vtexp` is the construction
  `new NodeValue(...)`, a new `FlagNew` node over the same arguments, checked
  by `typeLitNewChecked` told the one argument is already checked; its init is
  selected as for any value. A reference to the struct, or another struct,
  is not the struct itself, and goes to the inits.
- **Any other value type** (a number, an array, a tuple, an enum or its
  variant, a reference) has no init: exactly one positional argument is the
  value, `ErrorAllocValue` otherwise, checked against the value type and
  coerced to it as a variable's initial value is (`iexpTypeCheckCoerce`), so a
  number literal adopts the type and a variant becomes its enum; one that does
  not coerce is `ErrorInvType`.
- **A body whose length the reference carries** (`str`) has no value at all:
  exactly one positional argument is a borrow of one, `&str`, coerced to that
  type (`ErrorAllocValue` otherwise, `ErrorInvType` for a value that is no such
  borrow), and the allocation copies what it borrows ("A target that carries its
  length", below).

A generic's instance decides by its own types, since a template is checked
only as each instance: `new Rc[T](x)` with `x` a `T` is the finished value in
every instance.

Outside an allocation, a struct's construction given its own finished value,
`new Point(p)`, is `ErrorNewFinished` (`typeLitNewChecked`): a value is
constructed once.

`new Plain[mut, i32](5)`, a permission given to what is no region, is refused
by `fnCallRefusePermArg` as the same head in a type is (`ErrorPermNotRegion`),
before `typeLitNewChecked` would report it as no type.

### `allocateTypeCheck` and `allocateValueCheck`

`allocateTypeCheck` is the refused `+` spelling: it defaults the permission to
`uni`, checks the value, reports `ErrorPlusAlloc`, and goes on as
`allocateValueCheck` so that nothing after it reports again. A `new`
allocation reaches `allocateValueCheck` directly, its value already checked.

`allocateValueCheck`: default the permission to `uni` (`imm` for a value whose
type declares `Immutable`); refuse an abstract or
zero-size value type (an array literal's dimension is a constant there as
everywhere, so an allocated array is a fixed-size one, reached by a thin
reference); build the result type, a `RefTag`, unless `new` gave it the one it
wrote; then
`inodeTypeCheckAny` on it — **that line is load-bearing**, because it is what
routes to `refTypeCheck` and therefore what populates `typeinfo`, which
`genlallocref` dereferences unconditionally. Finally check the region declares
an `alloc` at all (its shape was checked at the region's declaration) and
validate the permission's `init`.

A region ref is a struct declaring `is RegionRef`, which `refTypeCheck` and
`refvirtTypeCheck` each require of an owning reference's region (`refRegionCheck`, `ErrorNotRegion`); `So` and `Rc` are
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
everything. `new`, the permission of an init's `self` and of nothing else
(`refTypeCheck` refuses it anywhere else, `ErrorPermNew`), coerces as `uni`
does, to `uni` too, so a method called on a filled `self` borrows it; nothing
coerces to it. Its flags are `uni`'s: that nothing reads through it before it
is filled, and that it does not escape, is flow's to check
([Flow](../phases/flow.md), "An init's self"). A lock permission coerces to
nothing but itself, `opaq` included: its allocation holds the lock where a
static permission's holds nothing, so a coercion would misplace the header. A
guard's permission coerces as `uni` (the mutable lock held) or `imm` (a read
lock held) does, and nothing coerces to one (below, "Lock permissions").

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
moves seen as one that copies: `&Rc[mut, T]` (or `imm`, `ro`, `opaq`, `mut1`)
from `&Rc[T]`, and a slice of them. A read through the outer reference copies
a copy type out, so the view would make a second owner of a value `uni`
promised unique. `permMatches` is untouched, and a move of the owner itself
still coerces `uni` down. A `So[T]` owner keeps its view as `So[mut, T]`, since
every `So` reference moves and a move out through a borrow is refused in flow.

`refvirtMatchesRef` builds a fat pointer, so it refuses `Monomorph` outright and
applies **no** value-type variance. Same-struct requires `HasTagField`, since
the tag is what selects the vtable at runtime. A reference to an enum converted
to a different trait needs the tag for the same reason: `structVirtRefMatches`
registers each variant's implementation, not the enum's, and the conversion
selects among them by the tag. A reference to an open trait converts to no other
trait ([struct](struct.md), "A reference to an enum converts").

`arrayRefMatchesRef` handles `&Array[T, n]` → `&Array[T]` and is never better than
`ConvSubtype` — a fat pointer must be built.

`ptrMatches` is fully invariant, but `itypeMatches` separately accepts a
reference as a `ConvSubtype` to a pointer, **ignoring region and permission
entirely**. A slice is not accepted: it is an address and a count, and its pointer
is asked for with `as` ([cast](cast.md), `castTypeCheck`).

### `refAdoptInfections`

Where a reference type acquires `MoveType`: **when its permission lacks
`MayAlias`, or its region is itself a move type.** Of the six permissions only
`uni` lacks `MayAlias`, and a region ref is a move type by declaring `is Move`,
as `So` does ([struct](struct.md), "Move and Copy"). Since a managed reference
type defaults to `uni`, every owning reference written without a permission moves (but
for one to a type declaring `Immutable`, which defaults to `imm` and copies); one with an
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
| a borrow, any permission, or a guard (a lock held, below) | `RefBindsBorrow`: its lifetime is checked in one thread alone |
| into a region declaring `Traced` | `RefBindsTraced`: the collector is single threaded |
| an owner that cannot be aliased: `uni`, or any owner of a `Move` region (`So`) | crosses if what it points at does: it moves, taking its value |
| any other owner whose permission is not `RaceSafe` (`mut`, `ro`, `mut1`, a lock permission not declaring `ThreadSafe`) | `RefBindsPerm` |
| any other owner whose region does not declare `ThreadSafe` (`Rc`) | `RefBindsShared` |
| otherwise (`Arc[imm, T]`, `Arc[opaq, T]`, `Arc[Mutex, T]`) | crosses if what it points at does |

The walk adds a raw pointer (always bound: nothing checks its target), a
reference to an open trait (its implementers are not all known), and a struct
declaring `Sendable` (taken at its word, but bound where one of its type
arguments is). `genericTypeIs` grants `Sendable` to what the walk finds unbound;
an unmet `T is Sendable` is `ErrorNotSendable`, whose message names the culprit
and its path (`itypeThreadBoundWhy`). See [Generics](generic.md), Constraints.

**A borrow of the whole program** is the one exception to the borrow row, where
the walk is asked to allow it (`itypeThreadBoundHow`, a `StaticBorrow`;
`refStaticCrosses` is the rule for one borrow). It crosses if its permission is
`imm`, `opaq` or `uni` (`ro` and `mut` only stop or allow one holder: `ro`
does not stop another's writing) and its lifetime is not an invariant one (a
brand: its arena dies with its owner), and then, like an owner, if what it
points at does, which is the `Arc[imm, T]` test for `imm` and `opaq` and the
move test for `uni`. A guard never crosses. A `uni` borrow of a named global is
refused where it is made, so the walk need not tell one. That the borrow lasts
the whole program is not in the type, which instancing erases, so the mode says
who vouches: `StaticVouched` for a generic's parameter bounded `+ 'static`
(`genericStaticHow`; each call is held to hand it only global borrows, by
`lifePartStatic`), `StaticWritten` for a signature that writes the borrow
`'static`, the only form an actor's behaviour or initializer takes, in its
parameters and its reply. A struct's
remembered answer is the strict one: a mode other than `StaticOff` trusts its
"not bound", asks again where it was "bound", and remembers neither.

## Lock permissions

A struct declaring the built-in trait `LockPermission` may stand in a managed
reference's permission slot: `Arc[Mutex, T]`, `Rc[Rwcell, T]`
(`ir/types/permission.c`). The compiler knows no lock by name, as it knows no
region: it calls the methods the struct declares, held to their shapes at the
declaration (`lockPermCheck`, `ErrorLockPermShape`): `acquireMut` and
`releaseMut`, required; `acquireRead` and `releaseRead`, both or neither; each
taking only `self`, a borrowed reference to the lock, and returning nothing,
but that an acquiring one may take `(file &Array[u8], line u32)` after it and is
then handed the borrow's place in the source; and `init`, run by an
allocation on the lock in place (`permInitTypeCheck`). The lock is in the
header, `{region, lock, value}`.

**Where the reference type is checked** (`refLockCheck`, from `refTypeCheck`
and `refvirtTypeCheck`): a struct that does not declare the trait is
`ErrorNotLockPerm`; the region must count its owners (`aliasRef` and
`dealiasRef`), not be `Move`, and declare `ThreadSafe` exactly where the lock
does (a lock for threads, `Mutex`, on `Arc`; one for one thread, `Rwcell`, on
`Rc`); a virtual reference takes none; each is `ErrorLockRegion`. A traced
region's refusal is its own rule's (`regionTracedJudgeRef`, `ErrorTracedPerm`).

**The reference reaches nothing.** `permGetFlags` answers a lock permission
with `MayAlias | MayAliasWrite`, and `RaceSafe` where the lock declares
`ThreadSafe`: the reference copies and counts, and an `Arc[Mutex, T]` crosses
threads where `T` does, but every read, write, method receiver, lend to a
borrowed parameter and operator in place through it is refused, each where the
permission was asked before (`flowLoadThroughRef`, `assignlvalrtype`,
`swapFlow`, `borrowMutRef`, `fnCallLowerMethod`, `fnCallFinalizeArgs`), with
`ErrorLockAccess` in place of the static permissions' messages.

**A borrow through it reads through a guard** (`borrowLockPlace`, from
`borrowTypeCheck` before anything else looks at the place). The place's
nearest dereference of a lock-managed reference (`&mut *p`, `&mut p.x`,
`&mut p[i]`, the inner link of an index chain) has its reference wrapped in a
conversion flagged `FlagLockAcquire`, to the **guard type**: the same region
and value type, its permission a `PermNode` of the lock's (`permHeld`,
`PermNode.lock` and `held`, one node per lock and kind) holding the mutable
lock (`LockHeldMut`, `uni`'s flags) for a borrow whose permission may write,
or the read lock (`LockHeldRead`, `imm`'s flags less `MayAlias`) otherwise,
the mutable one where the lock has no read pair. A guard type is the same
machine type as the lock-managed one: `itypeIsRunSame` and `itypeHash` read a
guard's permission as its lock, so both share one interned `typeinfo`, and
`genlType` lays a guard's permission out as its lock. It may not be aliased,
so it moves; it binds to its thread (`refThreadBinds`).

The guard is an ordinary temporary owner: flow copies the reference into it,
counted (`flowLoadValue`'s `CastTag` arm, `flowHandleMoveOrCopy`), or moves a
temporary reference in, so the value outlives the guard whatever becomes of
the reference; and the borrow is a borrow of the guard. So where the lock is
given back is where the guard dies, by the existing rules for a temporary:
- **in a local's initializer**, an extending position, `varDclExtendTemp`
  makes the guard a hidden local of the block, released at the block's end on
  every path (the dealias lists, drop flags);
- **anywhere else** it is a statement's temporary (`flowTempRead`, at the
  dereference `borrowFlowPlace` walks), finalized at the statement's end, and
  the borrow, as any borrow of a temporary, is typed with the block's
  lifetime (`borrowTempScope`). The
  lifetime checks refuse it returned or stored outward, and the loan walk,
  whose stand-in for the temporary ends with the statement (`pwTempsEnd`),
  refuses a holder of it used after (`ErrorFrozen`).
An operator's rewrite (`x += 1`, `v <- (a, b)`) is a block whose temporaries
live to its end (`FlagKeepTemps`), so `(&mut *p).n += 1` holds the lock
through the write.

**Generation** takes the lock as the conversion is generated
(`genlLockAcquire`: the header from the value pointer, `genlRegionHeader`;
the lock its `PermField`; the acquiring method called on it) and gives it back
first thing in an owner's release when the owner is a guard
(`genlRegionDealiasPart`), before the `dealiasRef` that may free the header.

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

**The `scope` a borrow recorded is enforced at two consumers, neither of them
the borrow site**: `assignBorrowLifetimeCheck` when a borrow is stored into a
longer-lived lval — by `assignlvalrtype` for an assignment, and by `swapFlow`
once in each direction for a swap, which stores both ways — and
`returnFlowEscape` when one is returned. A borrow passed to a call beside a
`&mut` or `&uni` argument that points at a place able to hold one is the loan
walk's ([Flow](../phases/flow.md), "Stores through a reference"). Each reads `RefTag`, `ArrayRefTag`
and `VirtRefTag` alike: a virtual reference is a borrowed reference carrying a
vtable, and a slice borrows as a single reference does. `returnFlowEscape`
looks into a returned `if`, `match` (an `if` once desugared) or block and checks
each arm's or the block's last value where it is written, so the refusal names
the arm that would dangle.

**An owner reached through a borrow that does not hold it alone lends only what
it lets.** `iexpGetLvalInfo` takes the permission of a dereference from the
reference dereferenced, which for an owner (`So`, `Rc`) is its own, `uni` for a
`So`. Held in a field of a `&Holder`, the owner would lend `&mut` and change what
the borrow was lent to read; held in a field of a `&mut Holder`, it would lend
`&uni` or `&imm`, promises that another name for the holder breaks by replacing
the owner ([refborref](../../../../doc/reference/refborref.html), "Freezing access
to the source of a borrow"). Viewpoint adaptation
([refperm](../../../../doc/reference/refperm.html)) is the intersection. So where
the owner's place is reached through a borrow (`iexpPathThroughBorrow`) whose
permission is not `uni`, and the owner's permission may write, the place keeps the
borrow's: `ro` or `imm` lend no `&mut`, `mut` lends `&mut` and `&` and no `&uni`
or `&imm`. An owner whose own permission cannot write (`So[imm, T]`,
`Rc[imm, T]`) lends `&imm` through any of them. The refusal is `ErrorBadPerm`,
reported by `borrowTypeCheck` for a written `&mut *h.app` or `&uni *h.app` and by
`borrowOwnerLendRefused` for an owner lent implicitly: a method's receiver
(`borrowMutRef`), an argument or an initializer (`iexpCoerce`), a `So[fn]`
called. A local, a field of one held by value, or a field reached through a
`&uni` holder is not held to this and lends as before. An owner reached through
an `Rc[mut, T]` owner held by name, not through a borrow, is not either.
`ref_typecheck_owner_field` and `ref_typecheck_owner_field_shared` pin the
refusals; `ref_owner_field` and `ref_owner_field_shared` what runs.

**The lifetime of a borrow is that of the place borrowed**, which
`iexpGetLvalInfo` computes for every borrow, written or injected
(`borrowTypeCheck`, `borrowMutRef`, `borrowAuto`, `borrowUniReborrow` for a
`&uni` lent as a shareable borrow, `borrowOwnerLend` for a sole owner lent as a
`&uni`):

- a variable's own storage has its block's scope, a global 0 — and a
  **parameter's own storage is the function's top block, 2**, not the caller
  band: a borrow of a by-value parameter, of `self` by value, or of what an
  owner passed by value owns is the function's, never returned or stored
  outward;
- **a place reached through a borrowed reference has that reference's
  lifetime**, read from its type (`iexpScopeThroughRef`) when the reference is
  held in no variable (a borrow expression, a call's result) or is the whole
  value of a variable whose type says what it holds. Such a place has no
  variable at its root, and is a place all the same: `&id(&x).n` borrows `x`'s
  field, with the lifetime of the reference the call returned and the field's
  own permission, and flow sees the call's loans on `x`. A place rooted in a
  temporary is not one of these: it is given a lifetime only where an
  initializer has made the temporary a hidden local (`varDclExtendTemp`), and
  elsewhere `borrowTypeCheck` still leaves its borrow untyped
  (`get(&*mkso(5))` as a statement). A borrowed parameter's type
  carries the caller band, 1 (0 where it is written `'static`), on a copy `varDclTypeCheck` gives it, and nothing
  shorter may be stored into it, so what it points at is the caller's however
  it is reached — through an immutable local copy of it too, whose type carries
  its initializer's lifetime. A mutable local may since have been given a
  borrow its type does not record (a variable's lifetime does not yet follow
  what it holds), so a **store** through one keeps the holding variable's
  scope (`stored`), while a **borrow** of a place reached through one reads
  the lifetime its type records, as returning or handing on the local itself
  does; what it was later given is followed by the loan walk, which refuses it
  returned or stored past what it was borrowed from (`ref_flow_unicursor`).
  A reference held in a field or an element carries no lifetime of its own
  (the declared field type is shared), so a place reached through one keeps
  the holding variable's scope;
- a place reached through an owning reference lives as long as the owner's
  holder, so an owner lent as a borrowed reference — `iexpCoerceType` on the
  recast `iexpCoerce` injects — carries the lifetime of a borrow through it: a
  `So` parameter handed back as `&R` is refused as `&*s` is.

**A store asks a different question** (`iexpGetStoreLvalInfo`, from
`assignlvalrtype` and `swapFlow`): how long must what is stored live. The answer
differs only at a parameter, which stays at the caller band its type promises:
what is stored into it, or into a part of it held by value, can be read back
out with the lifetime its type gives, which no flow tracks yet.

Sites that build a reference type carry the scope onto it: `fnCallArrIndex` into
a borrowed element's; `fnCallFinalizeArgs` into a call's result, which takes the
narrowest scope among the borrowed arguments whose parameter's own lifetime
flows to one it holds, or, where only what the parameter's reference points
at holds one, of the borrow the argument points at, where that is a borrowed
reference with a scope (`fnCallNarrowestBorrowScope`, by `lifeCarry`; all of
them where the signature names none) on a `RefNode` of the call's own,
the declared return type being shared by every call site and unable to carry
it; `iexpCoerce` into the `CastNode` it injects for a borrow coerced to another
reference type — which is how a virtual reference is built at all, and how one
is widened to a base trait's reference — or for an owner lent as a borrow; and
`ifTypeCheck` and `blockTypeCheck` into the value of an `if`, a `match` or a
block, which takes the **narrowest** scope among the arms, or the last value and
the breaks, that can give it (`iexpNarrowestType`), whatever type was expected
of it. Each puts the scope on a copy belonging to that use
(`iexpScopedBorrowType`), the declared type being interned and shared by
everything written with it. A call returning several values gets a `TupleNode`
of its own on the same terms, each borrowed element carrying that scope,
because a multi-value assignment checks every element against its own lval.
A variable's type carries at most its initializer's lifetime
(`varDclTypeCheck` scopes a declared borrowed-reference type as an inferred one
is), not that of a borrow assigned to it later. Nothing checks a borrow stored
in a field or captured.

## Generation

| Tag | LLVM |
| --- | --- |
| `RefTag` | `ptr` — **identical for borrowed and owning** — or, where its target carries its length (`refIsFat`), anonymous `{ ptr, usize }` as a slice's, whatever the region |
| `VirtRefTag` | named `{ ptr, ptr }`: the object, then its vtable |
| `ArrayRefTag` | anonymous `{ ptr, usize }`, count at index 1 |

`genlRefTypeSetup` returns immediately for a borrow — a borrowed reference has
no allocation header. Otherwise it builds `%refstruct = { region, perm, value }`,
once, when an allocation or a region header first asks for it.
Measured: `{ %rc, %void, i32 }` where `%rc = { i64 }` and `%void = {}`; for
`Arc[Mutex, Point]`, `{ %Arc, %Mutex, %Point }`, the value 12 bytes in (a
`Mutex` is one `u32`), which a guard's type shares.

**`genlallocref` returns the pointer to `ValueField`**, so an owning reference
points into the *middle* of its allocation. It runs every allocation in one
order: the value init's arguments, `alloc`, the region's and permission's
`init`, the value's init in place; in a traced region the block is rooted and
its value zeroed as soon as `alloc` returns ([Generation](../phases/generation.md),
"An allocation runs in one order"). The region's methods other than
`alloc` are handed the header instead: `init` the one `genlallocref` reaches
from the new block, which it fills in place, and the others the one
`genlRegionHeader` reaches by stepping back the value's offset in `%refstruct`
— 8 bytes for `Rc`, none for `So` — so nothing assumes a region's size. The region's `free`, where
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

`BorrowTag` generates as nothing but `genlAddr(vtexp)`, but for a borrow whose
type is fat: what a fat reference points at has no address of its own, so the
borrow of its dereference is the reference itself (`&*owner`, an owner lent
as a borrow), as the borrow of a slice's dereference is the slice.

## A target that carries its length

A reference is thin or fat by its **target**, in every region and for a borrow
alike. `itypeLenBodyElem` answers whether a type is a *dynamically sized body*, one
whose elements run on from the pointer for a count the reference carries, and
what its element type is; `refIsFat` asks it of a `RefTag`'s `vtexp`. There are two
such bodies. `str`, `strTypeDcl` (corelib.c), is a struct declared `@opaque` with no
fields, element `u8`. `Array[T]` is an instance of core's generic struct of that name:
`arrayTypeDcl`, the struct the compiler makes for `Array[T, n]`, becomes the generic
when core declares `pub struct @opaque Array[T]` (`stdlibAdoptArray`, as `str` is
adopted), and `itypeLenBodyElem` finds an instance by the call that made it
(`dcl->instnode`, whose function is `arrayTypeDcl` and whose one type argument is the
element). `Array[T, n]` is still lowered to the array type at name resolution
(`arrayTypeLower`); a bracketed use with the element alone is left to be an ordinary
generic instance (`fnCallNameRes`). A trait's reference carries a vtable instead and
is its own tag, `VirtRefTag`.

**The borrow of `Array[T]` is the slice.** `&Array[T]` is the slice type, so nothing
downstream of the type check knows the body is there: a borrow
of an instance (`refNameRes` for the spelling written out, `refTypeCheck` for one
reached through an alias or built by the compiler) is retagged `ArrayRefTag` with the
element as its target. The retired spelling `&[]T` is refused (`ErrorSliceSpelling`,
at name resolution by `arrayRefNameRes`, which alone can tell the type from the
borrow `&[]x`; a type parameter counts as a type there). Only an owner keeps the body as its target, `RefTag` with
`vtexp` the instance, and it reaches the slice by the lend described under
"Type check" below. `Array[T]`'s methods are core's, written on the instance; their
`self` is that slice (`fnDclTypeCheck` accepts it as the type's own).

The markers `Sized` and `DynSized` (`itypeIsSized`, `itypeIsDynSized`, granted
in `genericTypeIs`) say which: `Sized` is a type with a size (`itypeIsConcrete`),
`DynSized` that, an open trait, or a body with a length. An `@opaque` type and
an `@unsized` enum are neither, and their references are thin.

- **Type check.** `refTypeCheck` checks the target as for any reference, and the
  body is refused where a size is asked, by `itypeNoSizeOwnCause` (a variable, a
  field, `mem.sizeof`: `ErrorNoSize`, `ErrorIntrinsicType`). A borrow of one with
  no permission written is `imm`, as `str` declares `Immutable` ("A type that
  never changes", below), where a borrow of any other type with no size is
  `opaq` (`borrowTypeCheck`). `castBitsize` gives a fat `RefTag` the size of a
  slice, so `as` converts between `&str` and `&Array[u8]` and between an owner and a
  borrow of the same body, checking nothing else. Implicitly a `&str` goes to a
  `&Array[u8]` (`arrayRefMatchesRef`, a recast, the permission narrowing as it must:
  `imm` to `ro`, never `mut`) and a `&Array[u8]` does not go to a `&str`.
  **An owner of `Array[T]`, in any region, is lent as the slice** by the same
  function, a recast whose element variance is a slice's own. Flow reads that
  recast as the borrow of the owner it is (`pwIsOwnedLent`, `flowGateIsOwnedLent`),
  so the owner may not be moved while the slice, a range of it or an element's
  borrow is used. Indexing an owner (`fnCallBodyAsSlice`) coerces the receiver to
  the slice first, `ro` for a read, `mut` for an assigned element, the borrow's own
  for `&mut o[i]`, and goes on as a slice's index does; a generic's element type is
  inferred through an owner (`genericInferType`).
- **A string literal is `&imm str`.** Its value is the pair of its constant's
  address and its length, `slitTypeCheck` types it as that reference, and it is a
  borrow of the whole program (global scope). It is copied into an owner of the
  body wherever one is wanted (`slitCoerce`): the node becomes the allocation
  `new R[str](lit)` builds, with no `new` written. Only the literal node itself is,
  never another `&str` ([literals](literals.md)).
- **Allocation.** There is no value to construct or move in. `typeLitAllocValue`
  takes exactly one argument, coerced to a `&str` borrow, as the allocation's
  value; `genlallocref` asks `alloc` for the header and `count * sizeof(element)`
  bytes (the count being the view's), and copies the view's elements in with a
  `memcpy`, returning the pointer and the count. The element type stands in the
  `{region, perm, value}` header's value field (`genlRefTypeSetup`), so the
  header's size is the same for every length and `genlRegionHeader` steps back
  by a constant. A region's `alloc` that takes a record is handed the element's,
  after a size that is the bytes the object really takes; a collector counts an
  object by that size, and not by its record's one element (`Gc` keeps the surplus
  in its header, in the padding after its colour). One byte is allocated beyond the
  elements: the NUL text keeps, and for an `Array[T]` only the byte that keeps an
  empty one in a region with no header from asking for no memory. An `Array[T]` in
  a region with a header gets no such byte, and `genlallocref` stores the NUL only
  where it was allocated (it once stored it anyway, one byte past the block, on top of
  the next block's heap header: `region_body_bounds`). An `Array[T]`'s
  allocation takes only elements that copy (`typeLitAllocValue`: the elements move
  in only with a `List`'s `freeze`, which casts the slice of its block to the owner).
- **Release.** `genlRefPtr` takes word 0 wherever a pointer is wanted (the
  header, a trace, a null test); a nullable-pointer enum holding one tests word 0.
  The owner's death finalizes each element, in a loop over word 1, where the
  element needs it, then calls `free` with the header (`genlRegionDeath`).
- **Tracing.** `genlTraceRef` marks a fat owner through word 0.

The count is not part of any region's header: it travels in each reference, so
each owner of one value holds its own copy, which is right only for a body that
never changes length.

## A type that never changes

`Immutable` (`immutableTrait`, corelib.c) is a marker a type **declares**:
`strTypeDcl` lists it in its `traits` as stdlibInit builds it, and any struct
may with `is Immutable`. It is not granted, so `itypeIsImmutable` is just
`structDeclaresTrait`, and a type holding an Immutable one is not for that
reason Immutable. It is a built-in trait (`corelibIsBuiltinTrait`), so no module
may declare a trait of that name.

It has two effects, each its own function in `types/reference.c` and called
apart, so the second can be loosened without touching the first:

- **`refImmutableDefaultPerm(target)`, the default permission.** A reference to
  the target written with no permission is `imm`, where the other default would
  be `ro` for a borrow and `uni` for an owner. It is asked in `refTypeCheck`
  *after* the target is checked (a generic instance, a name, `Self`), because
  the permission of a type in a field or a signature is settled before its
  target is known otherwise; the provisional default is replaced and its use node
  checked. A managed reference type's lowering (`fnCallLowerManagedRef`) therefore
  leaves an unwritten permission `unknownType` where it used to write `uni`.
  `allocateValueCheck` asks it for the retired `+` spelling, and `borrowTypeCheck`
  for `&x`, falling back to the ordinary default where the place cannot be
  borrowed as `imm` (a `mut` variable, a `ro` reference), so reborrowing
  something already held stays possible. An `imm` owner may be aliased, so
  `refAdoptInfections` leaves it a copy type: `Rc[str]` copies where `Rc[T]`
  moves. The autoborrow of a receiver (`fnCallBorrowReceiver`) tries `imm`
  between `ro` and `mut` for such a type, since its `self &` is `&imm` and `ro`
  does not coerce to it.
- **`refImmutableBan(node, perm, target)`, the ban on mutation.** `mut`, `mut1`
  and a lock permission written on a reference to the target are
  `ErrorImmutableWrite`, which names `Immutable`: in `refTypeCheck` for a
  reference type (in a signature, a field, an alias, a generic's argument, `new`'s
  type), and in `borrowTypeCheck` for `&mut *v`. A refused permission stands as
  `imm` afterwards so that its uses check quietly. **`uni` is not refused**: a
  `final`'s `self` is `&uni` (structSetDropFn, and the check that a `final` takes
  it), the drop the compiler builds takes one, and a unique owner of a `str`
  (`So[uni, str]`) is only unique, there being nothing to write through one.
  What an `Immutable` struct's own methods write through a `uni` receiver, or
  through a variable's name, is not checked: the marker is trusted as `Sendable`
  is. Slices (`&Array[T]` of an Immutable `T`), raw pointers and virtual references to
  a trait declaring it take no part in either rule.

## Hazards

- **A permission is not always a `PermNode`.** A lock permission is a struct,
  and a guard's is a `PermNode` whose `lock` makes it take the lock's room in
  the header. `permGetFlags`, `permMatches` and `permIsSame` answer for all
  three; code reading `PermNode.permflags` directly, or treating every
  `PermTag` as zero-sized, misreads them.
- **Sendable is also safe to read from several threads.** An `Arc[imm, T]` owner
  crosses when its pointee does, so a type granted or declaring `Sendable` must
  also bear being read through `&` from several threads at once. Nothing Cone
  can write mutates through `imm` except an atomic value, so the grant holds; a
  type declaring `Sendable` over raw pointers promises it too (Rust's `Send`
  and `Sync` are one question here).
- **A borrowed reference's inferred type has `typeinfo == NULL`.** The borrow
  path and the allocate path have different invariants for the same field.
  Anything reading `typeinfo` off an arbitrary reference type crashes on borrows
  only.
- **A `RefTag` is not always one pointer.** Where `refIsFat` holds its value is
  `{ptr, usize}`, so a null test, a header computed from the value or a GEP of
  it must take word 0 first (`genlRefPtr`); a place that asks the LLVM type of
  a `RefTag` for a pointer is wrong for it. `str` has no dereference worth
  loading: `*r` of a fat reference is refused where it would be held, and a
  borrow of it is the reference.
- **A virtual reference type has no `typeinfo`.** `genlRegionHeader` reads the
  header's offset from it, so anything reaching a region method through an
  owning virtual reference goes through `genlOwnerHeader`, which reads the
  alignment from the vtable's record instead.
- **Coming from Rust:** `&mut T` is invariant and `&ro T` covariant; `uni` is
  not `&mut` but the *unique* permission, which is what makes owning references
  move; lifetimes are a block-nesting integer that is not part of type identity
  and is checked at two sites, the loan walk following the rest, a name on a signature's reference being no
  generic parameter but a label the loan walk compares by identity; and there
  is no borrow checker.

## What lives elsewhere

- The model these tags implement — the three axes, and what each permits: [References and Regions](../../../../doc/design/references-and-regions.md)
- Moves, counting, and what the count counts: [Flow Analysis](../phases/flow.md)
- The allocation header and pointer levels: [Generation](../phases/generation.md)
- What a borrow's type check establishes, in context: [Type Check Reasoning](../phases/type-check-reasoning.md)
