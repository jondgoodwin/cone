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
checked against that name. **A subfolder holding its own designated file draws a
submodule** (`parseSubmodule`), so the folder tree carries the module tree: the
submodule is a module like any other, owned by the module whose folder holds it,
bound in its namespace, and reached from it by path.
Name resolution folds imports before it resolves anything else the
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
the module that holds it. **Every module of the program is in that one flat list,
however deep in the tree it sits**, which is what gives each of them name
resolution, type check and generation exactly once. The tree is not in the list:
it is a child's `dclinfo.owner` pointing at its parent, and the parent's
`namespace` holding the child's name.

**The registry is what makes a file read once and owned once.** Its key is the
**canonical** path rather than a name, because what must happen exactly once is
the *reading*: two folders may hold files of one basename, and a module's
declared name is not known until its file has been read. `fileCanonicalPath`
gives one spelling — separators as `/`, a `.` segment dropped, a `..` segment
cancelled against the one in front of it — so that a path a source writes to walk
somewhere and back finds the entry the folder sweep made rather than missing it
and reading the file a second time. A built-in module is a string inside the
compiler rather than a file, and stands in the registry under the pseudo-file
name its diagnostics are reported against — `corelib`, `stdio`.

**`ModuleNode`**

| Field | Meaning |
| --- | --- |
| `namesym` | the module's name: its **folder's**, where a designated file drew the module out of a folder, and `filesym` otherwise. What an importer binds it under, what a path through it is written with, and what its declarations' symbols are spelled after |
| `filesym` | the name derived from the module's *filename* — the source file's basename for the root, the imported file's for every other. It names a module that is one file, and nothing else reads it |
| `foldersym` | the module's folder, when that folder's designated file drew it; NULL for a module that is one file. It is what a `mod` declaration's name is checked against, and what says a folder was swept |
| `dclinfo` | the declaration facts — [Names and Namespaces](../phases/names-and-namespaces.md), "Symbols". `owner` is **the parent module for a submodule**, and NULL for the root, for a module that is one file and for one an `import` reached. **The root is the module without `DclNamesChain`**: it has a name and contributes it to no symbol. `DclPrivate` is set on a submodule that does not write `pub`, and on no other module, because a module with no parent has nothing to be visible outside of |
| `imports` | `ImportNode`s only, held apart from `nodes` so folding can run before anything else resolves |
| `moduses` | `ModUseNode`s only — every standalone `use` written at module scope, of an enum or of a submodule — held apart from `nodes` for the same reason, and because the statement is neither a declaration nor a field: what it declares is bindings, made in the fold pass |
| `nodes` | every declaration the module owns, in source order, **an enum's variants among them**: a variant is walked, checked and generated as the module's, though its name is bound in its enum. This is what printing and generation iterate |
| `namespace` | every name *visible* in the module: what it declares, **every declaration and fold of the module it extends — not the names that module's imports bind — each an `AliasDclNode` as visible here as it is there**, **the module an import bound and every name an import folded in, each an `AliasDclNode` carrying the import's own visibility**, what a global's `use` clause folded in, **the variants a `use` of an enum folded in**, **the names a `use` of a submodule folded in, each an `AliasDclNode` carrying the statement's visibility**, **each submodule its subfolders drew**, and — when a folder or a `mod` declaration named it — the module's own name. **Not an enum's variants by themselves**: those are names of the enum |
| `flags` | `FlagGenMod`; `FlagModDcl` for a module a `mod` declaration named; `FlagPub` for a submodule its declaration opened |
| `foldpass`, `folding` | the fold pass (`modFoldAll`) that last reached the module, and whether its folds are running in it. A module reached again while *folding* has closed a cycle: it is read as far as it has got, and the passes go on until they settle |
| `extendsname` | `mod A extends B`: B as written, a `NameUseNode` bound to the module once `modExtendsResolve` finds it; NULL where the module extends nothing |
| `extends` | the fold `extends` makes: an `ImportNode` marked `isextends`, whose module is B and whose clause is a star clause over B's declarations and folds, private ones included, but not the names B's imports bind to their modules, made once B resolves. **Not on `imports`** — it binds no name of its own — and folded first by `modFoldNames` |

**A module's own name is in its own namespace, and that is what makes a hidden
module-level name reachable.** `mymod.x` reaches an `x` that a local or a type
member hides, because a path begins with the name of the namespace it walks and
this is the only name a module has. The binding is not in `nodes`, so nothing
prints, generates or folds the module into itself.

**`nodes` and `namespace` are not the same set, and the difference is exactly
where folding lives.** A folded name is added to `namespace` and never to
`nodes`, so the receiving module can resolve it but does not own, print or
generate it. That holds for all four folds a module has — what it extends, an
import's `use` clause, a global's, and a standalone `use` of an enum or a
submodule. **A fold transits**, because `importNameRes` reads the
source module's `namespace`: it carries across every *public* binding, whether
the source declared that name or folded it in.

`ModuleNode` extends `IExpNodeHdr` and so carries a `vtype` slot, but
`ModuleTag` is a named node in `StmtGroup`: `isExpNode` is false,
`newModuleNode` never sets `vtype`, and nothing reads it.

**`ImportNode`** holds `module` — the `ModuleNode` it binds — `fold`, the
`FoldClause` of its `use` clause, or NULL where the import folds nothing, and
`ispub`, the visibility of the module's own binding. The clause is a global's
exactly, parsed by `parseFoldClause`: `*`, `* but`, a list with `as`, a block, and
`pub use`. The clause is the one spelling of a fold [Jon 23 Sep]: a period after
the module is refused (below). **Two `pub`s, and they do not overlap:**
`pub` before the statement sets the node's `ispub` and the clause's, so it makes
every binding the import creates public — the module's own name and each fold —
where `pub use` sets only the clause's. `pub import m pub use …` therefore says
what `pub import m use …` says, and is accepted as the same import.

**A module imports another once.** `parseImport` finds a prior import of the same
module among the module's `imports`, whatever file of the module wrote it, and
asks `importSame`: the same `ispub` on each binding, and the same clause — star or
not, the same `but` names, or the same listed names under the same spellings, in
any order. Either way the second import is `ErrorDupImport`, reported at the
second and naming the file and line of the first; `importSame` decides only what
the message says, the same import again or two that disagree. An identical
repeat was once dropped without a word; it is two ways of bringing in the same
thing, "a cleanliness issue", and is refused [Jon 23 Sep]. This is the
parse-time face of the rule the folds follow — a name the module writes twice is
an error — and it has to be decided here because the import binds the module's
own name at parse, before any fold runs.

**`ModUseNode`** (`ir/stmt/fold.h`) is a module's standalone `use`, `use Colors;`
or `use scaling;`: `source`, the enum or submodule as written — a name or a path,
resolved only when the fold is expanded, which is when the two part ways — and
`fold`, the `FoldClause` of what it admits, parsed by the grammar a type body's
sibling `use` has (`parseModUse`). Its `ispub` comes from the `pub` before the
statement, `pub use Colors;`, read by `parseGlobalStmts` with every other
declaration's and passed in: `pub` comes first, before the keyword of what it
makes public. The retired `use pub` is `ErrorBadPub`, naming the spelling, and
then taken as meant. `modfold` is NULL until the source resolves to a submodule;
then it is the fold over it, an `ImportNode` marked `isuse` whose `fold` is this
node's own clause (`foldModUseModule`).

## Constructors

| Function | Note |
| --- | --- |
| `newProgramNode` | one per compile, with an empty file registry |
| `pgmAddMod` | appends a module and takes its flags. The caller sets `filesym`, `foldersym` and `namesym` afterwards |
| `pgmFindFile` / `pgmSetFile` | the file registry, by path. **This is what makes a file read once** however many modules name it, and what makes it belong to one module |
| `newModuleNode` | `namesym`, `filesym` and `foldersym` NULL, `dclinfo` cleared, empty `imports`, `nodes` and `namespace` |
| `newImportNode` | `module` NULL, `fold` NULL |
| `newModUseNode` | `source` NULL, an empty `fold` positioned at the `use`, `modfold` NULL |

## Parse

`parsePgm` establishes the program in an order that matters:

1. The root `ModuleNode` is added first and flagged `FlagGenMod`. It is not given
   `DclNamesChain`, so it prefixes nothing.
2. The main source file is located, and every file its folder sweeps in is
   registered to the root — before anything is parsed, so that which files the
   root holds does not depend on what a parse of one of them imports. An import
   cycle back to any of them then finds the root in the registry.
3. `corelib` is parsed, from the `corelibSource` string in `corelib.c`.
4. An `ImportNode` carrying a star clause is added to the root for `corelib`.
5. The submodules its subfolders designate are drawn, each recursively.
6. The root's own files are parsed, its designated file first.

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
  the only one that may declare the module. The auto-import of `corelib`, a star
  clause, is added first, and `modHook` swaps the name table once for the whole
  set — so a module neither sees nor collides with the names of the module whose
  parse reached it, parent or importer alike. Every declaration any of those files
  adds through `modAddNode` records the module as its owner.

**`parseModuleTree` is the last step of all three paths**: it draws the
submodules the module's subfolders designate — `parseSubmoduleDraw` for every one
of them, then `parseSubmoduleParse` for each, then the module's own files.
**Every sister is drawn before any is parsed**, which is the rule the files
follow one level down and holds for the same reason: a submodule importing a
sister resolves that name against this namespace, so a sister drawn later would
be a name that was not there — and her files would be unregistered, so a path
spelled to one of them would read it a second time. **The submodules come before
the module's own files** too, and both reasons are about what a name means before a file is read. A
subfolder's module is a name of the namespace that no statement in any of the
module's files declares — the folder is the declaration — so binding it ahead of
them makes the declaration the duplicate, and the declaration is where a
collision's first diagnostic lands. And it registers the submodule's files, so a
file of this module that names one of them reaches the module that holds it
rather than reading it a second time.

**A module is positioned at its designated file's first line** — line 1,
column 1 of `matrix/matrix.cone`, or of the one file of a module that is a file —
from the moment it is made, and `parseModulePosition` is what does it, on all
three paths. **Ruled by the author, 23 September 2026:** a module named only by
its folder has no line in any source that declares it, and the file that makes
the folder a module is the nearest thing it has to a declaration, so that is
where its half of a diagnostic — `ErrorDupName` against its name — is
reported. A `mod` declaration is nearer,
and moves the position to itself. **The file is read when the module is made**,
before any file of the tree is parsed, because the module's name is bound then
and a collision is reported as the second binding is made: a submodule named like
its parent meets the parent's own name at the draw. `lexLoadPath` reads it into a
lexer block that is not yet current, and that block is the one
`parseModuleFilesParse` makes current when the designated file's turn comes, so
the file is still read once. A built-in module has no file, and keeps wherever
the lexer stood when it was made.

### The folder tree

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
then the folder's other `.cone` files by name, then each **organisational**
subfolder's files, by name and at any depth. A `.cone` extension is what
qualifies a file, so `.orig`, `.rej` and editor droppings never join.

**The folder is the only thing that brings a file into a module.** A file joins
by where it sits, and nothing written in one file brings another in — which is
what lets a tool handed one file tell its module from the path. `include`, which
did bring a named file in, is retired: the word stays a keyword, and the statement
is `ErrorInclude` at the word and skipped, the file it names not looked for.

**Every subfolder gets the same probe**, and what it answers is the whole of the
distinction:

- **A subfolder holding its own designated file is a SUBMODULE.** The sweep stops
  there and leaves that folder's files to the module it draws, which sweeps its
  own folder by the same walk. So the folder tree carries the module tree.
- **Any other subfolder is ORGANISATIONAL**: its files, however deep, belong to
  the enclosing module, which is what lets a large module group its files by topic
  without minting namespaces for them.

**A module folder must be a direct child of its parent module's folder**, so the
module tree's shape mirrors the folder tree's. A designated file beneath an
organisational folder is therefore refused (`ErrorModFolder`) rather than drawing
a module deeper than the shape allows; the folder holding it stays organisational,
so that file is swept into the enclosing module like any other and the compile
goes on with a coherent file set. A `mod` declaration in it is then `ErrorModDcl`,
because a swept file declares nothing — the refusal's consequence, and the shape
of what was wrong.

Four conditions are diagnosed, and each names full paths, because the paths are
the only thing that tells the files apart:

| Condition | Code |
| --- | --- |
| A file another module already holds, brought into a second one — by the sweep, or by an `import` naming the importing module's own file or its own submodule | `ErrorModFile` |
| Two files of one module sharing a basename, which leaves neither nameable | `ErrorDupFile` |
| A designated file beneath an organisational folder, too deep to draw a module | `ErrorModFolder` |
| An `import` reaching a module inside a tree by a path to its file, or naming the module that contains it | `ErrorModReach` |
| Two of the module's files declaring one name, or a file declaring the name a subfolder's module already has | `ErrorDupName`, which is the ordinary namespace rule: a subfolder is a namespace exactly when it draws a module, and an organisational one never is |

▸ **The collision between two sibling module folders needs no diagnostic, because
it cannot be written.** A module's name is its folder's, and a filesystem gives
two children of one folder two names — so what is left of that collision is a
submodule's name meeting another name of the parent, which is `ErrorDupName` like
any other duplicate.

A file that cannot join is dropped rather than parsed, and the module goes on
with the rest of its files.

### What a submodule is, and what it is not

`parseSubmodule` draws one, and it is a module in every respect — its own
namespace, its own folder sweep, its own subfolders, its own entry in the
program's module list, and its own turn at every later phase. **Two things make it
a child rather than a neighbour, and they are the whole of the difference:**

- **Its parent owns it** (`dclinfo.owner`), so `namePath` spells its declarations
  after the parent's name and a submodule of a submodule after both — `sub.fn`,
  `sub.inner.fn`. That needed no new spelling: the encoding already had a case for
  a module whose owner is a module.
- **It is private to its parent unless its declaration writes `pub`**, which
  `dclInfoJoin` writes as `DclPrivate` and `pub mod sub;` clears. Its own
  declaration is the only place that can be written, because the parent declares
  nothing about a subfolder.

**A parent reaches into it by path, and that is the ordinary rule and no new
lookup code.** `sub.name` is a bare name that resolves to the submodule — a name
of the parent's own namespace — followed by the hop `fnCallNameResPath` takes
through any module's namespace, with the privacy check it already made: the
qualifying module is not the asking one, so a private name of the submodule is
`ErrorNotPublic`. The same check refuses a grandparent naming a submodule that is
not `pub`.

**A submodule reaches SIDEWAYS by importing a sister's name, and the registry
that name is resolved against is its parent's namespace.** A module is the
registry for its children: they are public to each other and to it, invisible
outside it unless it publishes them. So `import wheels` inside a submodule is a
*lookup* — the sweep has already drawn wheels, nothing is loaded, no path is
composed — and it binds her name here as an alias, under the import's own
visibility. It is the mechanism that will reach an external package, asking a
different registry: one lookup, two kinds of neighbour.

**And it reaches UP by importing a public name of its parent** [Jon 23 Sep].
The registry is the parent's namespace, so it holds more than modules: what the
parent declares, what its imports bound and what its folds brought in. `import
Point` inside a submodule binds the parent's public `Point` — a type, a function,
a global — as an alias exactly as `import wheels` binds a sister. A private name
of the parent is `ErrorNotPublic`, as a private submodule is outside its parent,
so `pub` shares a name downward as well as outward. A sister and a declaration
of the parent cannot share a name, being names of one namespace, so the import
is never ambiguous.

**Nothing arrives unasked.** A module's namespace holds what it declares, what a
fold brought in, its own children and what its imports bound. A sister nobody
imported is `ErrorUnkName`, which is the dependency being stated rather than
handed, and so is a name of the parent nobody imported: a bare name does not
climb.

⚠ **The registry is SCOPED, not accumulating, and this is provisional.** It is
the *immediate* parent's namespace and no ancestor's, so descending a level drops
the level above out of reach: a module two deep does not see its parent's
sisters, and a parent re-exports what its children need. That keeps a module
liftable, because its dependencies are stated at its own `mod`. The reading was
adopted provisionally and is to be revisited — it is decision 10 of the module
design brief, and what would settle it is a case where a re-export is pure
ceremony.

🛑 **A module may not name its parent** (`ErrorModReach`): that is a reference back
along the edge that contains it, which is the one shape the tree rules out, and
it is not needed, because what the parent published is already in the registry
the child reads, and each name of it is imported by that name.

🛑 **And a neighbour may not be reached by a path to her file** (`ErrorModReach`).
A path spelled through a folder and back out of it arrives at a module the sweep
has already drawn, and the modules such a path can reach are exactly the ones the
scoped rule says must not resolve — which of them it hits would be an accident of
spelling. So a neighbour is reached because the registry holds her name, never
because a path arrived at her. Any module with an owner is refused this way; a
path reaches a module with no parent, which is what an external module is today.

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
precede it, and a file the folder swept in declares nothing —
`parseGlobalStmts` is told whether it is reading the designated file.

**`pub` on the declaration opens a submodule to its parent's neighbours**, and is
`ErrorBadPub` on any other module: the root, a module that is a file of its own
and one an `import` reached are each inside nothing, so there is nothing for `pub`
to open them to. It is the same `pub` every declaration carries, reaching the one
declaration that draws a module.

**`mod name extends base;` makes this a module that reuses another**, and the
declaration only records `base`, as a `NameUseNode` on `extendsname`: what it
names is resolved at name resolution, once every import is bound, so the
declaration may come before the import that puts the base in reach. One base, by
one name — a second after a comma is `ErrorExtends`, a path `ErrorModExtends`,
nothing at all `ErrorNoName` — and each refusal passes over what it refused, so
the declaration still names the module. What `extends` does is in "Name
resolution" below.

**`mod trait`, a module's abstraction, is admitted and unbuilt**, reported where
it is written and its body skipped whole (`ErrorUnbuiltKind`); its spelling is
settled by `trait` being a modifier on the kind. **A `mod name { ... }` block does
not exist**: a module is never declared inside a file, and nesting is by folders
only, a nested module being a subfolder with its own designated file. The parser
recognises the block only to say so, under the same code, and skips its body.

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

- `import stdio use *` emits `stdio.print` **and definitions** for
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

`parseImport` refuses a period after the module, `ErrorBadTerm` whose message
names the `use` clause: `.*` is a wildcard import, spelled `use *`, and `.name` a
selective one, spelled with a list. `.*` is read as the `use *` it means, or
passed over where a clause follows it, so one error is all it costs. It then
answers the name **in two places, the registry first**:

- **The registry.** Where the written name is a bare identifier and the importing
  module has a parent, `parseImportRegistry` looks the name up in the parent's
  namespace. A module found there is the answer, and nothing is located, read or
  registered. That is how a sister is reached.
- **The filesystem.** Otherwise the name is a path, and
  `parseLoadAndParseModuleFile` locates, registers and parses it. That is how an
  external module is reached today, and where a package name will be resolved.
  **A module the path reaches that has an owner is `ErrorModReach`** — see "What
  a submodule is" — and one resolving to a file of the importing module's own
  folder or to its own submodule is `ErrorModFile`.
- **The registry again, for a name that is not a module** [Jon 23 Sep]. A
  submodule is parsed before its parent's own files (`parseModuleTree`), so at
  parse the parent's namespace holds only its children and its own name; what
  the parent declares and what its imports bind are not there yet. So where the
  written name is a bare identifier, the importing module has a parent, and
  neither the registry nor a file answers it, `parseImportName` holds the import
  with an alias not yet bound (`binding`), and `importBindName` binds it in the
  fold passes, once the parent's own folds have run. Importing one name twice is
  refused at parse (`ErrorDupImport`), as a module imported twice is.

Either way the module is bound into the importing module's namespace under its
own `namesym`, as an `AliasDclNode` carrying the import's visibility
(`importBindModule`). So a module drawn out of a folder is bound and pathed
through by the folder's name, and the file an import happened to name is only
where the module was found. A name of the parent is bound under the name
written, as an alias of the same kind whose target is the parent's own binding
— see "Name resolution" below.

⚠ **So a file answers a bare name before a declaration of the parent does**,
which is the one place the registry-first order is not what decides: the file is
found at parse, the declaration only after. A file relative to a submodule is one
of its own files or its own submodule, both `ErrorModFile`, so what can arrive
this way is a module on the package search path (`--path`). Where the parent
answers the same name with something else, the import is `ErrorDupName` in the
pass that reports (`importCheckNamedFile`), rather than meaning whichever was
parsed first; where the parent binds the same module, the two are one.

## Name resolution

**Every module's FOLDS run before any module's body is resolved.** `pgmNameRes`
runs the folds of every module (`modFoldAll`), and only then `modNameRes` on
each. `modFoldNames` is dependency-first — for what the module extends and for
each import it folds the source module's own names first, recursively — so a module's folds are complete before
anything folds from it, which is what makes a re-export transit. Where the
imports form no cycle, one pass over the module list is the whole of it.

**A module's folds resolve in that module's names alone**, whichever module's
pass reached them. Dependency-first means one module's folds often run from
inside another's — a parent's `use` of its child, an import of a sister — and
`modHook` hooks the inner module's namespace *in place of* what is hooked, not
over it ([Name Resolution](../phases/name-resolution.md), "Hooking"), so a
`use Dir;`, a global's fold type and its value, and a type demanded from another
module see nothing of the module that got there first. That is "nothing arrives
unasked" holding inside the fold pass as it does in a body; `module-fold-scope-nameres`
pins it from a child and from a sister.

**Round a cycle of imports, the folds run again until they settle** [Jon 23
Sep]. Where A and B import each other, one of them reads the other while the
other's folds are running (`folding`), and a name the other re-exports is not
there yet; every module's own *declarations* are bound at parse, so what is
missing is a re-export and never a declaration. So `modFoldAll` repeats the pass
over the module list until one binds nothing new — a fixpoint, the way Rust
resolves glob imports — and the re-export arrives. What makes that cheap is
`modFoldBind`: two bindings of the same declaration under one name are one
binding, so a pass re-binding what an earlier pass bound changes nothing, and
"nothing new" is the whole test. A pass counts as new a name it bound and a
binding it made public or made a fold (`FlagImportName` cleared), since either
lets a module folding from this one take more. A namespace only grows and a
binding only becomes more visible, so the passes end. Another pass is run only
where a pass met a module mid-fold or left a fold waiting, and made progress: a
program whose imports form no cycle takes one pass, and a cycle typically three
(the first, the one that brings the late names, and one that finds nothing new).

**Until the passes settle, nothing a later pass might bring is reported
missing.** A fold that cannot be made yet WAITS (`modFoldWait`): a listed item
whose name the source has not got, or has only privately (a second route may yet
make the binding public), and a global's fold or a standalone `use` whose source —
its type, or the enum or submodule — is named through a binding not there yet
(`modFoldAwaits`, a lookup that resolves nothing and reports nothing). A star
clause needs no waiting: each pass reads its module afresh (`importFoldStar`).
The pass after the last REPORTS (`modFoldReporting`): it reads no module afresh,
and each fold still waiting is made or reported with the message it always had —
a listed name missing or private, a `but` naming what the module does not have
or does not show, an unknown type or enum. So a `but`, a listed name and an `as` are judged against
the module once it is complete, which is what a single pass over an acyclic
program judges them against.

**A collision is reported once, where a single pass would have met it.** Two
different declarations under one name is final the moment it is met — neither
binding ever leaves — so it is reported then, and a star clause keeps an item
under the name so that a later pass passes it over rather than reporting it
again (`importStarHas`; a listed item is made once). A pass round a cycle can bind
a later fold's name before an earlier fold's arrives, so the collision is
reported at the fold that comes second in the order a module's folds run —
`extends`, then the imports, the globals and the enum `use`s, each in the order
written — not at whichever arrived second (`modFoldCollisionAt`). Which pass met
it does not change what is reported.

**That order is what stops the file load order deciding what a name means.** A
module's folds used to run at the start of its own name resolution, and modules
are resolved in the order they were loaded — so a module resolved earlier, the
root among them, looked a folded name up before it was there, and one resolved
later found it.

**An import of a name of the parent is bound among the imports, in the order
written** (`importBindName`) [Jon 23 Sep]. It reads the parent's namespace, so
`modFoldNames` runs the parent's folds first, as it runs an imported module's:
what the parent re-exports is there to be taken. The binding is an alias to the
parent's own binding — the origin kept, and a global's fold reached through the
same global — under the import's visibility and marked `FlagImportName`, since it
states a dependency as a module's binding does, and a module extending this one
does not take it. It is WRITTEN, so it merges with a wildcard's arrival of the
same declaration and collides with anything else the module wrote under that
name: its own declaration (reported at the import) or a listed item
(`modFoldCollisionAt`, which places the import among the imports). A name the
parent has not got, or holds privately, WAITS like a listed item, and is
`ErrorUnkName` or `ErrorNotPublic` in the pass that reports. Where the parent's
answer is a module — one the parent imported and re-exported — it becomes the
import's `module`, and the import's `use` clause folds from it in the same pass
as any import's; a clause on anything else is `ErrorBadFold`, since a type's
members are reached through the type, not folded from it.

**Ahead of the folds, what every module's `extends` names is resolved**
(`modExtendsResolve`, then `modExtendsCheckCycle` over the whole list), so that
every edge the fold pass follows is known and a cycle of them is cut before the
first fold runs.

`modFoldNames` runs four kinds of fold, in order: what the module extends, then its `imports`,
then every global carrying a `use` clause, then every standalone `use` on
`moduses` — last, so an enum may be named through anything the others folded
in. What it extends goes first so that a name the base has and something of the
module's own also brings in is reported at what the module wrote. `modNameRes` then hooks the namespace, runs the type
alias pass and the everything-else pass, and unhooks. **The folds go first
for one reason** — a name folded in, and a name a `typedef` binds, must be in
place before any declaration that uses it is resolved, and a module's names do
not depend on the order they were written in. Each leaves its own nodes out
of the last pass, so nothing is resolved twice: a global's fold and an enum's
`use` are made once (`FoldClause.expanded`), however many fold passes run. A
`use` of a submodule resolves its source once too, and then runs its fold in
every pass, as an import's clause does.

**What a module extends is its first fold**, and it is an import's star fold:
`extends` is an `ImportNode` marked `isextends`, folded by `importNameRes` after
`modFoldNames` has run on the base, so a chain of `extends` transits exactly as a
re-export does. It differs from an import in four things. **It takes the base's
declarations and the base's folds, private ones included, and each alias is as
visible here as it is there** [Jon 23 Sep]: the module is inside its base's boundary, as an enriching
type is inside its base's, so its code reads the base's private names; a public
name of the base is public here, since what a module extends is part of its own
surface; and a private one stays private, so an importer of the module sees the
base's public surface and nothing more. `importStarAdmits` admits the private names
where the clause is an `extends`, and `importFoldItem` sets each alias's `FlagPub` from the
base's binding rather than from a clause. **It binds no name of its own**: the
base is a name here only if an import binds it. And **a name the base has may not
be redeclared**, public or private — a declaration of the module colliding with
one of the base's aliases is `ErrorExtendsOverride`, reported at the declaration,
the type rule; any other collision is `ErrorDupName`. A private name of the base
named from outside the module is refused as any private name of the module is:
`ErrorNotPublic` by path or in a listed clause, and passed over by a star clause.
And **it does not take what the base's imports bind to their modules** [Jon 23
Sep]. A module's imports are its dependencies, not its contents, and scoped
imports exist so that each module states its own: the base's `import c` binds `c`
for the base alone, and the module's code names `c` only by importing `c` itself
(`ErrorUnkName` otherwise); the base's `pub import c` does not put `c` in the
module's surface either. What the base's `use` clauses fold in — on an import, a
global or an enum — is a part of the base and does come across, each fold as
visible as it is there: the base's `import c use x` makes `x` a name of the
module, so a declaration of the module named `x` is `ErrorExtendsOverride`, and
one named `c` is not. The binding an import makes carries `FlagImportName`, which
`importStarAdmits` leaves out of an `extends`; where a fold of the base also
brought the same module in under that name (the base imports `c` and folds `c` in
by a wildcard of a module re-exporting it), `modFoldBind` clears the flag, since
the name is then a fold of the base too. Listing the name in that clause instead
writes it twice, and is `ErrorDupName` in the base [Jon 23 Sep]. The automatic
core import binds no module name — it is a `use *` fold alone — so the base's
core fold still arrives beside the module's own: the same declarations, which
neither module wrote, so one binding each (below).
**What it may name** — a module already in
reach, looked up in this module's namespace and then in the registry its parent
is, never loaded; not the module itself, one it contains, its parent, a trait or
anything else that is not a module — and why, are in
[Names and Namespaces](../phases/names-and-namespaces.md), "A module extending a
module", which owns the rules.

**A global's `use` clause is a module's third fold**, and the whole of it is
`foldGlobalExpand` (`ir/stmt/fold.c`): the global's type supplies the members,
every entry is an `AliasDclNode` carrying the global as its `through`, and a use
of the name is lowered to `global.name`. The rules, the diagnostics and why it is
cheaper than a field's fold are in
[Names and Namespaces](../phases/names-and-namespaces.md), "Folding through a
global", which owns them.

**A standalone `use` is a module's fourth fold**, and the whole of it is
`foldModUseExpand` (`ir/stmt/fold.c`). It name resolves the source, which names a
namespace the module reaches without an import [Jon 23 Sep]. An **enum**: it binds
each variant it admits as an `AliasDclNode` in the module's namespace — the
no-receiver binding an import's fold makes, private unless the statement is
`pub use` — and admits variants and nothing else of the enum. A **submodule**, by
name or by a path through submodules: `foldModUseModule` makes the `ImportNode`
marked `isuse` that folds it, and from there it is an import's clause, folded by
`importNameRes` after `modFoldNames` has run on the submodule. A module reached
through an import is `ErrorUseImported`, since its import's own clause folds it;
any other module that is not a submodule, the module itself included, is
`ErrorModReach`; a second `use` of one submodule is `ErrorDupImport`; anything
that is neither an enum nor a module is `ErrorUseEnum`, an instance of a generic
enum included. The rules are in
[Names and Namespaces](../phases/names-and-namespaces.md), "Folding an enum's
variants into a module" and "Folding a submodule's names into a module".

**A type alias's target is resolved in a pass of its own, and then checked for a
cycle.** A forward reference to a `typedef` is ordinary, so the target has to be
bound before anything asks whether the name is a type at all; `aliasDclCheckCycle`
then reports a chain that comes back to itself and cuts it, so no later walk
loops.

`importNameRes` does nothing unless the import carries a fold clause. When it
does, a star clause has `foldStarItems` make an item per public name of the source
module's **`namespace`** that its `but` does not leave out, a list or a block
arrives with its items already parsed, and `importFoldItem` binds each one as an
`AliasDclNode` in the importing module's namespace, under the item's local
spelling. **Reading the namespace rather than `nodes` is what
makes a fold transit**: a fold writes to the namespace, so walking the
declarations was exactly what left a re-exported name behind.

**What each binding holds:** its local spelling, the source's own binding as its
target — the chain rather than the declaration at the end of it, so the origin is
kept — and a visibility of its own. Where the source's binding is reached through
a global, this one is reached through the same global, and the member is spelled
as its type names it, so the lowering to `global.name` reads the same from any
module.

**A fold is private to the module that made it unless the import says `pub`**,
before the statement or as `pub use`. That is the transit rule, and it is nothing
but the visibility rule read on a binding: what a third module sees through this
one is what this one re-exported. A listed item is parsed as a member alias, so
`importFoldItem` clears both bits it starts with before setting the import's.
The import's binding of the *module's own name* is set by `pub import` alone, so
`pub import wheels` is what lets a path walk `engine.wheels.turn`, and
`import wheels pub use *` re-exports her names while `wheels` stays private here.

Only a public binding of the source folds, asked through `inodeIsPrivate` — the
declaration's `DclPrivate` bit where the source declared the name, the alias's own
`FlagPub` where the source folded it. A private name of the source is
`ErrorNotPublic` where a selective clause names it and is passed over by a star
clause, so it is `ErrorNotPublic` after `but` too: seen from outside a module a
private name behaves as a missing one does, and there is nothing to leave out. A
name the source has not got, listed or after `but`, is `ErrorNoMbr`;
the source module's own name is passed over by a star clause and is `ErrorBadFold`
where a list names it, because the import bound it already. (A module's `extends`
is the one fold that takes private names too, above.)

**A name the module writes twice is an error; one it never wrote merges**
[Jon 23 Sep]. Every fold into a module's namespace — an import's clause, star or
listed, an `extends`, a global's clause, a standalone `use` — binds through
`modFoldBind`, which, where the name is taken, compares what the two bindings stand
for: the declaration at the end of each chain of fold aliases (a `typedef` counts
as a declaration and stops the chain, since its target is resolved only after the
folds), and the global each is reached through. Then it asks whether each binding
was **written** by the module's own source. Everything is, except what a star
clause made — a wildcard `use *`, an `extends`, the implicit core import — which
`importFoldStar` (and `foldStarItems`, for a global's) marks `FlagUnlisted`. An enum's `use Colors;` is written even
though it lists no variant, since it names the enum. A submodule's `use scaling;`
is not: it is folded as an import's `use *` is, and a second `use` of one
submodule is refused whole, as a second import is (`ErrorDupImport`).
- **Both written, same declaration**: `ErrorDupName` — the same name listed twice
  in a clause, two identical `use Colors;` (once per variant), a module both
  imported and listed (`import meter; import relay use meter;`), a listed name
  that is the module's own declaration handed back, a global's clause listing a
  member twice. "If you've got two different ways of bringing in the same thing,
  that should be an error… it's a cleanliness issue." The message says it is the
  same thing twice (`modFoldDupReport`).
- **At least one unwritten, same declaration**: the binding the name has already,
  and nothing is added. A listed name meeting a wildcard's arrival of the same
  declaration is one binding, since that name was not written twice. Where the
  new one was written, the binding counts as written from then on, so a third
  that writes the name again is refused whichever order the three folded in.
  **Where one route is public and the other private, the binding is public**, so
  a re-export is not lost to whichever route happened to fold first. A
  declaration of the module keeps its own visibility, since nothing folds a
  private name of it back as a public one.

That is what lets a diamond compile — `d` wildcard-importing `a` and `b`, which
each re-export `c`'s `x` — a module wildcard-importing a module that extends it,
which hands the module's own declarations back, and the core fold an extending
module takes from its base meeting its own; neither the outcome nor the
visibility depends on fold order. **Different declarations under one name collide
everywhere**: `ErrorDupName`, reported at the listed item or, for a star clause, at
its `use` (or `ErrorExtendsOverride` for `extends`, above), and so does one member
folded through two different globals, since each is reached through its own. An
overload name folds as one
node, the `FnOverloadDclNode`, with its candidates riding inside it; a public name
holds only public candidates (`ErrorPrivOverload`), so the fold carries nothing
private.

## Type check

`modTypeCheck` type checks the imported modules first, then every declaration
the module owns, in source order, and right after an enum that extends another,
that enum's copies of its base's variants, which the module does not own
([struct](struct.md), "An enum extending an enum"). As everywhere in this phase,
**order decides when a declaration is checked, not whether** — a name reached
from elsewhere pulls its declaration forward. See
[Type Check Phase](../phases/type-check.md).

**Nothing refuses an import cycle.** Reuse by file in the registry stops the
parser recursing forever, but no phase asserts that module dependencies form a
DAG. `modFoldNames` notices one — a module reached while its folds are running —
but only to run the fold passes again, so that a re-export travels round it; a
cycle is not reported, and whether one should be is an open question, not this
code's.

## Flow and generation

Flow analysis has no module concept; it runs per function body.

`genlProgram` is two strict passes over `pgm->modules`:

1. **Symbols.** Every module, generating or not. A declaration is skipped only
   when it is private *and* its module is not generating.
2. **Implementations.** Only modules flagged `FlagGenMod`.

**A submodule is flagged `FlagGenMod`, and an imported module is not.** A
subfolder's module is part of the program the compiler was pointed at — its
bodies belong in this object exactly as a swept file's do — where an imported
module is supplied from elsewhere and only declared. ▸ **So a program spanning a
module TREE links and runs today, and one spanning an import does not**, which is
what lets a folder scenario reaching two levels of submodule be a `run` scenario.

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
- **A subfolder is a submodule when it holds its own designated file**, and
  organizational otherwise. An organizational folder's files, at
  any depth beneath it, belong to the enclosing module. This is what lets a
  forty-file module group its files by topic without minting namespaces for
  them.
- **A module folder must be a direct child of its parent module's folder.**
  So the module tree's *shape* mirrors the folder tree's. A
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
module, and two files of one module can share a basename. ▸ **Two sibling module
folders declaring one module name is not among them**, though it was listed here
while the name was free: the folder names the module, and a filesystem gives two
children of one folder two names.

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
  package is an error, whether its fold spec is identical or differs: two ways of
  bringing in the same thing is a cleanliness issue [Jon 23 Sep]. (An identical
  one was once silently ignored.) Identity is the resolved package plus the
  normalized fold spec — the wildcard flag and the set of source-name/local-name
  pairs, order-insensitive — and decides only whether the error says "the same
  way" or "differently".
- **Folding accumulates into the one module namespace.** There is no file-level
  scope: a module's imports come right after its `mod` declaration [Jon 23
  Sep], all of them fold into the module, and the
  namespace's existing uniqueness rule reports a collision. One consequence is
  deliberate — a spelling cannot be aliased two ways within one module, because
  within one namespace it is one thing. A name the module writes twice is an
  error even for the same declaration; the same declaration reaching one
  spelling by a route the module never wrote — a wildcard, an `extends`, the
  core import — is one binding, public if either route is [Jon 23 Sep].
- **`use` states what to fold**, as a clause of `import` for a package and
  standing alone for a namespace already in scope:

  ```
  import opengl use setColor, sub.* but green
  use matrix;
  ```

  The fold lives with the declaration when there is one, which is what keeps a
  package's fold spec in a single place. `but` binds to the wildcard it
  follows. A `use` clause does not unbind the package name, which stays
  available as a qualifier. A long list takes a block form. `using` stays
  reserved so that spelling can be diagnosed rather than merely rejected.
- **`include` is retired.** A module spanning source files does properly what
  `include` did by injection, and it answers what `include` never could: a tool
  handed one file knows its module from the path, where `include` was a pointer
  written in the including file and invisible from the included one.
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
- **A submodule is private to its parent unless `pub`**, which is
  how a package keeps internals internal without a second visibility level.
  Written on the submodule's own `mod` declaration, since the parent declares
  nothing about a subfolder.
- **A folded or imported name is private to the module that folded it**,
  whatever its visibility at the origin. `import B use c as d` binds both `B` and
  `d` in A, and neither is reachable as `A.B` or `A.d`. `pub` opts in, at either
  of two grains: `import B pub use c as d` makes `d` public and leaves `B`
  private, and `pub import B use c as d` makes both public. Both together say
  what `pub import` says alone. A module's public surface is therefore what it
  declares and deliberately re-exports, never what it happens to depend on.
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
folder's name. **The module tree is real**: a subfolder holding its own designated
file is a submodule, private to its parent unless it writes `pub`, spelled after
its parent in every symbol, and reached from its parent by path — while a
subfolder that holds none is organisational at any depth, and a designated file
too deep to be a direct child is refused. `include` is retired and reported
(`ErrorInclude`), since the folder is what brings a file in. **A module reaches SIDEWAYS too**: it
imports a sister by name, resolved against the registry its parent is, and
because every module of a tree is compiled into one object that import *links* —
which an import between two loaded modules cannot do. **And UP**: it imports any
public name of its parent the same way, a type, a function or a global bound as
an alias [Jon 23 Sep]. The registry is the
immediate parent's namespace and no ancestor's, which is the scoped reading,
adopted provisionally. There is no nesting within a *file*, and none is planned —
a `mod name { ... }` block is refused, `ErrorUnbuiltKind` — no package, no manifest and no interface artifact;
`mod trait` holds the spelling of a module's abstraction against the day there is
something behind it; `import` takes a file path where the registry has no answer, and
folds with a `use` clause — selecting, renaming and excluding as a global's
clause does. A module that is one file is
still named after that file, and its declaration still renames it. Sections and
COMDATs are not emitted per function. What does work is the multi-module
*generation* path, exercised by `stdio` on every compile that prints, and folding
into a single module namespace, which is what the accumulation rule above asks
for.

**`use` is one clause at every module site.** A module's **global** and an
**import** carry it whole — `*`, a list, `as`, `but`, a block form, and
`pub use` — parsed by the one `parseFoldClause`, so a module's names are folded
from another module the way a singleton's members are folded from its type. A
module's `use` *statement* names an enum. See "Folding through a global" and
"Import and name folding" in
[Names and Namespaces](../phases/names-and-namespaces.md).

**A module imports another once.** A second import is `ErrorDupImport`, naming
both: an identical repeat [Jon 23 Sep] as much as one that differs in its clause
or its `pub`. So is a second import of one name of the parent.

**A name written twice is an error; one never written merges** [Jon 23 Sep].
Every fold into a module's namespace binds through `modFoldBind`. The same
declaration under a name already taken is `ErrorDupName` where the module's own
source wrote both bindings — listed twice, `use Colors;` twice, imported and
listed, listed back onto its own declaration (`module-fold-written-nameres`) —
and is that binding, public if either route is, where a wildcard, an `extends` or
the core import made either one. Different declarations collide everywhere. The
diamond compiles (`module-fold-diamond`), whatever order the folds ran in.

**A module may extend another**, `mod solids extends shapes;`, reusing the base's
declarations and folds, but not its imports [Jon 23 Sep] — an alias, so the declaration, its symbol and its state stay the
base's, and as visible here as there, so its code reads the base's private names
while its importers see only the public ones [Jon 23 Sep] — and adding its own
declarations beside them. The base is a module already in reach, a sister or one
an import bound; a chain of `extends` transits, and a tree that extends links and
runs. What it does not do: extend a trait (the module-trait reading,
`mod arena extends Region`, is unbuilt), or extend more than one module.

**Every binding has a visibility of its own, and an import's bindings are
bindings.** A declaration has `DclPrivate`, written from the absence of `pub`
when it joins its namespace and read by every check through `inodeIsPrivate`. A
fold makes an `AliasDclNode`, whose `FlagPub` is its own: a global's from
`pub use`, an import's from `pub use` or from the `pub` before the statement —
which alone also reaches the module's own binding. `fnCallNameResPath` enforces
it from outside.

**Transit falls out of that bit.** `importNameRes` reads the source module's
`namespace`, so what it carries across is every public binding — declared there
or folded there — and a fold is private to the module that made it unless the
import said `pub` or `pub use`. What a third module sees through this one is what this one
re-exported.

**And it no longer depends on load order.** Every module's folds run before any
module's body is resolved, dependency-first (`modFoldNames`, called from
`pgmNameRes`), so a root module naming `mid.plain`, where `plain` was folded into
`mid`, gets the same answer as a module loaded after `mid` does. A type resolved
by demand ahead of its module asks for that module's folds rather than hooking
what they would be: `structNameResDemand` calls `modFoldNames`, which is a no-op
except where the fold pass itself is what reached the type.

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

- **Fold at import.** `import bigint use BigInt`, then write `BigInt`. The
  clause is built, and this is what Rust does with `use`.
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
compiler cannot read, and the `extern` block that today sits in a file of its own
beside the module that uses it becomes the package itself.

What that needs, and none of it is designed: how a Cone name maps to an
unmangled C symbol, how calling convention and `trust` are stated, how opaque
types are declared, and whether such a package is written in Cone source or
generated. `--safe=package`, which exists in the option help and controls which
packages may use C FFI, is the policy half of the same question.

Until then a file holding an `extern` block needs nothing of its own: it joins
its module the way any other file does, by being in the module's folder.

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

- **A path is canonicalized before it becomes a registry key** (`fileCanonicalPath`),
  so two spellings of one file are one key and the file is read once. What that
  closed was a miscompile: `import "../b/b"` inside submodule `a` composed a path
  that missed the entry the sweep made for sibling `b`, a second module was built
  from b's files, LLVM renamed the duplicate symbols, and the call in `a` was
  left referencing a declaration nothing defined. ⚠ **Case is not part of it**: a
  filesystem that ignores case still gives two keys for two spellings of one
  name.
- **A file named with no folder in front of it takes its folder's name from the
  current directory.** `conec matrix.cone` run from inside `matrix/` sweeps, as
  `conec matrix/matrix.cone` and `conec matrix` do: a file's module may not depend
  on the spelling of the path used to reach it. Where the current directory cannot
  be read the file is a module of one file.
- **A cycle among non-root modules is fine.** Name resolution runs after all
  parsing, so the half-parsed module the registry returns is complete before
  anything reads it. Nothing refuses a cycle; `modFoldNames` notices one only to
  run the fold passes again ("Name resolution" above).
- **`FlagGenMod` is decided by a `strcmp` on the filename.** A user module named
  `stdio` would have its bodies generated.
- **A module's public names are folded whether or not anything uses them.** A
  wildcard import walks the source's whole namespace, so a name the importer
  never mentions still takes a binding and still collides with a declaration of
  the importer's own.
- **A struct that a global's fold needs is resolved once, even mid-fold.** A
  global's `use` clause needs its type's members in place, so the fold resolves
  that struct on demand (`structNameResDemand`), in its own module's scope. Where
  that module is mid-fold round a cycle, a name the struct's own declaration
  reaches through a re-export not yet arrived — a field's type, say — is reported
  missing, and the fold passes do not retry it: a struct is resolved once.
  Measured: `alpha` imports `beta`, then re-exports `delta`'s `Inner`, and
  declares `struct Box { pub v Inner use get; }`; `beta` imports `alpha` and
  writes `mut g alpha.Box = alpha.Box[delta.Inner[3i64]] use get;`. `Inner` is
  reported unknown in `Box`, and `get` then fails to fold — the same three
  diagnostics as before the fold passes were repeated. The global's type itself
  waits for a late name (`modFoldAwaits`); what that type's declaration names does
  not. Relatedly, a global carrying a `use` clause is resolved whole in the fold
  pass, so a late name in its initial value is reported unknown; an initial value
  must be a literal, so that changes only which refusal such a program gets.
- **A cycle made of `extends` alone is refused before any fold runs**
  (`modExtendsCheckCycle`). A cycle an `extends` edge closes together with
  imports is folded as any cycle of imports is, and a re-export travels round it.
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
