`CastNode` serves three syntaxes and two injected forms. All five share the
struct; the tag and one flag tell them apart.

**At a glance.** Built by `parseCast` from `as` and `into`, by `parseCmp` from
`is`, and injected by `iexpCoerce` whenever a coercion needs a node. Name
resolution walks both children. Type check decides whether the conversion is
permitted at all. Generation emits the instruction — picking by **LLVM type
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

## Parse

`parseCast` sits between `parseMult` and `parsePrefix` in the precedence
cascade, so a cast binds tighter than any binary operator and looser than a
prefix one. `is` is **a keyword**, not an operator symbol, and `parseCmp`
handles it at comparison precedence. `parsefnflow.c` also builds `IsTag` nodes
when desugaring `match` arms and bound patterns.

## Name resolution

`castNameRes` walks `exp` and `typ`. Nothing else.

## Type check

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
no value to construct. This used to be accepted by a fall-through from the
reference row into the pointer row, and `genlConvert` has no arm for it: the
assert saying so was compiled out of the Release build and `p into &i32` died on
a null LLVM value. `p as &i32` is the spelling that keeps the bits.

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

## Flow

Nothing. `flowLoadValue` descends into `exp` and that is all — a cast neither
moves nor counts.

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
nullable-pointer union (compare against null), and tagged (read the
`IsTagField` and compare against `tagnbr`).

## Hazards

- **`as` and `into` are not interchangeable.** `as` reinterprets and demands
  equal size; `into` converts values. A reader who assumes C's single cast will
  reach for the wrong one.
- **`FlagConvert` can be cleared during type check**, so the flag on a node
  after checking does not tell you what the author wrote.
- **A struct reinterpret is checked in generation, not type check.** A size
  mismatch surfaces late, as `ErrorRecastSize`.
- **`genlConvert`'s two "unknown source" arms now report `ErrorUnreachable` and
  exit.** Reaching either means the conversion table above accepted something
  generation has no arm for, which is how `p into &i32` used to crash.

## What lives elsewhere

- Which verdict makes `iexpCoerce` inject which of these: [Type Check Reasoning](../phases/type-check-reasoning.md), "Coercion"
- What `as`/`into`/`is` permit, in one table: [Type Check Reasoning](../phases/type-check-reasoning.md), "Casts and `is`"
- Pointer levels and why generation picks by LLVM kind: [Generation](../phases/generation.md), "Pointer levels"
