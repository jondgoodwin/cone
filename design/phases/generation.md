Generation lowers the analyzed IR to an LLVM module and emits an object file. It
validates almost nothing: every assumption in section 5 is a hard prerequisite,
and what guards them is uneven — the sites that meant *unreachable* report and
exit, while the ordinary value asserts beside them are compiled out of the
release build. Section 5 says which is which.

Read section 4 before writing any cast, GEP, load or store. Being one level of
indirection off is the characteristic bug in this phase, and it does not
produce a type error — it produces a value where an address was wanted.

*Provenance: read from source; the type lowerings, the allocation header and all
three enum shapes were measured against emitted LLVM IR. Claims about
unreachable paths are reading only, and say so. See [Measuring](../diagnostics/measuring.md).*

## 1. Principles — [derived]

⚠ **Read from source and measured against emitted IR.** **What has not been
checked with the author is the claim that these are ruling positions rather than
a description of the present arrangement.**

1. **Types are lowered lazily and memoized**, on `llvmtype` for a named type and
   on the interned `typeinfo` for a reference type. There is no type pass. ▸
   **Forbids** a whole-program type-lowering stage, and **settles** that adding a
   type costs a lowering function and nothing global.
2. **Symbols for the whole program are declared before any body is emitted**, so
   forward references resolve. **That is the only global ordering.** ▸ **Settles**
   that nothing else here may depend on visit order, which is what lets bodies be
   emitted in any sequence.
3. **Permissions and regions are erased.** They shape the allocation header and
   nothing else; move-ness, thread-binding and lifetimes are erased entirely. ▸
   **This is [Performance](../topics/performance.md)'s central bet cashed in
   here**, and it **forbids** any safety distinction needing a runtime
   representation.
4. **Generation decides nothing about memory.** Every release, count adjustment
   and drop call was injected by flow analysis; generation replays the lists. ▸
   **Forbids** this phase reasoning about ownership at all — a double-release bug
   is a flow bug, and searching for it here wastes the search.
5. **Every definition is separately discardable.** Each leads a COMDAT of its
   own, so the linker decides at symbol granularity rather than object-file
   granularity. ▸ **Settles** the one-object-file-per-package model: coarse
   objects cost nothing when inclusion is decided per symbol.

## 2. Ordering, and why setup runs before parsing

`main` calls **`genSetup` before `parsePgm`**, because `genSetup` computes
`opt->ptrsize` from the target data layout, and `stdlibInit` sizes `usize` and
`isize` from it. The front end's own type table depends on the target: literal
type inference, `castBitsize` comparisons, array-length types and the slice
length word are all sized before a token is read. An unusable `--triple` fails
here, before parsing.

`genlProgram` is a strict two-pass walk over the modules:

1. **Symbols** — `genlGlobalSyms` for every module, generating or not, skipping
   private nodes of non-generating modules. Declares every global and function.
   A skipped private node is declared on first use when a public inline body,
   generated in its caller, names it: `genlFnSym` and `genlVarSym` in
   `genlexpr.c` ask for the symbol rather than assume it.
2. **Implementations** — `genlGlobalImpl`, only for modules flagged
   `FlagGenMod`.

Both recurse into a type's method list, into a generic's
`genericinfo->memonodes`, and into an enum extension's copies of its base's
variants, the front of its `derived` list (`structEnumCopyCount`): neither
instances nor copies are any module's nodes. The copies are reached first, ahead of
a generic's own instances, because a generic extension's copies are templates and
their instances are reached only through them. An uninstantiated generic generates
nothing.

### Symbols, linkage and COMDATs

**The rules are not here.** What a symbol is spelled, what linkage it gets and
why are [Names and Namespaces](names-and-namespaces.md), "Symbols"; this phase
lowers them, in two functions and a derivation:

- **`nameSymbol`** (`ir/name.c`, IR layer, no LLVM in it) spells the symbol of
  a function or global from the node alone: its owner chain, its declared name,
  and for an instance of a generic its type arguments, each spelled by
  `nameType`. `nameVtable`,
  `nameVtableImpl` and `nameVtableList` spell a vtable type, a vtable and a
  trait's vtable list. Nothing in generation builds a name string;
  `genlGloFnName` and `genlGloVarName` hand `LLVMAddFunction` and
  `LLVMAddGlobal` what `nameSymbol` returns.
- **`genlLinkage`** sets linkage, storage class and calling convention
  together, from the declaration facts and one argument saying whether this
  object defines the symbol — `genlIsDefinedHere` for a declared node: its
  module is flagged `FlagGenMod`, it is not `extern`, and a function has a
  body; always for a vtable, which no node declares. It is the one place that
  decides them, and it sets no visibility.
- **`genlComdat`** then derives the COMDAT selection kind from the linkage
  already on the global, at each definition site.

A section is all-or-nothing, so without further help every function an object
file defines ships whether or not the program can reach it. **Each definition
leads a COMDAT of its own, named for its symbol**, which lets the linker discard
the unreachable ones and, transitively, whatever only they referenced.

**Linkage and selection kind are not independent**, which is why the second is
read off the first rather than chosen beside it. A new kind of symbol has only
to reach `genlLinkage` and cannot end up with a selection kind that contradicts
its linkage:

| What the symbol is | Linkage `genlLinkage` sets | What `genlComdat` then does |
| --- | --- | --- |
| a definition of a program — a function, a global, an instance of a generic, a vtable, a vtable list | `internal`: nothing outside the object may resolve against a program's symbols | `nodeduplicate` — nothing can collide with it, and a duplicate within the object is a real error |
| `main`, or a C-style definition | external | `nodeduplicate` — a duplicate definition is a real error and stays one |
| a definition nothing outside can name — an anonymous `fn`, a string literal | `internal`, set at the site that names it | `nodeduplicate`; nothing can collide with it in any case |
| a declaration — an imported module's function, an `extern` | external, since an LLVM `declare` can be nothing else | nothing: **only a definition may lead a COMDAT**, and `LLVMVerifyModule` rejects one that does not |

A package compile, which does not exist yet, will add the row L2 of [Names and
Namespaces](names-and-namespaces.md) describes: an instance of a generic and a
vtable as `linkonce` with a COMDAT of `any`, since every object that uses one
produces it. The vtable list stays internal there too — it holds the
implementers this compile saw, so no two objects could agree on one.

That last row is why the attachment is at the definition sites and not in the
symbol pass. An imported module's functions have bodies in the IR and are
declarations in this object; `genlIsDefinedHere` tells them apart for the
linkage by asking the owning module for its `FlagGenMod`, and the COMDAT is
attached where the body is generated.

**The rule reaches only what passes through `genlComdat`.** A global built with
`LLVMAddGlobal` and never handed to it sits in a shared section, is not
individually discardable, and keeps alive everything it names. A vtable did
exactly that: a struct coerced to `&<Trait` kept every one of its trait methods
in the image whether or not anything ever dispatched. The call sites are
`genlFn`, `genlGloVar`, `genlVtableImpl`, `genlVtable`, and the `StringLitTag`
case in `genlAddr`. **A new kind of generated global needs a sixth.**

**Not every object format has COMDATs, so `genSetup` asks the triple** and stores
the answer in `gen->comdats`. Mach-O has no COMDAT concept and needs none — its
assembler emits `.subsections_via_symbols`, which already lets the linker strip a
symbol at a time — while WebAssembly lowers only `any`, so on wasm every symbol
is mergeable whether it wants to be or not. Both are hard errors inside LLVM's
backend rather than something it works around, and the C API exposes no
object-format query.

**An anonymous `fn` literal is given a name and internal linkage** in `genlFn`,
because it is lifted to module scope with neither. It needs a name for a COMDAT
to be named after, and internal linkage because the name LLVM's mangler invents
for an unnamed symbol — `__unnamed_1` — is the one every other object file
invents too; it is set here rather than left to `genlLinkage` so that it holds
in a package compile as well. Internal linkage lets the inliner delete the ones
nothing calls and fold the rest into their callers, so an unused literal never
reaches the object file — as for every other internal definition. The suffix LLVM
appends to keep `anon` unique is the module symbol table's counter, so it shifts
when unrelated globals are added.

`genlFn` per function: entry block, a dummy `allocaPoint` alloca, an alloca and
store for **every** parameter, then `genlBlock` on the body, then erase the
alloca point. Every parameter and local is memory-backed on purpose — the
comment is that all allocas belong in the entry block so `PromoteMemoryToRegister`
and SRoA can undo it.

`genpgm` then optionally verifies, dumps `.preir`, runs the pass manager
(mem2reg, reassociate, GVN, CFG simplification, plus function inlining), dumps
`.ir`, and emits. **There is no `--release` flag** — release is the default and
`--debug` turns it off, dropping optimization and enabling DWARF.

## 3. Type lowering

| Cone | LLVM |
| --- | --- |
| integer / float | `i1`…`i64`, `float`/`double`. Bool is a 1-bit unsigned |
| **`void`** | **`%void = type {}`** — a zero-field named struct, *not* LLVM `void`. A function returning nothing returns `%void`; so does `nil` |
| **permission** | **`%void`** — permissions are fully erased |
| `*T` | `T*` |
| **`&T`, `&mut T`, `+rc T`, `+so T`** | **`T*`, identically.** Region and permission contribute nothing to the reference value |
| **`&[]T`** | **anonymous `{ T*, usize }`** — element pointer at 0, element **count** at 1 |
| **`&<Trait`** | **named `{ i8*, Vtable* }`** — object as `i8*`, then vtable pointer |
| `fn` signature | `LLVMFunctionType`, never varargs; a `&fn` is a pointer to it |
| struct / trait | named struct, fields in declaration order. **A trait's body is its own fields**, which are a prefix of every implementer's, so `&Trait` points at the trait's layout and reaches the fields the trait declares. Only a type declared `@opaque` is left an opaque LLVM struct, and `DeclaredOpaque` — not `OpaqueType` — is what says so: a trait carries `OpaqueType` because it has no size as a *value*, which does not mean it has no fields |
| enum | `i8`…`i64` by `EnumNode.bytes` |
| tuple | anonymous struct |
| array | nested `LLVMArrayType`; each dimension must be a `ULitTag` |

Verified: `&[]i32` emits `{ i32*, i64 }`, with `extractvalue ..., 1` yielding a
*count* of 3 for a 3-element array — not a byte length.

**Erased with no representation at all:** lifetimes (`LifetimeTag` has no
lowering case and would assert), `QuesTag`, `BorrowRegTag`, move semantics,
thread-binding.

### Enums

Three shapes, the first two chosen in `genlSetupTaggedTrait`:

- **Nullable pointer.** Exactly two variants under `SameSize`, one with one
  field and one with two whose second is a pointer-like: **no struct is emitted
  at all**, and the value *is* the pointer. A null pointer is the empty variant.
  Each enum decides this for its own set: an extension's variants are copies, so an
  `Option`-shaped base keeps the layout whatever extends it, and the extension, with
  a third variant for which there is no pointer to be, is tagged. The same holds per
  instance: `Option[&i32]` is a bare pointer beside a tagged instance of an enum
  extending `Option[T]`.
- **Same size.** Each variant is re-emitted as a named struct with `[N x i8]`
  trailing padding to one size: the largest variant's store size, rounded up to
  the strictest alignment of any variant's field, or a byte-aligned largest
  variant would leave a stricter one padded past it. Reading a `%Shape` as a
  `%Circle` is safe only because they are the same size.
  - **The enum's own body is its own fields, then bytes** out to that size, then
    a zero-length array of the most strictly aligned field type where the bytes
    alone would under-align it: `%Shape = { i8, i32, [8 x i8] }`,
    `%Message = { i8, i32, [8 x i8], [0 x i64] }`. **It is never a copy of one
    variant's layout**, because an enum value is loaded, stored and passed as a
    first-class aggregate, and LLVM does not carry an aggregate's padding bytes:
    a smaller variant's field in a hole of the largest's layout — a `Bool` at
    byte 1 beside an `i32` at byte 4 — was lost in the copy. The enum's own
    fields are the discriminant and any common fields, which begin every variant
    at the same offsets, so a common field or the tag is still read by index.
  - **Every variant is in exactly one enum's list**, so an extension's copies are
    padded to the extension's size and the base's own variants to the base's.
- **Unpadded**, for an `@unsized` enum: each variant keeps its own size, the
  discriminant still first. Measured, for an empty variant beside one holding
  three `i64`s: `%Ping = { i8, i32 }` and
  `%Payload = { i8, i32, i64, i64, i64 }`.

The discriminant is an ordinary field flagged `IsTagField` whose type is an
`EnumNode`, lowered to `i8`, `i16`, `i32` or `i64` by its `bytes`.

⚠ **Its width is not decided here.** It follows the largest tag **value**, not the
variant count, and a value the author pinned is invisible in `derived->used` — so
type check settles it, where the pinned values and any declared integer type are
both known. Generation reads `bytes` and does not adjust it.

### Vtables

A named `"<Trait>:Vtable"` struct whose fields are, per slot, either a function
pointer **whose self parameter is erased to `i8*`** (to avoid LLVM type-check
errors on self) or an `i32` **byte offset** for a virtual field. One `internal
constant` per implementing struct, plus one internal list per trait, prewired in
`derived` order for the enum-to-virtref coercion. `nameVtable`, `nameVtableImpl`
and `nameVtableList` spell the three from the trait and implementing type nodes.

**A vtable may contain itself**, through a slot whose method takes or returns a
virtual reference to the same trait (`fn cmp(self &, o &<Self)`). So `genlVtable`
creates the vtable struct and the fat-pointer struct as named types and stores
both on the `Vtable` **before** it types any slot. A slot that names the trait's
virtual reference then gets that stored type, whose vtable body is still empty
and is filled in once every slot is typed, the same way a struct that points to
itself is built. The same cycle runs through the symbol pass: declaring such a
method types its signature, which builds the vtable, which asks for the symbol
of every method in its slots. So `genlGloFnName` types the signature first and
checks again whether the function has been declared before declaring it.

**Which vtable a tag names is found two ways, and the tag values decide.** Where
every variant's tag value is its position in `derived`, the tag indexes the list
directly — one GEP and one load. A pinned value breaks that, since the list has one
entry per variant and `Red = 0xFF0000` would index far past the end, so the sparse
case is a chain of `select`s comparing the tag against each variant's value with the
last as the fall-through. The tag came out of a value of the enum, so no default arm
is reachable; and because it is selects rather than branches, nothing about it
depends on where the coercion sits. `genlTagsIndexVtables` is the test,
`genlVtableForTag` the chain.

**A slot a folded method fills holds a thunk** (`genlVtableThunk`). Through the
fat pointer the concrete type is erased, so the shift from the object to the
field the method was folded through cannot happen at the call site; the thunk
is a function of the slot's own type that shifts the receiver one hop per field
on the recorded path (`VtableImpl.foldpaths`) — a `structgep` for a field held
by value, a load for one held through a reference — and tail-calls the method.
It is internal with a COMDAT like any definition, spelled by `nameVtableThunk`
after the type, the trait and the slot, and nothing in the language can name it.
The vtable's shape and the virtual call are untouched; a slot a declared method
fills is the bitcast it always was, and a folded field fills no slot.

### The allocation header

`genlRefTypeSetup` builds, per interned reference type:

```
%refstruct = type { <region>, <perm>, <value> }   ; RegionField, PermField, ValueField
```

Verified for `+rc-mut` of an `i32`:

```llvm
%void      = type {}
%rc        = type { i64 }
%refstruct = type { %rc, %void, i32 }    ; 16 bytes
```

**The reference value points at `ValueField`, not at the allocation base.**
`genlallocref` GEPs to that field and hands the result out. So the header sits
*before* the payload and the reference cannot see it.

Two consequences that are easy to get wrong:

- **`genlRcCounter` finds the count at `((usize*)ref) - 1` and frees from
  *that* pointer.** That is correct only because `rc` has exactly one `usize`
  field and the permission is zero-sized. Nothing checks it. A region with a
  two-field header, or a non-zero-size (locked) permission, would silently
  corrupt memory.
- **`genlDealiasOwn` calls `free(ref)` directly**, correct only because `so`'s
  region struct is empty, so the payload offset is 0 and the reference *is* the
  allocation base.
- **An owning slice is the fat `{T*, usize}` value, and the header sits before
  its pointer word.** `genlRefPtr`, at the entry of `genlDealiasOwn` and
  `genlRcCounter`, `extractvalue`s word 0 of an `ArrayRefTag` reference, so every
  release site — scope exit, a `RefCountNode`, `genlStore` — hands over the
  value as generated. `genlDealiasFlds` walks no slice's elements: what an
  element owns is left where an array of owning references leaves it.

A region is any struct with a suitable `_alloc`; `so` and `rc` are declared in
Cone source in the core package, `packages/core/core.cone`, not built into the
compiler. `malloc` is an
ordinary `extern`; `free` is declared directly by `genlFree`. `conestd` supplies
only stdio, no allocator.

## 4. Pointer levels

This is what the CLAUDE.md warning is about. The conventions:

| Value | LLVM level |
| --- | --- |
| a local or parameter (`var->llvmvar`) | **pointer to** its type — always an alloca |
| `genlExpr(nameuse)` | the loaded value |
| `genlAddr(x)` | pointer to `x`'s type |
| `&T` value | `T*` |
| `&[]T` value, `&<Trait` value | an **aggregate value**, not a pointer |
| owning reference value | `T*` pointing **past** the header |
| owning slice value | `{T*, usize}`, its `T*` pointing past the header |
| allocation base | `((usize*)ref) - 1` for `rc`; `ref` itself for `so` |
| vtable field slot | an `i32` **byte offset**, applied to an `i8*` |
| vtable method slot | reached by `structgep` **then load** |

Concrete hazards, each of which has been gotten wrong here before:

- **`genlDealiasFlds` must load after `StructGEP`.** The GEP gives `T**` for a
  ref-typed field; the release routines want the reference the field holds.
- **`genlAddr`'s array index uses `genlAddr(objfn)` for an array but
  `genlExpr(objfn)` for a reference to one.** An array *is* memory; a reference
  *holds* the address. One level apart, same GEP shape.
- **`genlRcCounter`'s bitcast to `usize*` changes the GEP stride**, which is the
  only reason `-1` lands on the counter.
- **A struct field read and a field address are different instruction
  sequences, chosen by `FlagBorrow`** — not by context. Without the flag,
  generation loads the *whole aggregate* and `extractvalue`s. With it, it GEPs.
  Getting the flag wrong is not a type error.
- **Mutating intrinsics take self as an lvalue pointer; non-mutating ones take a
  value.** The intrinsic switch dispatches on the LLVM *type kind* of argument
  0, so both land in the same branch and are told apart only by which intrinsic
  it is.
- **`genlRecast` picks by generated LLVM kinds, not Cone tags** — deliberately,
  because a reference is not always a plain pointer once fat pointers are in
  play.
- **`genlAddr` and `genlExpr` must test the same tag for a tuple element.** Both
  test `ULitTag`, because `fnCallLowerIntField` leaves the index as the
  `ULitNode` the source wrote; `UintNbrTag` is that literal's *type*. `genlAddr`
  tested the type, matched nowhere, and — with the assert beside it compiled out
  — fell into the next case and read the node as a string literal, which is what
  made `&t.0` segfault.

## 5. What generation assumes

Everything below is required, unchecked, and fatal if violated.

- **Every expression node has a resolved, non-NULL `vtype`.** `--checktree` is
  the only thing that looks; generation reads the hole and faults.
- **No `UnknownTag`, `QuesTag`, `TupleTag`, `StarTag`, `NameUseTag`,
  `LifetimeTag` or `BorrowRegTag` reaches `genlType`.** `unknownType` is legal
  as a *block* type meaning "no value", never as a type to generate.
- **Macros expanded, generics instantiated, overloads resolved.** `genlFn`
  asserts the node is a concrete `FnDclTag`: an overload name is a namespace
  binding that selection replaces long before generation.
- **Operators are already calls** bound to a declaration whose body is an
  intrinsic or a block. Generation has no operator concept.
- **`each` is already a loop block with a synthesized step.**
- **Flow ran.** Everything about memory arrives already decided;
  [Flow Analysis](flow.md), "What generation relies on", is the contract.
  Without it nothing is ever released, and there is no fallback.
- **Field indices, vtable slot indices, tag numbers and parameter positions are
  correct.** All are consumed without validation.

**An impossible state is reported, not assumed away.** The documented build is
`Release`, which defines `NDEBUG`, so an `assert` is not a trap there. Every
site that means *unreachable* calls `errorUnreachable`, which reports
`ErrorUnreachable` against the node — with its instantiation trace — and exits
`ExitGen`. The ordinary value asserts scattered through generation are still
asserts and still compiled out; do not add one expecting it to catch anything
shipped.

## 6. Statements and expressions

**Basic blocks are created only when needed.** A non-loop block with at most
one break emits its statements straight into the current block. A loop, or a
block with several breaks, gets a `blockend`, a `GenBlockState` pushed on a
fixed 256-deep stack, and a phi at the end over the accumulated values.

Phi predecessors are always recorded as `LLVMGetInsertBlock` at the moment of
the branch, never the block generation was positioned in — evaluating a
subexpression may have emitted branches of its own and split the block.

`if` builds `endif` first, then per arm a next-condition block and a body
block. An arm whose last statement is a return, break or continue contributes no
fallthrough and no phi edge. `while` is not a generation concept: it arrives as
a loop block containing `if not cond { break }`.

Short-circuit `and`/`or` are two blocks and a 2-way `i1` phi. `not` is
`xor i1 %x, true`.

**`FlagInline` functions are inlined by the Cone generator, not by LLVM.** They
get no symbol at all: their parameters become allocas at the call site and their
body is generated inline. This is how the region allocator becomes a direct
`malloc` call at each allocation. Having no symbol, one cannot be borrowed:
`borrowTypeCheck` refuses `&name` on an inline function (`ErrorInlineRef`), so
`genlAddr`'s function arm never meets a declaration without an `llvmvar`.

**`llvm.trap` is emitted as a call, not a terminator.** Both panic sites —
allocation failure and bounds check — rely on the block falling through and
branching to the join point.

Bounds checks are emitted for arrays and slices, per dimension, against the
compile-time extent or the slice's count word. **A raw pointer index is not
bounds checked.**

## 7. Output, and what does not work

`--llvmir` writes **two** files: `.preir` before the pass manager and `.ir`
after. `--ir` is not an LLVM option at all — it dumps the Cone IR/AST.
`--asm` adds a `.wat` or `.asm`. `--verify` runs `LLVMVerifyModule` and is off
by default. `--debug` emits DWARF and drops optimization — it is the only
switch here, with release as the default. Debug info covers only files and
subprograms, and the file name is hardcoded.

**Cross-module linking is broken.** A symbol is spelled from its owner chain,
and the root module contributes no name to it — [Names and Namespaces](names-and-namespaces.md),
"Symbols". So compiling `modulesub.cone` directly makes it the root and emits
`@scaleInt`, bare; compiling a `main.cone` that imports it makes it an imported
module and emits `@_CNvC9modulesub8scaleInt`, `modulesub.scaleInt`. The two
object files never resolve against each other. Compounding it, an ordinary
imported module does not get `FlagGenMod`,
so only a `declare` is emitted for it. Separate compilation is what has to
settle it.

Also absent: closures with an environment — an anonymous `fn` is lifted to
module scope and a `&fn` value is a bare function pointer with no capture
struct. No exception handling or unwinding. No debug info for types or
variables.

## 8. Hazards

- **Some `LLVMBuild*` calls run with the builder outside any basic block** —
  `genlVtableImpl`, and a global's initializer. They work only because every
  operand constant-folds. A non-constant operand there would be catastrophic.
- **A string literal emits a fresh global per occurrence.** Nothing deduplicates
  them, and constant merging is not in the pass list.
- **The block stack is a fixed 256 entries** and overflow is a hard exit.

## 9. Code pointer map

| File | Function | Purpose |
| --- | --- | --- |
| `conec.c` | `main` | calls `genSetup` **before** parsing, for target pointer size |
| `genllvm/genllvm.c` | `genSetup`, `genClose` | target machine, data layout, context, `%void` |
| | `genpgm` | generate, verify, dump, optimize, emit |
| | `genlProgram` | the two-pass symbols-then-implementations walk |
| | `genlGlobalSyms`, `genlGlobalImpl` | declare a node's symbol; emit its body |
| | `genlFn`, `genlParmVar`, `genlAlloca` | function body, parameter allocas, entry-block alloca placement |
| | `genlGloFnName`, `genlGloVarName` | declare a function or global under the symbol `nameSymbol` spells |
| | `genlLinkage`, `genlIsDefinedHere` | linkage, storage class and calling convention, together, from the declaration facts and whether this object defines the symbol |
| | `genlComdat`, `genlNameAnonFn` | the per-definition COMDAT that lets the linker drop a symbol, its kind read off the linkage; the private name an anonymous `fn` needs to have one |
| | `genlComdatSupport` | what the target's object format does with COMDATs |
| | `genlOut` | set triple and layout, emit object and asm |
| `genllvm/genltype.c` | `genlType`, `_genlType` | the memoizing entry and the per-tag lowering switch |
| | `genlSetupTaggedTrait`, `genlSameSizeTrait` | the three enum shapes |
| | `genlVtable`, `genlVtableImpl` | vtable type, per-struct constants, the virtref fat pointer |
| | `genlVtableThunk` | the function filling a slot a folded method satisfies: shift the receiver along the recorded field path, tail-call the method |
| `genllvm/genlstmt.c` | `genlBlock` | block creation, phi state, terminator suppression |
| | `genlBreak`, `genlReturn` | phi edges and dealias; inlined-return-as-break |
| `genllvm/genlexpr.c` | `genlExpr`, `genlAddr`, `genlStore` | the value / address / store trio — section 4 |
| | `genlFnCallInternal` | indirect calls, virtual dispatch, generator-level inlining, the intrinsic switch |
| | `genlConvert`, `genlRecast`, `genlIsType` | the three cast forms |
| | `genlArrayIndex`, `genlBoundsCheck` | multi-dimensional GEP and its checks |
| `genllvm/genlalloc.c` | `genlRefTypeSetup`, `genlallocref` | the `{region, perm, value}` header and its emission |
| | `genlRcCounter`, `genlDealiasOwn`, `genlReleaseOwning`, `genlDealiasNodes` | count adjustment, free, releasing what a variable (or a tuple's rc elements) holds, and replaying flow's lists |
| `ir/types/reference.h` | `enum ManagedRefFields` | `RegionField`, `PermField`, `ValueField` |
| `ir/name.c` | `nameSymbol`, `nameType`, `nameVtable`, `nameVtableImpl`, `nameVtableList` | spelling a symbol from a node's owner chain and facts, and a type argument within it — the rules are in [Names and Namespaces](names-and-namespaces.md), "Symbols" |
| `ir/dclinfo.c` | `dclInfoJoin` | writes the declaration facts where a declaration joins its namespace |

## 10. What lives elsewhere

| Question | Note |
| --- | --- |
| What a region and a permission mean before they are erased | [References and Regions](../topics/references-and-regions.md) |
| What injected the reference-count nodes and dealias lists | [Flow Analysis](flow.md) |
| What guarantees every node has a `vtype` | [IR Nodes](../nodes/_index.md), "--checktree" |
