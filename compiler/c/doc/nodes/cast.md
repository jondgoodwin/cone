`CastNode` serves two syntaxes, a bound pattern's conversion, two injected
forms, and a match's value alone until type check decides what it is. All six
share the struct; the tag and a flag tell them apart.

**At a glance.** Built by `parseCast` from `as`, by `parseCmp` from
`is`, by `parsefnflow.c` for patterns, and injected by `iexpCoerce` whenever a
coercion needs a node. Name resolution walks both children. Type check binds a
pattern's bare name against the matched value, then decides whether the
conversion is permitted at all. Generation emits the instruction — picking by **LLVM type
kind**, not by Cone tag.

*Provenance: read from source.*

## Shape

| Field | Meaning |
| --- | --- |
| `exp` | the value being converted or tested |
| `typ` | the target type, or the type being tested against |
| `vtype` | `typ` for a cast; `bool` for `is` |
| `variants` | an `is` test of a trait pattern: the variants of the matched enum that comply, once checked; else NULL ([Trait patterns](#trait-patterns)) |

Six forms:

| Source | Tag / flag | Built by | Means |
| --- | --- | --- | --- |
| `x as T` | `CastTag` | `newRecastNode` | reinterpret the bits |
| `x is T` | `IsTag` | `newIsNode` | is this the runtime type? |
| `case imm c &Circle` | `CastTag` + `FlagConvert` + `FlagMatchBind` | `newConvCastNode` from `parseBoundMatch` | the matched value, narrowed, that initializes `c` |
| *(injected)* | `CastTag` | `newRecastNode` from `iexpCoerce` | a `CastSubtype` coercion |
| *(injected)* | `CastTag` + `FlagConvert` | `newConvCastNode` from `iexpCoerce` | a `ConvSubtype` coercion |
| `case v` | `IsTag` + `FlagMatchValue` | `newMatchValueNode` | a value alone as a pattern: `is v` for a variant of the matched enum, else `== v` |

**`FlagConvert` is the whole distinction between a reinterpretation and a
conversion**, and type check may clear it: a reference-to-reference conversion
drops the flag on the spot, because it is a bitcast after all. No operator
builds a conversion. A number converts with its type's `from`, which type check
lowers to a type literal that generation hands to the same `genlConvert`
([literals](literals.md)), and a reference narrows to a variant only through a
pattern, whose `is` test checks the variant first.

**A bound pattern desugars to two of these sharing one `typ`**: `case imm c
&Circle` is an `is` test and a conversion (`FlagMatchBind`) that initializes
`c`. The variable itself declares no type and takes the conversion's, because a
clone copies `typ` once per holder and a third copy would be left unbound (see
"Type check").

**A bound pattern to a variant declared as a type binds its contents.**
`castBindProject`, which `varDclTypeCheck` calls for a variable initialized by a
`FlagMatchBind` conversion, checks the conversion once (`FlagMatchChecked`: a
second check would find the reference conversion already made and read it as a
reinterpretation) and looks at what it narrows to. For a variant declared as a
type it declares a hidden local, `-bound` (`matchAliasName`), initialized by the
conversion, before the variable (`varDclHoistBefore`, the temporaries' hoist, so
`blockHoist` puts it ahead of the statement), and gives the variable the
contents: the local's payload field, or, where the conversion narrowed a borrowed
reference, a borrow of that field with the reference's permission. The local is a
match binding like any other (`flowMatchBound`), the matched value under the
variant's name, so the payload moves out of it by the sole-field rule
(`flowTakesSoleField`). A reference into a managed value is refused
(`ErrorTypeVariant`): its contents cannot be lent as a binding yet.

An injected cast over a **borrowed reference** is typed with a copy of the
target reference type carrying the source borrow's scope, not with the declared
type node itself, which is interned and shared and holds no lifetime. That is
what lets flow check a `&<Trait`, or a reference widened to a base trait's,
against the value it was borrowed from.

## Parse

`parseCast` sits between `parseMult` and `parsePrefix` in the precedence
cascade, so a cast binds tighter than any binary operator and looser than a
prefix one. Casts chain left to right: `p as *T as usize` casts `p as *T`.
**`into` is retired**, and stays a keyword only to be refused: `parseCast`
reports `ErrorInto` at the word (`parseRetiredInto`), naming `T.from(x)` for a
value and a `match` or bound `if` for a reference, and reads the type after it,
if one is written, so the rest of the expression parses; nothing is built for
it. `is` is **a keyword**, not an operator symbol, and `parseCmp`
handles it at comparison precedence. `parsefnflow.c` also builds `IsTag` nodes
when desugaring `match` arms and bound patterns, and for each clause of a
generic's `where` clause, `T is Integer`, which is never type checked or
generated: it is a question asked of a type argument at compile time
([generic](generic.md), "Shape").

**Every pattern's root name is marked** (`castPatternMark`, `FlagPattern`): the
bare name, the referent's under `&` or `&<`, or the callee's where type arguments
are written (`castPatternName` finds it). A path, `Shape.Circle`, has no root and
is taken as written. The mark is what tells the later phases that the name is
looked up in the matched value's enum first. A pattern written as a managed
reference, `+Rc-mut Circle`, is refused here (`ErrorPatManaged`): a pattern names
only the variant.

**A value alone as a pattern is an undecided `is` test.** A value alone means
equality with the matched value, but a bare name may be a variant of the
matched value's enum, and only type check knows that enum. So `parseMatchPattern`
builds `newMatchValueNode`: an `is` node flagged `FlagMatchValue`, the value in
`typ`, positioned on the value. Only a value that is a bare name is marked
`FlagPattern`: marking the root of `&x` would keep it a reference type where it
must become a borrow. Name resolution walks `typ` as it would any `is` test's,
which resolves a value just as well.

**The same word declares nominal conformance**, in `parseStruct`: `struct Gauge is
Meter` asserts of a type what `p is Mobile` asks of a value. The two cannot be
confused. Here `is` is **infix** — `parseCmp` only looks for it once a left
operand has been parsed — so it can never begin an expression, and the only
other leading position is immediately after `case`. A type declaration's header
parses no expression at all, so the `is` that opens a base list is reached from
nowhere an expression could be. See [struct](struct.md).

## Name resolution

`castNameRes` walks `exp` and `typ`, except a bound pattern's conversion
(`FlagMatchBind`), which walks only `exp`: its `typ` is the `is` test's, already
resolved there, and a second resolution is not idempotent — a reference whose
referent is a value has become a borrow, which has no name resolution arm. A
path is the exception to "shared": `fnCallNameResPath` collapses `Shape.Circle`
by replacing the slot that held it, and only the `is` test's slot is replaced.
The conversion, still holding the hop, takes its member, which is the name the
path collapsed to.

A marked root with no lexical meaning is left unbound rather than reported
(`nameUseNameRes`), and `refNameRes` keeps a reference over a marked root a
reference type, whatever the name means for now.

## Type check

### `castPatternBind`

`castIsTypeCheck`, and `castTypeCheck` for a bound pattern's conversion
(`FlagMatchBind`), call it after checking `exp` and before checking `typ`. For a marked root it clears the mark and binds the name:

1. a variant of that name in the matched value's enum — `exp`'s type, through a
   reference — found in the enum's `derived` list, not its namespace: an instance
   of a generic enum lists its instantiated variants there (`genericMemoize`),
   which is what supplies omitted type arguments, and an extension lists its
   base's beside its own. Skipped when type arguments are written, since the
   list holds instances;
2. otherwise the lexical binding name resolution made;
3. otherwise nothing: `ErrorPatArgs` if the matched enum has the variant and
   arguments were written, else `ErrorUnkName`. A lexical binding to a value is
   `ErrorNotType`. Either way the name is bound to `errorType` and the check
   returns — the conversion's `vtype` becomes `errorType`, so the variable the
   pattern declares is silenced too.

**The matched value supplies the reference** (`castPatternAdoptRef`, run by
`castPatternBind` once the name is bound). A pattern that names a type, not
written as a reference, against a matched value that is a reference of any kind
(a borrow, an owner into a managed region, a virtual reference) becomes a
reference to that type with the matched value's region and permission:
`case imm c Circle` on an `&imm Shape` narrows to `&imm Circle`, on an
`Rc[mut, Shape]` to `Rc[mut, Circle]`. The `is` test and the conversion each wrap
their own copy of the pattern, around the one shared name. A pattern that is
already a reference, `&Circle`, or an alias of one, is left as written: it is the
check, and `castIsTypeCheck` refuses it where the matched value is no reference,
is in another region, or has a weaker permission.

**Only the `is` test reports.** In source the two nodes share `typ`, so the `is`
test, checked first, binds it for both and the conversion finds the mark
cleared. A clone — a generic instance's body, a default method's copy — has
copied `typ` once per node, and then the conversion binds its own copy to the
same answer, quietly.

### `castMatchValueTypeCheck`

`inodeTypeCheck` sends an `is` node flagged `FlagMatchValue` here, with the slot
that holds it, and the flag is cleared first. A value that is a bare name
(`FlagPattern`) is asked of the matched value's enum, through a reference, as
`castPatternBind` asks: a variant of that name makes the node the `is` test it
already is, and `castIsTypeCheck` binds and checks it, so `case Circle` is
`case is Circle` in every respect, exhaustiveness and a reference matched
included. Otherwise the mark is cleared and the name keeps its lexical meaning,
or, having none, is `ErrorUnkName`. A value that is then a type is no value to
compare with, `ErrorPatType`, whose message says to write `is`, and `typ` becomes
`errorType` — except a variant of an enum named through it, a path such as
`Shape.Circle`, which is the `is` test the node already is, as the bare variant
is. A bare name bound to another type, `i32`, `Some[i32]`, is refused. Anything else replaces the node in its slot with
`matched == value`, an operator call positioned on the value, and checks that
call, so a value alone reports exactly what `case ==value` would.

### `castTypeCheck`

Check both children, set `vtype` from `typ`, then split on `FlagConvert`.

**Reinterpret** (`as`) requires **identical bit size** via `castBitsize` — except
to a struct, which is not checked here at all, because `castBitsize` knows
nothing of field layout, padding or alignment. That check is deferred to
generation, where the data layout exists.

**Between two references, `as` may only be a legal coercion of the permission**
(`castRefPermCheck`, before the size test; any kind of reference counts: a
borrow, a slice, an owner, a virtual reference). The target's permission must be
the source's or a supertype of it, which is the question an implicit coercion
asks (`permMatches`): `uni` to `mut`, `imm`, `&`; `mut` and `imm` to `&`; the
same to the same. Anything else, `&` to `&mut`, `&imm` to `&uni`, `&mut` to
`&uni`, is `ErrorCastPerm`, whose message names both permissions and says it
needs `trust`, which is not built yet. Reinterpretation is trusted
([safe-trust.html](../../../../doc/manual/safe-trust.html#reinterpret), "Reinterpretations that need trust"),
but a reference's permission is what flow, the loan walk and
the checks of `parallel each` rest on, so a cast that changed it would be a way
round every one (`&mut` as `&uni` slips past `ErrorParRoot`; `&` as `&mut`
writes through a read-only view). **A raw pointer `as` a reference is not asked**
(it is how a collection, an actor or the collector hands out memory it manages:
`(ptr + i) as &mut T`), nor is a reference `as` a pointer, and a library that
needs a `&uni` slice of memory it owns calls `mem.sliceFromPartsUni`, not a cast.
**One case is let through for now: a `ro` source made the `imm` a type
declaring `Immutable` gives a reference that writes no permission**, which is how
a slice of bytes is made text (`bytes as &str`, `p as Uni[str]`, documented in
lib-collections.html). It is keyed on the target being *defaulted*, not written:
`refTypeCheck` sets `RefNode.permImmDefault` when it replaces the unwritten
permission with `refImmutableDefaultPerm`, and `castRefPermCheck` asks it of the
target. A permission written out (`as &imm P` from a `&`, `bytes as &imm str`,
`Uni[imm, str]`) is checked as any other and is `ErrorCastPerm`; so is `&mut` as
`&imm`. The name `str` appears nowhere in the test. The cast promises that the
bytes are text and that nothing changes them while the text is used, neither of
which is checked, so it is for `trust` to guard too once that is built.

**`usize` and `isize` are pointer-sized**, and a pointer's width is the
target's, so no reinterpretation joins either of them to a fixed-width number
(`i64`, `u32`, `f64`, ...) in either direction, even where the widths agree:
`ErrorPtrSizedAs`, whose message names the conversion, `usize.from(x)` or
`u64.from(x)`. That test comes before the size test, so what compiles for x64
compiles for wasm32. The size test then decides the rest the same on every
target, through the sentinel width `castBitsize` gives `usize`, `isize`, a
pointer and a reference (twice it for a slice): `usize` and `isize` reinterpret
as each other, keeping the bits (`-1isize as usize` is all ones), and as a raw
pointer or a reference and back; a slice reinterprets as neither
(`ErrorInvType`). Generation picks `ptrtoint`, `inttoptr` or a bitcast by the
LLVM kinds, so it needs nothing of its own for them.

**A slice `as` a pointer to its own element type is the one conversion `as`
makes** (`xs as *u8`): a slice is two words, so it cannot be a reinterpretation, and
`castTypeCheck` turns the node into a conversion (`FlagConvert`) before the size
test, which `genlConvert` generates as the extraction of the slice's pointer word.
It is what the implicit slice-to-pointer coercion used to build, asked for:
`itypeMatches` no longer accepts a slice for a pointer, and `iexpCoerce` reports
the attempt (`iexpCPtrMismatch`, `ErrorCPtrConv`) in the way it reports a `bool`
for a number, building the conversion anyway so that nothing more is said. The same
code refuses text (`&str`), a `cstr` and a raw pointer where the other is wanted,
each message naming the explicit spelling.

**Convert** is a bound pattern's conversion (`FlagMatchBind`), the only one
type check sees: an injected conversion is built already typed. It permits,
and nothing else:

| To | From |
| --- | --- |
| `RefTag` | `VirtRefTag`, or another `RefTag` |
| struct | a struct carrying `SameSize` |

Anything else is `ErrorInvType`, "Unsupported built-in type conversion",
usually a follow-on to the pattern's `is` test, checked first, refusing the
narrowing, as `enum_typecheck_narrow` pins.

**A reference narrowed from a sum type to a variant** (`RefTag` to `RefTag`,
the from-type's referent a trait with `HasTagField` or `SameSize` — an enum, a
tagged trait, an `Option`-shaped enum — and the to-type's another struct) is a
reference into the value's payload, which a change of variant would reread as
the wrong type. It is the collection rule's case: allowed, and refused only if
something could change the variant while the narrowed reference is used.
`castSumInterior` asks, once the conversion has become a recast:
- the from-reference's permission has `MayIntRefSum` — `uni`, `imm`, `mut1`: no
  other name can change the variant, and nothing more is asked; or
- it is a read-only or `uni` borrow made here of a place reached as `uni`
  (`castBorrowsUni`): the value of the hidden `_` variable a `match` or bound
  `if` captures its scrutinee in is a `BorrowTag` whose place
  (`castUniPlace`) is a non-static variable of this function held by value, a
  field or array element of one (not reached through a reference, slice or
  pointer), or a dereference of a `uni` reference such a place holds. The loan
  walk then freezes that place while the borrow, and the binding that holds its
  loan, are used. A `mut` borrow (`match &mut s`) is no longer such: it is an
  alias loan, the local has other names while it is held, and the narrowing is
  followed by the loan walk as one through a shared path is (flow.md, "A change
  through another name").

Everything else — a parameter of reference type, a reborrow through a shared
path, a field reached through a `mut` reference, a variable the program names
holding a borrow (a copy of it would reach the local another way, which freezing
does not follow) — is a narrowing the loan walk follows: the cast is marked
`FlagVariantNarrow` (`castSumNarrows`), which gates the function for the walk
(`FlowGateShape`, `flowGateNarrow`) and makes `pwNarrowShared` lend the place the
reference is read through to whatever holds the narrowed reference, marking the
loan as the loan of a borrow into a collection is marked (flow.md, "A change through
another name"). A narrowing of an owning reference, an `Rc[imm, T]` narrowed to an `Rc[imm, Variant]`, is
a copy of the owner typed as the variant, allowed only where no writer can store another variant
through any owner (`MayIntRefSum`: `imm`, `uni`, `mut1`): the cast is a recast marked
`FlagOwnerNarrow`, which flow counts as it counts any copy of an owner (a recast is its operand).
No loan stands for the other owners, so an owner whose permission lets one store another variant
(`mut`, `ro`, a lock) is refused (`ErrorBadPerm`). The variant is at its enum's address, but
the allocation's header before it is rounded up to the alignment of the type, which a variant
may have less of: generation asks that the value sits at the same offset in both allocations
(`genlOwnerNarrowCheck`, `ErrorOwnerNarrow`). The `is` test binds
nothing and is not asked. A narrowing from a virtual reference is not asked either.

**Flow typing narrows a reference the same way** (`castNarrowRef`, from narrow.c): a
recast built already typed and marked checked (`FlagMatchChecked`) from a name use to a
reference to the variant, through `castSumInterior`, so a reference others may share is
marked `FlagVariantNarrow` and followed by the loan walk. It initializes the hidden
`-narrow` local ([narrow](narrow.md)).

### Trait patterns

A trait named as a pattern on an enum (`case imm m Measured`, `s is Measured`) matches the
variants that comply with it. It is the same pair, an `is` test and a conversion that binds,
and the same shared `typ`; what differs is what the name is bound to. `castPatternTrait` finds
a trait (a struct with `TraitType`, not an enum) at the root of a pattern, looking through a
written reference, and `castMatchedEnum` says the matched value is an enum, so the pattern
is a trait pattern; where the matched value is not an enum, the downcast checks of
`castIsTypeCheck` refuse it with `ErrorPatTrait` (a closed family alone can say which values
comply).

**Which variants comply** (`castTraitVariants`, `castVariantComplies`): a variant of the
enum's `derived` complies when a generic's `where T is Trait` would say yes of it
(`genericTypeIs`: it declares the trait with `is`, or, for a trait that requires something,
has it) and the variant can fill the trait's vtable (`structVirtRefMatches`, which registers
its implementation). So a marker trait is complied with only by declaring it, and a method over
the whole enum (`self &Shape`, which has no copy in any variant, `structIsWholeEnumMethod`)
makes no variant comply. None at all is `ErrorPatTraitNone`.

**The test.** `castTraitIsCheck` stores the complying variants on the `is` node (`variants`).
Everything that reads an `is` test of a variant reads that list when there is one:
`castIsVariants` marks the variants a test names, which `matchClassify` (exhaustiveness and
unreachable cases, [if](if.md)), `ifExhaustCheck` and the flow-typing facts ([narrow](narrow.md))
use, so a trait case counts as the variants it names and narrows by elimination as they do.
Generation tests the tag against each variant in turn and `or`s them (`genlIsType`,
`genlIsVariant`).

**The binding.** The matched value supplies the reference kind and permission (`castPatternAdoptRef`):
a borrow of an enum gives a virtual reference to the trait, `&<imm Measured` from `&imm Shape`,
with the borrow's region and permission. Only a borrowed reference can lend one, so a binding
where the matched value is a value or an owner is `ErrorPatTraitBind` (`castTraitBind`), as is a
pattern written as a plain reference to the trait (a plain reference to a trait has no vtable
to carry). `&<Measured` is accepted as the check that the matched value agrees, as `&Circle`
is for a variant. The conversion is flagged `FlagConvert | FlagTraitBind`, and its type is the
virtual reference carrying the matched borrow's lifetime (`iexpCoerceType`), as a coercion
from a reference to an enum to another trait's virtual reference has it. It is a reference into
the variant like any narrowing from an enum, so `castSumInterior` asks the collection rule of it:
through a reference others may share, the loan walk follows it (`FlagVariantNarrow`,
`pwNarrowShared`) and refuses a use after a change of variant.

Generation builds the virtual reference as it does for a reference to an enum coerced to a
virtual reference to another trait: the pointer, and the vtable the tag names. That list holds
only the variants that comply, so `FlagTraitBind` lets the selection skip the others
(`genlVtableForTag`'s `partial`); they cannot reach the arm, because the test that guards it
named only the others. The selection is always by comparing the tag, never by index, since the
trait's vtable list is in the order conversions registered their implementers.

### What a binding takes from a place

An arm that binds, `case imm c Circle` or `if imm c Circle = s`, on a place takes the
place's value into a hidden variable of its own (`FlagMatchArm`, made by
`parseBoundMatch`), which the binding's conversion reads. Where the variant is **plain
data** (`castVariantIsPlain`: nothing in it to move, finalize, count or trace),
`castArmCopies` makes that variable hold the variant alone, a conversion of the place
to the variant read from its leading bytes: a copy, and the place is not moved from. An
owner's variant keeps the whole place moving into the variable, as it always has. Flow
reads a conversion as a new value, a temporary, so it could not count or move what a
payload held; that is why only plain data is copied this way.

A guard's binding (`FlagMatchGuardBind`, the second declaration of the variable the guard
`and`s with the pattern) reads the matched value before the arm is chosen, so it takes
nothing from it: `castGuardBorrow` rewrites it, before it is checked, to a conversion of a
read-only borrow of the value to a reference to the variant, wherever the variant is not
plain data; plain data is copied, as an arm copies it. The value is a place, or the
hidden variable that holds a call's answer (or a later column's): both alike, so that a
guard that fails leaves a held payload whole for the next case and for the arm that
binds it (`enum_match_guard_held`). A pattern that names a reference to the variant is
not rewritten: its guard binds the reference, which takes nothing.

### `castIsTypeCheck`

Decides only whether a **downcast specialization** is possible, and needs a
discriminant to do it.

- **Reference to reference** (from a virtual or a plain ref): regions must be
  **identical** — two regions are represented differently in memory, so no
  recast exists between them, not even into a borrow. Then permissions must
  match. Then the pointed-at structs: a virtual reference asks
  `structVirtRefMatches`; a plain one needs `HasTagField` on the source and
  `structMatches` under `Regref`. A virtual reference to an untagged trait may
  not narrow to a trait or an enum: it was made from a concrete type, its vtable
  pointer is compared with that type's, and no vtable is built for a trait or an
  enum, so the comparison had nothing to find and failed at link. (A trait is
  `ErrorPatTrait`: a trait pattern is for an enum, [Trait patterns](#trait-patterns).)
- **Struct to struct**: needs `HasTagField`, then `structMatches` under
  `Coercion`.

Note the variance: **region and permission downcast covariantly while the
structure is contravariant.** Without a tag, "impossible to downcast without a
tag" — there is nothing at runtime to test.

**Narrowing something already concrete is told apart from naming two
incompatible types**, because it is usually not a downcast the author wrote. A
method with a body on an enum is a *default*, cloned into every variant with
`Self` repointed, so inside the copy `self` is one variant and `match self` asks
to narrow a type that is already as narrow as it gets; in the copy for the
variant the pattern names, the two types are one, which is not handed to
`structMatches` (it answers about two distinct declarations). The message names the
variant and its enum, and says to declare the method without a body and implement
it per variant — the shape that dispatches. It is reported once per copy, so an
enum of two variants gives two. `enum_typecheck_narrow` holds this, through a
reference `self` and a by-value one, and the same mistake written directly on a
variant.

## Flow

`flowLoadValue` descends into `exp`, and a cast adds no flow of its own. **A
recast (no `FlagConvert`) is transparent to the hand-over decision**: when one
is moved, counted or returned, flow acts on its operand, because the injected
recast between an enrichment and its base is one value under two type names
(see [flow](../phases/flow.md), "A recast is its operand"). A conversion is not
looked through.

## Generation

Two functions, and both **pick by the generated LLVM type kinds rather than the
Cone tags**, deliberately: a reference is not always a plain pointer once
virtual references and fat pointers are in play.

`genlConvert` (`FlagConvert`, and a number's conversion method, `u64.from(x)`,
which is lowered to a type literal, [literals](literals.md)):
- anything → `bool` is tested **first**, and asks what the `isTrue` intrinsic
  asks of a condition: a ref/ptr `LLVMBuildIsNotNull`, an integer `icmp ne 0`,
  a float `fcmp une 0.0` (0 and -0 false, NaN true). Without that arm, `bool`
  being a 1-bit unsigned would send it down the number path: an integer
  truncated to its low bit (2 false), a float through `fptoui` (0.5 false, NaN
  poison), a pointer truncated.
- numbers: `fptoui`/`fptosi`/`trunc`/`sext`/`zext`/`uitofp`/`sitofp`/
  `fptrunc`/`fpext`. A float whose truncated value the integer type cannot hold
  (a negative float to an unsigned type, 300.0 to a `u8`) is LLVM's poison: the
  value differs between an optimized and an unoptimized build (`u32` from
  -3.7 printed 4294967293 unoptimized and a value past `u32`'s range
  optimized). The checked and saturating conversions that would define it are
  not built.
- struct: alloca-store-bitcast-load, because LLVM does not bitcast structs. The
  alloca is `genlAlloca`, so it lands in the entry block — a mid-block one inside
  a loop is a fresh frame slot per iteration, which mem2reg does not promote, and
  a variant assigned to its enum's variable in a long loop would run the stack
  out (`typemgmt_success`).
- `RefTag` from `VirtRefTag`: `extractvalue 0`, then bitcast.
- `ArrayRefTag` from a ref-to-array: bitcast the pointer, then `insertvalue`
  the pointer and the compile-time dimension into the fat pointer.
- `VirtRefTag`: build the vtable if needed, find the implementation, then
  `{object pointer, vtablep}`. Every pointer is LLVM's `ptr`, so the bitcasts
  here and above fold away as they are built. From a reference to an enum the implementation
  is chosen by the tag; a trait pattern's conversion (`FlagTraitBind`) has an implementation for
  the variants that comply only, and the selection skips the rest.

`genlRecast` (no flag): **re-checks size for a struct or array target** with
`LLVMABISizeOfType` and reports `ErrorRecastSize` — this is the check
`castTypeCheck` could not do — then goes through a stack slot, since LLVM
bitcasts no aggregate. An array target reaches it as an array of variants
wanted as an array of their enum (`Array[None[T], 4]` as `Array[Option[T], 4]`); where
the two arrays are one LLVM type the value passes unchanged. Otherwise
pointer→int is `ptrtoint`, int→pointer is `inttoptr`, everything else is
`bitcast`.

`genlIsType` has three paths: virtual reference (compare vtable pointers),
nullable-pointer enum (compare the pointer against a null of its own type: a
slice's or a virtual reference's first word, never the two-word value), and tagged (read the
`IsTagField` and compare against `tagnbr`). The last two are `genlIsVariant`, one variant's test;
a trait pattern's test (`variants` set) is that test for each variant that complies, `or`ed.

## Hazards

- **`as` and `from` are not interchangeable.** `as` reinterprets and demands
  equal size on every target; a type's `from` converts the value. A reader who
  assumes C's single cast will reach for the wrong one, `x as usize` above all.
- **`FlagConvert` can be cleared during type check**, so the flag on a node
  after checking does not tell you what the author wrote.
- **A pattern's root may be unbound until its `is` test is checked.** Anything
  that reads a pattern's `typ` before then — `ifExhaustCheck` scanning the later
  arms — must ask `castPatternPending` first, and must skip an `is` node still
  flagged `FlagMatchValue`, whose `typ` is a value until it is checked and may
  never be a type. And a pattern whose name was
  bound to nothing (`castPatternBind` reported it) is never type checked, so a
  pattern written with arguments is still the unchecked call node: ask
  `isTypeNode` before unwrapping one.
- **A struct reinterpret is checked in generation, not type check.** A size
  mismatch surfaces late, as `ErrorRecastSize`.
- **`genlConvert`'s two "unknown source" arms report `ErrorUnreachable` and
  exit.** Reaching either means a conversion was built, by a pattern or a
  coercion, that generation has no arm for.

## What lives elsewhere

- Which verdict makes `iexpCoerce` inject which of these: [Type Check Reasoning](../phases/type-check-reasoning.md), "Coercion"
- What `as`, a pattern's conversion and `is` permit, in one table: [Type Check Reasoning](../phases/type-check-reasoning.md), "Casts and `is`"
- Pointer levels and why generation picks by LLVM kind: [Generation](../phases/generation.md), "Pointer levels"
