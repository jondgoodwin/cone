An **intrinsic** is a function whose value is an `IntrinsicNode` in place of a
body: the compiler supplies what it does, and generation expands it at each call
rather than calling anything (`genlFnCallInternal`). There are two kinds, and
they differ in who declares them and in how their meaning is decided.

| | Built in C | Declared in Cone with `@intrinsic` |
| --- | --- | --- |
| Declared by | `corenumber.c`, `corelib.c`, `struct.c` (an enum's `==`) | `packages/core/src/core.cone` |
| Kinds | `NegIntrinsic` … `FinalAllIntrinsic` | `SizeofIntrinsic` onward (`FirstDeclaredIntrinsic`) |
| Meaning decided | at generation, by the LLVM type kind of argument 0 | by the registry, in Cone terms; the Cone type rides on the node (`typearg`) |
| Reference page | none: the number and pointer methods | `doc/reference/refintrinsic.html` |

The kinds built in C are left as they are until the number types are rebuilt
over generics; nothing new is to be added to them. **A new intrinsic is declared
in core, entered in the registry, and given an arm in `genlDeclaredIntrinsic`.**

*Provenance: read from source and measured, September 2026.*

## Principles — [derived]

**The registry is the definition of record, and it speaks Cone.** Each entry in
`intrinsicRegistry` (`ir/stmt/intrinsic.c`) is a name, its signature as shapes
over the one type parameter `T`, whether a call can break memory safety (and so
belongs in `trust`), whether a Cone fallback body may be written, the phase that
answers it, and whether this back end lowers it itself. No LLVM name appears in
it. ▸ **Forbids** passing an LLVM intrinsic name through (`@intrinsic("llvm.…")`)
and deciding a new intrinsic's meaning from an LLVM type: the LLVM instructions
in `genlDeclaredIntrinsic` are one implementation of the entry, which a native
generator would implement again from the same entry and the reference page
[Jon 26 Sep: "capabilities we would be proud to build in the native code
generator"].

**An intrinsic never has a symbol.** Its lowering is expanded at the call, and so
is its fallback body, which is compiled as an inline function. ▸ **Settles**
`fnDclIsExpanded` (true for `DclIntrinsic`, so the include file keeps the
declaration whole), the refusal of `extern` and `@c` beside `@intrinsic`, and the
refusal of a borrowed reference to one (`ErrorInlineRef`).

**A fallback body must mean what the entry means.** It is what a back end with no
lowering runs, and `--intrinsic-fallback` runs it in place of the lowering, so
the two are tested against each other by running one scenario both ways
(`intrinsic_success`). An entry nothing in Cone can express (`sizeof`,
`finalize`, `writeRaw`) refuses a body.

## The life of a declared intrinsic

1. **Parse** (`parseFn`). `@intrinsic` after `fn`, before or after `@c` and
   `@initpure`, sets `DclIntrinsic` and lets the declaration end without a body.
   `@c` beside it is `ErrorCAttr`; `extern` is `ErrorBadExtern`
   (`parseExternFnCheck`). `dclInfoJoin` keeps the fact.
2. **Name resolution** (`fnDclNameRes`, then `intrinsicDclNameRes`), once the
   signature and any body are resolved:
   - the declaration must sit directly in a root module named `core`
     (`intrinsicInCore`), else `ErrorIntrinsicPlace`;
   - its name must be in the registry, else `ErrorIntrinsicName`;
   - its signature must match the entry's shapes, else `ErrorIntrinsicSig`. In a
     generic template `*T` is still a `DerefTag` and `&[]T` an `ArrayBorrowTag`,
     because a type parameter is not yet a type (`cloneStarNode`,
     `cloneRefNode`), so both spellings are accepted;
   - a body the entry allows no fallback for, or no body where there is no
     lowering, is `ErrorIntrinsicBody`.
   A declaration that passes either keeps its body as an **inline** function
   (forced fallback, or no lowering), or has its value replaced by an
   `IntrinsicNode` whose `typearg` is a use of the type parameter.
3. **Instantiation.** Cloning a generic intrinsic clones the node
   (`cloneIntrinsicNode`), and the use of `T` is substituted like any other, so
   each instance carries the Cone type it is for.
4. **Type check** (`fnDclTypeCheck` → `intrinsicDclTypeCheck`). A declared
   intrinsic's instance has no body to check; its `typearg` is type checked and
   must have a size (`itypeNoSizeCause`), else `ErrorIntrinsicType`, reported at
   the call that instantiated it (`instnode`), not in core.
5. **Flow** sees an ordinary call: an argument passed by value is moved into it,
   as `writeRaw`'s value is.
6. **Generation** (`genlDeclaredIntrinsic`), dispatched by kind before the C-built
   kinds' LLVM-type switch.

## Each kind, and how LLVM implements it

| Kind | Phase | LLVM implementation |
| --- | --- | --- |
| `sizeof[T]` | constant | `LLVMABISizeOfType` (alloc size, tail padding included), `genlSizeof` |
| `alignof[T]` | constant | `LLVMABIAlignmentOfType`, `genlAlignof` |
| `needsFinal[T]` | constant | `itypeNeedsFinal`, a front-end question, emitted as an `i1` |
| `finalize[T]` | expansion | `genlFinalizeAt`: release an owning reference; else the type's drop, then `genlReleaseFlds` |
| `sliceFromParts[T]`, `…Mut` | expansion | two `insertvalue`s into the `{T*, usize}` pair |
| `readRaw[T]` | expansion | a load (`%rawread`) |
| `writeRaw[T]` | expansion | a store |
| `moveRaw[T]` | operation | `LLVMBuildMemMove` of `count * sizeof(T)` bytes |

`finalize` runs what a region-held value's death runs, less the region's `free`
(`genlRegionDeath`): the drop, then the owners the fields hold. `itypeNeedsFinal`
is true exactly when that does something.

## Hazards

- **The constants are the target layout's, read at generation.** `sizeof` and
  `alignof` are constants in the IR, but the type checker cannot fold them: Cone
  does not compute layout itself, so neither can be an array length yet.
- **`trust` is recorded, not enforced.** The registry marks `finalize`, both
  slice constructors and the three raw operations; `trust` does not exist
  (`doc/design/safety.md`).
- **Type arguments are not inferred through a pointer.** `finalize(p)` with `p`
  a `*Fin` cannot infer `T` (`genericInferFnParms` captures only a parameter
  whose type is `T` itself), so every call names it: `finalize[Fin](p)`.
- **Core's public names are reserved in every module.** A module declaring a
  function named like a core intrinsic is `ErrorDupName`, as for `malloc`.

## Where it lives

| Step | File, function |
| --- | --- |
| lexer keyword | `lexer.c`, `IntrinsicAttrToken` |
| parse | `parsefnflow.c` `parseFn`; `parsemod.c` `parseExternFnCheck` |
| registry and checks | `ir/stmt/intrinsic.c`: `intrinsicRegistry`, `intrinsicDclNameRes`, `intrinsicDclTypeCheck` |
| hooks | `fndcl.c` `fnDclNameRes`, `fnDclTypeCheck`, `fnDclIsExpanded` |
| forced fallback | `--intrinsic-fallback` → `intrinsicForceFallback` (`conec.c`) |
| generation | `genlexpr.c` `genlDeclaredIntrinsic`; `genlalloc.c` `genlFinalizeAt`, `genlReleaseFlds`; `genltype.c` `genlAlignof` |
| declarations | `packages/core/src/core.cone` |
| tests | `test/cases/intrinsic/` |
