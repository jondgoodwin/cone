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
- Compiling that same module as the root emits `define i64 @scaleInt(i64)`,
  unprefixed, because the root's `gennamePrefix` is the empty string and
  `nameGenFnName` applies nothing to an empty prefix.

So **a symbol's identity depends on which compilation the module was the root
of**, and the two spellings never resolve against each other.

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
(`ProgLing/plingsite/content/post/cone-modules.md`); what it means for Cone is
in [Modularity](../northstar/modularity.md). The decisions:

- **A module is package-sized, not file-sized.** `Module >= Source File`: one
  module is implemented by one or more source files, and each source file is
  wholly owned by exactly one module.
- **Every package is a module, and a module is essentially a package**, whether
  or not it is ever published.
- **Modules are single-level**, and their dependencies form a DAG.
- **There are no header files.** A module's public interface is inferred from
  its definitions, marked by spelling: a leading `_` is private.
- **Building a package emits a serialized public interface** for importers to
  read instead of re-parsing implementation sources.
- **The compilation unit is the module, not the source file**, with per-source
  object files preserving selective linking.
- **Name folding happens at source-file level.**

**None of the multi-file half of this is implemented.** A module is a source
file today — the shape the design explicitly rejects — and there is no package,
no manifest, no interface artifact, and no way to declare a module's name
independently of the file it was found in.

## What the model has not decided

Each of these is a question the current mechanism answers by accident, or that
two documents answer differently. They are recorded here so that work in this
area starts from what is actually open.

### Whether a module may contain a module

`cone-modules` says a module cannot be composed of sub-modules. `refmodule.html`
says a module may hold "even other named modules", and `module.h`'s own comment
lists "Modules nested within this parent module". The parser provides no syntax
for declaring one.

The distinction none of the three draws is between a module namespace
**containing a binding to** another module — which `parseImport` already does,
and which qualified names require — and a module **declaring** a module inside
itself. Settling this means choosing which of those "nested modules" means, and
saying so in the reference page and the header comment together.

### What `extern` is for

`refinclude.html` documents `extern` blocks as the C FFI mechanism, complete
with `trust`, opaque structs, extern methods, and the extern-block-as-include
idiom the sample projects use. [Names and
Namespaces](../phases/names-and-namespaces.md) instead designs `extern` as the
form a package's *serialized interface* takes, with matching `extern`s and one
implementation merging into a single canonical binding.

Those are two futures for one keyword — FFI declaration, interface declaration,
or both — and the code-generation provenance rules differ between them. Nothing
decides which.

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

### Where a folded name lives when a module spans files

Folding is decided to be per source file, and `import` today folds into
`mod->namespace`, of which there is exactly one per module. **There is no
file-level scope anywhere in the IR** — `include` shares the including module's
namespace outright.

Once `Module >= Source File` is real, two files in one module will each want
their own imports and their own folding choices, and a single module namespace
cannot hold both. Settling this means deciding whether a source file becomes a
lookup scope between the block scopes and the module namespace, and what that
does to the rule that a namespace is one uniqueness domain.

### What the compilation unit is, and what an object file is

The decision as written couples two claims that may be separable: *the module is
the compilation unit* — a semantic claim, that these sources are parsed,
resolved and checked together and can see each other's names without
declarations — and *per-source-file object files preserve selective linking*, a
build claim.

The second rests on an assumption worth re-examining. Selective linking at
object-file granularity was the C-era answer; a linker doing section-level
collection reaches the same result from one object file per module. Against
that, per-file object emission requires deciding which source file *owns* each
declaration, and that question is ill-posed for exactly the entities Cone leans
on hardest: a generic instance or an expanded macro has no home file, only the
use sites that triggered it — which is why `linkonce` is needed already.

The framing that may dissolve it: **a codegen partition is a build-performance
knob, not a semantic boundary**, and need not correspond to source files at all.
Settling this means stating separately what the semantic unit is, what the
emitted artifact is, and what — if anything — chooses a partition inside it.

The same question reaches upward. *Every package is a module* makes the package
and the semantic unit the same thing. If a package could instead hold several
modules, the compilation unit would be the package and a module would be a
namespace within it — which is also an answer to how a core library subdivides
without submodules.

### Consequences that follow whichever way those go

- **Symbol identity must stop depending on which module was the root.** The
  measured asymmetry above is the mechanism; what is open is what the stable
  prefix is a function of, what separator it uses, and how an overload name's
  concrete candidates and a generic's instances are spelled so `linkonce`
  merges them across units.
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
  folds A's, whether Z sees B's names is posed in
  `ProgLing/plingsite/content/post/modules-vs-types.md` and left as "option on
  which". A prelude for a core library is exactly this question.
- **Module substitution and generativity are aims without a design.** Both
  drafts that would carry them are outlines.
  [Modularity](../northstar/modularity.md) states the aim and measures the
  distance.

## Hazards

- **`include` and `import` look alike and are not.** One injects declarations
  into the current module and leaves no trace; the other builds a namespace.
- **The root module's `namesym` is NULL.** Anything keying on a module's name
  must handle it, and the empty `gennamePrefix` that goes with it is why root
  symbols are unprefixed.
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
