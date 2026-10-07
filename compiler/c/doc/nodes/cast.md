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
looked up in the matched value's enum first.

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
or, having none, is `ErrorUnkName`. A value that is then a type — a bare name
bound to one, a path such as `Shape.Circle`, `i32`, `Some[i32]` — is no value
to compare with, `ErrorPatType`, whose message says to write `is`; `typ`
becomes `errorType`. Anything else replaces the node in its slot with
`matched == value`, an operator call positioned on the value, and checks that
call, so a value alone reports exactly what `case ==value` would.

### `castTypeCheck`

Check both children, set `vtype` from `typ`, then split on `FlagConvert`.

**Reinterpret** (`as`) requires **identical bit size** via `castBitsize` — except
to a struct, which is not checked here at all, because `castBitsize` knows
nothing of field layout, padding or alignment. That check is deferred to
generation, where the data layout exists.

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
the wrong type (Jon's 2018 rule: for `mut` references to shape-changing types,
no interior references). `castSumInterior` allows it when nothing can change
the variant while the narrowed reference is used, and refuses it
(`ErrorBadPerm`) otherwise:
- the from-reference's permission has `MayIntRefSum` — `uni`, `imm`, `mut1`; or
- it is a borrow made here of a place reached as `uni` (`castBorrowsUni`): the
  value of the hidden `_` variable a `match` or bound `if` captures its
  scrutinee in is a `BorrowTag` whose place
  (`castUniPlace`) is a non-static variable of this function held by value, a
  field or array element of one (not reached through a reference, slice or
  pointer), or a dereference of a `uni` reference such a place holds. The loan
  walk then freezes that place while the borrow, and the binding that holds its
  loan, are used.

Everything else — a parameter of reference type, a reborrow through a shared
path, a field reached through a `mut` reference or a `Rc[mut, T]` owner, a variable
the program names holding a borrow (a copy of it would reach the local another
way, which freezing does not follow) — is refused. The `is` test binds nothing
and is not asked. A narrowing from a virtual reference is not asked either.

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
  enum, so the comparison had nothing to find and failed at link.
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
  here and above fold away as they are built.

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
`IsTagField` and compare against `tagnbr`).

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
