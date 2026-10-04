An **intrinsic** is a function whose value is an `IntrinsicNode` in place of a
body: the compiler supplies what it does, and generation expands it at each call
rather than calling anything (`genlFnCallInternal`). There are two kinds, and
they differ in who declares them and in how their meaning is decided.

| | Built in C | Declared in Cone with `@intrinsic` |
| --- | --- | --- |
| Declared by | `corenumber.c`, `corelib.c`, `struct.c` (an enum's `==`) | `packages/core/src/core.cone`, as functions of the opaque struct `mem` |
| Named | as a method or operator of a type | through `mem`: `mem.sizeof[T]()` |
| Kinds | `NegIntrinsic` … `FinalAllIntrinsic` | `SizeofIntrinsic` … `ShrMaskedIntrinsic` (`FirstDeclaredIntrinsic` onward) |
| Meaning decided | at generation, by the LLVM type kind of argument 0 | by the registry, in Cone terms; the Cone type rides on the node (`typearg`) |
| Reference page | none: the number and pointer methods | `doc/reference/refintrinsic.html` |

The kinds built in C are left as they are until the number types are rebuilt
over generics; nothing new is to be added to them. **A new intrinsic is declared
in core, entered in the registry, and given an arm in `genlDeclaredIntrinsic`**
— or, for one whose meaning depends on its call as well as its instance, as the
atomic operations' orderings do, in `genlAtomicIntrinsic`, which `genlFnCall`
reaches with the call's own arguments.

**An integer's bit intrinsics are also its methods.** `countOnes`,
`leadingZeros`, `trailingZeros`, `rotateLeft`, `rotateRight`, `shlMasked` and
`shrMasked` are declared in core's `mem` like the rest, and `corenumber.c`
(`nbrBitMethods`) also gives every integer type a method of each name whose
`IntrinsicNode` is the same declared kind, its `typearg` the type itself: the
instance the generic declaration would have for that type. So `x.leadingZeros()`
and `mem.leadingZeros(x)` are one registry entry and one arm of
`genlDeclaredIntrinsic`, and the meaning is still decided by the Cone type,
never by an LLVM type kind. The methods are built in C only because Cone source
cannot yet add a method to a number type; they keep the lowering under
`--intrinsic-fallback`, which is what lets `intrinsic_bits` check each fallback
body against it in one run.

*Provenance: read from source and measured, September 2026.*

## Principles — [derived]

**The registry is the definition of record, and it speaks Cone.** Each entry in
`intrinsicRegistry` (`ir/stmt/intrinsic.c`) is a name, its signature as shapes
over the one type parameter `T` (or over none: `traceRoots`, `srcFile`,
`srcLine`, `isDebugBuild` and the provisional constants of the build beside it are the entries without it, and their instances carry no `typearg`), whether a call can break memory safety (and so
belongs in `trust`), whether a Cone fallback body may be written, the phase that
answers it, whether this back end lowers it itself, and the class of types `T`
may be where that is narrower than every type with a size (`IntrinsicClass`: the
atomic operations take an integer of 8 to 64 bits, `Bool` or a raw pointer, or
part of that, and an integer's bit operations an integer). No LLVM name appears in
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
(`intrinsic_success`, `intrinsic_atomic`). An entry nothing in Cone can express (`sizeof`,
`finalize`, `writeRaw`) refuses a body. An atomic operation's body is the plain
operation, which means what the entry means for one thread, all a back end with
no atomic instructions can run.

**What is refused is refused both ways.** A type outside an entry's class and an
atomic ordering that is not a constant, or is one the operation forbids, are
judged whether the call is lowered or runs its fallback body: the class on the
instance before its body is checked (`intrinsicClassCheck`), so a fallback body
is never type checked at a type it was not written for, and the orderings at the
call (`intrinsicCallCheck`), since an instance is shared by every call at its
type while each call gives its own orderings.

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
     `cloneRefNode`), so both spellings are accepted. An atomic ordering is
     core's enum named `MemOrder` (`memOrderEnum`, known by name and package as
     `TypeRecord` is), and compareSwap's result the tuple `T, Bool`;
   - a body the entry allows no fallback for, or no body where there is no
     lowering, is `ErrorIntrinsicBody`.
   A declaration that passes either keeps its body as an **inline** function
   (forced fallback, or no lowering), or has its value replaced by an
   `IntrinsicNode` whose `typearg` is a use of the type parameter.
3. **Instantiation.** Cloning a generic intrinsic clones the node
   (`cloneIntrinsicNode`), and the use of `T` is substituted like any other, so
   each instance carries the Cone type it is for.
4. **Type check** (`fnDclTypeCheck`). First, for an entry with a type class,
   lowered or not, `intrinsicClassCheck` reads `T` from the instance's first
   parameter, the pointee of a `*T` (the atomic operations) or a `T` itself (an
   integer's bit operations), and refuses one outside the class with
   `ErrorIntrinsicType`, before a fallback body is checked. It is reported at
   the outermost place that instantiated it, where the program chose the type,
   and once there: a generic type whose methods call `atomicAdd[T]` for a `T`
   it does not constrain is refused at the program's `Bump[Bool]`, by the first
   of those methods, not in the generic's source once per method. (core's
   `Atomic[T]` says with `where T is Integer` that its `add` exists only for
   an integer, so an `Atomic[Bool]` never reaches this.) Then `intrinsicDclTypeCheck`: a declared
   intrinsic's instance has no body to check; its `typearg` is type checked and
   must have a size (`itypeNoSizeCause`), else `ErrorIntrinsicType`, reported at
   the call that instantiated it (`instnode`), not in core. An instance of
   `writeRaw` or `moveRaw` is also noted for the rule that no traced reference
   is placed in raw memory (`regionTracedRawNote`, `ErrorTracedRaw`), judged
   once type check has finished and reported at the outermost instantiation
   — a collection's in the program's source, where it was reached through the
   collection's body ([What a region is](module.md)). Each instance is judged
   once, for its type, as an instance is made once.
5. **The call** (`fnCallFinalizeArgs` → `intrinsicCallCheck`), once its
   arguments are coerced and its defaults appended: each argument an entry's
   `ShapeOrder` parameter receives must be a constant `MemOrder`, a variant's
   literal or a `const` holding one, seen through the coercion to the enum
   (`intrinsicOrderOf`), else `ErrorAtomicConst`; and one the operation allows,
   else `ErrorAtomicOrder`: a load's `Release` or `AcqRel`, a store's `Acquire`
   or `AcqRel`, a compareSwap failure ordering of `Release` or `AcqRel` or
   stronger than its success ordering. Every call to a function carrying
   `DclIntrinsic` is asked, lowered or not.
6. **Flow** sees an ordinary call: an argument passed by value is moved into it,
   as `writeRaw`'s value is.
7. **Generation** (`genlDeclaredIntrinsic`), dispatched by kind before the C-built
   kinds' LLVM-type switch. An atomic operation is caught earlier, in
   `genlFnCall` (`intrinsicAtomicCallee`), where the call's arguments are still
   at hand: `genlAtomicIntrinsic` reads its orderings from them
   (`intrinsicCallOrders`), which type check has already found allowed.

## Each kind, and how LLVM implements it

| Kind | Phase | LLVM implementation |
| --- | --- | --- |
| `sizeof[T]` | constant | `LLVMABISizeOfType` (alloc size, tail padding included), `genlSizeof` |
| `alignof[T]` | constant | `LLVMABIAlignmentOfType`, `genlAlignof` |
| `needsFinal[T]` | constant | `itypeNeedsFinal`, a front-end question, emitted as an `i1` |
| `finalize[T]` | expansion | `genlFinalizeAt`: release an owning reference; a struct's or an enum's drop; a tuple's elements, an array's in element order |
| `sliceFromParts[T]`, `…Mut` | expansion | two `insertvalue`s into the `{ptr, usize}` pair |
| `readRaw[T]` | expansion | a load (`%rawread`) |
| `writeRaw[T]` | expansion | a store |
| `moveRaw[T]` | operation | `LLVMBuildMemMove` of `count * sizeof(T)` bytes |
| `typeRecord[T]` | constant | the address of T's record, a private constant `genlTypeRecord` builds once per object |
| `holdsTraced[T]` | constant | `itypeHoldsTraced`, a front-end question, emitted as an `i1` |
| `trace[T]` | expansion | `genlTraceAt`: each traced reference the value holds, loaded and, where not null, handed to its region's `mark` with its permission and `mode` where `mark` takes them |
| `traceRoots` | expansion | a call to conestd's `cone_traceRoots(mode)`, which walks the chain of frames each function holding traced references links ([Generation](../phases/generation.md), "Roots") and calls each root's record's trace |
| `atomicLoad[T]`, `atomicStore[T]` | operation | a `load atomic` or `store atomic` with the call's ordering, `genlAtomicIntrinsic` |
| `atomicSwap[T]`, `atomicAdd[T]` … `atomicXor[T]` | operation | an `atomicrmw` (`xchg`, `add`, `sub`, `and`, `or`, `xor`), answering the value before |
| `atomicMin[T]`, `atomicMax[T]` | operation | an `atomicrmw` `min` and `max` for a signed `T`, `umin` and `umax` for an unsigned one (`ClassInt`) |
| `atomicCompareSwap[T]` | operation | a strong `cmpxchg` with both orderings, its `{T, i1}` rebuilt as the result tuple. On a GPU target each atomic is settled after the pipeline, its scope its memory's, and a `cmpxchg` made LLVM's `llvm.spv.cmpxchg` ([Generation](../phases/generation.md), "What invocations share") |
| `workgroupBarrier`, `storageBarrier` | operation | on a GPU target a call to `llvm.spv.group.memory.barrier.with.group.sync` or `llvm.spv.device.memory.barrier.with.group.sync` (`genlGpuBarrier`); on a native target a load of conestd's thread-local `cone_barrierHook` and, where it is not null, a call of it with `cone_barrierCtx` and the kind, 0 or 1 (`genlCpuBarrier`; [Generation](../phases/generation.md), "Barriers on the CPU"); on WebAssembly nothing. Declared at core's top level with an empty fallback body, beside `setBarrierHook` and `clearBarrierHook`, which are conestd's |
| `srcFile`, `srcLine` | expansion | constants of where the call is, the source file's name without its folders (a private constant, one per file per module) and the line, `genlFnCall`. Declared at core's top level rather than in `mem`. Each may be a parameter's default value, the one default that is not a literal (`varDclTypeCheck`); `fnCallFinalizeArgs` appends a copy placed at the call taking the default (`intrinsicSrcCallAt`), so each call answers its own place. Written in a macro's body, each answers where the macro is used: `macroExpand` sets `CloneState.srcsite` to the outermost use (`macroSrcSite`), and `cloneFnCallNode` places a call to either there, while an argument keeps its own place ([generic](generic.md), "Macros") |
| `isDebugBuild` | constant | an `i1`, true where `opt->release` is 0 (`conec --debug`, or `build: debug` in a build description), answered in `genlFnCall` by `intrinsicBuildConst` before any argument is generated. A constant of the build, not of a type: `genlIf` generates only the side of an `if` on it that the build takes (below). Declared at core's top level; core's `assertDebug` and `assertDebugMsg` macros branch on it |
| `isWindows`, `isLinux`, `isMacOS`, `isWasm` | constant | TEMPORARY, a provisional mechanism whose final design is open. An `i1` read from `opt->triple` (filled in with the host's by `genlCreateMachine` before parse): `windows` or `win32`, `linux`, `darwin` or `macos` in it, or a `wasm` architecture. Answered and branched on as `isDebugBuild` is |
| `isDefined(name)`, `definedInt(name)` | constant | TEMPORARY, as above. Whether `-D` defined `name`, an `i1`, and its integer, an `i64`, 0 when it was not (`opt->defines`, parsed by `coneOptDefine`). `name` must be a string literal under its borrow and coercion (`intrinsicCallCheck`, `ErrorDefineName`); the literal is read, never generated |
| `countOnes[T]`, `leadingZeros[T]`, `trailingZeros[T]` | operation | `llvm.ctpop`, and `llvm.ctlz` and `llvm.cttz` told 0 is not poison (`is_zero_poison` false), so 0 counts as the width; the count resized to an `i32`, `genlBitIntrinsic`. `T` an integer of 8 to 64 bits (`ClassInt`) |
| `rotateLeft[T]`, `rotateRight[T]` | operation | `llvm.fshl` and `llvm.fshr` with `x` as both halves, the `u32` amount resized to `T`; the funnel shifts take it modulo the width themselves |
| `shlMasked[T]`, `shrMasked[T]` | operation | the amount resized to `T` and anded with width - 1 (the width a power of two, so that is modulo the width), then `shl`, or `ashr` for a signed `T` and `lshr` for an unsigned one |

**Constants of the build drop the untaken side at generation.**
`intrinsicBuildConst` answers whether a node is one of the constants above, or
`!`, `and` or `or` of them, and its value. `genlIf` asks it of each condition:
a false one's block is not generated at all, and a true one's is generated as
an `else` would be, with nothing after it. So no call on the untaken side, and
no reference to its symbol, reaches the object, in a debug build as in a
release one: a call to an `extern` only another platform defines links. Name
resolution, type check and flow analysis still see both sides, so every name on
either side must resolve on every target; generic instances and functions
reached only from the untaken side are still generated.

Each atomic instruction is aligned as `T` is (`LLVMABIAlignmentOfType`), and
its ordering is LLVM's name for the `MemOrder` (`genlAtomicOrdering`: `Relaxed`
is `monotonic`). A `Bool` is an `i1` in a byte, and LLVM's atomics take no
`i1`, so it is operated on as that byte: the value zero-extended in, the result
truncated out.

`finalize` runs what a region-held value's death runs, less the region's `free`
(`genlRegionDeath`), which is what a local's death at its scope's end runs: its
`final`, its fields that need it, then the owners it holds; a tuple element by
element, an array in element order. `itypeNeedsFinal` is true exactly when that
does something.

`typeRecord`'s result is the one registry shape that is not built from `T`: a
pointer to core's struct named `TypeRecord` (`typeRecordIsPtr`, shape
`ShapePtrTypeRecord`), which the declaration's result names and generation
builds. The record's finalizer is `finalize`'s expansion made a function of
its own, and its trace `trace`'s; what the record holds, and why each slot is
never null, is
[Generation](../phases/generation.md), "The allocation header". The same record
is what a region whose `alloc` takes `ty *TypeRecord` is handed
([What a region is](module.md)). Accepting core's `typeRecord` declaration
also remembers the struct its result points at (`typeRecordStruct`), which is
how generation builds a root map's records, where no declaration names the
type.

`mem` also holds one function that is no intrinsic: `sliceEq[T](a &[]T,
b &[]T) Bool`, what `==` and `!=` on two slices call ([fncall](fncall.md), "A
comparison of two slices compares their elements"). Its body is its
implementation, compiled as any generic function's, and it has no registry
entry; the compiler knows it the way it knows `TypeRecord`, by its name and its
package, and `sliceEqDclNameRes` holds the declaration to that one signature
(`ErrorIntrinsicSig` otherwise) and remembers it (`sliceEqFn`).

## Hazards

- **The constants are the target layout's, read at generation.** `sizeof` and
  `alignof` are constants in the IR, but the type checker cannot fold them: Cone
  does not compute layout itself, so neither can be an array length yet.
- **`trust` is recorded, not enforced.** The registry marks `finalize`, both
  slice constructors, the three raw operations and the atomic operations;
  `trust` does not exist (`doc/design/safety.md`).
- **An ordering is read from the call's arguments, so it must stay a constant
  there.** Type check and generation each look through the coercion to
  `MemOrder` and any `const` to a variant's literal (`intrinsicOrderOf`); a
  change that folded or hoisted the argument into something else would make
  generation's `errorUnreachable` fire rather than a diagnostic. A default
  ordering is not possible yet: a parameter's default must be a literal, and a
  variant coerced to its enum is not one (`litIsLiteral`).
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
| registry and checks | `ir/stmt/intrinsic.c`: `intrinsicRegistry`, `intrinsicDclNameRes`, `intrinsicDclTypeCheck`, `intrinsicClassCheck`, `intrinsicCallCheck`; `sliceEqDclNameRes`, `sliceEqFn` for `mem.sliceEq` |
| hooks | `fndcl.c` `fnDclNameRes`, `fnDclTypeCheck`, `fnDclIsExpanded`; `fncall.c` `fnCallFinalizeArgs` |
| forced fallback | `--intrinsic-fallback` → `intrinsicForceFallback` (`conec.c`) |
| generation | `genlexpr.c` `genlDeclaredIntrinsic`, `genlBitIntrinsic`, `genlAtomicIntrinsic` (from `genlFnCall`); `genlalloc.c` `genlFinalizeAt`, `genlTypeRecord`, `genlTraceAt`; `genltype.c` `genlAlignof` |
| an integer's bit methods | `corenumber.c` `nbrBitMethods` |
| declarations | `packages/core/src/core.cone`, `struct @opaque mem` and `enum MemOrder` |
| tests | `test/cases/intrinsic/` |
