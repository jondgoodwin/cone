An **intrinsic** is a function whose value is an `IntrinsicNode` in place of a
body: the compiler supplies what it does, and generation expands it at each call
rather than calling anything (`genlFnCallInternal`). There are two kinds, and
they differ in who declares them and in how their meaning is decided.

| | Built in C | Declared in Cone with `@intrinsic` |
| --- | --- | --- |
| Declared by | `corenumber.c`, `corelib.c`, `struct.c` (an enum's `==`) | `packages/core/src/core.cone`, as functions of the opaque struct `mem` |
| Named | as a method or operator of a type | through `mem`: `mem.sizeof[T]()` |
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

**Core folds one name into every module for them, not one per intrinsic.** The
core package is every module's prelude, and its public names are folded into
each module, where a module may not declare the same name (`ErrorDupName`). So
core declares the intrinsics inside one public name, `mem`, and a module may
declare its own `finalize` or `sizeof`. `mem` is meant to be a submodule,
`core.mem`, and is a struct today only because a submodule of core cannot be
reached (below, Hazards); the path a call writes, `mem.sizeof[T]()`, is the one
a submodule would give, so moving it changes no call.

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
   - the declaration must be of the core package (`intrinsicInCore`): a function
     of a module whose root module, having no owner, is named `core`, or a
     function taking no `self` of a plain struct such a module declares (not a
     generic type, whose functions are cloned per instance, nor a trait); else
     `ErrorIntrinsicPlace`;
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
| `sliceFromParts[T]`, `…Mut` | expansion | two `insertvalue`s into the `{ptr, usize}` pair |
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
- **`mem` is a struct because a submodule of core cannot be reached**, measured:
  a direct compile finds core on the package search path as the one file
  `core/src/core.cone`, and the sweep reads no file beside it (its folder is
  `src`, not `core`), so a submodule file there is never read; under Congo, core's
  include file writes a submodule only as a private, pruned block of what the
  root reaches ([Module](module.md), "Generating the include file"), so no
  importer can name `core.mem`; and no module can name `core` itself (the
  prelude import binds no name, and `import core` is `ErrorDupImport`). Congo's
  compile of core would also give a submodule of core the prelude import of core,
  a loop (`ErrorImportLoop`). `mem` is `@opaque`, so it holds no value. Its one
  name is still folded into every module, as a public submodule of core would be
  (`intrinsic_nameres_submodule`, where a folder-laid-out core is swept).

## Where it lives

| Step | File, function |
| --- | --- |
| lexer keyword | `lexer.c`, `IntrinsicAttrToken` |
| parse | `parsefnflow.c` `parseFn`; `parsemod.c` `parseExternFnCheck` |
| registry and checks | `ir/stmt/intrinsic.c`: `intrinsicRegistry`, `intrinsicDclNameRes`, `intrinsicDclTypeCheck` |
| hooks | `fndcl.c` `fnDclNameRes`, `fnDclTypeCheck`, `fnDclIsExpanded` |
| forced fallback | `--intrinsic-fallback` → `intrinsicForceFallback` (`conec.c`) |
| generation | `genlexpr.c` `genlDeclaredIntrinsic`; `genlalloc.c` `genlFinalizeAt`, `genlReleaseFlds`; `genltype.c` `genlAlignof` |
| declarations | `packages/core/src/core.cone`, `struct @opaque mem` |
| tests | `test/cases/intrinsic/` |
