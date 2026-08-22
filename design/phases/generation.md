Generation lowers the analyzed IR to an LLVM module and emits an object file. It
validates almost nothing: every assumption in section 5 is a hard prerequisite,
and what guards them is uneven — the sites that meant *unreachable* report and
exit, while the ordinary value asserts beside them are compiled out of the
release build. Section 5 says which is which.

Read section 4 before writing any cast, GEP, load or store. Being one level of
indirection off is the characteristic bug in this phase, and it does not
produce a type error — it produces a value where an address was wanted.

*Provenance: read from source; the type lowerings, the allocation header and all
three union shapes were measured against emitted LLVM IR. Claims about
unreachable paths are reading only, and say so. See [Measuring](../diagnostics/measuring.md).*

## 1. Key principles

1. **Types are lowered lazily and memoized**, on `llvmtype` for a named type and
   on the interned `typeinfo` for a reference type. There is no type pass.
2. **Symbols for the whole program are declared before any body is emitted**, so
   forward references resolve. That is the only global ordering.
3. **Permissions and regions are erased.** They shape the allocation header and
   nothing else. Move-ness, thread-binding and lifetimes are erased entirely.
4. **Generation decides nothing about memory.** Every release, every count
   adjustment, every drop call was injected by flow analysis. Generation replays
   the lists.

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
2. **Implementations** — `genlGlobalImpl`, only for modules flagged
   `FlagGenMod`.

Both recurse into a type's method list and into a generic's
`genericinfo->memonodes`. Generic instances get `LLVMLinkOnceAnyLinkage`. An
uninstantiated generic generates nothing.

**Each definition leads a COMDAT of its own, named for its symbol** — every
function and every global variable. Without one a section is all-or-nothing, so
every function an object file defines ships whether or not the program can reach
it; with one the linker discards the unreachable ones and, transitively,
whatever only they called. The attachment is at the definition sites, `genlFn`
and `genlGloVar`, and not in the symbol pass, because **only a definition may
lead a COMDAT**: an imported module's functions have bodies in the IR but are
declarations in this object, and `LLVMVerifyModule` rejects a declaration in a
COMDAT. Hidden visibility does not prevent it — a private function strips like
any other.

**The selection kind follows the linkage.** `any` merges silently, keeping one
copy of a symbol several object files each define. That is what a generic
instantiation needs and what nothing else should ask for, so everything not
`linkonce` or `weak` gets `nodeduplicate` instead, leaving a genuine duplicate
definition the link error it should be.

**Not every object format has them, so `genSetup` asks the triple** and stores
the answer in `gen->comdats`. Mach-O has no COMDAT concept at all and needs
none — its assembler emits `.subsections_via_symbols`, which already lets the
linker strip a symbol at a time — while WebAssembly lowers only `any`, so on
wasm every symbol is mergeable whether it wants to be or not. Both restrictions
are hard errors inside LLVM's backend, not something it works around.

**An anonymous `fn` literal is given a name and internal linkage** in `genlFn`,
because it is lifted to module scope with neither. It needs a name for a COMDAT
to be named after, and internal linkage because the name LLVM's mangler invents
for an unnamed symbol — `__unnamed_1` — is the one every other object file
invents too. Internal linkage also lets the inliner delete the ones nothing
calls, so an unused literal never reaches the object file. The suffix LLVM
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
| struct / trait | named struct, fields in declaration order |
| enum | `i8`…`i64` by `EnumNode.bytes` |
| tuple | anonymous struct |
| array | nested `LLVMArrayType`; each dimension must be a `ULitTag` |

Verified: `&[]i32` emits `{ i32*, i64 }`, with `extractvalue ..., 1` yielding a
*count* of 3 for a 3-element array — not a byte length.

**Erased with no representation at all:** lifetimes (`LifetimeTag` has no
lowering case and would assert), `QuesTag`, `BorrowRegTag`, move semantics,
thread-binding.

### Unions

Three shapes, chosen in `genlSetupTaggedTrait`:

- **Nullable pointer.** Exactly two variants under `SameSize`, one with one
  field and one with two whose second is a pointer-like: **no struct is emitted
  at all**, and the value *is* the pointer. A null pointer is the empty variant.
- **Same size.** Each variant is re-emitted as a named struct with `[N x i8]`
  trailing padding to the largest variant's store size; the base trait's body is
  a copy of the largest variant's fields. Reading a `%Shape` as a `%Circle` is
  safe only because they are the same size.
- **Tagged.** The discriminant is an ordinary field flagged `IsTagField` whose
  type is an enum, widened to 2/3/4 bytes by variant count.

### Vtables

A named `"<Trait>:Vtable"` struct whose fields are, per slot, either a function
pointer **whose self parameter is erased to `i8*`** (to avoid LLVM type-check
errors on self) or an `i32` **byte offset** for a virtual field. One `linkonce
constant` per implementing struct, plus a `vtable-list` array indexed by tag
number for the trait-to-virtref coercion.

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

A region is any struct with a suitable `_alloc`; `so` and `rc` are declared in
Cone source inside `corelibSource`, not built into the compiler. `malloc` is an
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
`malloc` call at each allocation.

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

**Cross-module linking is broken, and the rule is worth stating exactly.** A
declaration's linker symbol is the module prefix, plus each enclosing type name,
plus the source name — except that an `extern` declaration is never prefixed.
The main module's prefix is empty. So compiling `mymod.cone` directly makes it
the main module and emits `@scaleInt`; compiling a `main.cone` that imports it
makes it an imported module and emits `@mymod_scaleInt`. The two object files
never resolve against each other. Compounding it, an ordinary imported module
does not get `FlagGenMod`, so only a `declare` is emitted for it.
Separate compilation is what has to settle it.

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
| | `genlComdat`, `genlNameAnonFn` | the per-definition COMDAT that lets the linker drop a symbol; the private name an anonymous `fn` needs to have one |
| | `genlComdatSupport` | what the target's object format does with COMDATs |
| | `genlOut` | set triple and layout, emit object and asm |
| `genllvm/genltype.c` | `genlType`, `_genlType` | the memoizing entry and the per-tag lowering switch |
| | `genlSetupTaggedTrait`, `genlSameSizeTrait` | the three union shapes |
| | `genlVtable`, `genlVtableImpl` | vtable type, per-struct constants, the virtref fat pointer |
| `genllvm/genlstmt.c` | `genlBlock` | block creation, phi state, terminator suppression |
| | `genlBreak`, `genlReturn` | phi edges and dealias; inlined-return-as-break |
| `genllvm/genlexpr.c` | `genlExpr`, `genlAddr`, `genlStore` | the value / address / store trio — section 4 |
| | `genlFnCallInternal` | indirect calls, virtual dispatch, generator-level inlining, the intrinsic switch |
| | `genlConvert`, `genlRecast`, `genlIsType` | the three cast forms |
| | `genlArrayIndex`, `genlBoundsCheck` | multi-dimensional GEP and its checks |
| `genllvm/genlalloc.c` | `genlRefTypeSetup`, `genlallocref` | the `{region, perm, value}` header and its emission |
| | `genlRcCounter`, `genlDealiasOwn`, `genlDealiasNodes` | count adjustment, free, and replaying flow's lists |
| `ir/types/reference.h` | `enum ManagedRefFields` | `RegionField`, `PermField`, `ValueField` |
| `ir/name.c` | `nameGenFnName`, `nameNewPrefix` | the symbol naming rule of section 7 |

## 10. What lives elsewhere

| Question | Note |
| --- | --- |
| What a region and a permission mean before they are erased | [References and Regions](../northstar/references-and-regions.md) |
| What injected the alias nodes and dealias lists | [Flow Analysis](flow.md) |
| What guarantees every node has a `vtype` | [IR Nodes](../nodes/_index.md), "--checktree" |
