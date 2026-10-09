Generation lowers the analyzed IR to an LLVM module and emits an object file. It
validates almost nothing — what it refuses in a program is only what nothing
before it can see: a C name declared two ways, which only the object file's one
symbol table can see (section 2, "Symbols, linkage and COMDATs"); an `as` onto
a struct of another size, which only the data layout measures
([cast](../nodes/cast.md)); and an untyped integer literal too large for the
`i32` default that nothing replaced, which only generation reaches after
everything that could have typed it has run ([literals](../nodes/literals.md),
`ErrorLitRange`); and on a GPU target, a global holding a reference and a
function calling itself, which only the module's globals and its calls show
(section 7). Every assumption in section 5 is a hard prerequisite,
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
   **This is [Performance](../../../../doc/design/performance.md)'s central bet cashed in
   here**, and it **forbids** any safety distinction needing a runtime
   representation.
4. **Generation decides nothing about memory.** Every release, count adjustment
   and drop call was injected by flow analysis; generation replays the lists. ▸
   **Forbids** this phase reasoning about ownership at all — a double-release bug
   is a flow bug, and searching for it here wastes the search. What it adds of
   its own is the roots of traced references (section 3, "Roots"), which are a
   fact of where a value sits on the stack, not of who owns it.
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

**`genlProgram` gives the module the target's triple and data layout as it
creates it**, before any IR is built. The IR builder and the optimization passes
fold sizes, offsets and alignments against the module's own layout, and a module
with none gets LLVM's default, in which `i64` aligns to 4: pointer arithmetic on
a global would then fold to a stride the backend does not use.

`genlProgram` is a strict two-pass walk over the modules:

1. **Symbols** — `genlGlobalSyms` for every module, generating or not, skipping
   private nodes of non-generating modules. Declares every global and function.
   A skipped private node is declared on first use when a public inline body,
   generated in its caller, names it: `genlFnSym` and `genlVarSym` in
   `genlexpr.c` ask for the symbol rather than assume it.
2. **Implementations** — `genlGlobalImpl`, for modules flagged `FlagGenMod`;
   for every other module, `genlImportedInstances`, which generates only the
   instances this compile made of that module's generics. An imported package
   has no instance for an importer to link against — it cannot know which ones
   its importers make — so the importer defines each one it uses, from the body
   the include file carries. A private generic's instance counts too (a public
   generic's body may make one), and the symbol pass skipped it, so it is named
   here.

Then, **the program's stitched init and final** (`genlStitch`), where a call to
`initAll()` or `finalAll()` asked for one (`genlStitchFn`, from the intrinsic's
case in `genlFnCallInternal`): `cone.initAll` calls each module's `initfn` in
`pgm->initorder`, and `cone.finalAll` each module's `finalfn` in the reverse,
declaring the symbol of one this object does not define. They are built last
because they call what the passes above named. Both are internal; the entry glue
that will call them is unbuilt. [module](../nodes/module.md), "Init and final".

**An `imm` global is an LLVM constant only where this object gives it its value
and nothing writes it** (`genlGloVarIsConstant`): it has an initial value, is
not `extern`, not thread-local, and its type has no drop function. A global
without a value is assigned by its module's `init` at run time, an `extern`
one's value is another object's and may be assigned by that package's `init`,
and a finalizing one is handed to its `final` as `&uni` by its module's `drop`.
A constant's loads may be assumed never to change, and its storage may be
read-only.

**A `@threadlocal` global is LLVM `thread_local`** (`DclThreadLocal`, set in
`genlGloVarName` on the definition and on every declaration alike, since each
object reaches it through its own thread-local access sequence). The model is
general-dynamic, LLVM's default: right whether the global ends up in a program
or a shared library, relaxed by an ELF linker building an executable, and
ignored on Windows, whose one access sequence goes through the TEB and
`_tls_index`. Uses reach the global directly rather than through
`llvm.threadlocal.address`, which LLVM reads as the current thread's copy. It
is never a constant: the parser refuses one `imm`, and `genlGloVarIsConstant`
excludes it besides.

Both recurse into a type's method list, into a generic's
`genericinfo->memonodes`, and into an enum extension's copies of its base's
variants, the front of its `derived` list (`structEnumCopyCount`): neither
instances nor copies are any module's nodes. The copies are reached first, ahead of
a generic's own instances, because a generic extension's copies are templates and
their instances are reached only through them. An uninstantiated generic generates
nothing.

### Symbols, linkage and COMDATs

**The rules are not here.** What a symbol is spelled, what linkage it gets and
why are [Names and Namespaces](../../../../doc/design/names-and-namespaces.md), "Symbols"; this phase
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
  together, from the declaration facts and one argument, an `enum
  GenlDefinition`, saying what this object does with the symbol. For a
  declared node `genlDefinition` answers it: `GenlDeclared` unless
  `genlIsDefinedHere` — its module is flagged `FlagGenMod` or it belongs to a
  generic's instance (`dclIsInstance`), it is not `extern`, and a function has
  a body. A defined instance is then `GenlShared` in a described build and
  `GenlDefined` otherwise; anything else defined is `GenlExported` where a
  library compile exports it (`dclIsExported`, below), else `GenlDefined`. A
  vtable, which no node declares, is `GenlShared` in a described build and
  `GenlDefined` otherwise (`genlVtableDefinition`). It is the one place that
  decides them, and it sets no visibility. A `DclSystemCC` function takes the
  x86 stdcall convention whether defined here or not, and the DLL import
  storage class only when it is `extern`; every call to one is made with the
  convention too (`genlFnCallInternal`).
- **`genlComdat`** then derives the COMDAT selection kind from the linkage
  already on the global, at each definition site.

**One symbol is one LLVM global.** LLVM keeps one global per name and renames a
second one added under a name it holds (`abs.1`), which nothing defines, so the
link fails. Two declarations spell one symbol legitimately when both are
C-named — two modules that each declare C's `abs`. So `genlGloFnName` and
`genlGloVarName`, once the global is added and its linkage set, hand it to
**`genlClaimSymbol`**, which does nothing unless LLVM renamed it. Then the
global already holding the name is found (`LLVMGetNamedFunction`, then
`LLVMGetNamedGlobal`), and its declaring node in `gen->symnodes`, the list of
every declaration given a global so far, and one of them gives way:

| The two | What happens |
| --- | --- |
| neither C-named | `errorUnreachable`: two of Cone's own spellings meeting is a compiler defect. Nothing in the suite or Congo's tests reaches it: a candidate reached twice, an instance named by the symbol pass and again by `genlImportedInstances`, a method a vtable declared while its signature was typed, a module's `init` the stitch names — each finds its `llvmvar` set and returns before adding anything |
| the newcomer is local (internal or private) | it keeps the name LLVM gave it: nothing links against a local symbol's name. A private C-named definition beside an `extern` declaration of the same C name stays its module's own, and the declaration still reaches the C function |
| the holder is local | it gives the name up, taking a suffix (and a COMDAT renamed to match), and the newcomer takes it |
| both external, and they disagree — a function's LLVM function type (`genlFnDclType`, so a C-named one's C ABI shape) or `DclSystemCC`, a global's LLVM value type, permission or `DclThreadLocal`, or a function and a global | `ErrorCNameConflict`, at the newcomer, naming the holder's file and line |
| both external, both defined here | `ErrorCNameDefTwice` |
| the newcomer only declares it | it shares the holder's global, and its own is deleted |
| the newcomer defines it, the holder only declares it | the definition takes over: every use of the declaration and every node pointing at it is moved to the definition's global, the declaration is deleted, and the definition takes the name. So the linkage, calling convention, storage class and debug subprogram are the definition's whichever is generated first, and `genlFn` or `genlGloVar` attaches the body or the value to that one global |
| the holder is an external symbol the compiler declared itself | an LLVM intrinsic or a conestd entry it calls by name (`llvm.trap` and the `cone_panic…` entries, `genlPanic`): a function declaration shares it, cast to its own type where they differ. Anything else is `ErrorCNameConflict`. The compiler declares no C function of the C library: a region's memory goes back through the region's `free`, which calls `libc`'s. It declares seven of conestd's, and only while generating bodies, after every declaration has its global: `cone_gcframes` and `cone_traceRoots` (roots, below), `cone_panicIndex`, `cone_panicSlice` and `cone_panicAlloc` (a failed check, section 6), and the thread-locals `cone_barrierHook` and `cone_barrierCtx` (a barrier on the CPU, section 7), each looked up by name first and shared with any declaration already holding it |

"Defined here" is `genlDefinition`'s answer, not whether a body is written: an
imported module's `fn @c` body is a declaration in this object. An error leaves
the renamed global in place, so generation carries on and stays well typed, and
`genpgm` then emits nothing.

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
| a definition of a program compiled with no build description — a function, a global, an instance of a generic, a vtable, a vtable list | `internal`: nothing outside the object may resolve against a program's symbols | `nodeduplicate` — nothing can collide with it, and a duplicate within the object is a real error |
| in a described build, library or program, an instance of a generic, a member of one, or a vtable (`GenlShared`) | `linkonce_odr`: every object that uses it defines it, all the copies are the same, and an unused one may be dropped | `any` — the linker keeps one copy. `nodeduplicate` would make the copies a duplicate-symbol error (LNK2005), and internal would leave two vtables with two addresses |
| a library's export (`GenlExported`) | external: an importer's object links against it, and the optimiser keeps it though the library itself may never use it | `nodeduplicate` — one package defines it |
| a described build's other definitions — a program's, or a library's that are not exported, and every vtable list | `internal` | `nodeduplicate` |
| `main`, or a public C-named definition (`pub` and `@c` export to C) | external | `nodeduplicate` — a duplicate definition is a real error and stays one |
| a private C-named definition | `internal`, as a program's | `nodeduplicate` |
| a definition nothing outside can name — an anonymous `fn`, a string literal | `internal`, set at the site that names it | `nodeduplicate`; nothing can collide with it in any case |
| a declaration — an imported module's function, an `extern` | external, since an LLVM `declare` can be nothing else | nothing: **only a definition may lead a COMDAT**, and `LLVMVerifyModule` rejects one that does not |

**A library compile** is one whose build description says `output: library`
(or `--library` with no description), which sets `opt->library`;
`genlProgram` then records the root module in `gen->libroot`. What it exports
is `dclIsExported`'s answer, the rule L1 and L5 of [Names and
Namespaces](../../../../doc/design/names-and-namespaces.md), "Linkage", state.
It lives in the front end (`ir/export.c`), not here, because the include-file
generator asks the same question to decide what the include file declares
([module](../nodes/module.md), "Generating the include file"), and one function
answering both is what keeps the object and the include file from disagreeing:

- only a definition of the package's own modules — the root, or a module whose
  chain of owners reaches it — never `core` or a package the search path
  compiled in beside them;
- never an instance of a generic, nor a method or static of a generic type's
  instance, nor a function or global of a generic module's instance
  (`dclIsInstance`, which asks the owners too, up to the module: a member of a
  type's instance carries no instantiating node of its own, only its type does,
  and an instance module answers for everything it holds) —
  those are shared instead, below;
- a declaration name resolution marked `DclExpandReached` — named by an
  `inline`, generic or macro body, a trait's default or a generic type's method,
  bare or through a path ([Name Resolution](name-resolution.md), "Contract");
- a module's `init`, its `final` and the `drop` it is given (`DclLifecycle`),
  public or not: the program's stitched init and final call them from its own
  object;
- a public function or global of a module;
- a function of a type an importer can reach — a public type, one an
  expanded body names, or one the package's include file declares
  (`DclIncluded`, which the include-file generator writes before any code is
  generated: an importer holds values of such a type through a field or a
  signature, whether or not it can name it) — where the function is public, or the type holds an
  expanded body (`typeHoldsExpanded`) or its module holds one anywhere
  (`modHoldsExpanded`), which can reach a private method through a receiver
  that name resolution never binds, a type's private members being its
  module's, or the function is the
  type's `final` or `clone` (`fnIsTypeLifecycle`), which an importer's object
  calls wherever it drops or copies a value of the type, naming neither
  (`module_init_link` drops a package's type whose `final` is private), or,
  in a region ref, one of the methods the compiler calls at a reference's
  events — `alloc`, `init`, `aliasRef`, `dealiasRef`, `free`, `mark` — which an
  importer's object calls wherever it allocates, copies, drops or traces a
  reference into the region (`fnIsTypeLifecycle` too; `region_traced_link`
  allocates from and traces into a package's region whose methods are all
  private and none inline), or the
  function meets a requirement of a trait the type is (`fnIsTraitMethod`),
  which a vtable an importer builds calls. That is asked of every trait the
  type took members from (its `traits`), so a later name of its `is` list
  counts as its first does, and so does a trait one of them is
  (`module_build_trait_methods`).

Everything else is internal, as in a program.

**A described build shares** what every object using it produces, the row L2
describes: `opt->described`, set by `parseBuildDesc` for a library or a program
alike, makes an instance of a generic, every member of a generic type's
instance, and a vtable `GenlShared`, which `genlLinkage` lowers to
`linkonce_odr` and `genlComdat` reads as a COMDAT of `any`. The generic's own
package defines the instances it uses; each importer defines the ones it uses,
from the full body its include file carries (a generic cannot be `extern`);
identical instances in two objects merge at link, and one only an importer
makes is in that importer's object alone. A vtable is shared because pattern
matching tells a concrete type by comparing a virtual reference's vtable
address with the one its own object built (`genlIsType`): a reference built in
the program and tested in the library matched nothing while each object kept
an internal copy (measured 23 Sep 2026). The vtable list stays internal — it
holds the implementers one compile saw, so no two objects could agree on one;
so does a folded method's thunk, which a surviving vtable reaches in its own
object. **A compile with no build description is unchanged**: it is the
program's only object, and its instances and vtables are internal.

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
`genlFn`, `genlGloVar`, `genlVtableImpl`, `genlVtable`, `genlStitch`, and the
`StringLitTag` case in `genlAddr`. **A new kind of generated global needs a
seventh.**

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

**A `main` returning nothing is generated returning `i32 0`** (`genlIsVoidMain`).
The C runtime takes what `main` returns as the process's exit status, and Cone's
`void` lowers to the empty struct `%void`, whose `ret %void undef` left the
status to whatever the return register held. So `genlGloFnName` declares such a
`main` with an `i32` return, `genlFn` sets `gen->exitzero` for its body, and
`genlReturn` returns 0 at every return; `genlFnSym` hands a call or a reference
to it the function bitcast to the type its signature declares, so the IR stays
well typed. The test is the symbol `main` itself, as `genlLinkage`'s is, so a
C-named entry, `fn @c("main") start()`, is treated alike. A `main` that declares a
return type returns what it returns.

`genlFn` per function: entry block, a dummy `allocaPoint` alloca, an alloca and
store for **every** parameter, then `genlBlock` on the body, then, where the
function holds a traced reference, its frame's push and pops (`genlRootFrame`,
section 3, "Roots"), then erase the alloca point. Every parameter and local is memory-backed on purpose — the
comment is that all allocas belong in the entry block so `PromoteMemoryToRegister`
and SRoA can undo it.

`genpgm` then optionally verifies, dumps `.preir`, runs LLVM's new pass manager
through `LLVMRunPasses`, dumps `.ir`, and emits. **A release build runs LLVM's
standard pipeline, `default<O2>`**, handed the target machine, so that the
inliner's, the vectorizers' and the unroller's costs are the target's: the
`--cpu` and `--features` the machine was made with, `generic` by default (on
x86-64, SSE2 and no more). That is the pipeline clang's `-O2` runs: SROA,
instcombine, inlining, LICM, unrolling, dead global elimination (so an
internal definition nothing reaches is gone from `.ir`), and the loop and SLP
vectorizers. Measured against `default<O3>` (3 Oct 2026, LLVM 23), O3 ran no
faster on any benchmark but one matrix product (9%), compiled 5 to 11% slower
and made larger code. **No float is ever reordered**: generation marks no
operation fast-math or contractable, so a vectorizer widens only what keeps
each lane's operations in their written order (a loop scaling a slice, a
matrix's columns), never a sum over a loop, and no multiply and add are fused,
whatever the CPU. A `--debug` build runs only
`function(mem2reg,reassociate,gvn,simplifycfg)`, with no target machine, and
turns off the code generator's optimization and enables DWARF. A GPU target's
pipeline is its own, at every level (section 7). **There is no `--release`
flag** — release is the default and `--debug` turns it off.

## 3. Type lowering

| Cone | LLVM |
| --- | --- |
| integer / float | `i1`…`i64`, `float`/`double`. bool is a 1-bit unsigned, char a 32-bit one |
| **`void`** | **`%void = type {}`** — a zero-field named struct, *not* LLVM `void`. A function returning nothing returns `%void`; so does `nil` |
| **permission** | **`%void`** — permissions are fully erased |
| `*T` | `ptr` |
| **`&T`, `&mut T`, `Rc[T]`, `So[T]`** | **`ptr`, identically.** Region and permission contribute nothing to the reference value |
| **`&str`, `So[str]`, `Rc[str]`** (any reference whose target carries its length, `refIsFat`) | **anonymous `{ ptr, usize }`**, as a slice's, in every region and for a borrow |
| **`&Array[T]`** | **anonymous `{ ptr, usize }`** — element pointer at 0, element **count** at 1 |
| **`&<Trait`** | **named `{ ptr, ptr }`** — the object, then its vtable |
| `fn` signature | `LLVMFunctionType`, never varargs; a `&fn` is a `ptr` to it. A C-named function's structs are lowered to the C ABI's shape ("C-named functions and the C ABI", below) |
| struct / trait | named struct, fields in declaration order. **A trait's body is its own fields**, which are a prefix of every implementer's, so `&Trait` points at the trait's layout and reaches the fields the trait declares. Only a type declared `@opaque` is left an opaque LLVM struct, and `DeclaredOpaque` — not `OpaqueType` — is what says so: a trait carries `OpaqueType` because it has no size as a *value*, which does not mean it has no fields |
| enum | `i8`…`i64` by `EnumNode.bytes` |
| tuple | anonymous struct |
| array | nested `LLVMArrayType`; each dimension must be a `ULitTag` |

Every pointer is LLVM's opaque `ptr`, whatever it points at: the table's pointee
lives in the Cone type, never in the LLVM one (section 4).

**A pointee is generated after the type that reached it, not inside it.**
Lowering a `*T`, `&T` or `&Array[T]` queues `T`, and `genlType` generates the queue
when the outermost type it was asked for is done, before returning to anything
but type generation. Generated in place, a struct reaching itself through a
reference is reached again while its own body is still empty: with `A` holding
`So[AState]` and `AState` holding `Option[A]`, sizing `Some[A]` there measured
`A` as nothing, and `Option[A]` came out one byte. Nothing outside type
generation sees the difference, since every pointee is generated before
`genlType` returns to it — which the nullable-pointer flag (below), set when an
enum is generated and read by expression generation, needs.

Verified: `&Array[i32]` emits `{ ptr, i64 }`, with `extractvalue ..., 1` yielding a
*count* of 3 for a 3-element array — not a byte length.

**Erased with no representation at all:** lifetimes (`LifetimeTag` has no
lowering case and would assert), `QuesTag`, `BorrowRegTag`, move semantics,
thread-binding.

### C-named functions and the C ABI

A struct handed to LLVM as a first-class value crosses a call one register per
field, which no C ABI does. (Cone's own convention passes one of more than 16
bytes, holding more than two scalars, by a pointer instead, to its caller's
own storage where nothing can change it during the call, and returns an
aggregate as a vector, as integers or through a slot: section 6, "Large
aggregates" and "Lending a place to a call", which leave a C-named
function's convention alone.) So a
**C-named function a module owns**
(`genlIsCAbiFn`: `DclCName`, its owner a module) — an `extern` one C defines,
a body C calls — has its structs lowered in `genlcabi.c`, the one place that
knows the platform's C ABI (`gen->cabi`, `genlCAbiTarget`, from the triple).
For **Win64**, as clang lowers the same C declaration for
`x86_64-pc-windows-msvc`: a struct of 1, 2, 4 or 8 bytes is one integer of its
size, whatever its fields; any other size is a `ptr` to a copy the caller makes
in its own frame; and a return of any other size is `void`, through a hidden
first parameter marked `sret(%T)`, the caller's result slot. **SysV x86-64 and
wasm32 are not built**: their structs still cross whole, which matches their C
ABI only for a struct of one scalar.

An **integer narrower than C's `int`** keeps its LLVM type, but carries the
widening C's convention gives it in its register (`genlCAbiExtend`), on the
declaration or definition and at each direct call (`genlCAbiMarkExtends`), as
an argument and as a result. A `bool` is C's `bool`, `zeroext` on Win64, SysV
and wasm32 alike; without it a bool whose register's upper bits were never
cleared reaches C, which trusts the whole byte, as that register's low byte. An 8- or
16-bit integer is `signext` or `zeroext` by its sign on SysV and wasm32, and
unmarked on Win64, where the callee widens it: the marks clang gives the same
C declaration for each target. A C-named body's incoming bool is then trusted
to be 0 or 1. A `bool` field is one byte holding 0 or 1, as a C `bool` field
is, so a struct holding one needs nothing more.

Only a Cone **struct** is lowered (`StructTag` whose LLVM type is a struct). A
slice, a virtual reference, a tuple or an array has no C counterpart and keeps
Cone's convention — a slice's pointer and length arrive as two arguments, which
is how a C function's `(char *, size_t)` takes them. A struct the
nullable-pointer optimization made a bare pointer is a pointer. `cstr`, C's
`const char *`, is a struct of one raw pointer, not made a pointer by anything:
it is 8 bytes, so Win64 passes and returns it as one integer, the register a
pointer is in, and C reads it as the `char *` it is. It has no case of its own
here.

The lowering is applied at the four places a function's values cross:
`genlFnDclType` (its LLVM function type, which `genlGloFnName` declares and
`genlSymAgree` compares), `genlFnDclCall` (a direct call, from
`genlFnCallInternal`: the integer made through a stack slot, the copy, the
result slot, and the Cone value rebuilt from what came back),
`genlFnDclParm` (each parameter's Cone value in `genlFn`'s prologue) and
`genlFnDclReturn` (`genlReturn`, through `gen->fndcl`). Every other function
passes through the same four with its Cone signature untouched.

**Not lowered:** a call through a `&fn`, which is typed by its Cone signature
(`genlPointeeType`) and carries no widening marks. A C-named function's address handed to C is right, since
C calls it; Cone calling a C-named function with a struct parameter or result
through a `&fn`, or C's function pointer with one, is not (such a call takes
Cone's convention, section 6, "Large aggregates"). A type's `fn @c` method keeps
Cone's convention, since a vtable slot calls it by its Cone signature.

### Enums

Three shapes, the first two chosen in `genlSetupTaggedTrait`:

- **Nullable pointer.** Exactly two variants under `SameSize`, one with one
  field and one with two whose second is a pointer-like: **no struct is emitted
  at all**, and the value *is* the pointer. A null pointer is the empty variant.
  A slice or a virtual reference is two words, and the value is that pair: its
  first word, the pointer, null is the empty variant, so the literal writes that
  word and the variant test (`genlIsType`) and the drop read it, the second word
  left alone. Each enum decides this for its own set: an extension's variants are copies, so an
  `Option`-shaped base keeps the layout whatever extends it, and the extension, with
  a third variant for which there is no pointer to be, is tagged. The same holds per
  instance: `Option[&i32]` is a bare pointer beside a tagged instance of an enum
  extending `Option[T]`.
- **Same size.** Each variant is re-emitted as a named struct with `[N x i8]`
  trailing padding to one size: the largest variant's store size, rounded up to
  the strictest alignment of any variant's field, or a byte-aligned largest
  variant would leave a stricter one padded past it. Reading a `%Shape` as a
  `%Circle` is safe only because they are the same size.
  - **The enum's own body is its own fields, then its payload** out to that
    size, then a zero-length array of the most strictly aligned field type where
    the members alone would under-align it. **It is never a copy of one
    variant's layout**, because an enum value is loaded, stored and passed as a
    first-class aggregate, and LLVM does not carry an aggregate's padding bytes:
    a smaller variant's field in a hole of the largest's layout — a `bool` at
    byte 1 beside an `i32` at byte 4 — was lost in the copy. The enum's own
    fields are the discriminant and any common fields, which begin every variant
    at the same offsets, so a common field or the tag is still read by index.
  - **The payload is a scalar wherever every variant that has anything in those
    bytes has the same integer, float or pointer there, and bytes everywhere
    else** (`genlEnumPayload`), the members explicit through to the size so no
    padding is left that a variant uses. `Option[i64]` is `{ i8, [7 x i8], i64 }`;
    `%Shape = { i8, i32, i32, i32 }`, where `Circle` and `Rect` agree on an `i32`
    at byte 8; `%Solid = { i8, i32, [8 x i8], i64, i64 }`, where `Cube`'s first
    `i64` lies over their `i32`s, so those are bytes, and its other two are
    alone. An array, a vector or any other type is bytes, and past 64 leaves
    between the variants the whole payload is bytes, as `{ i8, [15 x i8], [0 x i64] }`
    for an `i64` beside an `f64`. **Why:** bytes are all that scalar replacement
    cannot see through. A payload of fifteen bytes was broken into eight byte
    values at every load of an `i64` and put together at every use, and a loop
    over `Option[i64]`s did not vectorize (measured 8 October 2026; with the
    `i64` named, it does). The GPU keeps the bytes and integers below.
  - **On a GPU target the padding carries the alignment itself**: bytes out to
    the strictest alignment, then integers of that size (`Option[f32]` is
    `{ i8, [3 x i8], [1 x i32] }`), since a zero-length array is a runtime
    array to SPIR-V, which its OpenCL form refuses (`LLVM ERROR`). Integers
    have no holes either. An alignment past 8 keeps the zero-length array.
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
pointer, called with the function type `genlVtableSlotFnType` gives it, **whose
self parameter is erased to a plain pointer** so one slot type serves every
implementer, or an `i32` **byte offset** for a virtual field, **then one last
`ptr`: the implementer's core `TypeRecord`** (`genlTypeRecordOf`, so its size,
alignment and finalizer), which an owning virtual reference's death reads
(`genlVirtRecord`; [references](../nodes/references.md), "Generation"). Last,
so every slot's `vtblidx` is what it would be without it and a dispatch pays
nothing for it; every vtable has it, a marker trait's too, so none is empty.
One `internal
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

`genlRefTypeSetup` builds, per interned reference type, the first time an
allocation or a region header asks for it (generating the reference's own type
does not, since that would generate the pointee inside whatever holds it):

```
%refstruct = type { <region>, <perm>, <value> }   ; RegionField, PermField, ValueField
```

Verified for `Rc[mut, T]` of an `i32`:

```llvm
%void      = type {}
%rc        = type { i64 }
%refstruct = type { %rc, %void, i32 }    ; 16 bytes
```

**The reference value points at `ValueField`, not at the allocation base.**
`genlallocref` GEPs to that field and hands the result out. So the header sits
*before* the payload and the reference cannot see it.

What follows from that:

- **A region method is handed the header, found from the layout.**
  `genlRegionHeader` steps back from the value pointer, a GEP over `i8`, by
  `LLVMOffsetOfElement(structype, ValueField)` bytes — 8 for `Rc`, 0 for `So`,
  16 for a region with a `{usize, u32}` header; for `So` the header is the
  value pointer itself, and no instruction is emitted. Every call of `aliasRef`, `dealiasRef` and `free` goes through it, so a
  wider header or a permission with state moves the value and the header with
  it. The optimizer folds the byte step and the region's field GEP into one
  constant offset: for `Rc` the same address the count has always had.
- **A lock permission's lock is the header's `PermField`.** A borrow through
  `Arc[Mutex, T]` calls the lock's acquiring method on it as its guard is made
  (`genlLockAcquire`), and the guard's release calls the giving-back one first
  (`genlRegionDealiasPart`), both reaching it from the value pointer as a region
  method reaches the header ([references](../nodes/references.md), "Lock
  permissions"). A guard's type lays its permission out as the lock
  (`genlType`'s `PermTag` arm), so its header is the lock-managed one's.
- **An owning virtual reference is the fat `{ptr, ptr}` value, and its concrete
  type is read from the vtable's last slot.** `genlRefPtr`, at the entry of
  `genlRegionDealias` and `genlRegionAlias`, takes word 0, the object, so every
  release site — scope exit, a `RefCountNode`, `genlStore` — hands over the
  value as generated. Its type has no `typeinfo`, so `genlOwnerHeader` steps back by the
  region and permission's size rounded up to the `align` of the implementer's
  `TypeRecord`, loaded through word 1 (`genlVirtHeader`; nothing is loaded for
  a `So`, whose header is empty), and its death calls the record's `finalize`
  on the object (`genlVirtFinalize`) where a plain reference's calls
  `genlFinalizeAt`.
- **An owning reference to a body that carries its length is the fat `{ptr, usize}`
  value, and its header is as constant as a thin one's.** The element type
  stands in the allocation's value field (`genlRefTypeSetup`), since the body has
  no size, so the header sits a fixed offset before the first element, found
  from word 0 by `genlRegionHeader` with no record read. The allocation asks
  `alloc` for that offset plus `count * sizeof(element)` and copies the view's
  elements in (`genlallocref`); the owner's death finalizes the elements, where
  they need it, in a loop over word 1, and then frees the header
  (`genlRegionDeath`).

**The release routines call the region's methods and know no region.**
`genlReleaseOwning` is one owner going away: `genlRegionDealias` calls the
region's `dealiasRef` and branches on its `bool` to the death. A region without
`dealiasRef` goes straight to the death when it is `Move` (single owner,
`regionIsMove`), and emits nothing at all otherwise: that value never dies by an
owner, whether its copies are counted or free. The death,
`genlRegionDeath`, runs in two steps: the value dies in place
(`genlFinalizeAt`), as a value on the stack does at its scope's end — for a
single reference the value it points at, for a virtual reference through its
record's `finalize` — then
the region's `free` if it has one.

**One routine is a value's death in place, whatever its type**
(`genlFinalizeAt`), and every death reaches it: a local's at its scope's end
(flow lists the variable, or for a struct or an enum a call to its drop), a
region value's before its `free`, a field's inside its holder's drop, a
module's global in the module's `drop` (listed as a local is), and the
`finalize` intrinsic. An owning reference is released (`genlReleaseOwning`). A
struct or an enum calls its drop, which is the whole death, its owners' release
included. A tuple finalizes each element that needs it, in order, each reached
by its address (`StructGEP2`). A fixed-size array finalizes each element in
element order, first element first, as a struct's fields die in field order,
in a loop (`genlEachElem`): the release pipeline unrolls a loop where that
pays, while no pass rolls code written out back up into a loop. A type for which `itypeNeedsFinal` is false generates
nothing. The drop call recasts the value's pointer to the drop's parameter
type (`genlCallDrop`), since a variant laid out as a nullable pointer is reached
through a pointer to its enum.

**A store releases what it replaces** (`genlStore`), after the new value is
evaluated and before it is stored: an owning reference, or a tuple of owners
only, through `genlReleaseOwning`, as always; anything else with a death dies
in place (`genlFinalizeAt`). Not where flow found the target held nothing
(`FlagFirstAssign` on a variable, `FlagPartNoPrior` on a part of a local's own
value), and not a finalizing value reached through a raw pointer, which may be
memory never given one (`genlStoreThroughPtr`; an owner there is released as it
always was). A place reached through a reference always holds a value.

**Drop flags.** A variable flow gave `VarDropFlag` has an `i8` slot of its own,
`<name>.held` (`genlDropFlagBegin`), stored where the variable begins: `1` at a
declaration with a value or a parameter's entry (a function's, an inline
body's), `0` at a declaration without one. Where its value arrives or leaves
the slot is stored again (`genlDropFlagSet`): `1` after a store over the whole
variable (`genlStore`, a left assignment), `0` or `2` at a name use flow marked
as a move out of it or out through it (`genlDropFlagUse`, from `genlTerm` and
`genlAddr`; a match's binding's move clears the matched value's variable's
flag). A release that depends on it — a `DropFlagNode` in a release list, a
store's old value under `FlagDropTest`, a `HollowNode` with `test` — loads the
slot, compares it with the state it runs in, and branches round the release
(`genlDropFlagIf`, `genlDropFlagEnd`). The optimizer's mem2reg keeps the slot
in a register and folds each test where one path's value is known.

**A drop the compiler gives a type is built here, not lowered**
(`genlTypeDrop`, reached from `genlFn` for a function `structIsGeneratedDropFn`
recognizes). A struct's (`genlStructDrop`, for the function `structSetDropFn`
made) runs the calls its block holds — the type's own `final`, then, for a
variant keeping its enum's, the enum's — then finalizes each field that needs
it in place, in field order, then releases each owning reference a field holds,
in field order: the ruled order, `final`, fields, owners [Jon 26 Sep]. A
variant laid out as a nullable pointer is only its reference, so its drop loads
the value itself as that reference and releases it. An enum's
(`genlEnumDrop`, for the function `structSetEnumDropFn` made, which has an
empty block) reads the tag through the enum's own layout and switches on it;
each variant with anything to do is a case, which recasts the pointer to the
variant and finalizes it in place — the variant's drop. A variant with nothing
to do has no case, and the default leaves. The nullable-pointer layout has no
tag: the value is the one variant's reference, tested against null. No
expression places a generated drop's calls, so in a debug build each is placed
at the type, where a call without a location would fail verification.

**A copy of a value holding counted references its death releases** (flow's
`flowHeldCounted`: a struct, an enum, a tuple or an array holding one, however
deep) reaches `genlAliasHeld` from the `RefCountNode`: the copied value is
stored to a slot and walked as its death would walk it — each field of a
struct, the variant an enum's tag picks, each element of a tuple, each element
of an array in a loop — calling `aliasRef` on each counted reference there. A
tuple's `RefCountNode` counts per element, and an element holding counted
references rather than being one is stored to a slot and walked the same way.

**A hollow death** (`genlHollowDeath`) is the death of a value that, or an
element of which, was moved out through its sole owner (flow's `HollowNode`).
`genlHollowRelease` turns each recorded move into a path of steps outward from
the variable (`genlMovedPath`: dereferences and element indexes — never a field
access, since nothing moves out of a field), and the owner goes away through
`genlRegionDealiasPart`, the same `dealiasRef` question with the hollow death in
place of the death. That runs no finalizer for the value, releases what is left
(`genlReleasePart`), then calls `free`. A path that ends at a value moved all of
it out, leaving nothing to release; one that runs on through an owning
reference (`**b`) makes that reference's own death hollow in turn; one through
an array element finalizes none of the array, since which elements are left is
not tracked: the ones that did not move leak rather than one being finalized
twice. A slice's elements are not walked, for the same reason.
`genlRegionAlias` calls `aliasRef` once per owner a `RefCountNode` adds — written
out in line up to `RegionAliasUnroll` (16), a loop beyond, since calls written
out fold together in either pipeline, while a loop of them folds only where the
release pipeline chooses to unroll it. Core's methods are
`inline`, so each call is the method's body pasted at the site
(`genlFnCallInternal`); after optimization `Rc`'s and `So`'s events are the
instructions the compiler used to emit itself.

**A type record is a constant, one per type in each object**
(`genlTypeRecord`), built the first time a region's `alloc` asks for one
(`genlallocref`, where `regionAllocTakesRecord`), `mem.typeRecord[T]()`
names one, or a vtable is built for `T` (`genlVtableImpl`), and found again by type (`itypeIsSame`) from a list on the
generator's state. It is a private global `cone.tyrec.<n>` of core's
`TypeRecord`, whose layout the compiler checks as it builds the first — two
`usize`, a function pointer of `fn(p *u8)` and one of `fn(p *u8, mode u32)`, a
`u32` — and fills with the value's ABI size and alignment (as `mem.sizeof` and
`mem.alignof`), its finalizer, its trace, and flags (bit 0, `itypeNeedsFinal`;
bit 1, `itypeHoldsTraced`). The finalizer of a type that finalizes is a
private function `cone.tyrec.final.<n>` whose body is `genlFinalizeAt` on its
parameter, and the trace of a type holding a traced reference is a private
function `cone.tyrec.trace.<n>` whose body is `genlTraceAt` on its two
parameters; each is generated on the spot by `genlTypeRecFn`, with the
generator's per-function state set aside around it, as `genlFn` does, and in a
debug build has a subprogram and a location at the type, since the calls it
makes (an `Rc` owner's inline `dealiasRef`, a region's inline `mark`) need one.
The finalizer of a type with nothing to finalize points at one private
do-nothing function, `cone.tyrec.nothing`, and the trace of a type holding no
traced reference at another, `cone.tyrec.untraced`, so no slot is null. The
record is remembered before its functions are generated, so one that asks for
records, its own type's among them, finds them. Being private, two objects hold
two records of one type; nothing compares records across objects.

**A trace walks a value statically and completely** (`genlTraceAt`, the body
of a record's trace and the expansion of `mem.trace`): for every reference into
a region declaring `Traced` that the value holds inline, it loads the reference
and, where it is not null, calls the region's `mark` with the header
(`genlRegionHeader`) — and, where `mark` takes them
(`regionMarkTakesContext`), the reference's permission, a constant (its
`PermNode` flags), and the mode the trace was called with. It reaches a struct's
fields and a tuple's elements that hold one (`itypeHoldsTraced`), each element
of a fixed-size array whose element type does, in a loop (`genlEachElem`), and
the variant an enum's tag picks, by a switch, as `genlEnumDrop` dispatches; the
nullable-pointer layout, which has no tag, is its one reference — and so is a
variant of such an enum held as itself (`*t` for `t &Some[Gc[T]]`, a birth),
whose layout is its enum's (`genlTraceNullableVariant`). It stops at
every other reference and every pointer: the placement rules keep a traced
reference from hiding behind them. It does not share the finalizer's walk,
which finalizes an owner's value where the trace must not follow one.

**An allocation runs in one order, in every region** (`genlallocref`): the
value init's arguments are evaluated (`genlNewArgs`; for the implicit init,
the struct's literal, and for a finished value, `new Rc[i32](5)`, the value
itself, moved); then `alloc`, and its null check; then the region's `init` and a lock
permission's fill their parts of the header in place, each called with its
part's address as its `self`; then a declared init fills the value's part in
place (`genlNewFill`), or the value made or finished is stored there, no
value init running, or an array's contents (`FlagAllocFill`, the literal
not evaluated before `alloc`) fill it in place, element by element and run by
run (`genlArrayLitInto`), each value evaluated then; then the reference
goes to its destination. A declared init fills in place in two other places:
a local it initializes, and a construction written raw,
`mem.writeRaw[T](p, new T(...))`, whose init is called with `p` as its `self`
(`genlFnCall`) -- how an actor's state is made in its block, so that its init
runs on the state where it stays. A `trynew`'s contents are therefore evaluated only
on the path where the memory was had. A traced region's `alloc` may collect, so the
arguments' traced parts are births ("Roots", below), rooted while it runs.
And `alloc` links the new block into the collector's heap, where an `init`
or a value of the contents that allocates may run a collection step around
it: so before anything else runs, the value's part is zeroed when its type
holds a traced reference (`itypeHoldsTraced`; an array by a `memset`, never
one aggregate store), and the block is rooted in a slot of its own
(`filling`), which a step finds it through, its fields null or filled. The
init's stores into it take the write barrier, as any store through a
reference does, and so does each element the contents store, since the next
element's value may run a step. A value made before `alloc` and stored whole takes the
barrier only where a region's or permission's `init` ran in between, since
otherwise nothing can have marked the block yet. Once filled, the slot is
nulled and the reference becomes the allocation's birth, or the local it is
given to, so the block lives no longer than its owners.

`So` and `Rc` are declared in Cone source in the core package,
`packages/core/src/core.cone` ([What a region is](../nodes/module.md)). `malloc`
and `free` are `libc`'s ordinary `extern` declarations, which `core`'s import
of `libc` puts in every compile; the regions' `alloc` and `free` call them by
their qualified names, and a program's own declaration of `free` meets
`libc`'s as any two declarations of one C name do (`genlClaimSymbol`).
`conestd` supplies stdio and the head of the roots' chain and its walk (below),
no allocator.

### Roots: the shadow stack

**Every function holding a traced reference on its stack links a frame of them
into one chain** while it runs, so a collector can find every live one:
`mem.traceRoots(mode)` is a call to conestd's `cone_traceRoots`
(`packages/conestd/roots.cone`), which walks the chain from its head,
`cone_gcframes`, and calls each root's record's trace. A **root** is a stack
slot whose type holds a traced reference (`itypeHoldsTraced`), noted as it is
made (`genlRootNote`, on `gen->roots`):

- a **local's or a parameter's** alloca (`genlLocalVar`, `genlParmVar`, and an
  inline function's parameters, which are its caller's slots and so in its
  caller's frame); a match's binding by value has no alloca of its own, and so
  no root: it is the matched value's variable's slot (below);
- a **birth's**: a slot of its own for each site that makes such a value
  outside a local, stored as soon as the value exists (`genlRootBirth`, from
  `genlExpr`, whose switch is `genlTerm`), so that no collection during a later
  call of the same expression finds it only in a register. `genlIsBirth` says
  which: an allocation, a call's result, a load through a reference or
  pointer (a dereference, or a field or element whose address is reached
  through one: `genlAddrThroughRef`), the old value a swap or `<-` hands back,
  and a cast making a traced reference of something holding none. A value about
  to be given to a local skips its birth (`genlExprForLocal`: a declaration's
  value, and an assignment's to a variable whose old value's release does
  nothing), the local being its root. A birth slot keeps what it holds until
  its site runs again or its function returns: a little floating garbage,
  bounded by the sites. A borrow is never a root: what it borrows from is held
  by an owner that is, for at least the borrow's life.

Once the body is generated, `genlRootFrame` builds the frame of a function
with any root. One with none gets nothing, which is why a program with no
traced region generates exactly what it did before roots existed.

```llvm
@cone.roots.<k> = private constant { i64, [n x ptr] } { i64 n, [n x ptr] [<each root's record>] }
%gcframe = alloca { ptr, ptr, [n x ptr] }       ; prev, map, each root's address
```

The **push** goes at the top of the entry block, after its allocas and before
anything the body does: every root zeroed (a collection may come before a local
is assigned, and a trace passes a null reference over), each root's address and
the map stored into the frame, the chain's head loaded into the frame's `prev`,
and the frame stored as the head. The **pop**, that loaded head stored back,
goes before **every `ret`**, found by scanning every block's terminator rather
than by trusting the return sites: a `return` from inside loops and nested
blocks, and the function's end, all leave through one. `break` and `continue`
stay in the function and need none; a panic aborts, so nothing unwinds past a
pop. The map's records come from core's `TypeRecord` (`genlTypeRecordOf`,
handed `typeRecordStruct`). A record's own finalizer and trace are functions
`genlTypeRecFn` builds mid-function, which root themselves the same way and set
the function's roots aside around them (`genlRootsSave`, `genlRootsRestore`),
as `genlFn` does for its own. Every root is an alloca whose address is stored
into memory, so `mem2reg` never promotes it and no pass can shorten its life;
LLVM's inliner moves an inlined callee's frame into its caller with its push and
pop. The chain is one global: a thread would need its own, thread-local.

Where a **safe point** would go — a check whether to collect, rather than
collecting inside `alloc` — is at a frame's push, where every parameter is
rooted and nothing else is yet, and on a loop's back edge (`genlBlock`'s branch
to `blockbeg`) for a loop that calls nothing. Nothing emits one yet.

### The write barrier

**After every store of a traced reference into memory that is not a local,
the reference goes to its region's `writeBarrier`**, where the region declares
one (`regionHasBarrier`). A collector that marks while the program runs needs
it: a store could otherwise put an object it has not reached into one it has
finished with. The barrier is keyed on **what was stored**, never on the
container, so a store through a borrow — which cannot know what it points
into — gets it as surely as one through a `R` reference.

- **Where.** `genlStoreBarrier`, after each store an expression makes: an
  assignment's (`genlStore`, a parallel assignment's each), `:=`'s, and each
  half of a swap. The destination is memory other code can reach when
  `genlAddrThroughRef` says its address is reached through a reference or a
  pointer — a dereference, a field or element of one, a slice's element, a
  virtual reference's field — and the barrier goes only there. A variable, a
  field or element of a local value, and a parameter get none: they are roots,
  which a collector traces again, uninterrupted, before it ends a mark. An
  allocation's store of a value made before its `alloc` gets none unless a
  region's or permission's `init` ran between them: a new object nothing has
  run beside cannot have been finished with. A declared init's stores into
  its `self` are stores through a reference, and get it. `mem.writeRaw` and
  `mem.moveRaw` get none, since their
  instances at a type holding a traced reference are refused.
- **What.** `genlBarrierAt` walks the value just stored, at its destination,
  with the trace's walk (`genlTraceWalk`), handing each reference into a
  region with a `writeBarrier` to it as the header of what it points at
  (`genlRegionHeader`), and passing over a null one: a reference itself, each
  such field of a struct, element of a tuple, element of a fixed array (in a
  loop), and the fields of the variant an enum's tag picks. It descends only
  into parts holding such a reference (`genlHoldsBarriered`), so a type
  holding none emits nothing — every type, in a program with no traced region,
  which is why such a program generates exactly what it did before barriers
  existed. The walk is shared with `mark`'s: the same routines, the method
  chosen by a flag set around the walk (`genlTraceBarrier`), cleared around a
  trace generated while a barrier walks.
- **The call** is an ordinary region method call (`genlFnCallInternal`), so an
  `inline` `writeBarrier` is expanded at the store: the `collector` package's
  tests a flag in line and calls out only while it is on: while marking, and
  between collections when it runs generationally. The method must
  not collect: no birth slot holds what a swap or `:=` hands back until the
  barrier has run.

## 4. Pointer levels

This is what the CLAUDE.md warning is about. The conventions:

| Value | LLVM level |
| --- | --- |
| a local or parameter (`var->llvmvar`) | **pointer to** its type — always an alloca; a match's binding by value (`flowMatchInPlace`) shares the matched value's variable's, read through the variant's type, which every variant, padded to the enum's size, allows, as the conversion to the variant reads it (`genlLocalVar`) |
| `genlExpr(nameuse)` | the loaded value |
| `genlAddr(x)` | pointer to `x`'s type |
| `&T` value | a `ptr` to the `T` |
| `&Array[T]` value, `&<Trait` value, a reference whose target carries its length (`&str`, `So[str]`) | an **aggregate value**, not a pointer |
| owning reference value | a `ptr` to the `T`, **past** the header |
| allocation base, the region's header | `ref` stepped back by the value's offset in `%refstruct` (`genlRegionHeader`) |
| vtable field slot | an `i32` **byte offset**, applied to the object pointer as a GEP over `i8` |
| vtable method slot | reached by `structgep` **then load** |
| vtable record slot (last) | a `ptr` to the implementer's `TypeRecord`, `structgep` then load |

**What a pointer points at is never asked of the LLVM pointer.** Every load,
GEP and call names the type it reads, steps over or calls, and that type comes
from the Cone type: `genlPointee` for what a reference, pointer or slice points
at, `genlAddrType` for what `genlAddr`'s address points at, the function's own
signature for a call, and `genlVtableSlotFnType` for a call through a vtable
slot. LLVM's pointers are opaque: a pointer is only `ptr`, with no element
type to ask, and the bitcasts between pointer types generation still emits fold
away as they are built. So a load or GEP one level off is not even an LLVM type
error: the verifier accepts it. `genlAddrType` reads through a dereference to the reference's
own pointee rather than the dereference's type, because a dereference the
compiler builds itself — a synthesized drop's — carries no type.

Concrete hazards, each of which has been gotten wrong here before:

- **A struct's drop must load after `StructGEP`.** The GEP gives the address
  of a ref-typed field; the release routines want the reference the field holds,
  so `genlFinalizeAt` loads it.
- **`genlAddr`'s array index uses `genlAddr(objfn)` for an array but
  `genlExpr(objfn)` for a reference to one.** An array *is* memory; a reference
  *holds* the address. One level apart, same GEP shape.
- **An aggregate with no memory gets a temporary.** An index needs its array's
  address and a field its container's, so `make().m[1]` asks `genlAddr` for the
  address of a call's result. Its `default:` arm stores an array, struct or
  tuple value into an unnamed alloca and returns that; any other value there is
  `ErrorUnreachable`. Writing into or borrowing such a temporary is refused at
  type check, so the alloca is only ever read.
- **`genlRegionHeader` steps back in bytes, a GEP over `i8`**: a GEP over the
  value's own type would scale the offset by the value's size.
- **A struct field read and a field address are different instruction
  sequences, chosen by `FlagBorrow`** — not by context. Without the flag,
  generation loads the *whole aggregate* and `extractvalue`s, except through a
  dereference (`self.cnt` in a method, `p.x` for a reference `p`), where it GEPs
  to the field and loads it alone: a store to the field and a later read of it
  are then the same address and type, which `GVN` forwards and which an
  aggregate load would hide. With the flag, it GEPs. Getting the flag wrong is
  not a type error.
- **Mutating intrinsics take self as an lvalue pointer; non-mutating ones take a
  value.** The switch for the intrinsics built in C dispatches on the LLVM *type
  kind* of argument 0, so both land in the same branch and are told apart only by
  which intrinsic it is. What that pointer points at — a number to add to, or a
  pointer to step — is read from argument 0's Cone type, which `genlFnCall`
  passes down as `selftype`. The intrinsics declared in core never reach that switch:
  `genlDeclaredIntrinsic` decides each by its kind and its instance's Cone type
  ([intrinsic](../nodes/intrinsic.md)), and a new intrinsic goes there, never
  into the LLVM-type switch. An atomic one is taken before either, in
  `genlFnCall`, by `genlAtomicIntrinsic`, since its orderings are the call's
  arguments rather than its instance's.
- **A shift is a compare and a select, never a bare LLVM shift.** Cone's `<<`
  and `>>` are defined for every amount: the width or more gives 0, or the sign
  for `>>` on a signed integer, the amount read unsigned so a negative one is
  past the width too (`doc/reference/refexpr.html`, "Shift operators"). LLVM's
  `shl`, `lshr` and `ashr` by the width or more are poison, so `genlShift`
  compares a run-time amount with the width and uses the shift's result only
  below it (a signed `>>` instead clamps its amount to width - 1), and writes no
  compare for a constant amount. The optimizer removes the compare where it can
  prove the amount below the width (`n & 63`, `core_genllvm_shift`). An LLVM
  shift written for a Cone shift anywhere else brings the poison back; the
  masked shifts, whose amount is masked first, are the one other place one is
  written (`genlBitIntrinsic`).
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

**An arm whose condition is a constant of the build is decided at
generation.** `genlIf` asks `intrinsicBuildConst` of each condition:
`isDebugBuild()`, the provisional target-OS and `-D` constants beside it
(TEMPORARY; its final design is open), or `!`, `and`, `or` of them. A false
arm generates nothing, not even its test; a true one generates its body as the
`else` would and ends the chain, the arms after it generating nothing. A call
on an untaken side therefore never reaches the object, in either build, and an
`extern` only that side names is declared and never referenced. Everything
before generation still sees both sides ([intrinsic](../nodes/intrinsic.md),
"Constants of the build drop the untaken side at generation").

Short-circuit `and`/`or` are two blocks and a 2-way `i1` phi. `not` is
`xor i1 %x, true`.

**`FlagInline` functions are inlined by the Cone generator, not by LLVM.** They
get no symbol at all: their parameters become allocas at the call site and their
body is generated inline, as a block whose returns are breaks out of it. With
more than one return that block converges on a phi of the function's return
type, which type check stored as the body block's `vtype`. This is how the region allocator becomes a direct
`malloc` call at each allocation. Having no symbol, one cannot be borrowed:
`borrowTypeCheck` refuses `&name` on an inline function (`ErrorInlineRef`), so
`genlAddr`'s function arm never meets a declaration without an `llvmvar`.

**A failed check is a cold call and `unreachable`** (`genlPanic`). The three
checks the compiler inserts — an index against its count, a slice's range
against its count, a region's `alloc` answering null — each branch to a block
of their own that calls conestd's entry for the failure (`cone_panicIndex`,
`cone_panicSlice`, `cone_panicAlloc`, in `packages/conestd/panic.cone`), handing
it the values compared (each a usize), the source file's name and the line,
and ends in `unreachable`. The entries are declared `noreturn`, `cold` and
`nounwind`, so the check costs the hot path a compare and a branch LLVM
expects never to take, and the call is laid out of line. Nothing joins back
from the failure block: the allocation's phi has one incoming edge, not two,
unless a `?` allocation's null path is one. WebAssembly links no conestd, so
there each failure is `llvm.trap` and `unreachable`. Nor has a GPU: there it
is a call to `cone.gpu.fail`, which a kernel turns into a record in its error
buffer and a return (section 7, "Compute entry points").

The file is the name without its folders (`genlSrcFileName`), a private
constant made once per file per module; `srcFile()` and `srcLine()` generate
the same constants where their call is, and a default value of either is a
copy of the call placed at the call taking it (`intrinsicSrcCallAt`, in
`fnCallFinalizeArgs`).

Bounds checks are emitted for arrays and slices, per dimension, against the
compile-time extent or the slice's count word. **A raw pointer index is not
bounds checked.**

**A call that does not return ends its path.** Type check makes a block that
ends in a call returning `Never` end in a `return` of that call
(blockTypeCheck), and `genlReturn` and `genlBreak` generate such a return, or
one of an `if` every path of which jumps away, as the value and then
`unreachable`: nothing is released and nothing handed back. `genlIf` leaves a
branch that ends in a return out of its phi, and answers `undef` where no
branch reaches the end. A function returning `Never` is declared `noreturn`.

### Temporaries

**A temporary is kept in a slot and finalized where its part ends.** Flow wraps
each in a `TempNode` ([Flow](flow.md), "Temporaries"). Generating one
(`genlTerm`, or `genlAddr` where its field or element is wanted) generates its
value, stores it into an alloca of its own (`genlTempKeep`) and pushes that slot
on `GenState.temps`, a stack in evaluation order; a `kept` one is generated as
its value alone, or, where its address is wanted, stored in its slot and never
finalized. The end of each part that makes temporaries finalizes those
it pushed, newest first, and pops them (`genlTempsEnd`), each as a local dies
(`genlFinalizeAt`), or hollow where flow noted a value moved out through it
(`genlTempRelease`, `genlMovedPath` walking to the node instead of a variable):

| Part | Where it ends |
| --- | --- |
| a statement | `genlBlock`, after the statement; for a `blockret`, and an inlined body's one `return`, after its value and before the block's `dealias`. In a block flagged `FlagKeepTemps`, an operator's rewrite ([Flow](flow.md), "Temporaries"), after its last statement only |
| an `if` or `elif` condition, and so a `while`'s | `genlIf`, once the condition is computed, before the branch |
| the right operand of `and` or `or` | `genlLogic`, before the branch to the phi |
| an array's repeated value generated in a loop | `genlArrayRun`, each time round, after the element's store |

A part's end runs on the one path that reaches it, since every construct that
branches inside an expression is a part of its own, so the slot of every
temporary still on the stack is filled wherever its finalization is generated.
**A jump finalizes without popping** (`genlTempsJump`): a `break` or a
`continue` every temporary made since its target block began
(`GenBlockState.tempmark`), a `return` every one made since its function began
(`GenState.tempbase`), after its value and before its `dealias`; the
statement's entries are then dropped unfinalized, as nothing after the jump
runs. A function generated in the middle of another (a drop a death asks for)
starts its own base and leaves the stack as it found it.

**A `break`'s phi edge is recorded after its releases**, from the block the
jump leaves: a release (`dealiasRef`'s test) splits the block.

**A small aggregate is merged through a slot, not a phi** (`genlMergeSlot`).
Where a phi block (`genlBlock`, `genlBreak`) or an `if` (`genlIf`) converges on
a struct or array of at most `GenlAggCopyMin` bytes, each path stores its value
into an `alloca` made in the entry block (`%merge`) and the join loads it. LLVM
never splits a phi of an aggregate into one per scalar, so a loop carrying one
(an inlined function returning an `{ok, v}` struct or an `Option[i64]`) stayed
scalar, while mem2reg and SROA turn the slot into a phi per scalar. Measured
8 October 2026 on a cursor summing a `&Array[i64]`: the optimized loop was
scalar before and the same vector loop as a counted `while` after. A large
aggregate keeps its phi, because the memory pass below gives that a slot of its
own, and a GPU target keeps it too, where an aggregate is carried as its leaves.
A pointer, including the nullable-pointer `Option[&T]`, is not an aggregate and
keeps its phi.

### Large aggregates

**A struct, array or tuple of more than `GenlAggCopyMin` (64) bytes is never
carried as one LLVM value.** Expression generation makes every aggregate a
first-class value — a variable read is one `load` of the whole, a literal an
`insertvalue` chain, a copy one `store`, an argument and a result passed
whole — and LLVM's backend takes such a value apart element by element, as
instcombine does a load or store of one up to 1024 elements long: an
`Array[u64, 1024]` is a thousand values for instruction selection to schedule
and allocate, and a copy of an `Array[i8, 100000]` crashed it. So once the
module is generated, before it is verified or dumped (`.preir` shows the
result), `genlAggCopies` (`genlaggcopy.c`) moves each such value into memory,
which is how clang generates C's structs. **Why 64:** measured on seven
structs of N `u64`s and one more, each made by a call and gathered by a
literal, carried whole a value of 24 to 136 bytes cost a debug build's code
generation a few milliseconds and a release build nothing measurable, while
one of 1 KiB cost 0.1 s and one of 8 KiB 5.6 s (debug) or 3.6 s (release); and
at 64 every `geomath` type is still carried whole (a `Mat4` is 64 bytes), so
its bench runs as it did, where at 16 a `Mat4` times a point ran 65% slower
(and a `Mat4` times a `Vec4` four times faster). Measured 4 October 2026.

The rewrite:

- each value gets a **home**, an address holding it. A `load` copies what it
  reads into a slot of its own with `llvm.memcpy`, at the load, so the value
  is what memory held there whatever is stored after; one whose only uses
  read a part, or store or return it once, with nothing between that could
  write memory, is read where it lies instead. An `insertvalue` copies the
  aggregate it adds to, or takes its slot over where it is that value's only
  use in the same block, and stores the part. An `extractvalue` is the
  part's address in its aggregate's home: loaded for a small part, the
  address itself for a large one. A select chooses between two homes, and a
  phi has a slot of its own, copied into at the end of each predecessor (a
  block put on an edge from a block branching elsewhere too);
- a `store` and a `ret` copy from the home; a null constant is a `memset`, a
  struct constant stored field by field, any other array constant copied from
  a private constant of it;
- **a function taking one takes a pointer** (`genlAggByPtr`, a smaller one
  too: "Lending a place to a call", below), and **one returning one fills a
  slot its caller passes first** (`sret`), as clang's Win64 convention does
  for C. Each such
  function is remade with that type under its name (its body, attributes,
  COMDAT and debug information moved over), and every call to that type,
  direct or through a vtable slot or a reference, passes homes; every object
  this compiler makes agrees. A **C-named function** keeps the C ABI
  (`genlcabi.c`): `genlCAbiDeclare` marks it `"cone-cabi"`, which the pass
  reads and removes.

**A smaller aggregate result is returned in registers or through its caller's
slot, never one register per field** (`genlAggRetClass`). LLVM returns a
first-class aggregate one register per field, and Win64's backend sends the
third and fourth floats of a `Vec4` through the x87 stack, through memory, on
every return. The same remaking, of every function, declaration and call of
that type, direct or through a vtable slot or a `&fn`, returns:

- a struct, array or tuple of **2 to 4 `f32`s, or 2 `f64`s**, with no padding,
  as one vector (`<4 x float>`, `<3 x float>`, `<2 x double>`): one `xmm`
  register. The callee builds the vector from the value's parts, and the
  caller the value from the vector's elements;
- **any other of up to 16 bytes** as one integer of its size rounded up to 1,
  2, 4 or 8 bytes, or `{ i64, i64 }` (two integer registers), through a slot
  of the frame, as clang coerces a C struct: the callee zeroes the slot where
  the value's scalars leave a byte (padding, or bytes past its end), stores
  the value and loads the integers; the caller stores the integers and loads
  the value. **One or two scalars return as they are**, as Rust's `ScalarPair`
  does, since LLVM already gives each its own register: a slice's pointer and
  length, a pair of `i32`s;
- **one of 17 to 64 bytes through a slot its caller passes first** (`sret`),
  the value stored there whole and loaded whole after the call.

The return shape is a function of the LLVM type alone, so every module
computes the same one.
**Measured** against returning every aggregate whole, on `geomath`'s bench
with its matrix methods out of line (each operation a call; i7-13700HX, best
of 15, at the `generic` / `raptorlake` CPU): `Mat4 * Vec4` 6.6 / 3.8 ns
against 11.7 / 6.9, `Mat3 * Mat3` 6.7 / 6.2 against 10.0 / 9.9, `Mat4 * point`
3.2 / 3.3 against 3.6 / 3.5, and `Mat4 * Mat4` 18.6 / 9.4 against 18.3 / 10.9,
every result the same bits. Measured 4 October 2026.

A home is never written once its value exists, which is what lets a part's
address, a select or a callee read it in place. Each copy is made where its
value was loaded, stored, passed or returned, so what each sees of memory,
and the order of every call, a drop among them, is what it was. An
instruction the pass does not know is handed its operand loaded whole from
the home, and stored into a slot when it makes one, as before. A release
build's optimizer then forwards the copies (`memcpyopt`, SROA); a debug build
keeps them. **Not on a GPU target**, where `llvm.memcpy` is not legal Vulkan
SPIR-V and its own pipeline breaks every aggregate into scalars
(`genlGpuAggregates`, section 7).

### Lending a place to a call

**A struct, array or tuple argument of more than 16 bytes, holding more than
two scalars, is passed as a pointer** (`genlAggByPtr`, `GenlPassPtrMin`), and
that pointer is to its caller's own storage, with no copy, wherever the
permissions prove that nothing can change that storage until the call
returns. "By value" stays the language's promise: the callee sees the value
its caller passed, whatever the call does meanwhile. The rule (`genlLendable`,
every case written out, anything not named copied, since a wrong lend is a
miscompile):

- the argument's type is passed as a pointer, nothing finalizes it, and it
  holds no traced reference (a finalizer and a collector's root take its
  slot's address);
- it is a variable, or a field, tuple element or array element of one at any
  depth, **never reached through a reference or a pointer**. A `mut` slice's
  element may be written through another alias, and what an `imm` reference
  reaches may be freed by a later argument: in `take(*r, consume(own))`,
  with `r` borrowed from the `Rc` `own`, the borrow's last use is before
  `consume`, which may free what `r` points at, and flow lets it;
- the variable is a local or a parameter: never a global or a static, never
  a match's binding sharing another variable's storage (`flowMatchInPlace`);
- flow saw its whole function, and nothing there borrows it, or a part of it,
  with a permission that may write (anything but `imm` and `ro`, `opaq`
  included, since an atomic is written through one), nor makes a raw pointer
  of a borrow of it (`VarLendWritable`, set by `flowLendNote` as `borrowFlow`
  walks each borrow). So no `&mut` route to it is passed in the call or held
  anywhere: `viaMut(u, &mut u)` copies `u`;
- an `imm` variable is then lent. One the program may assign is lent only
  when no other argument, nor the callee's expression, holds anything that
  could assign it (`genlLendQuiet`: names, literals, reads, borrows, casts,
  calls, and literals of them; an assignment, a swap, a block or an `if` is
  not), since those may run after its load: `take(z, (z = w).b)` copies `z`.
  A call cannot reach it, since nothing borrows it to write.

Only a call keeping Cone's convention lends (`genlCallLends`): a direct call
of a Cone function, or one through a `&fn`, a pointer or a vtable, which
every module remakes alike. Not a C-named function, which keeps C's ABI
(`genlcabi.c`); not an `inline` function or an intrinsic, which have no call;
not a construction by a declared `init` (`genlNewArgs`), whose arguments are
copied; not in a split method, whose arguments may wait in flight across a
seam (`genlExprsAcross`); not on a GPU target, where nothing is passed as a
pointer and every function is inlined.

**Generation hands the pass a place.** `genlFnCall` evaluates each lendable
argument as its place's address (`genlAddr`) and a load from it, marked
`GenlLendMark` (`genlArgsLending`), every other argument as its value. In
`genlAggCopies`, a marked load whose every use is an argument a Cone function
takes as a pointer (`genlAggLent`) is passed its address, and the load goes.
Anything else passed as a pointer is copied into a slot of the caller's
frame (`genlAggArgPtr`): a value loaded for that call alone by
`llvm.memcpy` where it was loaded, so it is what memory held there, copied
wide; another call's result of 17 to 64 bytes is passed the slot it was
returned in, which only that call writes; any other value is stored, one of
floats alone 16 bytes at a time, as vectors (`genlAggWideStore`), so the
callee's 16-byte loads are forwarded from stores of the same bytes. A large
value's home is its place where it is lent, and is passed as before
otherwise.

**The callee reads its caller's storage where it is**, unless it may change
it. Its parameter is `ptr noalias readonly captures(none) dereferenceable(N)`:
nothing changes what it points at until the call returns, and nothing keeps
it past. `genlParmVar` stores each parameter into its variable's slot, as
always, and marks the store `GenlParmHomeMark` where the parameter is only
read (`genlParmInPlace`: its binding is not `mut`, flow saw no borrow of it
at all, nothing finalizes its type, it holds no traced reference, and the
function is neither split nor C-named); the pass then makes the pointer the
variable's slot (`genlAggParmInPlace`), and every read, and every lend of it
on, is through the pointer. Any other parameter is copied into its slot as
the body begins: a large one by `llvm.memcpy` from its home, a smaller one
loaded whole (`genlAggParmLoad`) and stored. A borrow of a parameter takes
the address of that copy, so the pointer is never kept.

**Why 16 bytes.** Passed whole, a 16-byte `Vec4` is four floats in registers
or stack slots. Passed as a pointer, a `Vec4` made in registers must be
stored first: measured on out-of-line functions of a 12-byte `Vec3` and a
16-byte `Vec4` made fresh in each call, a pointer for every struct of three
scalars or more was 2.1 to 2.7 times slower on `Vec3` (`cross` then `add`,
4.9 → 10.4 / 13.2 ns) and 1.3 times slower on a `Vec4` dot product (1.09 →
1.38 ns), while it gained on `Mat4 * Vec4` (1.39 → 1.08 ns) and a `Vec4`
chain whose accumulator it lent. At more than 16 bytes the pointer gained
everywhere measured.

**Measured** on `geomath`'s bench with its matrix methods out of line (each
operation a call; i7-13700HX, best of 9, interleaved, at the `generic` /
`raptorlake` CPU), against passing every argument of 64 bytes or less whole:
`Mat4 * Mat4` 6.9 / 5.9 ns against 18.8 / 8.9, `Mat4 * Vec4` 1.40 / 1.08
against 6.8 / 3.9, `Mat4 * point` 1.66 / 1.50 against 3.2 / 3.4, and
`Mat3 * Mat3` 4.4 / 4.2 against 6.8 / 6.2, every result the same bits; the
shipped bench, its methods `inline`, is unchanged. The matrix, an `imm`
local, is lent; the bench's other operand, a `mut` slice's element, is
copied, and a release build's optimizer forwards that copy itself where it
proves nothing the call reaches can write the slice's memory. Measured 4
October 2026.

### A split method

An actor's behaviour holding an `await` is cut at each one, a seam, where it
returns to the dispatcher to wait ([Flow](flow.md), "A seam", which says what
each variable in scope does there and which behaviours are split). A seam is a
return to the dispatcher, not to the author: what the author can see -- when
each value dies, and in which order -- is what the method does uncut. So a
split method (`awaitSplitOf`, `GenState.seams`) is generated whole, more than
once (`genlawait.c`):

- **Its first half** is the method itself (`genlFn`). Each seam (`genlAwait`)
  evaluates what is awaited, then in the seam's order: every lock whose
  borrow ends there is given back (`genlSeamGiveBack`) -- a local guard's
  (flow marks it `VarSeamHeld`, and generation gives it a flag as a drop flag
  is given) and a temporary guard's (`GenTemp.held`); the record is built, each
  value **moved** into it by a load of its slot (`genlSeamRecord`), with no
  count adjusted; nothing droppable is left, so nothing more is dropped; the
  record is parked ("A message's reply", below); and the function returns,
  with no value. What follows the seam is generated into a block of its own,
  `resume`, which no path reaches in this half -- but at a seam awaiting a
  future, where the path finding the future's value there already goes on
  into it ("A message's reply", "A future").
- **Each seam's second half**, `<method>'<n>` (`genlSplitHalf`), is the method
  generated again as a function of its own: its entry stores 'self', moves the
  record's values back into the slots they came from (`genlSeamEntry`), sets
  the drop flag of each variable in scope at the seam that the record does not
  carry, and a given-back guard's, to say it holds nothing, and branches to the
  seam's `resume` block, where the value awaited is its result parameter. The
  method's body before the seam is generated too, in a block no path reaches
  but a loop's back edge. Every scope's end after the seam, every `return`, and
  each statement's temporaries' end are the method's own, so each value dies
  where, and in the order, it would uncut.

Every half generates every seam, so a seam in a loop, or a later one, ends the
second half too, building its record again. The record is laid out by the
first half to generate the seam (`genlSeamLayout`, `AwaitNode.genseam`) and
checked in every later one: its fields, in the order the values would die,
are the values **in flight** across it, the newest first; the temporaries
waiting for their statement's end (`GenTemp`), the newest first, but a lock's
guard and any that does nothing as it dies; and flow's record of variables
(`seamvars`: holding its value, and used after the seam or droppable), the
last declared first, but `self`, which the dispatcher lends each half afresh,
each bringing its drop flag where it has one. A value in flight is one
generated before a later operand holding a seam -- `x` in `two(x, await
give())`, a call's argument or receiver, a literal's field or element -- which
belongs to no variable: `genlExprsAcross` keeps it in a slot of its own
(`GenFlight`, rooted as a local is) and reads it back once the last such operand
is made, so that slot is what the record carries. A struct or tuple literal
makes all its values before it builds the aggregate, so none of it is in flight
part built.

A second half's parameters are `self`, the record (one struct, named
`<method>'<n>.record`, by value), and the value awaited. **An empty record is
no parameter**, and a void `await` (under `--await-direct`, below) gives no
result. A second half returns what the method returns. A seam awaiting a
behaviour waits for its reply (next). Under `--await-direct` (`awaitDirect`),
which is for tests, a seam awaiting anything else hands its record straight to
its second half, with the value awaited, and returns what the second half
returns. `concurrency_await_split` runs each split behaviour beside its twin
with no `await` and pins the halves' shapes.

**Where a seam cuts inside a statement**, the rest of the statement runs after
it, so an address is never held across it: a place written to the left of the
`await` -- a plain path ([Flow](flow.md), "A seam") -- is reached again after
the seam. `genlExprsAcross` takes its operands in `awaitOrder`'s order, a
receiver or a borrow of a plain path written before the last operand holding a
seam made just after it, never in flight; an index holding a seam is made
before the place it indexes (`genlAddr`, its values handed to
`genlArrayIndex`); a swap's side holding a seam reaches its place before the
other side does; and an assignment's value, made first, is kept in flight
(`genlKeepAcross`, `genlKeptAcross`) while a place holding a seam is reached,
then stored (`genlStoreTo`). An `await` whose construct still holds an address
or memory being filled across it -- an index whose place's base holds a seam
too, both places of a swap, a slice's bounds, a parallel assignment's places,
an array's contents filled in memory -- is not split ([Flow](flow.md), "A
seam").

### A message's reply

What a seam awaits is a behaviour of an actor, one that returns a value (one
returning nothing sends no reply, and type check refuses an `await` on it),
sent awaited: type check made
the call the handle's second method for it, whose last argument is the
request's envelope (`AwaitReplyNode`). The envelope is in the message, never
in the method's parameters; what the author wrote is unchanged. The seam's
record waits in the awaiting actor's **pending table**, `Pending`, a hidden
field of its state, and the reply's dispatch calls the second half.

- **The envelope** (`genlAwaitReply`) is generated where it is passed, before
  the seam, since the request carries the record's id. So the seam is laid out
  there: what is in flight across it is what was in flight as its `await`
  began (`GenState.awaitflights`), and its temporaries are all made by then;
  `genlAwait` checks both again at the seam. A record that is not empty is
  given a slot in the table (the actors package's `parkReserve`, with the
  record's size and its **drop function**, below), which allocates its block:
  the slot's index is the record's **id**, a plain index with no generation.
  The actor's generated
  `<Actor>.replyid'` (or `<Actor>.reply'`, where the record is empty) makes the
  envelope, an `actors.Reply`: it takes one count of the awaiting actor, so
  that it cannot die while the reply is owed, and allocates the reply's mailbox
  node, with room after the message for the value returned, aligned to it; the
  node's message is already the actor's own resume variant, naming the seam's
  **resume function** (the tag) and the id, with where the value will be.
- **The seam** moves its record into the slot's block (`parked`) and returns.
  The reply cannot be dispatched before then: it comes to this actor's
  mailbox, which runs one message at a time.
- **The answer.** The actor answering writes its value where the envelope says
  and pushes the node (`actors.answerNow`, `answerAt`, `answered`), knowing
  nothing of the awaiting actor's type. The count the envelope held becomes
  the scheduler's owner when the push finds the actor idle, as a send's would.
- **The resume function**, `<method>'<n>.resume(state, [id,] data)`
  (`genlSeamResume`), is what the reply's dispatch arm calls
  (`actors.resume`, `resumeId`): it takes the record out of the table at id,
  frees its block, and calls the second half with the record and the value,
  both moved. **An empty record has no slot, no id and no lookup**: its resume
  variant and resume function take none.

A behaviour that returns a value and holds an `await` answers its own request
whichever half returns its value. The request's envelope waits, while the
behaviour runs, in the state's hidden `Answer` slot, which the dispatcher fills
(`actors.ask`, or `askNone` for a send with no `await`, whose value is then
dropped). Each seam of such a method takes it into its record first
(`GenSeamAnswer`), leaving the slot empty, and the second half's entry puts it
back. So the dispatcher answers only when the slot still holds it, the first
half having returned a value rather than parking (`answerAt`, which reads the
value only then: the dispatcher keeps the call's value through a raw pointer,
`actors.keep`, so that a parked method's undefined one is never finalized).
The resume function answers likewise once the second half returns
(`answerTo`), dropping the value where nothing awaits it.

**An operation's reply.** What a seam awaits may instead be an operation: a
value of an instance of the actors package's `Awaitable[R]` (type check set
`AwaitNode.awaitable`; the aio package's I/O operations are such values), whose
answer is an `R`. It comes back as a behaviour's value does, in a reply, so
everything above holds but the request. The seam is laid out once the operand
is made, as a seam with no envelope argument is; its envelope is made there by
the same functions (`genlSeamReply`); and the Awaitable, moved into a slot of
its own, is handed with the envelope to the actors package's `startAwait`,
which calls the start function its maker stored in it. Nothing of the Awaitable
is finalized: its maker's context is the start function's from then on. The
maker writes the answer into the envelope as `answerNow` does, from whatever
thread finishes the operation, and the reply's dispatch calls the resume
function. The 'await' adds nothing to the `R`.

**A future.** What a seam awaits may instead be a future: a value of an
instance of the actors package's `Future[T]` (type check set
`AwaitNode.future`), the consumer's reference to a future that a call of a
behaviour returning a `T` made where its value is kept (`actorFutureCall`
made the call the handle's method that makes the future and sends the request
awaited, its envelope the future's producer's reference). `genlAwaitFuture`
generates the seam. Every borrow ends and every lock is given back first,
whichever way the behaviour goes on; the future is moved into a slot of its
own, and `actors.futureReady` asks whether it has its ending already. If it
has, the behaviour goes on **where it stands**: no record, no envelope, no
return to the dispatcher, the code after the seam reached from here. If not,
the envelope is made as for a message, its data room for a waiter (the
future's reference, a link, the actor awaiting and the node), and handed to
`actors.futureRegister` with the future, which parks the envelope's node on
the future's waiter list with one compare-and-swap, or finds the list closed
because the future has its ending by now. Parked, the record moves into its
slot and the method returns, as at any seam; the future pushes the node when
its value arrives, and the resume function opens it. Abandoned instead, the
future drops the node, as an envelope dropped unanswered is, so the record
stays parked until its actor's death drops it ("An abandoned record"). Not parked, the slot
reserved for the record is given back, its block freed with nothing in it,
and the behaviour goes on where it stands, as above. **Opening the future**
(`genlFutureOpen`), on either path, asks `actors.futureOpen` where its value
is -- a panic, at the `await`'s line, if it never arrived -- reads the value
out, marks a move value taken (`actors.futureTaken`) so that the future does
not finalize it again, or counts a copied one again (`genlAliasHeld`), and
lets go of the future's reference the `await` took. The value waits in a slot
of its own (`GenState.resumeslot`), which the opened value fills on the
arrived path and the second half's entry fills from its parameter.

**An abandoned record.** A record parked in the pending table has a drop
function, `<method>'<n>.drop(record)` (`genlSeamDrop`), declared where the seam
is laid out and handed to `parkReserve`: each of the record's values that does
something as it dies is finalized, in the record's order -- the order the
values would die in -- a variable carried with its drop flag only where the
flag says it holds its whole value (a hollow one, part moved out, is left).
The table calls it when its actor dies with the record still parked
(`Pending`'s finalizer), which happens only when the envelope the record was
owed was dropped unanswered: the I/O it awaited was ended at the program's end,
or the actor that was to answer abandoned its own continuation, which held the
envelope, or the future the record waits on was abandoned so, dropping the
envelope parked on it ("A future"). The second half never runs. An empty record parks nothing and has no
drop function.

The pending table is not traced: a record that would hold a traced reference
is refused before generation ([Flow](flow.md), "A seam").

### A parallel each

A `parallel each` ([Block](../nodes/block.md), "Type check") stays one loop in
the IR, which flow checks as it checks any loop, and is generated in two places
(`genlpar.c`). Its outer block is `FlagParallel`; `genlBlock` generates the
statements before the index `k'` where they stand (the source made ready, the
range `lo'` and `hi'`), and at `k'` calls `genlParallelRun`, which generates `k'`
and the loop into **a function of its own**, `<fn>.parallel(record *u8, lo
usize, hi usize)`, internal and with a debug subprogram in a debug build, and
puts a call of the actors package's `parallelEach` over `[lo', hi')` in their
place. The function is generated in the middle of its caller as `genlFn` is (the
caller's builder, alloca point, roots, temporaries and block stack are set aside
and restored), with `lo'` and `hi'` redirected to allocas of the piece's own
holding the parameters, so the loop counts the piece's range.

**What the loop uses from outside is found as it is generated, not before.**
No closure exists, and a pass is run by whichever worker takes it, so the piece
cannot use the caller's frame directly. `genlVarSym`, where every use of a
local reaches its slot, asks `genlParCapture` while a piece is being generated
(`GenState.parbody`): a variable whose slot is an instruction of another
function (`LLVMGetBasicBlockParent` differs from `gen->fn`) is *captured*: its
address is taken from the record at the function's entry, ahead of the allocas
(element `i` of the record, an array of pointers), and stands for the variable
from then on (`var->llvmvar` redirected). The first use fixes the variable's
place in the record. After the piece, each captured variable's slot is put
back, and the caller makes the record, an `alloca` of `[n x ptr]` holding the
slot of each, and passes it. So a variable is read in place, where it lives in
the caller's frame, for as long as the loop runs: that is why an outside
variable is read-only in a body (pareach.c checks it), and why the loop's end is a
join. A loop inside a piece's body is a piece of a piece: its captures are
taken from the outer piece's record in turn (the record is made inside the outer
piece, and `genlParCapture` captures there what the inner found), and the inner
loop is a different `parbody` with its own list.

**A number range's count is worked out here**: `genlParCount` widens the two
bounds to usize (sign-extending a signed type), takes their difference
(wrapping, which is exact as an unsigned count whatever the signs), adds one for
`<=`, and is zero where the last is below the first; the pass variable is the
first bound plus `T.from(k')`, wrapping, which cannot pass the last bound.

A parallel builder ([Block](../nodes/block.md), "Type check") needs nothing more of
generation: its piece's list is a local declared after `k'`, so it is generated
in the function with the loop, and the bag before `k'` and the join after the
loop are the caller's.

**In an actor's behaviour the loop is a seam.** Type check follows the loop with
an `AwaitNode` whose `par` is set (`awaitParNew`), so flow, `awaitSplitOrReport`
and `genlSplitHalves` treat it as they do an `await` that parks (`awaitParks`):
the actor's pending table, the seam's record, `.resume` and `.drop` functions. A
split method's second halves each generate the piece again (its name made
unique), which is harmless: only the first half's call is reached. At the end of
`genlParallelRun`, finding the `par` seam among the block's statements
(`GenParSeam`, `AwaitNode.genpar`), no call is made. The captured variables'
values are copied into a block of the runtime's (`parBlock`: `[n x ptr]` of
pointers, then a copy of each), the record points into the copies, and
`genlAwait` (`node->par`) hands the range, the piece, the block and the seam's
resume function to `parSeam` after parking the record in the pending table as an
`await` on a future does (`parkReserve`, `parked`). The behaviour then returns to
the dispatcher. The copies are bitwise and never finalized: the originals are in
the record, nothing writes the variables, and the second half moves them back
afterwards. A copy of a borrow would point into a frame that is gone, so type
check refuses a borrow held in a variable the body reads and a source array in
the frame (`ErrorParFrame`); `self` is the one borrow a piece reads, and what it
points at is the actor. The runtime side is the actors package's parallel.cone,
"A PARALLEL EACH IN A BEHAVIOUR". In any other actor method (a synchronous
`fn`, `init`, `final`) there is nothing to cut, and the loop is the blocking call.

`genlFn` clears `parbody` for the functions it generates in the middle of a
piece (a drop a death asks for).

### A generator

A generator ([Parse](parse.md), "It generates a generator's declarations") is a
struct holding its parameters as fields, and a method `next` whose body is the
author's (`genlyield.c`). Its seams are `await`'s, cut differently: `next` is
generated **whole, once**, entered through a switch on the struct's state field
(`GenInfo.state`) which `genlGenBegin` builds at the function's entry:

- state 0, the default: the body from its start, the generator just made;
- state *k*: just after the *k*-th `yield` in the order written
  (`YieldNode.yieldno`, numbered once the function is walked);
- `GenDone`: the body has ended, and `next` hands back None (`walk.none`)
  and runs nothing.

A `yield` (`genlYield`) makes the result `next` gives, ends its statement's
temporaries, stores its number in the state and returns the result. What follows
it is generated into a block of its own, which no path in this call reaches; its
seam adds a case to the entry switch, whose block (a stub) stores
`DropFlagEmpty` into the flag of every local in scope that the frame does not
keep, and goes to it. Every `return`, and the body's end, store `GenDone` first
(`genlGenReturn`, from `genlReturn`). Nothing is moved into a record: the
variables that must live across a seam are not in the function's frame of
allocas, but in the generator's.

**The frame** is hidden storage after the struct's fields, one more element of
its LLVM type (`genlGenFrameType`, from `genlStructFields`), so that every
generator of the kind is as large as its parameters and its frame. It holds each
local the loan walk noted, at any seam, as holding its value and either used
after the seam or finalized as it dies (`SeamLive`, `SeamDies`; `genlGenPlan`,
once for all seams), with its drop flag where it has one. A frame local is
generated where it is declared (`genlLocalVar`) but takes its storage, found by
a `getelementptr` of the frame at the function's entry (so that it dominates every
path that resumes the body), instead of an `alloca` (`genlGenFrameVar`), and a
drop flag in the frame is stored as any is (`genlDropFlagBegin`). A match
binding in the matched variable's storage (`flowMatchInPlace`) takes none of its
own, and the matched variable is kept in its place. The frame is not zeroed or
filled by the construction (the struct literal leaves it undefined); nothing in it
is read before the body's declaration made it.

**A generator that would hold itself** -- a local whose type is the generator's own
kind, as the sub-generator of `yield each walk(t.left)` is in `walk`, or one that
holds it in its frame -- has that local's storage on the heap (`GenFrameBoxed`,
`genlGenSelfHolding`): the frame holds its address, made by `malloc` at the first
call (state 0) before the switch, loaded at the entry as the local's storage, and
freed when the generator dies. Mutual recursion boxes the local that closes the
cycle. A generator that holds another of a different kind holds it inline.

**Dying** is the generator's `final`, an empty method the parser wrote, whose body
`genlGenBegin` begins with `genlGenDrop`: a generator not yet run holds nothing in
its frame, a finished one has finalized its locals as their scopes ended, and one
left at a seam finalizes what the frame holds there, newest first, a local with a
drop flag only if the flag says it holds its whole value (as `genlSeamDrop`
does); then the boxes are freed. The parameters, fields of the struct, die after,
with the struct's own drop. The deepest sub-generator therefore finalizes first.

Not built: a generator in a library's include file (it would have to carry the
body whole for an importer to make the struct again; `ErrorGenForm`). Not tried:
a GPU or WebAssembly target (a box is a C `malloc`).

## 7. Output, and what does not work

`--llvmir` writes **two** files: `.preir` before the pass manager and `.ir`
after. `--ir` is not an LLVM option at all — it dumps the Cone IR/AST.
`--asm` adds a `.wat`, `.spvasm` or `.asm`. `--verify` runs `LLVMVerifyModule` and is off
by default. `--debug` emits DWARF and drops optimization — it is the only
switch here, with release as the default. Debug info covers only files,
subprograms and each instruction's line and column, and the file name is hardcoded. A subprogram is attached only to a
function this object defines: an imported module's function has a body in the
IR but is a declaration here, and the verifier rejects a declaration carrying
one. Every `genlExpr` sets the builder's debug location to its node's line and
column, and `genlAlloca` puts it back after taking an alloca in the entry
block: positioning the builder before the `allocaPoint` takes that
instruction's location, which is none, and the verifier refuses a call with no
location in a function with debug info.

The environment variable `CONE_LLVM_OPTIONS` hands LLVM command-line options,
separated by spaces, parsed in `genSetup` before the LLVM context exists. It is
a testing aid, for LLVM's own debugging options, which the C API has no call
for.

**Cross-module linking works for a library built on its own, and nothing
else.** A symbol is spelled from its owner chain, and the root module
contributes no name to it — [Names and Namespaces](../../../../doc/design/names-and-namespaces.md),
"Symbols". So compiling `modulesub.cone` directly makes it the root and emits
`@scaleInt`, bare and internal; compiling a `main.cone` that imports it makes it
an imported module and emits `@_CNvC9modulesub8scaleInt`, `modulesub.scaleInt`.
The two object files never resolve against each other. A library built from a
build description settles both halves: its root is named, so `modulesub` built
that way emits `@_CNvC9modulesub8scaleInt` itself ([module](../nodes/module.md),
"A described build"), and it exports that definition (`dclIsExported`), so an
importer's `declare` resolves against it at link. `module_build_link` compiles
a package alone, compiles a program against the include file the package's
compile generated ([module](../nodes/module.md), "Generating the include
file"), links the two objects and runs the program. A generic's instances and the
vtables both objects build are defined in each, `linkonce_odr`, and merge at
link: the scenario instantiates a generic function and a generic type in the
package and in the program, at a type argument both use and at one only the
program uses, and tests in the package a virtual reference the program built.

**A SPIR-V triple emits a SPIR-V module, `.spv`.** `--triple=spirv64-unknown-unknown`
is SPIR-V's OpenCL form (physical addressing, the `Kernel` capability) and
`--triple=spirv1.6-unknown-vulkan1.3` its Vulkan form (logical addressing,
`Shader`). `genlCreateMachine` initializes every target the LLVM build holds;
`genSetup` marks a triple starting `spirv` a GPU target (`ConeOptions.gpu`),
which flow reads too (`flowGpu`), and one naming `vulkan` the Vulkan form
(`ConeOptions.vulkan`), whose `usize` is 32 bits: it addresses logically, with
no pointer to measure, and WebGPU has no 64-bit integer. Functions over
numbers, structs, arrays held in structs and enums, with calls, branches,
early returns, loops, references to locals, to parts of them and to module
globals, structs holding references, and string literals, come out at every
optimization level as a module SPIR-V's validator accepts in its universal
environment. A compute entry point is a kernel, valid in the Vulkan
environment, slices and all ("Compute entry points", below).

**A GPU types each pointer by the memory it points into**, its kind: SPIR-V's
storage class, LLVM's address space. Every Cone reference is address space 0,
while the data layouts put a global in another (`G1`, `CrossWorkgroup`, in the
OpenCL form; `G10`, `Private`, in the Vulkan one). The kind is not written in
Cone, nor carried in a reference's type; it comes from the origin, the way
HLSL's compilers settle it:

- **At the origin**, a global's address, a string literal's or a constant
  array's, is cast to address space 0 where it is taken (`genlFlatAddr`), so
  the IR is well typed whatever it is handed to.
- **Every call is inlined.** `genlGpuCalls` marks every function this object
  defines `alwaysinline`, and the GPU pipeline runs the always-inliner at every
  optimization level, `--debug` too; an internal function is then gone. Once
  inlined, each borrow has one origin, and one function used with a local and
  a global is in effect a copy per kind, with no instancing in Cone.
- **Structs and arrays break into separate values** (`sroa`), so a struct
  holding a reference dissolves into locals: logical addressing keeps no
  pointer in memory.
- **LLVM's address-space inference** (`infer-address-spaces`) then gives each
  use its origin's space back. It rewrites only casts into the target's flat
  space, which for the OpenCL form is 4, `Generic`; `genlLLVMOptions` names 0
  the flat space for both forms (`-assume-default-is-flat-addrspace`).

What inference cannot settle LLVM does not report: a choice of pointer becomes
an invalid `select` or a `Generic` pointer, and only the validator sees it. So
what would leave a pointer unsettled is refused before generation, in Cone's
terms: a reference chosen at run time (`ErrorGpuRefChoice`) and an array
holding references indexed at run time (`ErrorGpuRefIndexed`) by the loan walk
([Flow Analysis](flow.md), "GPU targets"); a global holding a reference
(`ErrorGpuRefGlobal`, `genlGloVar`) and a function calling itself, which no
inliner removes (`ErrorGpuRecursion`, `genlGpuCalls`), here.

**LLVM 23's SPIR-V backend takes only some shapes of IR**, and on the rest it
crashes, in its own passes, or emits a module the validator refuses. With
opaque pointers it types a pointer by the address computations made from it,
and it expects structured control flow and no struct moving whole through
it. So the GPU pipeline and what follows it keep to those shapes:

- **Addresses stay typed.** The pipeline folds with `instsimplify`, not
  `instcombine`, which rewrites a field's address as a byte offset from its
  struct's (`getelementptr i8`), and `genlLLVMOptions` turns off two code
  generation passes made for a CPU: loop strength reduction, which walks an
  array in a loop by a pointer stepped a byte count at a time, and
  CodeGenPrepare, after which a field's address computed from a parameter in
  one block and used in another crashed the pointer-cast legalisation. LLVM
  also drops an address computation whose indices are all zero (instsimplify,
  GVN and the inliner each fold it), so a struct's first field is read
  through the struct's own pointer and its array field indexed from it, and
  the backend indexes the struct itself; `genlGpuRetypeFn` puts the zero
  indices back, from the type an alloca, a global, an address computation, a
  storage buffer's element or a parameter's uses say the pointer points to,
  for a load, a store, an address computation or an atomic operation (an
  `Atomic[u32]`'s number is its first field). GVN does the opposite too:
  an array field's address and its first element's are one number, so it
  takes the one for the other, and the array is then indexed from its first
  element's address; `genlGpuRetypeFn` drops the trailing zero indices that
  made it the element's. And an address computed from
  another made in another block (a field of a parameter's field, read by an
  inlined method after two early returns) crashed the pointer-cast
  legalisation; `genlGpuGepChainsFn` computes such an address in one step
  from where the other starts, as instcombine would, or, when it steps from
  the other rather than into it, computes the other again beside it.
- **The control flow is structured** (`structurizecfg`, last in the
  pipeline, run by itself after the kernels are settled): each branch merges
  before the next is taken. The backend's own structurizer leaves a chain of
  three early returns, or a loop's body returning early from within a
  branch, as a module the validator refuses, and with CodeGenPrepare on, the
  latter crashed it. A function it structures has one return, so a kernel's
  returns, one for each failed check, are made one (`genlGpuOneReturn`).
  `structurizecfg` takes no switch: it leaves each of a switch's branches a
  `br i1 undef`, which the validator accepts and the GPU runs as it pleases.
  simplifycfg makes a switch of an if-elif chain on one integer, so
  `lower-switch` makes each switch a tree of comparisons just before it
  (`module_target_spirv_switch`). And `structurizecfg` wants every loop
  entered through a preheader, as LLVM's own GPU pipelines hand it loops:
  given one entered straight from a conditional branch (a loop inside an
  `if`, one of whose exits, a failed check, reaches the kernel's one return
  another way), it made the loop's back edge `br i1 true` to its exit, so the
  loop ran once and what followed it was skipped, in a module the validator
  accepts. simplifycfg folds away the empty block before such a loop, so
  `loop-simplify` gives each loop its preheader again just before
  `structurizecfg` (`module_target_spirv_loop_in_branch`, and gpusample's
  `loops` test on a GPU).
- **A struct or array is loaded from or stored into a storage buffer a scalar
  at a time** (`genlGpuBufferAccess`, after the pipeline): the backend gives
  the buffer's laid-out struct and a local's two SPIR-V types, and a whole
  one's store between them is a pointer cast the validator refuses.
- **A struct or array value is carried as its scalar leaves**
  (`genlGpuAggregates`, after the pipeline). A phi of a struct crashed the
  backend when an incoming value was a parameter (geomath's `Vec4.normalize`,
  an `if` choosing between `self` and a new vector), and broke dominance when
  one was a constant (an `Option` returned early) or when the value was made in
  a loop and used after it; and the backend breaks a function returning a
  struct taken whole out of another. So a phi or select of one becomes one per
  leaf, inserting and extracting parts only renames leaves, a parameter, load
  or call making one has its leaves extracted just after it, and a use taking
  it whole (a return, a store, a call) gets it rebuilt just before. One of
  more than 256 leaves is left whole.
- **`--asm` emits from a copy of the module, by a target machine of its
  own**, since the backend rewrites the module it emits and crashes emitting
  it again, and keeps what it learns of a module in the machine's subtarget,
  keyed by addresses: a second module emitted by one machine, the copy
  disposed between, failed instruction selection in about 3 compiles in 100
  (`genlOut`, `genlGpuOut`).

Where the backend still crashes, `conec` says where, as `llc` does: `genSetup`
enables LLVM's pretty stack trace, so the crash names the pass and the
function it was in.

`genlLLVMOptions` also turns machine CSE off on a GPU target: LLVM 23's SPIR-V
backend lets it hoist a computation a loop header's two successors share into
the header, after the `OpLoopMerge` already placed there, which must come just
before the branch, so the release build of such a loop was a module the
validator refused.

### Compute entry points

**A compute entry point is a kernel beside its function** (`genlgpu.c`). A
function declared `@compute(x, y, z)` (the parser reads the size; type check,
`fnDclComputeCheck`, its signature and what its buffers hold;
[reference](../../../../doc/reference/refgpu.html)) is generated as every
function is, internal and inlined, and on the Vulkan form `genlComputeEntry`
makes the kernel: an LLVM function of no parameters named as the Cone function
is (which, if its symbol is the same, takes `.body` after its name), marked as
Clang's HLSL marks a compute shader (`"hlsl.shader"="compute"`,
`"hlsl.numthreads"="x,y,z"`, which LLVM's SPIR-V backend reads for
`OpEntryPoint GLCompute` and `LocalSize`), and external, so a compile that is
no library keeps it. It fills each parameter and calls the function:

- core's `Invocation` from LLVM's SPIR-V built-in intrinsics
  (`llvm.spv.thread.id`, `.thread.id.in.group`, `.group.id`,
  `.flattened.thread.id.in.group`, `.num.workgroups`);
- a slice from a storage buffer: a handle (`llvm.spv.resource.handlefrombinding`,
  a `spirv.VulkanBuffer` of a run-time array of the element, storage class 12,
  written or not), set 0, the binding its place among the buffer parameters;
  the slice's pointer its first element's (`llvm.spv.resource.getpointer`,
  address space 11, cast to the flat 0), and its count the run-time array's
  length;
- a struct by value from a buffer of one (`getbasepointer`), a scalar at a
  time.

A binding's name, which the backend reads from a constant global and keeps one
variable per, is `<kernel>.<parameter>`, held as 32-bit words so that the
module asks for no 8-bit integers.

**The count is SPIR-V's `OpArrayLength`, which LLVM 23 cannot select**: its
`getdimensions` intrinsics are for images. So the kernel calls
`cone.arraylength.<kernel>.<binding>`, a function declared and never defined
(`memory(none)`, so an unused one goes), and `genlGpuOut` emits the module to
memory and `genlGpuPatch` rewrites it before it is written: each call to such
a function (`OpFunctionCall`, five words) becomes `OpArrayLength` of the
handle it was handed (the variable's copy), member 0 (five words), and the
declarations, their names and linkage decorations go, with the `Linkage`
capability when nothing else is imported. `--asm` writes LLVM's assembly,
which shows the calls (and the decorations against contraction, below).

**A failed check records itself and leaves the kernel.** On a GPU target
`genlPanic` calls `cone.gpu.fail(kind, file, line, value)`, declared and never
defined, instead of conestd, and a call to core's `panic` (conestd's
`cone_panic`, which `assert`, `unreachable` and `todo` call) becomes the same
call (`genlGpuPanic`), its message dropped. Once the GPU pipeline has inlined
everything into a kernel, `genlGpuEntries` settles it, between the pipeline's
two halves, before its control flow is structured:

- **Each failure call becomes a record and a return** (`genlGpuRecord`). The
  error buffer is the read-write storage buffer bound after the last buffer
  parameter, eight words: the count of invocations that failed, added to by
  `atomicrmw add` (device scope, acquire-release, which Vulkan asks of a
  storage buffer's atomic); then, written only by the invocation that made
  the count 1, the kind (1 index, 2 range, 3 panic, 4 allocation), the
  source file's id, the line, the invocation's global id, and the index or
  the range's end. The file's id is its place in the module's list of source
  files (`genlSrcFileId`), read from the global holding its name that the
  call was handed, through casts, selects and phis; `genlGpuPatch` lists the
  files in the module as OpSourceExtension `"cone.file <id> <name>"`. Then
  the invocation returns. The checks are kept in every build.
- **Each step by pointer arithmetic is folded into an access chain**
  (`genlGpuSlices`, `genlGpuFold`): an address computation whose first index
  is not zero, as indexing a slice or cutting one is, steps from what the
  slice's pointer came from, found through casts and through the insertions
  and extractions of a struct that carried it (`genlGpuPartOf`). From a
  buffer's element, it is the buffer's element that many on
  (`getpointer` with the indices added); from a fixed array, all of it or as
  its first field however deep (an alloca, a global, an address computation,
  a buffer's struct), the array indexed; from an element of a fixed array,
  that element's index added to. From anything else it is refused,
  `ErrorGpuSliceOrigin`, at the node the computation was made for, which
  generation marks on it (`genlGpuSite`, metadata `cone.site`, an index into
  `GenState.gpusites`). A reference chosen at run time was refused before
  generation (`ErrorGpuRefChoice`).

The second half (`globaldce`, `infer-address-spaces` again, for what the
folding made, `instsimplify`, `adce`, `lower-switch`, `loop-simplify`, `structurizecfg`) follows. A library
compiled for a GPU makes no kernel of a function that is no entry point: its
failure calls stay calls to a function the module imports, and a slice
indexed in it has nothing to be folded into.

An entry point compiled for the OpenCL form is refused
(`ErrorComputeTarget`), and two of one name in a compile
(`ErrorComputeAttr`), since a dispatch finds a kernel by its name.

### The C library's math

**No GPU has the C library, so its math is the GPU's own** (`genlGpuMath`,
called by `genlFnCallInternal` before a call or an inline body is
generated). A call to a function whose C symbol is one of the C library's
math functions, `libc`'s bindings or any `@c` declaration of the same
symbol, its arguments of the type the name says (`float` with an `f` after
it, else `double`), is on a GPU target the LLVM intrinsic of the same
meaning, which LLVM 23's SPIR-V backend selects as GLSL.std.450's extended
instruction: `sqrt`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`,
`exp`, `log`, `pow`, `fabs`, `floor` and `ceil`, each `llvm.<name>`; `fmod`
is `frem`, SPIR-V's OpFRem, whose sign is x's, as C's is. `fabsf`, which
`libc` writes inline as `fabs` widened and narrowed (the UCRT has no symbol
for it), is caught before its body is expanded, so the module asks for no
64-bit float. The OpenCL form selects OpenCL.std's instructions for the same
intrinsics. The CPU is untouched, and the C library is called there. Core's
`sqrt`, `sin` and `cos` methods are `llvm.sqrt`, `llvm.sin` and `llvm.cos` on
every target.

`module_target_spirv_math` pins the intrinsics and the instructions (its
`asm` check reads `--asm`'s `.spvasm`). The double forms of `sqrt`, `fabs`,
`floor`, `ceil` and `fmod` are valid at 64 bits, with the `Float64`
capability; GLSL.std.450 has no 64-bit trigonometry, `exp`, `log` or `pow`,
and the validator refuses a module that calls one. `copysign` and `ldexp`,
which `libc` does not bind, have intrinsics LLVM 23's backend cannot select.

What each computes, against the CPU's C library, was measured on an RTX 4060
over 65,536 arguments a function: `fabs`, `floor` and `ceil` bit for bit;
`fmod` bit for bit as the driver compiles it; `sqrt` within 1 ulp; `asin`,
`acos`, `atan` and `atan2` within 2; `exp` within 8 over [-10, 10]; `pow`
within 27; `log` within 87 ulp near 1, where it is near 0; `sin` and `cos`
within 4e-7 absolute in [-pi, pi], 1e-5 in [-100, 100]; `tan` within 4e-5 in
[-1.5, 1.5]. Vulkan's own bounds are in the [reference](../../../../doc/reference/refgpu.html).

**Every float operation but the remainder is decorated `NoContraction`**
(`genlGpuNoContraction`), as slangc's `-fp-mode precise` does, so no driver
fuses a multiply and an add into one rounding or reassociates, and each is
rounded on its own as the CPU rounds it. Undecorated, the RTX 4060's driver
fuses: noise's `fbm3`, all adds, multiplies and `floor`, matched the CPU at
52,304 of the bake's 262,144 voxels (within 1.2e-6); decorated, at all of
them, value and derivatives. LLVM 23's backend emits no such decoration from
IR (neither `!spirv.Decorations` metadata on an instruction nor
`llvm.spv.assign.decoration` on a value reaches the module), so it is added
to the emitted module after `genlGpuPatch`: an `OpDecorate <id>
NoContraction` for the result of each `OpFNegate`, `OpFAdd`, `OpFSub`,
`OpFMul`, `OpFDiv`, `OpVectorTimesScalar`, the matrix products,
`OpOuterProduct` and `OpDot` (every arithmetic instruction on floats SPIR-V
has), after the module's last annotation. `OpFRem` and `OpFMod` are left
free: the CPU's `fmod` is exact, and the RTX 4060's remainder matched it bit
for bit undecorated and at 62% of arguments decorated (within 3.8e-6), the
driver's own expansion of it needing the fused multiply-add. `--asm`'s text
gets the same decorations (`genlGpuNoContractionAsm`), so the `asm` check
target sees them (`module_target_spirv_nocontract`, and
`module_target_spirv_nocontract_frem` for the remainder). It is always on,
with no switch to turn it off; the bake's dispatch took about 9% longer
(100 to 109 microseconds for 64^3 voxels on the RTX 4060). `NoContraction`
needs the `Shader` capability, so the OpenCL form cannot carry it; that form
has no kernels (an entry point there is refused), and its own equivalent,
the `ContractionOff` execution mode, would go on a kernel's entry point.

The CPU never fuses: generation marks no float operation `contract` or fast
and emits no `llvm.fmuladd`, and LLVM's target machine fuses only those
(`FPOpFusion::Standard`, the default the C API leaves), so `--cpu=native`
on a CPU with FMA instructions emits `vmulss` and `vaddss`, no `vfmadd`, as
`generic` does.

**A signed integer remainder is never `OpSRem`** (`genlGpuSignedRem`, after
`genlGpuAggregates`). The RTX 4060's driver computes `OpSRem` and `OpSMod`
unsigned, whatever the signedness of the integer type the module declares:
`-7 % 3` came back 0, the remainder of 4294967289, and `5 % -3` came back 5.
It computes `OpSDiv` correctly, and the Intel UHD 770's driver computes
`OpSRem` correctly. So each `srem` is made `a - (a sdiv b) * b`, an `OpSDiv`,
`OpIMul` and `OpISub`, after the pipeline: instcombine folds that expansion
straight back into an `srem`. An unsigned remainder is `OpUMod`, as it was
(`module_target_spirv_srem`; `gpusample`'s `intops` test runs it on the GPU).

### What invocations share

**Invocations share a storage buffer and a `@workgroup` global**
(`genlgpusync.c`; [reference](../../../../doc/reference/refgpu.html#atomics)).
A buffer of `Atomic[u32]` or `Atomic[i32]` is a run-time array of core's
`Atomic` struct, one `i32` wide, and an atomic method is core's: a call to
mem's intrinsic on `self as *T`, the same `atomicrmw`, atomic `load` or
`store` or `cmpxchg` as on the CPU (`genlAtomicIntrinsic`, which on the Vulkan
form marks it with its call, `genlGpuAtomicSite`, unless the call is core's
own). A `@workgroup` global (`DclWorkgroup`) is made in address space 3,
SPIR-V's Workgroup storage class (`genlGloVarName`, `genlGpuGlobalSpace`), its
initializer `undef`, an `OpVariable` with none: a workgroup's copy starts
undefined, and a zero initializer would need Vulkan's
`shaderZeroInitializeWorkgroupMemory`. Its address is cast to the flat space
where it is taken, as any global's, and inference gives each use space 3 back:
a borrow of it has the Workgroup kind by its origin. On the CPU it is an
ordinary global, zero.

**Each atomic is settled after the pipeline** (`genlGpuAtomics`, once
`genlGpuAggregates` has run, over every function), when each pointer has its
origin's space:

- **its scope is its memory's**: `syncscope("device")` in a storage buffer
  (11), `syncscope("workgroup")` in workgroup memory (3). One left at LLVM's
  system scope would be SPIR-V's CrossDevice, which Vulkan refuses; the error
  buffer's count has its scope already;
- **sequential consistency is acquire-release** (a load's acquire, a
  store's release): Vulkan's validator refuses SequentiallyConsistent
  semantics outright (`VUID-StandaloneSpirv-MemorySemantics-10866`);
- **an atomic at a constant place** (a scalar `@workgroup` Atomic, a
  workgroup array's element at a constant index) is reached by an address
  computation that is an instruction (`genlGpuAtomicConstPtr`): LLVM folds a
  constant address of offset zero into the global itself, and the backend's
  pointer-cast legalisation crashed on an atomic through a pointer to the
  global's struct or array. The computation is built with a frozen first
  index, which nothing folds, and the constant put back; the first index steps
  the pointer and indexes no type, so the instruction's result type is known;
- **a compare-and-swap is `llvm.spv.cmpxchg`** (`genlGpuCompareSwap`): LLVM
  23's SPIR-V backend crashed on `cmpxchg` in every form measured, in its
  pointer-cast legalisation. The intrinsic, which the backend's own
  instruction emission makes of a `cmpxchg`, takes the pointer, the value
  expected, the value to store, the scope and the two semantics, written here
  (the failure's acquire, or none for relaxed), and answers the value seen;
  whether it stored is that value compared with the one expected;
- **in a kernel, one on memory no other invocation reaches** (a local
  `Atomic`, a global not `@workgroup`, the Private space 10) is refused,
  `ErrorGpuAtomicPlace`, at mem's call where the program wrote one, else once
  at the kernel; Vulkan allows atomics only on shared memory. Outside a
  kernel (a library's function taking a reference) the memory is not known,
  and its scope is the device's.

**Two things LLVM 23's SPIR-V backend writes wrongly are put right in the
emitted module** (`genlGpuSyncPatch`, before `genlGpuPatch`). A relaxed
atomic's semantics carry the storage class's bit (UniformMemory or
WorkgroupMemory) with no ordering, which Vulkan refuses
(`VUID-StandaloneSpirv-MemorySemantics-10871`); each such operand is made a
constant 0, None, added after the 32-bit integer type if the module has none.
And after `OpAtomicCompareExchange` the backend inserts the result and its
comparison into a value of the scalar type the intrinsic is declared with,
which no module may hold: the first insertion goes, and the second becomes an
`OpCopyObject` of the result, which is what every use of it wants. `--asm`
writes LLVM's assembly, before either.

**`workgroupBarrier()` and `storageBarrier()`** are core's intrinsics,
expanded by `genlGpuBarrier` on a GPU target into
`llvm.spv.group.memory.barrier.with.group.sync` (`OpControlBarrier`,
Workgroup execution and memory scope, AcquireRelease | WorkgroupMemory) and
`llvm.spv.device.memory.barrier.with.group.sync` (Workgroup execution, Device
memory scope, AcquireRelease | UniformMemory | ImageMemory), the ones Clang's
HLSL makes; both are `convergent`, which keeps the inliner and the structurizer
from moving them.

**Barriers on the CPU.** A kernel's CPU twin may run a workgroup's
invocations one at a time, where a barrier has nothing to wait for, or on
threads, where it must wait for the others. So on a native target each barrier
is a call to the running thread's barrier hook, when it has one
(`genlCpuBarrier`): it loads conestd's thread-local `cone_barrierHook`, which
core's `setBarrierHook` sets and `clearBarrierHook` clears
(`packages/conestd/barrier.cone`), and only where it is not null loads
`cone_barrierCtx` and calls the hook with it and the kind, `i32 0` for
`workgroupBarrier` and `i32 1` for `storageBarrier`. The compiler declares the
two thread-locals itself, by their C names, as it declares `cone_gcframes`.
With none set a barrier costs the thread-local load (on Windows `_tls_index`,
the TEB's TLS array, the block, the value) and a not-taken branch; the
measured cost is in `refgpu.html`, "Barriers". The call is indirect, so no
alias analysis lets LLVM move an access to a `@workgroup` global, an internal
global whose address never escapes, across it: GlobalsAA's answer for an
internal global is only for a direct call. The hook being the thread's own
lets two twins run workgroups at once, each on its own threads, and a pool's
thread set it once. WebAssembly links no conestd, so there a barrier stays
nothing; with `--intrinsic-fallback` it is core's empty fallback body, as on
every target.

What does not work yet:

- A `@workgroup` global has one native copy, which nothing resets or poisons
  between workgroups, and a twin cannot find a module's workgroup globals. A
  GPU's workgroup memory starts undefined too, so a kernel that reads one
  before writing it is wrong on both; finding them (a named section, its
  bounds, a core function) would serve only a poisoning debug aid, and could
  not work on WebAssembly, so it is not planned. (`gpuwork`'s twin runs a
  kernel with barriers, found from `OpControlBarrier` in its entry point's
  call tree, a workgroup at a time, one thread an invocation, each thread's
  hook a real barrier, #337; a kernel without one, an invocation at a time.)
- An imported package's functions are left as imports where a build
  description compiles the package on its own, so a kernel calling one
  carries the `Linkage` capability, which Vulkan's environment refuses; found
  on the package search path, as a direct `conec` compile finds geomath, the
  package is compiled into the kernel's object.
- A library's function taking a slice, compiled for a GPU on its own, has its
  slice's elements reached by arithmetic, which the validator refuses: only
  in a kernel is there an origin to fold them into.
- A program with a `main` compiled for the Vulkan form crashes LLVM's SPIR-V
  backend ("No unique definition is found for the virtual register").

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
- **A function nothing calls stays in the optimized module.** The new pass
  manager's inliner deletes only a function whose last call it inlined; the
  linker's `/OPT:REF` drops the rest, each being in a COMDAT of its own.
- **The block stack is a fixed 256 entries** and overflow is a hard exit.
- **A value held across the generation of another operand breaks a split
  method if that operand holds a seam.** A second half enters after the seam,
  so a value generated before it, in a block no path reaches there, does not
  dominate its use: `--verify` says so. Every site that evaluates operands in
  turn and uses the earlier ones afterwards goes through `genlExprsAcross`,
  which keeps each in flight across a seam ("A split method"); an address held
  that way cannot be, so it is taken after the seam, as a place written to the
  left of an `await` is, or its `await` is refused before generation
  (`awaitWalk`). A new such site needs one or the other, in the same order flow
  walks it.
- **A message seam is laid out before it is reached**, by its envelope, the
  last argument of the call it awaits ("A message's reply"). Anything that
  makes a temporary, or puts a value in flight, between that argument and the
  seam -- a call awaited whose result were itself a temporary, say -- makes
  the record laid out differ from the one the seam builds, which `genlAwait`
  refuses as unreachable rather than build a wrong record.
- **A jump out of the middle of a statement finalizes its pending temporaries
  before the locals of the blocks it leaves**, whatever order they were made
  in: `f(mk(1).n, { imm x = mk(2); if c { return; } 3; })` finalizes the first
  `mk` before `x`. Each dies once; only the order, in that shape, is not
  newest first.
- **A function taking an aggregate passed as a pointer, or returning any
  aggregate but one or two scalars, is a different LLVM function after
  `genlAggCopies`** (section 6, "Large aggregates", "Lending a place to a
  call"): remade with pointer parameters or another result type and the old
  one deleted, so a `FnDclNode`'s `llvmvar`, or any other function value
  kept from generation, is stale once it has run.
  Nothing after it reads one.

## 9. Code pointer map

| File | Function | Purpose |
| --- | --- | --- |
| `conec.c` | `main` | calls `genSetup` **before** parsing, for target pointer size |
| `genllvm/genllvm.c` | `genSetup`, `genClose` | target machine, data layout, context, `%void`; LLVM's pretty stack trace, for a crash inside LLVM |
| | `genlLLVMOptions` | the LLVM options `CONE_LLVM_OPTIONS` names, and a GPU target's own (the flat address space; machine CSE, loop strength reduction and CodeGenPrepare off), parsed before the context exists |
| | `genpgm` | generate, move large aggregates into memory (not on a GPU target), verify, dump, optimize (release: LLVM's `default<O2>` for the target machine; a GPU target's pipeline inlines, breaks up aggregates and infers address spaces, then the kernels are settled, then the control flow is structured, at every level), emit; nothing past generation once it reported an error, nor once a kernel's slice was refused |
| | `genlGpuCalls`, `genlCallWalk`, `genlRecursion` | on a GPU target, every defined function but a kernel marked `alwaysinline`, and a cycle of calls refused (section 7) |
| | `genlGpuAggregates`, `genlGpuAggregatesFn`, `genlAggLeaves` | on a GPU target, after optimization, each struct or array value carried as its scalar leaves (section 7) |
| | `genlGpuRetypeFn`, `genlGpuPointee` | on a GPU target, after optimization, a first field's address computed from its struct's type again (for a load, a store, an address computation or an atomic), and an array's from its first element's (section 7) |
| | `genlGpuGepChainsFn` | on a GPU target, after optimization, an address computed from another in one step, or beside it (section 7) |
| | `genlProgram` | create the module with the target's triple and data layout, the two-pass symbols-then-implementations walk, then the stitched pair |
| | `genlStitchFn`, `genlStitch` | the program's stitched init and final: declared on the first call to `initAll()` or `finalAll()`, built last, every module's `init` in the module order and every finalizer in the reverse |
| | `genlGlobalSyms`, `genlGlobalImpl` | declare a node's symbol; emit its body |
| | `genlImportedInstances` | emit the bodies of the instances this compile made of a module it does not generate |
| | `genlFn`, `genlParmVar`, `genlAlloca` | function body (a split method's first half, then its second halves), parameter allocas, entry-block alloca placement |
| | `genlParmInPlace` | whether a parameter passed as a pointer is only read, its store marked for the pass to read it in place (section 6, "Lending a place to a call") |
| | `genlRootNote`, `genlRootBirth`, `genlRootFrame`, `genlRootsSave`, `genlRootsRestore` | roots: a slot noted as one, a birth's slot, the frame's map, push and pops; the roots set aside around a nested function |
| | `genlGloFnName`, `genlGloVarName` | declare a function or global under the symbol `nameSymbol` spells |
| | `genlIsVoidMain` | whether a function is a `main` returning nothing, generated returning `i32 0` for the exit status |
| | `genlClaimSymbol`, `genlSymAgree`, `genlSymOwner` | one symbol, one global: which of two declarations spelling one symbol has it, or `ErrorCNameConflict` / `ErrorCNameDefTwice` |
| | `genlGloVarIsConstant` | whether an `imm` global is an LLVM constant: an initial value, not `extern`, no drop function |
| | `genlLinkage`, `genlDefinition`, `genlIsDefinedHere`, `genlVtableDefinition` | linkage, storage class and calling convention, together, from the declaration facts and what this object does with the symbol: declares it, defines it, defines and exports it, or defines it shared |
| | `genlComdat`, `genlNameAnonFn` | the per-definition COMDAT that lets the linker drop a symbol, its kind read off the linkage; the private name an anonymous `fn` needs to have one |
| | `genlComdatSupport` | what the target's object format does with COMDATs |
| | `genlOut` | emit object and asm, the asm from a copy of the module, by a target machine of its own, on a GPU target |
| `ir/export.c` | `dclIsInstance` | whether a declaration is a generic's instance or a member of one — every function and global of a generic module's instance among them |
| | `dclIsExported`, `typeHoldsExpanded` | whether a library compile exports a definition to its importers; the include-file generator asks the same |
| `genllvm/genltype.c` | `genlType`, `_genlType` | the memoizing entry, which generates the queued pointees once the outermost type is done, and the per-tag lowering switch |
| | `genlUsize` | `usize`'s LLVM type: `ConeOptions.ptrsize` bits, the target's pointer, but 32 on SPIR-V's Vulkan form (section 7) |
| | `genlPointee`, `genlPointeeType` | the Cone type a reference, pointer or slice points at, and its LLVM type: what every load, GEP and call through it is typed by |
| | `genlVtableSlotFnType` | a vtable slot's function type, self erased to `*u8`: the slot's type, a thunk's, and a virtual call's |
| | `genlSetupTaggedTrait`, `genlSameSizeTrait` | the three enum shapes |
| | `genlVtable`, `genlVtableImpl` | vtable type, per-struct constants (the implementer's type record last), the virtref fat pointer |
| | `genlVtableThunk` | the function filling a slot a folded method satisfies: shift the receiver along the recorded field path, tail-call the method |
| `genllvm/genlgpu.c` | `genlComputeEntry`, `genlGpuInvocation`, `genlGpuHandle`, `genlGpuArrayLength` | a compute entry point's kernel: its parameters from the bindings and the built-ins, each buffer's count by a call `genlGpuPatch` rewrites (section 7, "Compute entry points") |
| | `genlGpuFailCheck`, `genlGpuPanic`, `genlIsConePanic` | on a GPU target, a failed check and core's `panic` as a call to `cone.gpu.fail` |
| | `genlGpuMath` | on a GPU target, a call to the C library's math by its C symbol as the LLVM intrinsic, GLSL.std.450's instruction (section 7, "The C library's math") |
| | `genlGpuEntries`, `genlGpuRecord`, `genlGpuFileId`, `genlGpuOneReturn` | each kernel settled once everything is inlined: a failure recorded in its error buffer and the kernel left, its returns made one |
| | `genlGpuSlices`, `genlGpuFold`, `genlGpuPartOf`, `genlGpuSite` | each step by pointer arithmetic folded into an access chain of a buffer or fixed array, or refused (`ErrorGpuSliceOrigin`) where the node it was made for is |
| | `genlGpuSignedRem` | on a GPU target, after optimization, each `srem` made the dividend less the quotient times the divisor (section 7) |
| | `genlGpuBufferAccess` | a struct or array loaded from or stored into a storage buffer a scalar at a time |
| | `genlGpuOut`, `genlGpuPatch` | the Vulkan form's module emitted to memory, `OpArrayLength` and the source files' list written into it |
| | `genlGpuNoContraction`, `genlGpuNoContractionAsm` | every float operation's result but a remainder's decorated `NoContraction`, in the module and in `--asm`'s text (section 7, "The C library's math") |
| `genllvm/genlgpusync.c` | `genlGpuAtomics`, `genlGpuAtomicConstPtr`, `genlGpuCompareSwap`, `genlGpuAtomicSite` | on a GPU target, after optimization, each atomic's scope and ordering from its memory, an atomic at a constant place reached by an address-computing instruction, a compareSwap as `llvm.spv.cmpxchg`, one on unshared memory refused in a kernel (`ErrorGpuAtomicPlace`) (section 7, "What invocations share") |
| | `genlGpuBarrier`, `genlGpuGlobalSpace` | the two barriers as LLVM's SPIR-V intrinsics; a `@workgroup` global's address space |
| | `genlCpuBarrier` | a barrier on a native target: the thread's barrier hook called when it is set (section 7, "Barriers on the CPU") |
| | `genlGpuSyncPatch` | the emitted module put right: a relaxed atomic's semantics None, a compare-and-swap's result rid of the insertions the backend writes after it |
| `genllvm/genlcabi.c` | `genlCAbiTarget`, `genlIsCAbiFn`, `genlCAbiPass` | which C ABI the target follows, which functions cross by it, and how one struct crosses |
| | `genlCAbiExtend`, `genlCAbiMarkExtends` | the `zeroext` or `signext` a narrow integer crosses with, marked on a declaration or a call |
| | `genlFnDclType`, `genlCAbiDeclare`, `genlFnDclCall`, `genlFnDclParm`, `genlFnDclReturn` | a declared function's LLVM type, its `sret` and widening marks (and the `"cone-cabi"` mark `genlAggCopies` leaves alone), a direct call to it, its prologue's parameters and its returns — lowered for a C-named one |
| `genllvm/genlaggcopy.c` | `genlAggCopies`, `genlAggFn`, `genlAggRetClass` | once the module is generated, every struct, array or tuple of more than `GenlAggCopyMin` bytes moved into memory, function by function, and every smaller aggregate result returned as a vector, as integers or through a slot (section 6, "Large aggregates") |
| | `genlAggRetype`, `genlAggSig`, `genlAggCall` | a function taking or returning one remade with pointer parameters and a result slot first; each call to that type passing homes |
| | `genlAggInst`, `genlAggHomeOf`, `genlAggReadInPlace` | each instruction making or taking one, rewritten to copy between homes; a value's home, made where it is; a load read where it lies when nothing between could change it |
| | `genlAggCopyTo`, `genlAggConstTo`, `genlAggPhis`, `genlAggEdge` | a copy with `llvm.memcpy`, a constant's with `llvm.memset` or field by field, each phi's slot filled at its predecessors' ends, an edge given a block of its own |
| | `genlAggByPtr`, `genlAggPassesByPtr` | whether an argument of an LLVM type is passed as a pointer (section 6, "Lending a place to a call") |
| | `genlAggLent`, `genlAggArgPtr`, `genlAggWideStore` | a load generation lent, passed its address; any other argument passed as a pointer, copied: where it was loaded, a call's result slot passed on, or stored wide |
| | `genlAggParmInPlace`, `genlAggParmLoad` | a parameter only read, its variable's slot made the pointer; any other read whole as the body begins |
| `genllvm/genlstmt.c` | `genlBlock` | block creation, phi state, terminator suppression; each statement's temporaries finalized at its end |
| | `genlBreak`, `genlReturn` | phi edges, temporaries and dealias; inlined-return-as-break |
| `genllvm/genlexpr.c` | `genlExpr`, `genlAddr`, `genlStore` | the value / address / store trio — section 4 |
| | `genlFlatAddr`, `genlVarSym` | a global's address as a reference holds it: cast to the flat address space on a GPU target (section 7) |
| | `genlTerm`, `genlIsBirth`, `genlAddrThroughRef`, `genlExprForLocal` | an expression's value, and whether it is a birth to root (section 3, "Roots") |
| | `genlStoreBarrier` | after a store through a reference or pointer, the barrier on what was stored (section 3, "The write barrier") |
| | `genlAddrType` | the Cone type of what `genlAddr`'s address points at |
| | `genlFnCallInternal` | indirect calls, virtual dispatch, generator-level inlining, the intrinsic switch |
| | `genlDeclaredIntrinsic` | the LLVM implementation of each intrinsic declared in core, by kind and Cone type |
| | `genlBitIntrinsic` | an integer's bit intrinsics and the integer methods built from them: counts, rotates, masked shifts |
| | `genlShift` | `<<` and `>>` on an integer, defined past the width: a compare and a select, none for a constant amount |
| | `genlAtomicIntrinsic` | an atomic intrinsic, reached from `genlFnCall` with the call's constant orderings |
| | `genlLendable`, `genlLendQuiet`, `genlCallLends`, `genlArgsLending` | whether a call may be handed an argument's own storage, each case written out; a call keeping Cone's convention; its arguments, each lent place loaded from its address and marked (section 6, "Lending a place to a call") |
| | `genlConvert`, `genlRecast`, `genlIsType` | the three cast forms |
| | `genlArrayIndex`, `genlBoundsCheck` | multi-dimensional GEP and its checks |
| | `genlArrayLitInto`, `genlArrayRun` | an array's contents repeating a value, or the scalars of one written with several sizes, filled in place element by element, into a variable or an allocation: a `memset` for a null constant, a loop for any other repeated value, never one aggregate ([literals](../nodes/literals.md), "Generation") |
| | `genlArrayLitScalars`, `genlArrayConstRows` | a literal of scalars wanted as a value: the nested constant, rows cut from the scalars, or filled into a local and loaded |
| | `genlSubslice` | a borrowed range index, `&x[a..b]`: the slice `{&x[a], b - a}` once `a <= b <= count` is checked |
| `genllvm/genlalloc.c` | `genlRefTypeSetup`, `genlallocref` | the `{region, perm, value}` header and an allocation's emission, in its order (section 3) |
| | `genlRegionHeader`, `genlRegionAlias`, `genlRegionDealias`, `genlRegionDeath` | the header a region method is handed; calling `aliasRef`, `dealiasRef` and `free` at each reference event; a death in place, then `free` |
| | `genlOwnerHeader`, `genlVirtHeader`, `genlVirtRecord`, `genlVirtFinalize` | an owning virtual reference's header, from its vtable record's alignment; that record; its value's death through the record's `finalize` |
| | `genlHollowRelease`, `genlRegionDealiasPart`, `genlHollowDeath`, `genlReleasePart` | a hollowed variable's release: the death of a value moved out, or an element of it, finalizing none of it and freeing the memory |
| | `genlReleaseOwning`, `genlDealiasNodes` | releasing one owner of an owning reference or of each a tuple value carries, and replaying flow's lists |
| | `genlTempKeep`, `genlTempsEnd`, `genlTempsJump`, `genlTempRelease` | a temporary kept in its slot; those a part made finalized at its end, or before a jump ("Temporaries"); a lock's guard in a split method only where its flag says it holds its lock ("A split method") |
| `genllvm/genlawait.c` | `genlAwait`, `genlSeamLayout`, `genlSeamGiveBack`, `genlSeamRecord` | a seam of a split method: what is awaited, the record laid out (once) and its second half and resume function declared, the locks given back, the record built and parked, the return; what follows in a `resume` block ("A split method") |
| `genllvm/genlyield.c` | `genlGenBegin`, `genlGenEnd`, `genlYield`, `genlGenReturn` | a generator's `next` ("A generator"): the frame found and the first call's boxes made at the entry, the entry switch on the state, a `yield`'s state store, return and resume stub, the done block, a return's `GenDone` |
| | `genlGenPlan`, `genlGenFrameType`, `genlGenFrameVar`, `genlGenFrameFlag`, `genlGenSelfHolding`, `genlGenDrop` | the frame: which locals, in what kind of storage, its LLVM type after the struct's fields, a local's storage and drop flag found there, the generator's death |
| | `genlSplitHalves`, `genlSplitHalf`, `genlSeamEntry` | each second half: the method generated again, its entry moving the record's values back and branching to the seam's `resume` block |
| | `genlAwaitReply`, `genlSeamReply`, `genlSeamResume` | a message seam's envelope, the seam laid out and its record's slot reserved where it is passed (an operation's seam makes its envelope at the seam and starts the operation with it); the seam's resume function, which takes the record out of the pending table, calls the second half and answers ("A message's reply") |
| | `genlSeamDrop` | a parked record's drop function, which its pending table calls if the actor dies with it parked ("A message's reply", "An abandoned record") |
| | `genlAwaitFuture`, `genlFutureOpen` | a seam awaiting a future: gone on from where it stands when the future has its ending, else parked on it; the value opened out of the future on either path and in the resume function ("A message's reply", "A future") |
| | `genlExprsAcross`, `genlHasSeam` | operands in order (`awaitOrder`'s, where a seam cuts them), each made before a later one's seam kept in flight across it (`GenFlight`), a receiver or a borrow of a plain path made after it |
| | `genlKeepAcross`, `genlKeptAcross` | one value kept in flight across a seam to come, and read back after it: an assignment's value while its place, holding the seam, is reached |
| | `genlHeldBegin`, `genlHeldIf` | a temporary lock guard's flag, and code run while it holds its lock |
| `genllvm/genlpar.c` | `genlParallelRun`, `genlParCapture`, `genlParCount` | a parallel each: the index and loop generated as a function of their own, a variable of the caller's found through the function's record, the call of `actors.parallelEach`, a number range's count ("A parallel each") |
| | `genlFinalizeAt`, `genlCallDrop`, `genlEachElem` | a value's death in place, whatever its type: a local's, a field's, a region value's before its `free`, and the `finalize` intrinsic |
| | `genlTypeDrop`, `genlStructDrop`, `genlEnumDrop` | the body of a drop the compiler gave a type: a struct's `final` calls, its fields' deaths, its owners' release; an enum's tag dispatching to its variant's |
| | `genlAliasHeld` | a copied struct, enum, tuple or array: `aliasRef` on each counted reference its death releases |
| | `genlTypeRecord`, `genlTypeRecordOf`, `genlTypeRecFn`, `genlTypeRecNothing` | a type's record, once per object: its size, alignment, finalizer function, trace function and flags; what an `alloc` that asks is handed, `mem.typeRecord`, and a root map's entries |
| | `genlTraceAt`, `genlTraceWalk`, `genlTraceRef`, `genlTraceVariants` | a value's traced references, each handed to its region's `mark`: a record's trace, and `mem.trace` |
| | `genlBarrierAt`, `genlHoldsBarriered` | the write barrier: the same walk over a value just stored, each reference into a region with a `writeBarrier` handed to it |
| `packages/conestd/roots.cone` | `cone_gcframes`, `cone_traceRoots` | the head of the chain of frames, and its walk, which `mem.traceRoots` calls |
| `packages/conestd/barrier.cone` | `cone_barrierHook`, `cone_barrierCtx`, `cone_setBarrierHook`, `cone_clearBarrierHook` | the thread's barrier hook and its context, which a barrier on the CPU calls, and core's `setBarrierHook` and `clearBarrierHook` |
| `packages/conestd/hash.cone` | `cone_hashSeed` | the process's hash seed, 64 random bits from the OS (the C runtime's `rand_s`) drawn once, the first time core's `processSeed` asks, from any thread: what `Hasher.seeded()` starts from |
| `ir/types/reference.h` | `enum ManagedRefFields` | `RegionField`, `PermField`, `ValueField` |
| `ir/name.c` | `nameSymbol`, `nameType`, `nameVtable`, `nameVtableImpl`, `nameVtableList` | spelling a symbol from a node's owner chain and facts, and a type argument within it — the rules are in [Names and Namespaces](../../../../doc/design/names-and-namespaces.md), "Symbols" |
| `ir/dclinfo.c` | `dclInfoJoin` | writes the declaration facts where a declaration joins its namespace |

## 10. What lives elsewhere

| Question | Note |
| --- | --- |
| What a region and a permission mean before they are erased | [References and Regions](../../../../doc/design/references-and-regions.md) |
| What injected the reference-count nodes and dealias lists | [Flow Analysis](flow.md) |
| What guarantees every node has a `vtype` | [IR Nodes](../nodes/_index.md), "--checktree" |
