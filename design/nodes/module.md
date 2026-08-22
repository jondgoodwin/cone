`ModuleNode`, `ImportNode` and `ProgramNode` are one note, because a module is
only meaningful against the program that holds it and the imports that reach
into it.

This note also carries the **module / package / compilation-unit model** — what
a module is meant to be, how it becomes something a linker can consume, and
which parts of that are decided. The model spans parse, name resolution and
generation, so no phase note owns it, and it is what a reader usually needs
before touching any of the three nodes.

**At a glance.** `parsePgm` builds the root module and injects `corelib` into
it. `parseLoadAndParseModuleFile` loads every other module exactly once, keyed
by a name taken from its filename. Name resolution folds imports before it
resolves anything else the module declares. Type check walks imports first, then
every declaration in source order. Generation declares symbols for every module
and emits bodies only for those flagged `FlagGenMod`.

*Provenance: read from source. The symbol-prefix asymmetry and the `stdio`
exception were measured from emitted LLVM IR; the imported-module `declare`s are
pinned by the `module` test group. See [Measuring](../diagnostics/measuring.md).*

## Shape

**`ProgramNode`** carries only `Nodes *modules`: the root module first, then
every other module in the order it was first loaded. **There is no dependency
edge between modules** — the only structure is this flat list plus each module's
own `imports`.

**`ModuleNode`**

| Field | Meaning |
| --- | --- |
| `namesym` | the module's name — **NULL for the root module**, and derived from the *filename* for every other |
| `imports` | `ImportNode`s only, held apart from `nodes` so folding can run before anything else resolves |
| `nodes` | every declaration the module owns, in source order. This is what printing and generation iterate |
| `namespace` | every name *visible* in the module: what it declares, plus what an import folded in |
| `flags` | `FlagGenMod`, and nothing else |

**`nodes` and `namespace` are not the same set, and the difference is exactly
where import folding lives.** A folded name is added to `namespace` and never to
`nodes`, so the receiving module can resolve it but does not own, print or
generate it.

`ModuleNode` extends `IExpNodeHdr` and so carries a `vtype` slot, but
`ModuleTag` is `StmtGroup + NamedNode`: `isExpNode` is false, `newModuleNode`
never sets `vtype`, and nothing reads it.

**`ImportNode`** holds `module` — the loaded `ModuleNode` — and `foldall`,
recording whether `::*` was written. That is the whole of import: there is no
selective name list, no rename, and no exclusion.

## Constructors

| Function | Note |
| --- | --- |
| `newProgramNode` | one per compile |
| `pgmAddMod` | appends a module and takes its flags. The caller sets `namesym` afterwards |
| `pgmFindMod` | linear search by interned name. **This is what makes a module load once** however many modules import it |
| `newModuleNode` | `namesym` NULL, empty `imports`, `nodes` and `namespace` |
| `newImportNode` | `module` NULL, `foldall` 0 |

## Parse

`parsePgm` establishes the program in an order that matters:

1. The root `ModuleNode` is added first and flagged `FlagGenMod`. **Its
   `namesym` is never set.**
2. `corelib` is parsed, from the `corelibSource` string in `corelib.c`.
3. An `ImportNode` with `foldall` set is added to the root for `corelib`.
4. The root's own source is parsed.

`parseLoadAndParseModuleFile` is the single path by which any module is loaded.
It reuses an already-parsed module by name, pushes a `gennamePrefix` built from
the module name, decides `FlagGenMod`, injects the source, adds an auto-import
of `corelib` with `foldall`, and swaps the name-table hook with `modHook`.

**Two modules are built in, and neither is a file.** `corelib` is the
`corelibSource` string in `corelib.c`; `stdio` is the `stdiolib` string at the
top of `parsemod.c`. Both are injected by `lexInject` rather than read from
disk.

**Whether an imported module's bodies are generated is decided by its filename,
here.** The one expression in `parseLoadAndParseModuleFile` that computes the
flag grants `FlagGenMod` to the root module, withholds it from `corelib`, and
grants it to a module whose filename is exactly `stdio`. Every other imported
module is denied it.

That asymmetry is the whole of the separate-compilation gap, and both sides of
it are visible in emitted IR:

- `import stdio::*` emits `@stdio_print` **and definitions** for
  `@stdio_IOStream__appendStr` and its siblings. The multi-module generation
  path works, and is exercised on every compile that prints.
- Importing an ordinary module emits **only `declare`s** —
  `declare i64 @modulesub_scaleInt(i64)` — because its bodies are never reached.
  Measured, that is the module's *public* surface whether or not the importer
  calls it: a public function nothing references is still declared, and a private
  one is not declared at all. So what an import contributes today is already the
  shape of a `.h` file, derived from the imported source rather than from a
  reduced artifact.
- Compiling that same module as the root emits `define i64 @scaleInt(i64)`,
  unprefixed, because the root's `gennamePrefix` is the empty string and
  `nameGenFnName` applies nothing to an empty prefix.

So **a symbol's identity depends on which compilation the module was the root
of**, and the two spellings never resolve against each other. That, and not the
declarations, is why an import cannot be linked against: nothing can emit the
definitions those declarations name.

`parseImport` derives the module name from the filename through `fileName`,
accepts `::` only when `*` follows it, and binds the loaded module into the
importing module's namespace with `modAddNamedNode`.

`parseInclude` injects the named file's tokens and parses its global statements
into the *current* module. It builds no node, creates no namespace, and leaves
no record that it happened.

Generated-name prefixes are built by `nameNewPrefix` and `nameConcatPrefix`,
extended with the type name by `parseStruct`, and applied by `nameGenFnName` and
`nameGenVarName`. Both apply nothing when the prefix is empty, and nothing when
the declaration carries `FlagExtern`.

## Name resolution

`modNameRes` hooks the module's namespace, resolves `imports` first, then
`nodes`, then unhooks. Imports go first because a name folded in must be
bindable before any declaration that uses it is resolved.

`importNameRes` does nothing unless `foldall` is set. When it is, it walks the
source module's `nodes`, skips anything unnamed or private, and calls
`modAddNamedNode` on the target. **The fold binds the original declaration node
into a second namespace**: one node, two bindings, and nothing in the receiving
module recording where the name came from. A different local spelling is
therefore not expressible, which is why renaming and selective folding are
described in [Names and Namespaces](../phases/names-and-namespaces.md) and are
not implemented.

Private is spelling: `inodeIsPrivate` tests for a leading `_`. A public overload
name whose selected candidate is private still travels across the fold, because
the `FnOverloadDclNode` is what folds and the candidate rides inside it.

## Type check

`modTypeCheck` type checks the imported modules first, then every declaration
the module owns, in source order. As everywhere in this phase, **order decides
when a declaration is checked, not whether** — a name reached from elsewhere
pulls its declaration forward. See [Type Check Phase](../phases/type-check.md).

**Nothing detects an import cycle.** Reuse by name in `pgmFindMod` stops the
parser recursing forever, but no phase asserts that module dependencies form a
DAG.

## Flow and generation

Flow analysis has no module concept; it runs per function body.

`genlProgram` is two strict passes over `pgm->modules`:

1. **Symbols.** Every module, generating or not. A declaration is skipped only
   when it is private *and* its module is not generating.
2. **Implementations.** Only modules flagged `FlagGenMod`.

`ImportTag` is an explicit no-op in `genlGlobalImpl`. Generic instances get
`LLVMLinkOnceAnyLinkage` so the linker keeps one copy across object files, which
is the only place today's generation anticipates more than one object file at
all.

The privacy filter in pass 1 assumes nothing outside a module can reach its
private names. **A public overload name breaks that assumption**, so
`genlGlobalSyms` generates every candidate of an `FnOverloadDclNode` explicitly.

## The model, as decided

The argument is in the author's *When Modules Are Not Just Namespaces*
(`ProgLing/plingsite/content/post/cone-modules.md`) and *Modules vs Types*
(`modules-vs-types.md`); what modularity is for is in
[Modularity](../northstar/modularity.md).

### The package is the unit

- **A package is the unit of distribution, the semantic unit, and the
  compilation unit.** All of a package's source files are parsed, resolved and
  type checked together, which is what lets them see each other's names with no
  declarations written by hand.
- **A package emits one object file.** Selective linking is preserved by giving
  each function its own section and COMDAT, so the linker's inclusion
  granularity is the **function**, not the object file. `--gc-sections` on ELF
  and wasm, `/OPT:REF` on COFF.
- **Splitting a codegen partition is a build knob with no semantic content.**
  If LLVM's optimization time on a large package becomes a problem, the package
  may be emitted as several LLVM modules; nothing about the language changes.
  Splitting a large codebase into several *packages* is the coarser form of the
  same lever, and is the recommended one.
- **Package dependencies form a DAG.**

Two consequences of the package being the compilation unit are worth stating,
because they remove work rather than adding it:

- **No declaration needs an owning source file.** A generic instance, an
  expanded macro and a cloned trait default method have use sites rather than a
  home file, and with one object per package the question never arises.
- **Deduplication happens in the IR, not in the linker.** `genericinfo`'s
  `memonodes` memoizes an instantiation on the generic's declaration, matched by
  argument types, so twenty files instantiating `Option[i32]` produce one
  instance and one symbol. `LLVMLinkOnceAnyLinkage` is therefore a
  **cross-package** mechanism only.

### The module is a namespace

- **A module is a namespace**, holding global variables, functions, types,
  macros and other modules.
- **A package correlates to one top-level module.** Its name is the package's.
- **Modules nest, and are reached by path.** Nesting stays inside a package, so
  package names themselves are flat and build tooling never sees a multi-level
  name. The shape is .NET's: the assembly is the unit, and the namespaces within
  it are free to nest.
- **A module may span several source files.** Each source file belongs to
  exactly one module.
- **Namespace machinery is meant to be common to modules and types** — nesting,
  generics, interfaces and name folding, so that the layers look alike rather
  than each inventing its own.

**What separates a module from a type is state, not namespace.** A module's
state is global and singleton: gathered by the link editor, reached at a fixed
address, and its functions take no `self`. A type's state is per-instance and
may live anywhere in memory, reached through `self`. That distinction is what
makes a module the natural shape for a region or a subsystem and a type the
natural shape for a value, and it is why a module cannot be nested inside a
type. A module holding a single type is therefore not a special construct — it
is a type sitting at the package's top level.

### Source files and folders

A module's source files are the files of a folder, and the folder tree carries
the module tree:

- **A module folder holds one designated file, named for the folder** —
  `matrix/matrix.cone`. It alone carries the `mod` block. Every other `.cone`
  file in the folder is auto-included into that module, exactly as `include`
  injects a file's global statements today.
- **A subfolder is a submodule when it holds its own designated file declaring a
  `mod`**, and organizational otherwise. An organizational folder's files, at
  any depth beneath it, belong to the enclosing module. This is what lets a
  forty-file module group its files by topic without minting namespaces for
  them.
- **A module folder must be a direct child of its parent module's folder.** So
  the module tree's *shape* mirrors the folder tree's. A designated file found
  beneath an organizational folder is an error, not a deeper submodule.
- **A module's name comes from its `mod` declaration**, and is conventionally
  the folder's name rather than required to be. Structure corresponds; names
  need not.
- **The compiler is pointed at one file** and walks outward: siblings join the
  module, subfolders are probed for their designated file. It needs no package
  concept to do this. The layout convention that a package's top module lives in
  `src/` as `<package>.cone` is congo's, and the compiler never sees it.

Two collisions follow and want diagnostics that name full paths: two
organizational subfolders can each declare the same name into the enclosing
module, and two sibling module folders can declare the same module name.

### Composing packages

- **`import` names a package** — a name resolved through the search path, never
  a file path — and its declarations arrive **externally supplied**: declared
  for the linker, never defined by this compile. That subsumes `extern` for Cone
  packages; the importer writes no `extern` keyword.
- **A module imports a given package at most once.** A second import of the same
  package with an identical fold spec is silently ignored; a differing one is an
  error. Identity is the resolved package plus the normalized fold spec — the
  wildcard flag and the set of source-name/local-name pairs, order-insensitive.
- **Folding accumulates into the one module namespace.** There is no file-level
  scope: any file may write imports, all of them fold into the module, and the
  namespace's existing uniqueness rule reports a collision. One consequence is
  deliberate — a spelling cannot be aliased two ways within one module, because
  within one namespace it is one thing.
- **`use` states what to fold**, as a clause of `import` for a package and
  standing alone for a namespace already in scope:

  ```
  import opengl use setColor, sub::* except green
  use matrix::*
  ```

  The fold lives with the declaration when there is one, which is what keeps a
  package's fold spec in a single place. `except` binds to the wildcard it
  follows. A `use` clause does not unbind the package name, which stays
  available as a qualifier. A long list takes a block form. `using` stays
  reserved so that spelling can be diagnosed rather than merely rejected.
- **`include` is retired.** A module spanning source files does properly what
  `include` did by injection.
- **Building a package emits an interface artifact** for importers to read
  instead of re-parsing implementation sources. Until one exists an imported
  package is parsed for its declarations and linked against its prebuilt object,
  so the artifact buys build speed rather than the ability to link. It cannot
  hold signatures alone: generics monomorphize at the use site, macros expand at
  the use site, `inline` is macro-shaped, and trait defaults are cloned into
  implementers, so an importer needs those bodies — and a private helper reached
  from one of them has to travel with it, emitted by nobody and callable by
  nobody.

### Visibility

There are no header files and no export list: a package's public interface is
what its definitions say it is.

- **`_` is private to its module**, always — not to the package. A nested module
  neither sees its parent's private names nor exposes its own to it, so nesting
  is a real boundary. An organizational subfolder is how files are grouped
  *without* erecting one, which is why the boundary needs no escape hatch: no one
  is forced to nest for layout reasons.
- **A `_`-named submodule is private to its parent**, which is how a package
  keeps internals internal without a second visibility level.
- **A folded or imported name is private to the module that folded it**,
  whatever its visibility at the origin. `import B use c as d` binds both `B` and
  `d` in A, and neither is reachable as `A::B` or `A::d`. `pub` opts in:
  `import pub B use pub c as d`. A module's public surface is therefore what it
  declares and deliberately re-exports, never what it happens to depend on.
- **Folding never widens visibility beyond the origin** — only a public name can
  be folded at all, so no chain of re-exports can escalate.
- **`pub` marks a binding; `_` marks a declaration.** `pub` never appears on a
  declaration, and is contextual to `import`/`use` so it stays usable as an
  identifier.

**Visibility is a bit on the binding, and every check reads it.** The `_`
spelling writes that bit once, when a declared name's binding is built, and is
never consulted again. It could not be: the binding for `B` inside A must be
private while `B` is a public package in its own right, and no `_` appears
anywhere in the spelling to say so.

**The binding chain has two ends, and different questions want different ones.**

- **Access** is decided by the binding the caller *traversed*: X reaching `A::T`
  asks A's binding, never B's declaration. That is what makes a default-private
  fold enforceable, and it generalizes the existing rule that an overload name's
  visibility is checked against the spelling the caller used.
- **Linkage and code generation** are decided by the *declaring* binding, since a
  folded binding defines nothing and emits nothing.
- **Diagnostics want both ends**: the local spelling the author wrote, and the
  declaration that supplies the real name and source position. A failure reached
  through a `*` fold has to be able to say where the name came from.

So a binding carries its local spelling, its visibility bit, the value it refers
to, and a link to its origin — and the same record has to serve a type's
namespace, where folding a member is delegated inheritance.

### What is implemented

**Almost none of it.** A module is a source file today. There is no `mod`
declaration, no nesting, no package, no manifest, no interface artifact, and no
`use`; `import` takes a file path rather than a package name, folds only with
`::*`, and cannot rename or exclude. Sections and COMDATs are not emitted per
function. What does work is the multi-module *generation* path, exercised by
`stdio` on every compile that prints, and folding into a single module namespace,
which is what the accumulation rule above asks for.

**Visibility has no bit, and whether a fold transits is decided by load order.**
Six sites decide visibility and four of them read `namesym->namestr == '_'`
directly rather than through `inodeIsPrivate` — `nameUseNameRes`,
`fnCallLowerMethod`, `typeLitStructReorder` and the hidden-linkage test in
`genlGloVarName`. There is nowhere to record a folded binding's own visibility,
because `importNameRes` inserts the imported declaration node itself into the
receiving namespace.

Measured: `modNameRes` folds a module's imports at the start of *that module's*
resolution and `pgmNameRes` walks modules in load order, so a fold is invisible
to modules resolved earlier and visible to those resolved later. A root module
naming `mid::plain`, where `plain` was folded into `mid`, is rejected as an
unknown name; the same reference from a sibling module loaded after the folding
one compiles.

## What the model has not decided

Each of these is a question the current mechanism answers by accident, or that
two documents answer differently. They are recorded here so that work in this
area starts from what is actually open.

### The idiom for reaching a package's members

A package named after the thing it provides puts that name in every path twice —
`bigint::BigInt`. This is not an edge case: a module with no global state is
pure namespace, and a great deal of library code needs none, so single-type
packages will be common.

Four answers are available, and none is chosen:

- **Fold at import.** `import bigint::BigInt`, then write `BigInt`. Needs
  nothing beyond selective folding, and is what Rust does with `use`.
- **Name the top-level module independently of the package's distribution
  name**, so what you install and what you path through need not match.
- **Convention.** Name the package for the domain and the type for the thing, so
  the repetition never arises — `math3d::Point3`, Go's `bytes.Buffer`.
- **A shortcut rule**: a member whose name matches its module is reachable by
  the module name alone.

Prior art splits. Go accepts `time.Time` and tunes names so the qualified form
reads well; Rust accepts `regex::Regex` and leans on `use`; Python's
`datetime.datetime` is the cautionary case. Whether the answer should differ for
a package and for a nested module is part of the question.

### How a C library becomes a Cone package, and what an interface artifact is

These are one question. A C library's package and a package's generated
interface are the same artifact — public declarations, no bodies, symbols
supplied elsewhere — one written by hand and one emitted by the compiler.

Cone code must be able to use C-API libraries, and the mechanism must produce
something `import` can name — a package — rather than declarations sprinkled
through user code. `extern` therefore does not disappear so much as move: it
becomes how a package declares that its symbols are supplied by something the
compiler cannot read, and the `extern` block that today gets packaged into an
include file becomes the package itself.

What that needs, and none of it is designed: how a Cone name maps to an
unmangled C symbol, how calling convention and `trust` are stated, how opaque
types are declared, and whether such a package is written in Cone source or
generated. `--safe=package`, which exists in the option help and controls which
packages may use C FFI, is the policy half of the same question.

**`include` cannot be retired before this exists**, because packaging an
`extern` block into an include file is what the sample projects use it for.

### What a package exports, and what that does to its symbols

The linker has one flat symbol namespace and the language has many, which is why
a public name carries its package in its symbol. But that reasoning only bites
for names something else can reference, and **a program exports its entry point
and nothing else** — nothing imports a program, so no name of its ever has to be
referenced from outside. A program therefore needs the module path in its
generated names and not the package component.

What is open is the mechanism, and it is worth choosing rather than defaulting
into:

- **Linkage for what a program does not export.** Today private names get
  `LLVMHiddenVisibility`, in `genlGloVarName` and `genlGloFnName`. Hidden keeps a
  symbol out of a shared library's export table but leaves it a global symbol at
  static link, so it can still collide. `LLVMInternalLinkage` makes it
  object-local and collision-proof. The hazard that distinguishes them is silent:
  a program defining `fn log(...)` emits `@log` externally, a package calling
  libm's `log` emits a matching `declare`, the linker satisfies the reference
  from the program and never pulls `log.o` — so the package calls the wrong
  function with no diagnostic.
- **Whether private names in a *library* also become internal.** It shrinks the
  mangled namespace to exactly the names that cross a package boundary. It also
  means a private helper reached from a public inline or generic body must be
  re-emitted per importer rather than linked against, which is what `linkonce`
  already does for generic instances.
- **Build-mode defaults, or an explicit export set.** `--library` and congo's
  `exe`/`lib` targets already distinguish the modes. But a program built as a
  WebAssembly module or a DLL does export more than an entry point — the samples
  carry a `wasm.syms` listing exactly that. An explicit export set covers all
  three with one mechanism and three defaults, and gives the C ABI its hook,
  since exporting under an unmangled C name is the same operation.

### How far the module/type convergence goes

Modules and types are meant to share namespace machinery while staying distinct
in state. `mod X[T]:` is accepted syntax from the start so that no source needs
rewriting when the semantics arrive; what a generic module *means* is not
settled — per-instantiation global state, per-instantiation `init`, mangling
that encodes the instantiation, and cloning every declaration across every file
of the module rather than just a type's methods. Whether a package's own
top-level module may be parameterized is the sharpest form of the question,
since importing such a package would mean instantiating it.

**Substitution is wanted before generativity.** A region protocol — a module
supplying alloc, free, alias and dealias — is an interface, so module traits are
what the region work is waiting on, and a generic module is a separate axis.
Nothing forces that order technically; the demand does.

Also unsettled: whether folding into a module and folding into a type are
literally one operation. That is the case the author has called out as the
interesting one, since folding applied to types is delegated inheritance.

### What a region is

Three descriptions are live at once:

- *Region Modules* (`ProgLing/plingsite/content/post/region-modules.md`) says a
  region is an importable **module**, holding the annotation type, the region's
  global state, and its API.
- `refregionglo.html` shows a **`region` declaration** — `region @move so:` —
  with `fn alloc(size usize) Option[*u8]` and `fn free(self &uni rc)`.
- `corelibSource` implements them as **`struct @move so:`** with
  `fn _alloc(size usize) *u8` and no `free` method at all.

The `region` keyword is interned by the lexer and parsed nowhere.

This is not only a naming question. [Generation](../phases/generation.md)
records that `genlRcCounter` finds the reference count at `((usize*)ref) - 1`,
which holds only because `rc` has exactly one `usize` field and its permission
is zero-sized, and that `genlDealiasOwn` frees the reference pointer directly,
which holds only because `so`'s region struct is empty. Seven sites across
`ir/flow.c`, `genllvm/genlalloc.c`, `genllvm/genlexpr.c` and
`ir/exp/arraylit.c` branch on whether a region **is named** `rc` or `so`, via
`isRegion`. So a region is library code in placement only; its behavior is
compiled in. Any region beyond those two — arena, pool, tracing collector —
needs that name dispatch replaced by a protocol and the allocation header
generalized.

Whether that protocol is a contract the compiler holds structurally — a module
supplying the right methods — or one written in Cone as a module trait is itself
open. The first is buildable now; the second is what makes a region fully
library code.

That a module carries global singleton state, and a type does not, is why the
region-as-module description is the one that fits the state half; what a region
annotation on a reference names is a type.

### Consequences that follow whichever way those go

- **Symbol identity must stop depending on which module was the root.** The
  measured asymmetry above is the mechanism. A generated name has two
  components — the package, and the module path within it — and the package
  component is what makes a public name distinguishable once the linker flattens
  every namespace into one. What separator it uses, and how an overload name's
  concrete candidates are spelled, are open. So is the larger question below.
- **The serialized interface must carry bodies, not signatures.** Generics
  monomorphize at the use site, macros expand at the use site, and `inline` is
  macro-shaped, so an importer needs the body of each. The artifact is
  therefore serialized IR, and it exposes private declarations that a public
  generic or inline body calls — the same assumption the overload privacy
  filter already breaks.
- **Module `init` and `final` are specified and absent.** `refmodule.html`
  describes an `init` function marked `initpure`, a constraint that it read no
  uninitialized global of its own module and call only `pure` or `initpure`
  functions, compiler verification that every uninitialized global is assigned
  there, and dependency-ordered initialization across modules. `initpure`
  appears nowhere in the source. Region modules with global state — arenas,
  pools, collectors — cannot work without it.
- **Whether folding transits is unanswered.** If A folds B's public names and Z
  folds A's, whether Z sees B's names is posed in `modules-vs-types.md` and left
  as "option on which". A prelude for a core library is exactly this question.
- **Dependency fan-out is unmeasured.** Section GC decides what reaches the
  binary; it does not decide what must resolve at link time. Archive member
  extraction precedes it, so calling one function from a package pulls its whole
  object and every undefined symbol in it enters resolution. Whether that ends
  in a hard error for a symbol only unreachable code references depends on
  linker and version. It wants an experiment once a multi-package program
  exists, not an assertion.
- **Module substitution and generativity are aims without a design.** Both
  drafts that would carry them are outlines.
  [Modularity](../northstar/modularity.md) states the aim and measures the
  distance.

## Hazards

- **`include` and `import` look alike and are not.** One injects declarations
  into the current module and leaves no trace; the other builds a namespace.
- **The root module's `namesym` is NULL.** Anything keying on a module's name
  must handle it, and the empty `gennamePrefix` that goes with it is why root
  symbols are unprefixed. It also defeats `pgmFindMod`, which matches by name, so
  **an import cycle leading back to the root re-reads the root's file and parses
  it a second time as a distinct module**, prefixed with the root file's own
  name. Measured: the compile succeeds, and the second copy contributes a
  dangling declaration for every root name — a `declare` for each function and an
  `external global` for each variable. Nothing references them, so nothing fails.
  The duplication is latent rather than harmless: were that second copy
  generated, each becomes a second definition of a root function and a second
  allocation of a root global. Giving the root a name is what closes it.
- **A cycle among non-root modules is fine.** Name resolution runs after all
  parsing, so the half-parsed module `pgmFindMod` returns is complete before
  anything reads it. Nothing detects a cycle, and nothing needs to.
- **`FlagGenMod` is decided by a `strcmp` on the filename.** A user module named
  `stdio` would have its bodies generated.
- **A folded name is the same node in two namespaces.** Mutating a declaration
  through one binding is visible through the other, and the receiving module
  keeps no origin link.
- **`corelib` and `stdio` are C string literals.** A syntax error in either is
  reported against an injected pseudo-file, and editing either means rebuilding
  the compiler.
- **`parseModuleBlk` is declared in `parser.h` and defined nowhere.**
- **A module name resolves as a type name use** — the retag falls through to
  `TypeNameUseTag` by default, not because a module is a type.

## What lives elsewhere

- The name rules a module implements — lookup, qualification, visibility,
  folding, aliases, overloading: [Names and Namespaces](../phases/names-and-namespaces.md)
- What modularity is for, and how far Cone is from it: [Modularity](../northstar/modularity.md)
- Module loading as a parse-time activity, and the name-table hook:
  [Parse](../phases/parse.md)
- The symbol-naming rule, `linkonce`, and the allocation header:
  [Generation](../phases/generation.md)
- Mixins, trait inheritance, and types as namespaces: [struct](struct.md)
- Instantiation, cloning and memonodes: [generic](generic.md)
