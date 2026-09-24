`CastNode` serves three syntaxes and two injected forms. All five share the
struct; the tag and one flag tell them apart.

**At a glance.** Built by `parseCast` from `as` and `into`, by `parseCmp` from
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
| `vtype` | `typ` for a cast; `Bool` for `is` |

Five forms:

| Source | Tag / flag | Built by | Means |
| --- | --- | --- | --- |
| `x as T` | `CastTag` | `newRecastNode` | reinterpret the bits |
| `x into T` | `CastTag` + `FlagConvert` | `newConvCastNode` | convert the value |
| `x is T` | `IsTag` | `newIsNode` | is this the runtime type? |
| *(injected)* | `CastTag` | `newRecastNode` from `iexpCoerce` | a `CastSubtype` coercion |
| *(injected)* | `CastTag` + `FlagConvert` | `newConvCastNode` from `iexpCoerce` | a `ConvSubtype` coercion |

**`FlagConvert` is the whole distinction between `as` and `into`**, and type
check may clear it: a reference-to-reference conversion drops the flag on the
spot, because it is a bitcast after all.

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
prefix one. `is` is **a keyword**, not an operator symbol, and `parseCmp`
handles it at comparison precedence. `parsefnflow.c` also builds `IsTag` nodes
when desugaring `match` arms and bound patterns.

**Every pattern's root name is marked** (`castPatternMark`, `FlagPattern`): the
bare name, the referent's under `&` or `&<`, or the callee's where type arguments
are written (`castPatternName` finds it). A path, `Shape.Circle`, has no root and
is taken as written. The mark is what tells the later phases that the name is
looked up in the matched value's enum first.

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

### `castTypeCheck`

Check both children, set `vtype` from `typ`, then split on `FlagConvert`.

**Reinterpret** (`as`) requires **identical bit size** via `castBitsize` — except
to a struct, which is not checked here at all, because `castBitsize` knows
nothing of field layout, padding or alignment. That check is deferred to
generation, where the data layout exists.

**Convert** (`into`) permits, and nothing else:

| To | From |
| --- | --- |
| `Bool` | anything `castConvertsToBool` allows — numbers, refs, pointers |
| number | number |
| `RefTag` | `VirtRefTag`, or another `RefTag` |
| `PtrTag` | `RefTag` or `PtrTag` |
| `VirtRefTag` | — accepted unconditionally here; generation does the work |
| struct | a struct carrying `SameSize` |

A slice deliberately does **not** convert to an integer: the length and the data
address are both candidates and both are spelled better already, as `s.len` and
`p into usize`. Everything else is `ErrorInvType`.

**A pointer does not convert to a reference**, and the table above is what the
code does rather than what it means to do. A reference carries a region, a
permission and a lifetime and a raw pointer supplies none of them, so there is
no value to construct. `genlConvert` has no arm for it either, so anything the
table let through would reach generation with nothing to emit. `p as &i32` is
the spelling that keeps the bits.

### `castIsTypeCheck`

Decides only whether a **downcast specialization** is possible, and needs a
discriminant to do it.

- **Reference to reference** (from a virtual or a plain ref): regions must be
  **identical** — two regions are represented differently in memory, so no
  recast exists between them, not even into a borrow. Then permissions must
  match. Then the pointed-at structs: a virtual reference asks
  `structVirtRefMatches`; a plain one needs `HasTagField` on the source and
  `structMatches` under `Regref`.
- **Struct to struct**: needs `HasTagField`, then `structMatches` under
  `Coercion`.

Note the variance: **region and permission downcast covariantly while the
structure is contravariant.** Without a tag, "impossible to downcast without a
tag" — there is nothing at runtime to test.

**Narrowing something already concrete is told apart from naming two
incompatible types**, because it is usually not a downcast the author wrote. A
method with a body on an enum is a *default*, cloned into every variant with
`Self` repointed, so inside the copy `self` is one variant and `match self` asks
to narrow a type that is already as narrow as it gets. The message names the
variant and its enum, and says to declare the method without a body and implement
it per variant — the shape that dispatches. It is reported once per copy, so an
enum of two variants gives two. `enum_typecheck_narrow` holds both this and the
same mistake written directly on a variant.

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

`genlConvert` (`FlagConvert`):
- ref/ptr → `Bool` is tested **first**, as `LLVMBuildIsNotNull`. Without that
  arm, `Bool` being a 1-bit unsigned would send it down the number path and
  emit a truncation of a pointer.
- numbers: `fptoui`/`fptosi`/`trunc`/`sext`/`zext`/`uitofp`/`sitofp`/
  `fptrunc`/`fpext`.
- struct: alloca-store-bitcast-load, because LLVM does not bitcast structs. The
  alloca is `genlAlloca`, so it lands in the entry block — a mid-block one inside
  a loop is a fresh frame slot per iteration, which mem2reg does not promote, and
  `x into <struct>` in a long loop ran the stack out.
- `RefTag` from `VirtRefTag`: `extractvalue 0`, then bitcast.
- `ArrayRefTag` from a ref-to-array: bitcast the pointer, then `insertvalue`
  the pointer and the compile-time dimension into the fat pointer.
- `VirtRefTag`: build the vtable if needed, find the implementation, then
  `{bitcast to i8*, vtablep}`.

`genlRecast` (no flag): **re-checks size for a struct target** with
`LLVMABISizeOfType` and reports `ErrorRecastSize` — this is the check
`castTypeCheck` could not do. Otherwise pointer→int is `ptrtoint`, int→pointer
is `inttoptr`, everything else is `bitcast`.

`genlIsType` has three paths: virtual reference (compare vtable pointers),
nullable-pointer enum (compare against null), and tagged (read the
`IsTagField` and compare against `tagnbr`).

## Hazards

- **`as` and `into` are not interchangeable.** `as` reinterprets and demands
  equal size; `into` converts values. A reader who assumes C's single cast will
  reach for the wrong one.
- **`FlagConvert` can be cleared during type check**, so the flag on a node
  after checking does not tell you what the author wrote.
- **A pattern's root may be unbound until its `is` test is checked.** Anything
  that reads a pattern's `typ` before then — `ifExhaustCheck` scanning the later
  arms — must ask `castPatternPending` first.
- **A struct reinterpret is checked in generation, not type check.** A size
  mismatch surfaces late, as `ErrorRecastSize`.
- **`genlConvert`'s two "unknown source" arms report `ErrorUnreachable` and
  exit.** Reaching either means the conversion table above accepted something
  generation has no arm for.

## What lives elsewhere

- Which verdict makes `iexpCoerce` inject which of these: [Type Check Reasoning](../phases/type-check-reasoning.md), "Coercion"
- What `as`/`into`/`is` permit, in one table: [Type Check Reasoning](../phases/type-check-reasoning.md), "Casts and `is`"
- Pointer levels and why generation picks by LLVM kind: [Generation](../phases/generation.md), "Pointer levels"
