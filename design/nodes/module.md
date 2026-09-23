`ModuleNode`, `ImportNode` and `ProgramNode` are one note, because a module is
only meaningful against the program that holds it and the imports that reach
into it.

This note also carries the **module / package / compilation-unit model** — what
a module is meant to be, how it becomes something a linker can consume, and
which parts of that are decided. The model spans parse, name resolution and
generation, so no phase note owns it, and it is what a reader usually needs
before touching any of the three nodes.

**At a glance.** `parsePgm` builds the root module and injects `corelib` into
it. `parseLoadAndParseModuleFile` loads every other module, in three steps:
locate the file, ask the **file registry** which module holds it, and parse every
file of its folder into the module it draws. The registry is keyed by the file's
path, so a file is read once and belongs to one module. The folder is what names
the module, and a `mod` declaration in its designated file's first statement is
checked against that name. Name resolution folds imports before it resolves anything else the
module declares. Type check walks imports first, then
every declaration in source order. Generation declares symbols for every module
and emits bodies only for those flagged `FlagGenMod`.

*Provenance: read from source. The root-versus-import symbol asymmetry and the
`stdio` exception were measured from emitted LLVM IR; the imported-module
`declare`s and the root-cycle behaviour are pinned by the `module` test group.
See [Measuring](../diagnostics/measuring.md).*

## Shape

**`ProgramNode`** carries `Nodes *modules` — the root module first, then every
other module in the order it was first loaded — and `Namespace files`, **the file
registry**: every source file the compile has read, keyed by its path, mapped to
the module that holds it. **There is no dependency edge between modules** — the
only structure is the flat list plus each module's own `imports`.

**The registry is what makes a file read once and owned once.** Its key is the
path rather than a name, because what must happen exactly once is the *reading*:
two folders may hold files of one basename, and a module's declared name is not
known until its file has been read. A built-in module is a string inside the
compiler rather than a file, and stands in the registry under the pseudo-file
name its diagnostics are reported against — `corelib`, `stdio`.

**`ModuleNode`**

| Field | Meaning |
| --- | --- |
| `namesym` | the module's name: its **folder's**, where a designated file drew the module out of a folder, and `filesym` otherwise. What an importer binds it under, what a path through it is written with, and what its declarations' symbols are spelled after |
| `filesym` | the name derived from the module's *filename* — the source file's basename for the root, the imported file's for every other. It names a module that is one file, and nothing else reads it |
| `foldersym` | the module's folder, when that folder's designated file drew it; NULL for a module that is one file. It is what a `mod` declaration's name is checked against, and what says a folder was swept |
| `dclinfo` | the declaration facts — [Names and Namespaces](../phases/names-and-namespaces.md), "Symbols". `owner` is NULL for every file module. **The root is the module without `DclNamesChain`**: it has a name and contributes it to no symbol |
| `imports` | `ImportNode`s only, held apart from `nodes` so folding can run before anything else resolves |
| `nodes` | every declaration the module owns, in source order. This is what printing and generation iterate |
| `namespace` | every name *visible* in the module: what it declares, what an import folded in, and — when a `mod` declaration named it — the module's own name |
| `flags` | `FlagGenMod`, and `FlagModDcl` for a module a `mod` declaration named |

**A module's own name is in its own namespace, and that is what makes a hidden
module-level name reachable.** `mymod.x` reaches an `x` that a local or a type
member hides, because a path begins with the name of the namespace it walks and
this is the only name a module has. The binding is not in `nodes`, so nothing
prints, generates or folds the module into itself.

**`nodes` and `namespace` are not the same set, and the difference is exactly
where folding lives.** A folded name is added to `namespace` and never to
`nodes`, so the receiving module can resolve it but does not own, print or
generate it. That holds for both folds a module has — a wildcard import's, and a
global's `use` clause — and it is why neither transits an import: `importNameRes`
walks `nodes`.

`ModuleNode` extends `IExpNodeHdr` and so carries a `vtype` slot, but
`ModuleTag` is a named node in `StmtGroup`: `isExpNode` is false,
`newModuleNode` never sets `vtype`, and nothing reads it.

**`ImportNode`** holds `module` — the loaded `ModuleNode` — and `foldall`,
recording whether `.*` was written. That is the whole of import: there is no
selective name list, no rename, and no exclusion.

## Constructors

| Function | Note |
| --- | --- |
| `newProgramNode` | one per compile, with an empty file registry |
| `pgmAddMod` | appends a module and takes its flags. The caller sets `filesym`, `foldersym` and `namesym` afterwards |
| `pgmFindFile` / `pgmSetFile` | the file registry, by path. **This is what makes a file read once** however many modules name it, and what makes it belong to one module |
| `newModuleNode` | `namesym`, `filesym` and `foldersym` NULL, `dclinfo` cleared, empty `imports`, `nodes` and `namespace` |
| `newImportNode` | `module` NULL, `foldall` 0 |

## Parse

`parsePgm` establishes the program in an order that matters:

1. The root `ModuleNode` is added first and flagged `FlagGenMod`. It is not given
   `DclNamesChain`, so it prefixes nothing.
2. The main source file is located, and every file its folder sweeps in is
   registered to the root — before anything is parsed, so that which files the
   root holds does not depend on what a parse of one of them imports. An import
   cycle back to any of them then finds the root in the registry.
3. `corelib` is parsed, from the `corelibSource` string in `corelib.c`.
4. An `ImportNode` with `foldall` set is added to the root for `corelib`.
5. The root's own files are parsed, its designated file first.

`parseLoadAndParseModuleFile` is the single path by which any other module is
loaded, and it is three steps rather than one:

- **Locate.** `fileFindSrc` resolves the written name against the current file's
  folder and then each `--pkg-path` entry, trying `name.cone` and then
  `name/name.cone` — the designated-file convention. It returns the path and
  reads nothing. A built-in resolves to its pseudo-file name instead.
- **Register.** The path is interned and looked up in the file registry. A hit
  *is* the answer: that module already holds the file, and the file is not read
  again. A miss makes the module, sets `filesym`, `foldersym` and `namesym`,
  marks it `DclNamesChain`, decides `FlagGenMod`, and registers every file the
  folder sweeps in.
- **Parse into the module.** Each registered file is injected and its global
  statements parsed into the one module, the designated file first, since it is
  the only one that may declare the module. The auto-import of `corelib` with
  `foldall` is added first, and `modHook` swaps the name table once for the whole
  set. Every declaration any of those files adds through `modAddNode` records
  the module as its owner.

### The folder sweep

**A module's source files are the files of a folder**, and the compiler is given
one file and finds the rest. What makes a folder a module folder is the
**designated file** it holds, named for the folder — `matrix/matrix.cone`.

**That convention is the whole of the trigger.** The file the compiler is given
sweeps its folder exactly when it is that folder's designated file, which is the
same probe a subfolder would get. ▸ **So a file that is not its folder's
designated file is a module of one file, exactly today's program**, and the files
beside it are none of its business — which is what lets a folder hold a dozen
unrelated programs, as every one of the test suite's group directories does.

⚠ **Two other triggers were considered and are wrong, and the measurement is what
decides it.** *Always sweep* and *sweep when the given file declares a `mod`* both
make every `.cone` file in a folder one module, and the corpus is full of folders
that are not modules: each of the suite's seventeen group directories holds a dozen
independent programs, several of which declare a `mod` of their own. The
designated-file convention is the only trigger that is also the rule the design
already states, and it is the same test at the entry point and at every subfolder.

From the designated file the walk collects, in this order: the designated file,
then the folder's other `.cone` files by name, then each subfolder's files, by
name and at any depth. **Every subfolder is organisational** — its files, however
deep, belong to the enclosing module, which is what lets a large module group its
files by topic without minting namespaces for them. A `.cone` extension is what
qualifies a file, so `.orig`, `.rej` and editor droppings never join.

⚠ **A subfolder holding its own designated file is a submodule, and that is not
built.** `parseCollectFolder` is where the walk would probe for
`<sub>/<sub>.cone` and recurse into it as a module of its own; until it does,
such a subfolder is organisational like any other and its files are absorbed.

Three conditions are diagnosed, and each names full paths, because the paths are
the only thing that tells the files apart:

| Condition | Code |
| --- | --- |
| A file another module already holds, brought into a second one — by the sweep, by `include`, or by an `import` naming the importing module's own file | `ErrorModFile` |
| Two files of one module sharing a basename, which leaves neither nameable | `ErrorDupFile` |
| Two of the module's files declaring one name | `ErrorDupName`, which is the ordinary namespace rule: a subfolder is not a namespace |

A file that cannot join is dropped rather than parsed, and the module goes on
with the rest of its files.

### The `mod` declaration

`parseModuleDcl` parses `mod name;`. **What it does with the name depends on
whether a folder already supplied one.**

- **A folder module's name is its folder's**, bound into the module's own
  namespace at load, before any of its files is parsed. A name written in the
  declaration is **checked** against it and may not replace it (`ErrorModName`):
  a name on the filesystem is one a tool that cannot parse Cone can read off a
  path, and a declaration that disagreed would be a second answer.
- **A module that is one file is named after that file**, and its declaration
  still renames it and binds the new name. That is transitional, and it is what
  keeps today's programs working.

Either way the bound name is duplicate-checked against the module's declarations
like any other, and is in reach inside the module for the rest of the parse.

**The declaration is the designated file's first statement, and a module declares
itself once** (`ErrorModDcl` otherwise). It claims the module, so nothing may
precede it; a file the folder swept in declares nothing, and neither does an
*included* file, whose declarations join the including module —
`parseGlobalStmts` is told which of the two it is reading.

**Two shapes are admitted and unbuilt, each reported where it is written and its
body skipped whole** (`ErrorUnbuiltKind`): a nested `mod name { ... }` block,
which needs a namespace of its own, hook push and pop around its parse, and paths
reaching through it; and `mod trait`, a module's abstraction, whose spelling is
settled by `trait` being a modifier on the kind.

**A `mod` declaration in the root file names the module and does not change a
single symbol.** The root still has no `DclNamesChain`, so its declarations stay
bare and `main` stays linkable. Naming the module is what makes its hidden names
reachable; what spells a symbol is the owner chain, and that is a separate
question — see "Consequences that follow whichever way those go" below.

⚠ **Filename naming is transitional, and is what a folder replaces.** A module
that is one file is named after that file, which is what lets today's programs go
on working unchanged. A module drawn out of a folder is named after the folder,
and nothing about its filename is a name.

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

- `import stdio.*` emits `stdio.print` **and definitions** for
  `stdio.IOStream.appendStr` and its siblings, all internal. The multi-module
  generation path works, and is exercised on every compile that prints.
- Importing an ordinary module emits **only `declare`s** —
  `declare i64 @_CNvC9modulesub8scaleInt(i64)`, read `modulesub.scaleInt` —
  because its bodies are never reached.
  Measured, that is the module's *public* surface whether or not the importer
  calls it: a public function nothing references is still declared, and a private
  one is not declared at all. So what an import contributes today is already the
  shape of a `.h` file, derived from the imported source rather than from a
  reduced artifact.
- Compiling that same module as the root emits
  `define internal i64 @scaleInt(i64 %0)`, bare, because the root contributes
  no name to the owner chain its declarations are spelled from.

So **a symbol's identity depends on which compilation the module was the root
of**, and the two spellings never resolve against each other. That, and not the
declarations, is why an import cannot be linked against: nothing can emit the
definitions those declarations name. How a symbol is spelled from its
declaration, and the linkage it gets, is
[Names and Namespaces](../phases/names-and-namespaces.md), "Symbols".

`parseImport` derives the *file's* name through `fileName`, accepts a period only
when `*` follows it — anything else after it is `ErrorBadTerm`, since selective
import is unbuilt — and binds the loaded module into the importing module's
namespace with `modAddNamedNode`, under the loaded module's `namesym`. So a module
drawn out of a folder is bound and pathed through by the folder's name, and the
file the import happened to name is only where the module was found. **An import
resolving to a file of the importing module's own folder is `ErrorModFile`**: the
folder already brought that file in, and a module does not import itself.

`parseInclude` locates the named file, registers it to the *current* module and
parses its global statements into that module. It builds no node, creates no
namespace, and leaves no record that it happened beyond the registry entry — which
is what makes including a file twice, or a file another module holds, an error
rather than a pile of duplicate names.

## Name resolution

`modNameRes` hooks the module's namespace, then runs four passes over it and
unhooks: `imports`, then every global carrying a `use` clause, then every type
alias, then everything else. **The first three go first for one reason** — a name
folded in, and a name a `typedef` binds, must be in place before any declaration
that uses it is resolved, and a module's names do not depend on the order they
were written in. Each pass leaves its own nodes out of the last one, so nothing
is resolved twice.

**A global's `use` clause is a module's second fold**, and the whole of it is
`foldGlobalExpand` (`ir/stmt/fold.c`): the global's type supplies the members,
every entry is an `AliasDclNode` carrying the global as its `through`, and a use
of the name is lowered to `global.name`. The rules, the diagnostics and why it is
cheaper than a field's fold are in
[Names and Namespaces](../phases/names-and-namespaces.md), "Folding through a
global", which owns them.

**A type alias's target is resolved in a pass of its own, and then checked for a
cycle.** A forward reference to a `typedef` is ordinary, so the target has to be
bound before anything asks whether the name is a type at all; `aliasDclCheckCycle`
then reports a chain that comes back to itself and cuts it, so no later walk
loops.

`importNameRes` does nothing unless `foldall` is set. When it is, it walks the
source module's `nodes`, skips anything unnamed or private, and calls
`modAddNamedNode` on the target. **The fold binds the original declaration node
into a second namespace**: one node, two bindings, and nothing in the receiving
module recording where the name came from. A different local spelling is
therefore not expressible, which is why renaming and selective folding are
described in [Names and Namespaces](../phases/names-and-namespaces.md) and are
not implemented.

Private is the declaration's `DclPrivate` bit, asked through `inodeIsPrivate`. An overload name
folds as one node, the `FnOverloadDclNode`, with its candidates riding inside
it; a public name holds only public candidates (`ErrorPrivOverload`), so the
fold carries nothing private.

## Type check

`modTypeCheck` type checks the imported modules first, then every declaration
the module owns, in source order. As everywhere in this phase, **order decides
when a declaration is checked, not whether** — a name reached from elsewhere
pulls its declaration forward. See [Type Check Phase](../phases/type-check.md).

**Nothing detects an import cycle.** Reuse by file in the registry stops the
parser recursing forever, but no phase asserts that module dependencies form a
DAG.

## Flow and generation

Flow analysis has no module concept; it runs per function body.

`genlProgram` is two strict passes over `pgm->modules`:

1. **Symbols.** Every module, generating or not. A declaration is skipped only
   when it is private *and* its module is not generating.
2. **Implementations.** Only modules flagged `FlagGenMod`.

`ImportTag` is an explicit no-op in `genlGlobalImpl`. `genlLinkage` makes every
definition of a program internal except `main` and a C-style name, and leaves
an imported module's declarations external; the only place generation
anticipates more than one Cone object file is the comment saying a package
compile will make an instance of a generic and a vtable `linkonce`.

The privacy filter in pass 1 assumes nothing outside a module can reach its
private names, and a public overload name cannot break that assumption: a
private candidate may not join one (`ErrorPrivOverload`), so an
`FnOverloadDclNode` generates nothing of its own, and each candidate is
generated as the module's or type's node it also is.

## Principles — the model, as decided

**This section is this note's principles**, and it is `[planned]` almost
throughout — "What is implemented" below says how little of it exists. ▸ **So a
reader should take every statement here as ruling what gets built, not as
describing what runs.**

⚠ **Two of the sources below are superseded and neither says so.** *When Modules
Are Not Just Namespaces* (2022) states that modules are single-level, that every
package *is* a module, and that folding happens at source-file level. **The model
here has modules nesting within a package and folding accumulating into one
module namespace with no file scope at all.** The post is still the only public
statement of Cone's module design.

The argument is in the author's *When Modules Are Not Just Namespaces*
(`c:/src/progling/content/post/cone-modules.md`) and *Modules vs Types*
(`modules-vs-types.md`); what modularity is for — the six strategies and which
layers surface them — is in [Modularity](../topics/modularity.md), which
**defers the whole of Cone's specific module design to this note**.

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
- **A module's name is its own, and reaches its own hidden names.** There is no
  root anchor and no parent access: a module reaches an upper module by importing
  and naming it, and a name of its own that a local or a type member hides by
  qualifying it with the module's name. **A program's root may carry a `mod`
  header for exactly that reason** — to have a name — and naming the root changes
  no symbol.
- **Namespace machinery is meant to be common to modules and types** — nesting,
  generics, interfaces and name folding, so that the layers look alike rather
  than each inventing its own.

**What separates a module from a type is instantiability, not state and not
namespace.** A type can be instantiated: a value of it may live anywhere in
memory, and its methods reach that value through `self`. A module cannot: it
exists once, its state is gathered by the link editor and reached at a fixed
address, and its functions take no `self`. Singleton state is not what tells
them apart, because a type holds it too — a `static` in a type is one copy
shared by every value of the type, exactly as a module's globals are one copy
shared by everything in the module — and a generic module will hold one copy
per instantiation as a generic type's instances each hold their own. That
distinction is what makes a module the natural shape for a region or a
subsystem and a type the natural shape for a value, and it is why a module
cannot be nested inside a type. A module holding a single type is therefore
not a special construct — it is a type sitting at the package's top level.

### Source files and folders

A module's source files are the files of a folder, and the folder tree carries
the module tree:

- **A module folder holds one designated file, named for the folder** —
  `matrix/matrix.cone`. It alone carries the `mod` declaration. Every other
  `.cone` file in the folder belongs to that module.
- **A subfolder is a submodule when it holds its own designated file** `[planned]`,
  and organizational otherwise. An organizational folder's files, at
  any depth beneath it, belong to the enclosing module. This is what lets a
  forty-file module group its files by topic without minting namespaces for
  them.
- **A module folder must be a direct child of its parent module's folder**
  `[planned]`. So the module tree's *shape* mirrors the folder tree's. A
  designated file found beneath an organizational folder is an error, not a
  deeper submodule.
- **A module's name is its folder's name**, and a name written in its `mod`
  declaration is checked against that rather than replacing it. The boundary is
  declared in code — a folder is a module because it holds a designated file —
  but the name is a filesystem fact, readable by a tool that cannot parse Cone.
- **The compiler is pointed at one file** and walks outward: the folder's other
  files join the module, subfolders are probed for their designated file. It
  needs no package concept to do this. The layout convention that a package's top
  module lives in `src/` as `<package>.cone` is congo's, and the compiler never
  sees it.

Collisions follow, and each wants a diagnostic that names full paths: two
organizational subfolders can each declare the same name into the enclosing
module, two files of one module can share a basename, and two sibling module
folders can declare the same module name `[planned]`.

### Composing packages

- **`import` names a package** — a name resolved through the search path, never
  a file path — and its declarations arrive **externally supplied**: declared
  for the linker, never defined by this compile. That subsumes `extern` for Cone
  packages; the importer writes no `extern` keyword.
- **What monomorphizes or expands at the use site is the exception, and the
  importer defines it.** A generic instance, an expanded macro, an `inline` body
  and a cloned trait default have no definition in the imported package to link
  against, because the package cannot know which ones exist — the same reason
  the interface artifact below cannot hold signatures alone. So an importing
  compile does emit definitions for those, and `LLVMLinkOnceAnyLinkage` is what
  lets several importers each emit the same one. This is the whole of the
  exception: it does not extend to anything the imported package could have
  emitted itself.
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
  import opengl use setColor, sub.* except green
  use matrix.*
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

- **A declaration is private to its module unless `pub`**, always — private to
  the module, not to the package. A nested module neither sees its parent's
  private names nor exposes its own to it, so nesting is a real boundary. An
  organizational subfolder is how files are grouped *without* erecting one,
  which is why the boundary needs no escape hatch: no one is forced to nest for
  layout reasons.
- **A submodule is private to its parent unless `pub`** `[planned]`, which is
  how a package keeps internals internal without a second visibility level.
- **A folded or imported name is private to the module that folded it**,
  whatever its visibility at the origin. `import B use c as d` binds both `B` and
  `d` in A, and neither is reachable as `A.B` or `A.d`. `pub` opts in:
  `import pub B use pub c as d` `[planned]` — **built for a global's clause,
  `config Config use pub *`, and not yet for an import's.** A module's public
  surface is therefore what it declares and deliberately re-exports, never what
  it happens to depend on.
- **Folding never widens visibility beyond the origin** — only a pub name can
  be folded at all, so no chain of re-exports can escalate.
- **`pub` has one meaning, on a declaration and on a binding alike: this entry
  is visible outside this namespace.** A declaration and a fold are both entries
  in a namespace, so re-export is `pub` on an imported entry and needs no form
  of its own.

**Visibility is a bit on the binding, and every check reads it.** The `pub`
keyword writes that bit once, when a declared name's binding is built, and is
never consulted again. It could not be otherwise: the binding for `B` inside A
must be private while `B` is a public package in its own right, and nothing in
the spelling of either says so.

**The binding chain has two ends, and different questions want different ones.**

- **Access** is decided by the binding the caller *traversed*: X reaching `A.T`
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

**Little of it.** **A module spans a folder's files**, found by the walk from the
designated file the compiler is given, and named for the folder; `mod name;` as
that file's first statement declares the module and is checked against the
folder's name. **Submodules are not built**: a subfolder's files join the
enclosing module whether or not it holds a designated file of its own, so the
module tree is one level deep and nothing enforces a module folder being a direct
child. There is no nesting within a file either — a `mod name { ... }` block is
`ErrorUnbuiltKind` — no package, no manifest and no interface artifact;
`mod trait` holds the spelling of a module's abstraction against the day there is
something behind it; `import` takes a file path rather than a package name, folds
only with `.*`, and cannot rename or exclude. A module that is one file is still
named after that file, and its declaration still renames it. Sections and COMDATs
are not emitted per function. What does work is the multi-module *generation*
path, exercised by `stdio` on every compile that prints, and folding into a
single module namespace, which is what the accumulation rule above asks for.

**`use` exists at one of its sites.** A module's **global** carries the clause
whole — `*`, a list, `as`, `but`, a block form, and `use pub` — so the grammar
and the binding record are built and proved; what is missing is `use` as a clause
of `import` and standing alone against an imported module, which is the wiring
rather than the design. See "Folding through a global" in
[Names and Namespaces](../phases/names-and-namespaces.md).

**A binding has a visibility bit where a fold made it, and none where an import
did.** A declaration has one — `DclPrivate`, written from the absence of `pub`
when it joins its namespace, and what every visibility check reads through
`inodeIsPrivate`. A global's fold makes an `AliasDclNode`, which carries its own
`FlagPub` from `use pub`, so a fold is private to the module that made it unless
the clause says otherwise, and `fnCallNameResPath` enforces that from outside.
**An import still has nowhere to record one**, because `importNameRes` inserts
the imported declaration node itself into the receiving namespace.

**Whether a fold transits is still decided by load order**, and a fold's bindings
are not carried by a wildcard import at all, since `importNameRes` walks `nodes`
and a fold writes to `namespace`.

Measured: `modNameRes` folds a module's imports at the start of *that module's*
resolution and `pgmNameRes` walks modules in load order, so a fold is invisible
to modules resolved earlier and visible to those resolved later. A root module
naming `mid.plain`, where `plain` was folded into `mid`, is rejected as an
unknown name; the same reference from a sibling module loaded after the folding
one compiles. A type resolved by demand ahead of its module — a trait the root
declares is-a against, say — does not move this line: `structNameResDemand` hooks what the
module's imports will fold for the trait's own bodies to see, and folds nothing.

## What the model has not decided

Each of these is a question the current mechanism answers by accident, or that
two documents answer differently. They are recorded here so that work in this
area starts from what is actually open.

### The idiom for reaching a package's members

A package named after the thing it provides puts that name in every path twice —
`bigint.BigInt`. This is not an edge case: a module with no global state is
pure namespace, and a great deal of library code needs none, so single-type
packages will be common.

Four answers are available, and none is chosen:

- **Fold at import.** `import bigint.BigInt`, then write `BigInt`. Needs
  nothing beyond selective folding, and is what Rust does with `use`.
- **Name the top-level module independently of the package's distribution
  name**, so what you install and what you path through need not match.
- **Convention.** Name the package for the domain and the type for the thing, so
  the repetition never arises — `math3d.Point3`, Go's `bytes.Buffer`.
- **A shortcut rule**: a member whose name matches its module is reachable by
  the module name alone.

Prior art splits. Go accepts `time.Time` and tunes names so the qualified form
reads well; Rust accepts `regex.Regex` and leans on `use`; Python's
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

- **Linkage for what a program does not export** is settled and built: every
  definition of a program compile is `LLVMInternalLinkage`, in `genlLinkage`,
  except `main` and a C-style name, and nothing is `hidden`. Hidden visibility
  would keep a symbol out of a shared library's export table but leave it a
  global symbol at static link, so it could still collide; internal linkage
  makes it object-local and collision-proof. The hazard that distinguishes them
  is silent: were a program's `fn log(...)` emitted as an external `@log`, a
  package calling libm's `log` would have its `declare` satisfied from the
  program and `log.o` never pulled — the wrong function with no diagnostic.
- **Whether private names in a *library* also become internal.** It shrinks the
  mangled namespace to exactly the names that cross a package boundary. It also
  means a private helper reached from a public inline or generic body must be
  re-emitted per importer rather than linked against, which is the package
  compile's `linkonce` rule for generic instances (L2).
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

- *Region Modules* (`c:/src/progling/content/post/region-modules.md`) says a
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
  measured asymmetry above is the mechanism. A generated name is the module
  path, outermost first, and there is no package component: a package
  correlates to one top-level module, whose name is what makes a public name
  distinguishable once the linker flattens every namespace into one. The rules,
  and what of the spelling is still open, are
  [Names and Namespaces](../phases/names-and-namespaces.md), "Symbols".
- **The interface artifact must carry bodies, not signatures.** Generics
  monomorphize at the use site, macros expand at the use site, and `inline` is
  macro-shaped, so an importer needs the body of each. It exposes private
  declarations that a public generic or inline body calls — the one place a
  private declaration is needed from outside its module, since a public
  overload name may hold no private candidate.

  **The format is Cone source, not serialized IR.** `[planned]` **Decided by the
  author, 12 September 2026:** the artifact is **auto-generated**, with
  **hand-written as a transitional stage** — and nobody hand-writes serialized
  IR, so the format is the language itself. ▸ **This is also what makes a C
  library's package and a generated package interface one artifact with one
  loading path**, which is what that decision requires. Emitting it needs a
  printer producing valid Cone rather than the `--ir` debug dump; the prior art
  is Swift's textual `.swiftinterface`, chosen for the same reason — a module
  built by one compiler version stays readable by a later one.

  ⚠ **This paragraph previously read "the artifact is therefore serialized IR."**
  That was stated here and contradicted in the packages backlog item, with
  nothing saying which won.
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
  [Modularity](../topics/modularity.md) states the aim and measures the
  distance.

## Hazards

- **`include` and `import` look alike and are not.** One injects declarations
  into the current module and leaves no trace but a registry entry; the other
  builds a namespace.
- **The registry's key is the path as it was spelled, not a canonical one.** Two
  spellings of one file — a backslash path and a forward-slash one, `a/../b` and
  `b` — are two keys, so the file would be read twice and declare everything
  twice. Every path the compiler composes comes from `fileSrcUrl` or from the
  sweep, so the spellings agree in practice; the one that does not is the command
  line's.
- **A file named with no folder in front of it takes its folder's name from the
  current directory.** `conec matrix.cone` run from inside `matrix/` sweeps, as
  `conec matrix/matrix.cone` and `conec matrix` do: a file's module may not depend
  on the spelling of the path used to reach it. Where the current directory cannot
  be read the file is a module of one file.
- **A module collision is reported at the wrong place.** A `ModuleNode` is built
  while the lexer sits on the token after the `import` that loaded it, so
  `ErrorDupName` against a module — a file declaring `mod x` that imports a module
  also called `x`, say — points at the next declaration and at the injected
  pseudo-file rather than at either module. The condition is diagnosed; the
  position is not useful.
- **A cycle among non-root modules is fine.** Name resolution runs after all
  parsing, so the half-parsed module the registry returns is complete before
  anything reads it. Nothing detects a cycle, and nothing needs to.
- **`FlagGenMod` is decided by a `strcmp` on the filename.** A user module named
  `stdio` would have its bodies generated.
- **A folded name is the same node in two namespaces.** Mutating a declaration
  through one binding is visible through the other, and the receiving module
  keeps no origin link.
- **`corelib` and `stdio` are C string literals.** A syntax error in either is
  reported against an injected pseudo-file, and editing either means rebuilding
  the compiler.
- **A use of a module's name answers `isTypeNode` true** — `nameUseGroup`'s
  fallthrough for every declaration that is not a value, a macro or a generic
  parameter, not because a module is a type.

## What lives elsewhere

- The name rules a module implements — lookup, qualification, visibility,
  folding, aliases, overloading: [Names and Namespaces](../phases/names-and-namespaces.md)
- What modularity is for, and how far Cone is from it: [Modularity](../topics/modularity.md)
- Module loading as a parse-time activity, and the name-table hook:
  [Parse](../phases/parse.md)
- How a declaration's symbol is spelled and what linkage it gets:
  [Names and Namespaces](../phases/names-and-namespaces.md), "Symbols"
- The lowering of those rules, linkage, COMDATs, and the allocation header:
  [Generation](../phases/generation.md)
- Mixins, trait inheritance, and types as namespaces: [struct](struct.md)
- Instantiation, cloning and memonodes: [generic](generic.md)
