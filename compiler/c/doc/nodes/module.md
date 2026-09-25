`ModuleNode`, `ImportNode` and `ProgramNode` are one note, because a module is
only meaningful against the program that holds it and the imports that reach
into it.

This note also carries the **module / package / compilation-unit model** — what
a module is meant to be, how it becomes something a linker can consume, and
which parts of that are decided. The model spans parse, name resolution and
generation, so no phase note owns it, and it is what a reader usually needs
before touching any of the three nodes.

**At a glance.** `parsePgm` builds the root module and loads the `core` package,
which every module imports. `parseLoadAndParseModuleFile` loads every other
module, in three steps:
locate the file, ask the **file registry** which module holds it, and parse every
file of its folder into the module it draws. The registry is keyed by the file's
path, so a file is read once and belongs to one module. The folder is what names
the module, and a `mod` declaration in its designated file's first statement is
checked against that name. **A subfolder holding its own designated file draws a
submodule** (`parseSubmoduleDraw`), so the folder tree carries the module tree: the
submodule is a module like any other, owned by the module whose folder holds it,
bound in its namespace, and reached from it by path. **So does a file of the
folder whose first statement is a `mod` declaration**: a *one-file module*, named
for its file, and in every other respect the submodule a subfolder would draw.
**Or the compiler is handed a build description** (`parser/parsebuild.c`), which
names every module of one package and lists its files, and nothing is swept: see
"A described build".
Name resolution folds imports before it resolves anything else the
module declares. Type check walks imports first, then
every declaration in source order. Generation declares symbols for every module
and emits bodies only for those flagged `FlagGenMod`. **A generic module**,
`mod stack[T];`, is resolved once and never checked or generated; each
instance, `stack[i64]`, is a module of its own that type check clones from it
and the program then holds like any other: see "Generic modules".

*Provenance: read from source. The root-versus-import symbol asymmetry and the
generation of a package found on the search path were measured from emitted LLVM
IR; the imported-module
`declare`s and the refused loop back to the root are pinned by the `module` test group.
See [Measuring](../diagnostics/measuring.md).*

## Shape

**`ProgramNode`** carries `Nodes *modules` — the root module first, then every
other module in the order it was first loaded, and after type check every
instance of a generic module in the order made — and `Namespace files`, **the file
registry**: every source file the compile has read, keyed by its path, mapped to
the module that holds it. **Every module of the program is in that one flat list,
however deep in the tree it sits**, which is what gives each of them name
resolution, type check and generation exactly once. The tree is not in the list:
it is a child's `dclinfo.owner` pointing at its parent, and the parent's
`namespace` holding the child's name. It also carries `Nodes *initorder`: every
module again, **in dependency order**, each after every module it depends on —
the order the program's stitched init runs module `init`s in, and its stitched
final runs finalizers in the reverse of — made by `pgmModuleOrder` at name
resolution, each generic module's instances placed in it by `pgmInstanceOrder`
after type check ("The module order", below), and read by `genlStitch` ("Init
and final", below).

**The registry is what makes a file read once and owned once.** Its key is the
**canonical** path rather than a name, because what must happen exactly once is
the *reading*: two folders may hold files of one basename, and a module's
declared name is not known until its file has been read. `fileCanonicalPath`
gives one spelling — separators as `/`, a `.` segment dropped, a `..` segment
cancelled against the one in front of it — so that a path a source writes to walk
somewhere and back finds the entry the folder sweep made rather than missing it
and reading the file a second time. The packages `core` and `stdio` are files
like any other, registered under their canonical paths in the packages folder
("The packages folder", below).

**`ModuleNode`**

| Field | Meaning |
| --- | --- |
| `namesym` | the module's name: the **build description's** where one names the module, its **folder's** where a designated file drew the module out of a folder, and `filesym` otherwise. What an importer binds it under, what a path through it is written with, and what its declarations' symbols are spelled after |
| `filesym` | the name derived from the module's *filename* — the source file's basename for the root, the imported file's or the one-file module's for every other. It names a module that is one file, and nothing else reads it. **For a one-file submodule it is a filesystem fact as a folder is**, and a `mod` declaration's name is checked against it; for a lone file a declaration may rename it |
| `foldersym` | the module's folder, when that folder's designated file drew it; NULL for a module that is one file. It is what a folder module's `mod` declaration is checked against, and what says a folder was swept |
| `dclinfo` | the declaration facts — [Names and Namespaces](../../../../doc/design/names-and-namespaces.md), "Symbols". `owner` is **the parent module for a submodule**, whether a subfolder or a one-file module drew it, and NULL for the root, for a lone file and for a module an `import` reached. **The root is the module without `DclNamesChain`**: it has a name and contributes it to no symbol — except a library's root that a build description names, which carries the flag and is spelled as a top module. `DclPrivate` is set on a submodule that does not write `pub`, and on no other module, because a module with no parent has nothing to be visible outside of |
| `imports` | `ImportNode`s only, held apart from `nodes` so folding can run before anything else resolves |
| `moduses` | `ModUseNode`s only — every standalone `use` written at module scope, of an enum or of a submodule — held apart from `nodes` for the same reason, and because the statement is neither a declaration nor a field: what it declares is bindings, made in the fold pass |
| `nodes` | every declaration the module owns, in source order, **an enum's variants among them**: a variant is walked, checked and generated as the module's, though its name is bound in its enum. This is what printing and generation iterate |
| `namespace` | every name *visible* in the module: what it declares, **every declaration and fold of the module it extends — not the names that module's imports bind — each an `AliasDclNode` as visible here as it is there**, **the module an import bound and every name an import folded in, each an `AliasDclNode` carrying the import's own visibility**, what a global's `use` clause folded in, **the variants a `use` of an enum folded in**, **the names a `use` of a submodule folded in, each an `AliasDclNode` carrying the statement's visibility**, **each submodule its subfolders and one-file modules drew**, and — when a folder, a one-file module's file or a `mod` declaration named it — the module's own name. **Not an enum's variants by themselves**: those are names of the enum |
| `flags` | `FlagGenMod`; `FlagModDcl` for a module a `mod` declaration named; `FlagPub` for a submodule its declaration opened |
| `foldpass`, `folding` | the fold pass (`modFoldAll`) that last reached the module, and whether its folds are running in it. A module reached again while *folding* has closed a loop, which `pgmModuleOrder` refused already: it is read as far as it has got, and the passes go on until they settle |
| `dagmark` | the module-order walk (`pgmModuleOrder`): unvisited, on the walk's path, or placed in `initorder` |
| `extendsname` | `mod A extends B`: B as written, a `NameUseNode` bound to the module once `modExtendsResolve` finds it; NULL where the module extends nothing |
| `extends` | the fold `extends` makes: an `ImportNode` marked `isextends`, whose module is B and whose clause is a star clause over B's declarations and folds, private ones included, but not the names B's imports bind to their modules, made once B resolves. **Not on `imports`** — it binds no name of its own — and folded first by `modFoldNames` |
| `deffold` | `mod A use B`: the module's **default fold**, the `FoldClause` a bare import of it folds; NULL where its `mod` line has no `use`. Read by importers, never folded into this module |
| `traitname` | `mod A is T`: T as written, a `NameUseNode` bound to the module trait once `modTraitConform` finds it; NULL where the module conforms to none |
| `trait` | the `ModTraitNode` `traitname` names, once resolved: what `modTraitCheck` compares the module against in type check |
| `ntaken` | how many of `nodes`, at its end, are copies of the trait's defaults the module took (`modTraitConform`). They arrive resolved, so `modNameRes` walks the nodes before them only |
| `initfn` | the module's own `init`, once `modLifecycle` has found it well formed; NULL where it declares none. What the stitched init calls |
| `finalfn` | what finalizes the module: the `drop` `modLifecycle` gives it where a global needs finalizing, else its own `final`; NULL where it has neither. What the stitched final calls |
| `genericinfo` | `mod stack[T]`: a generic module's type parameters and its instances (`memonodes`, as a generic type's), from the `mod` line; NULL for any other module. See "Generic modules" |
| `generic` | an instance of a generic module: the generic it was cloned from; NULL for every module the source declares. `instnode` is then the call that made it, whose arguments are the instance's type arguments |
| `instdeps` | an instance: the modules it follows in the init order besides its generic — each its type arguments come from, and each instance its own body made (`pgmInstanceOrder`) |

**`ModTraitNode`** (`ir/stmt/modtrait.h`) is a module trait, `mod trait Shell
{ ... }`: `namesym`, `dclinfo` — owned by the module whose file declares it,
private unless `pub` — `nodes`, each member in the order written, a `FnDclNode`
or a `VarDclNode`, and `namespace`, each member by its name. A member is owned by
the trait. It is a named node in `StmtGroup`, as a module is, and a declaration
of its module: bound in the namespace, folded, imported and reached by path like
any other. See "Module traits" below.

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
**An import that writes no clause takes its module's default** (`deffold`, from the
module's `mod` line): when its first fold pass runs, `importDefaultFold` gives it
a copy of that clause as its `fold` and sets `isdefault`. A clause the import
writes replaces the default whole.

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
| `newModuleNode` | `namesym`, `filesym` and `foldersym` NULL, `dclinfo` cleared, empty `imports`, `nodes` and `namespace`, no `extends` and no `deffold` |
| `newImportNode` | `module` NULL, `fold` NULL, every flag clear |
| `newModUseNode` | `source` NULL, an empty `fold` positioned at the `use`, `modfold` NULL |

## Parse

`parsePgm` establishes the program in an order that matters:

1. The root `ModuleNode` is added first and flagged `FlagGenMod`. It is not given
   `DclNamesChain`, so it prefixes nothing.
2. The main source file is located, and every file its folder sweeps in is
   registered to the root — before anything is parsed, so that which files the
   root holds does not depend on what a parse of one of them imports. An import
   back to any of them then finds the root in the registry, rather than reading
   the file as a second module, and the loop it closes is refused naming the
   root (`module_cycle`).
3. `core` is loaded (`parseLoadCore`): `core/src/core.cone`, found on the package
   search path and nowhere else, so no file beside a program stands in for it.
   It is loaded exactly as an imported module is, and is the one module loaded
   with no auto-import of itself.
4. An `ImportNode` carrying a star clause is added to the root for `core`, and
   `ParseState.core` is set, so every module loaded from then on is given the
   same import.
5. The submodules its subfolders and one-file modules draw are drawn, each
   recursively.
6. The root's own files are parsed, its designated file first.

Given a build description, steps 1, 2 and 5 take its word instead of the
filesystem's: the root is named by the description, flagged `DclNamesChain` when
it says `output: library`, and holds the files it lists; the child modules drawn
are the ones it lists ("A described build", below).

`parseLoadAndParseModuleFile` is the single path by which any other module is
loaded, and it is three steps rather than one:

- **Locate.** `fileFindLocal` resolves the written name against the current
  file's folder, and where that finds nothing `fileFindPackage` tries each folder
  of the package search path in turn — every `--path` folder, then the packages
  folder — each trying `name.cone` and then `name/name.cone`, the designated-file
  convention, except that `fileFindPackage` tries a Congo package's
  `name/src/name.cone` between the two ("The packages folder", below). Either
  returns the path and reads nothing.
- **Register.** The path is interned and looked up in the file registry. A hit
  *is* the answer: that module already holds the file, and the file is not read
  again. A miss makes the module, sets `filesym`, `foldersym` and `namesym`,
  marks it `DclNamesChain`, gives it `FlagGenMod` where the search path found it,
  and registers every file the folder sweeps in (`parseLoadModulePath`).
- **Parse into the module.** Each registered file is injected and its global
  statements parsed into the one module, the designated file first, since it is
  the only one that may declare the module. The auto-import of `core`, a star
  clause, is added first, and `modHook` swaps the name table once for the whole
  set — so a module neither sees nor collides with the names of the module whose
  parse reached it, parent or importer alike. Every declaration any of those files
  adds through `modAddNode` records the module as its owner.

**`parseModuleTree` is the last step of all three paths**: it draws the
submodules the module holds — `parseSubmoduleDraw` for every one of them, in the
order of their names whichever shape each has, then `parseSubmoduleParse` for
each, then the module's own files.
**Every sister is drawn before any is parsed**, which is the rule the files
follow one level down and holds for the same reason: a submodule importing a
sister resolves that name against this namespace, so a sister drawn later would
be a name that was not there — and her files would be unregistered, so a path
spelled to one of them would read it a second time. **The submodules come before
the module's own files** too, and both reasons are about what a name means before a file is read. A
submodule is a name of the namespace that no statement in any of the module's
files declares — where its folder or its file sits is the declaration — so binding it ahead of
them makes the declaration the duplicate, and the declaration is where a
collision's first diagnostic lands. And it registers the submodule's files, so a
file of this module that names one of them reaches the module that holds it
rather than reading it a second time.

**A module is positioned at its designated file's first line** — line 1,
column 1 of `matrix/matrix.cone`, or of a module's one file —
from the moment it is made, and `parseModulePosition` is what does it, on all
three paths. **Ruled by the author, 23 September 2026:** until its `mod` line is
parsed, a module named by its folder has no line in any source that declares
it, and the file that makes the folder a module is the nearest thing it has to
a declaration, so that is where its half of a diagnostic — `ErrorDupName`
against its name — is reported. Its `mod` declaration is nearer, and moves the
position to itself when it is parsed; a collision found before that, at the
draw, and one with a module whose file is refused for having no `mod` line,
stay at the first line (`module_submodule_reject`). **The file is read when the module is made**,
before any file of the tree is parsed, because the module's name is bound then
and a collision is reported as the second binding is made: a submodule named like
its parent meets the parent's own name at the draw. `lexLoadPath` reads it into a
lexer block that is not yet current, and that block is the one
`parseModuleFilesParse` makes current when the designated file's turn comes, so
the file is still read once. The files a sweep finds are read the same way, when
the sweep finds them (next section), and each block is the one later parsed.

### The folder tree

**A module's source files are the files of a folder**, and the compiler is given
one file and finds the rest. What makes a folder a module folder is the
**designated file** it holds, named for the folder — `matrix/matrix.cone`.

**That convention is the whole of the trigger.** The file the compiler is given
sweeps its folder exactly when it is that folder's designated file, which is the
same probe a subfolder would get. ▸ **So a file that is not its folder's
designated file is a lone file, a module of its own and exactly today's
program**, and the files beside it are none of its business — which is what lets
a folder hold a dozen unrelated programs, as every one of the test suite's group
directories does. That holds whatever the file's first statement is: a `mod` in
it makes it a one-file *submodule* only when a module folder's sweep finds it.

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

**Every file gets a probe too: its first statement.** ▸ **A file of a module's
folder whose first statement is `mod lexer;` or `pub mod lexer;` is a ONE-FILE
MODULE** [Jon 23 Sep: "The only reason for going to a folder is if there's more
than one file."]. It is a submodule of the folder's module in every respect a
subfolder's would be — owned, private unless `pub`, bound under its name, drawn
before any file is parsed — and it holds that one file and sweeps nothing: it does
not join the folder's module, and none of that module's files join it. A file
with no `mod` joins the folder's module, as every file did before. So **growing a
one-file module into a folder** — moving `lexer.cone` to `lexer/lexer.cone` —
**changes nothing for anyone who names it**: the same name, the same paths, the
same symbols, and the same place in the order submodules are drawn, which is by
name whichever shape each has. `module_onefile` and `module_onefile_grown` are
that pair, and their symbols are the same.

The probe is `lexOpensWithMod`, **a look at the file's text rather than a
parse**: white space and comments are passed over, then an optional `pub`, then
the word `mod`. Nothing is lexed, so nothing about the file is reported ahead of
its parse, and which files a module holds still does not depend on what any
file's parse imports. To look, the sweep reads every file it finds into its lexer
block (`lexLoadPath`), and that block is the one `parseModuleFilesParse` or the
drawn module later parses, so each file is still read once. The given file itself
is not probed: a file that is not its folder's designated file is still a module
of its own, a lone program, whatever its first statement.

**A module must sit directly in its parent module's folder**, so the module
tree's shape mirrors the folder tree's. A designated file beneath an
organisational folder is therefore refused (`ErrorModFolder`) rather than drawing
a module deeper than the shape allows; the folder holding it stays organisational,
so that file is swept into the enclosing module like any other and the compile
goes on with a coherent file set. A `mod` declaration in it is then `ErrorModDcl`
— the refusal's consequence, and the shape of what was wrong. **A one-file module
in an organisational folder is refused under the same code**, with a message
saying it may move up into the module's folder or drop its `mod`; it is left out
of the compile, since the one thing it says is that it is not the enclosing
module's file.

Six conditions are diagnosed, and each names full paths, because the paths are
the only thing that tells the files apart:

| Condition | Code |
| --- | --- |
| A file another module already holds, brought into a second one — by the sweep, or by an `import` naming the importing module's own file or its own submodule | `ErrorModFile` |
| Two files of one module sharing a basename, which leaves neither nameable | `ErrorDupFile` |
| A designated file or a one-file module beneath an organisational folder, too deep to be a module | `ErrorModFolder` |
| A one-file module beside a module folder of the same name — `lexer.cone` declaring `mod` beside `lexer/lexer.cone` — naming both paths. The folder is kept and the file left out | `ErrorModFileFolder` |
| An `import` reaching a module inside a tree by a path to its file, or naming the module that contains it | `ErrorModReach` |
| Two of the module's files declaring one name, or a file declaring the name a submodule already has | `ErrorDupName`, which is the ordinary namespace rule: a subfolder is a namespace exactly when it draws a module, and an organisational one never is |

▸ **The collision between two sibling module folders needs no diagnostic, because
it cannot be written.** A module's name is its folder's or its one file's, and a
filesystem gives two children of one folder two names — so what is left of that
collision is a submodule's name meeting another name of the parent, which is
`ErrorDupName` like any other duplicate. The one pair a filesystem does allow is
`lexer.cone` beside `lexer/`, and that is `ErrorModFileFolder` above.

A file that cannot join is dropped rather than parsed, and the module goes on
with the rest of its files.

### What a submodule is, and what it is not

`parseSubmoduleDraw` draws one, and it is a module in every respect — its own
namespace, its own entry in the program's module list, its own turn at every
later phase, and, where a subfolder drew it, its own folder sweep and its own
subfolders. **Two things make it a child rather than a neighbour, and they are
the whole of the difference:**

- **Its parent owns it** (`dclinfo.owner`), so `namePath` spells its declarations
  after the parent's name and a submodule of a submodule after both — `sub.fn`,
  `sub.inner.fn`. That needed no new spelling: the encoding already had a case for
  a module whose owner is a module.
- **It is private to its parent unless its declaration writes `pub`**, which
  `dclInfoJoin` writes as `DclPrivate` and `pub mod sub;` clears. Its own
  declaration is the only place that can be written, because the parent declares
  nothing about a subfolder or a file.

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

🛑 **It may not reach UP: an import of a name of its parent is a loop, and
refused** (`ErrorImportLoop`) [Jon 23 Sep]. The registry is the parent's
namespace, so it holds more than modules: what the parent declares, what its
imports bound and what its folds brought in, and `import Point` inside a
submodule names the parent's public `Point` as `import wheels` names a sister.
But a parent depends on each of its children — *"The whole concept of programs
are hierarchical decomposition"* — so a child depending on its parent closes a
loop of two, which the module order refuses ("The module order", below). What a
parent and its children share moves into a sister they all import. A module the
parent binds by an import is a name of the parent too, and refused the same
way: the child imports what holds it, a package by its own name. The import is
still bound in the fold passes ("What an import reaches"), so the child's body
resolves and the loop is the one thing reported;
a private name of the parent is `ErrorNotPublic` beside it, and the rest of what
binding reports is reported too (`module_import_parent_nameres`).

**Nothing arrives unasked.** A module's namespace holds what it declares, what a
fold brought in, its own children and what its imports bound. A sister nobody
imported is `ErrorUnkName`, which is the dependency being stated rather than
handed, and so is a name of the parent nobody imported: a bare name does not
climb.

⚠ **The registry is SCOPED, not accumulating, and this is provisional.** It is
the *immediate* parent's namespace and no ancestor's, so descending a level drops
the level above out of reach: a module two deep does not see its parent's
sisters. A parent re-exporting what its children need is not a way round that,
since a child importing what its parent binds depends on the parent and is
refused; what a child needs from outside its parent it imports by its own name,
or a sister of its own holds. That keeps a module
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
whether the filesystem already supplied one.**

- **A folder module's name is its folder's**, bound into the module's own
  namespace at load, before any of its files is parsed. A name written in the
  declaration is **checked** against it and may not replace it (`ErrorModName`):
  a name on the filesystem is one a tool that cannot parse Cone can read off a
  path, and a declaration that disagreed would be a second answer.
- **A one-file module's name is its file's**, for the same reason and in the same
  way: bound at load, and checked (`ErrorModName`, "named for its file") — `mod
  lexer;` is written in `lexer.cone`.
- **A lone file — the root, or a module an `import` reached by its path — is
  named after that file**, and its declaration still renames it and binds the new
  name. That is transitional, and it is what keeps today's programs working.
- **A described module's name is the build description's** — the module's
  `name: { }`, or an import line's name for the file that line names — bound at
  load, and a `mod` line opening **any** of its files is checked against it
  (`ErrorBuildModName`), ahead of the rule that only the first listed file may
  declare. So a file the description put in the wrong module is reported as
  that, and a later file restating the right name is `ErrorModDcl` as ever.

Either way the bound name is duplicate-checked against the module's declarations
like any other, and is in reach inside the module for the rest of the parse.

**The declaration is its file's first statement, and a module declares itself
once** (`ErrorModDcl` otherwise). It claims the module, so nothing may precede it.
`parseGlobalStmts` is told whether it is reading the module's designated or one
file. A file the folder swept in declares nothing: one that opened with a
declaration would have been a one-file module instead, so a `mod` in a swept
file is always a late one.

### A file's header

**Every module's first file opens with its `mod` line** [Jon 24 Sep]: the
designated file of a folder module, a one-file module's file, a lone file, and
the first file a build description lists for a module or an import line
names. A first statement that is anything else — `mod trait` included, which
declares a trait and names no module — is `ErrorNoModDcl`, reported at the
statement, and a first file with no statement at all at its end; the message
gives the line to write. The statement is still parsed as written, and the
module is still the one its folder, file or description names, so nothing else
follows from the refusal. The line restates a name the filesystem already
gives; what it buys is that the name is where a reader of the file looks first,
and that the file says which module it opens. The other files of a folder
module carry none, since one would make the file a one-file module.

**A module's imports come right after its `mod` line**, ahead of every other
declaration, and so **all of them are in its designated file** [Jon 23 Sep]:
*"Imports can't happen in a sibling file because they all have to be after
`mod`, right? And a sibling file doesn't have a `mod`. So there's no place to
put it … all of that stuff has to be in the main designated file."* A module's
whole inbound dependency list is therefore at its declaration. The rule is
about `import` alone; `use` goes wherever a fold applies, in any file of the
module. `parseGlobalStmts` refuses an `import` anywhere in a file other than
the module's first — one the folder sweeps in, one in an organisational
subfolder, one a build description lists after the first — as
`ErrorImportLate`, naming the designated file. In the first file it notes the
first statement that is neither the `mod` line nor an `import` (a retired
`include` is neither refused nor counted), and an `import` after it, `pub` or
with a `use` clause alike, is `ErrorImportLate` too. Either way the import is
still made. So a module's header is comments, the `mod` line and the imports,
which is exactly what Congo reads of its designated file
(`tools/congo/README.md`); an import below the header, or in another file, is
one Congo never sees.

**`pub` on the declaration opens a submodule to its parent's neighbours**, and is
`ErrorBadPub` on any other module: the root, a lone file and one an `import`
reached are each inside nothing, so there is nothing for `pub` to open them to. It is the same `pub` every declaration carries, reaching the one
declaration that draws a module.

**`mod name extends base;` makes this a module that reuses another**, and the
declaration only records `base`, as a `NameUseNode` on `extendsname`: what it
names is resolved at name resolution, once every import is bound, so the
declaration may come before the import that puts the base in reach. One base, by
one name — a second after a comma is `ErrorExtends`, a path `ErrorModExtends`,
nothing at all `ErrorNoName` — and each refusal passes over what it refused, so
the declaration still names the module. What `extends` does is in "Name
resolution" below.

**`mod bigint use BigInt;` names the module's default fold** [Jon 23 Sep]: what a
bare `import bigint;` folds, so a package holding one thing becomes the thing
where it is imported. With `extends`, the clause comes last, `mod bigint extends
base use BigInt;`. `parseModDefaultFold` reads it with `parseFoldClause` — names,
a block, `*`, `* but` — and it is recorded on `deffold` only where the declaration
is accepted. Two spellings nobody ruled on are refused and the clause taken
without them: `as` (`ErrorBadFold`), since what an importer calls a name is its
own to say, and `pub use` or `use pub` (`ErrorBadPub`), since each import decides
how visible its folds are. A clause written before `extends` is `ErrorBadFold`.
The clause runs the other way from every other `use` — it folds into importers,
not into this module — and it is not an export list: `pub` still decides what is
reachable. What it names is checked once the module's names are known (below).

**`mod bigint is Shell;` says the module conforms to a module trait** [Jon 23
Sep], and the declaration only records the name, as a `NameUseNode` on
`traitname`: it is resolved once the module's folds have run, so the import that
puts the trait in reach may follow it. **The line's order is `mod prog extends
base is Shell use Y;`**: `is` written before `extends` is `ErrorModIs`, and a
`use` before `is` is `ErrorBadFold`, as a `use` before `extends` is. One trait, by
one name, as `extends` names one module: a path or a list after `is` is
`ErrorModIs`, nothing at all `ErrorNoName`, and each refusal passes over what it
refused. What `is` does is "Module traits" below.

**`mod stack[T];` declares a generic module** [Jon 23 Sep], its type parameters
in square brackets straight after the name, read by `parseGenericParms` as a
generic type's are, and recorded on `genericinfo` where the declaration is
accepted. The clauses follow them: `mod stack[T] extends base is Counter;`.
Type parameters only; an empty list is `ErrorNoGenParms`, as it is for a type.
A generic module is not C-named: `@c` on its line is `ErrorCAttr`, since a C
name has no room for an instance's type arguments. What a generic module is and
does is "Generic modules" below.

**`mod trait` is not this declaration.** `parseGlobalStmts` sees `trait` after
`mod` and hands the statement to `parseModTrait`, which declares a module trait,
a declaration of the module like a type — anywhere in the file, and never a late
`mod`. **A `mod name { ... }` block does not exist in source**: a module is never
declared inside a source file, and nesting is by files and folders only, a
nested module being a file of its own or a subfolder with its own designated
file. The parser recognises the block there only to say so, `ErrorUnbuiltKind`,
and skips its body. **A generated include file alone writes one** [Jon 25 Sep,
Q1], a private, pruned block for each submodule the file reaches
(`parseModuleBlock`, "Generating the include file").

**A `mod` declaration in the root file names the module and does not change a
single symbol.** The root still has no `DclNamesChain`, so its declarations stay
bare and `main` stays linkable. What names a root in its symbols is a build
description saying `output: library`, never the declaration. Naming the module is what makes its hidden names
reachable; what spells a symbol is the owner chain, and that is a separate
question — see "Consequences that follow whichever way those go" below.

⚠ **Filename naming that a declaration may override is transitional, and is what
a folder replaces.** A lone file is named after that file, which is what lets
today's programs go on working unchanged. A module drawn out of a folder is named
after the folder, and nothing about its filename is a name; a one-file module is
named after its file, and that name is as fixed as a folder's.

### Module traits

**A module trait is a module's abstraction** [Jon 23 Sep]: the interface a module
plugs into a framework with — a shell executable's entry, a web request's
receiver — spelled `mod trait`, `trait` modifying the kind as `struct trait`
does. **It is a declaration of the module whose file writes it**, as a struct
trait is: in-file, anywhere among the module's statements, `pub` or private,
named, imported and folded like any declaration (`import hosts use Runner`), and
reached by path. What reaching a member by path cannot do is below.

```
pub mod trait Runner {
    pub fn step(n i64) i64;         // a requirement
    pub mut scale i64;              // a requirement
    pub mut offset = 100i64;        // a default
    pub fn run(n i64) i64 {         // a default
        step(n) * scale + offset;
    }
}
```

**Its body holds functions and globals, and nothing else** (`parseModTrait`). A
function with a body and a global with an initialiser are **defaults**; one
without is a **requirement**. Anything else is `ErrorModTraitBody`, skipped
whole: a type (a trait requiring types is not built), an import, a `use`, a
macro, a `mod`. So is a generic function and an overload name, since a member is
one name with one signature for a module to match; a name written twice is
`ErrorDupName`. A trait with no body is a marker, with no members. **The probe
that finds one-file modules (`lexOpensWithMod`) passes over `mod trait`**, so a
swept file may open with one.

**A module conforms by saying so, and only by saying so**: `mod prog is Runner;`
(the `mod` declaration, above). Nothing is inferred from what a module happens to
declare. A module conforms to one trait. **A module is one instance, so
conformance is static**: no vtable, no dispatch — what the trait supplies becomes
the module's own.

**Conformance is made where it is written** (`modTraitConform`), in three steps:

1. **Resolve what `is` names**, in the module's namespace and then in the
   registry its parent is, looked up and never loaded, as `extends` looks. A name
   nothing binds is `ErrorUnkName`; one that is not a module trait — a struct's
   trait, a module, a function — is `ErrorModIs`, with the cause in the message.
2. **Resolve the trait** in the scope of the module that declares it, with its own
   members hooked over that module's names (`modTraitNameRes`, demanded from the
   conforming module), so a default's body names another member bare. Done once
   per trait. Its default bodies count as expanded ones (`fnDclIsExpanded`), so
   what they name of the declaring module is marked for a library to export.
3. **Meet each member.** A member the module has a name for is met by it: a
   declaration of the module, or a name its folds brought, what it `extends`
   among them — conformance is a question of names and signatures, as a struct
   trait's is. A member it has no name for is a **missing requirement**
   (`ErrorModTraitMissing`, at the `is`, naming it) unless the trait gives a
   default, which is **cloned into the module**: a shell for each default, owned
   by the module (`dclInfoJoin`), appended to its `nodes`, bound in its namespace,
   positioned where the trait wrote it, with `instnode` the `is` — before any
   body is copied, with every member mapped (`cloneDclSetMap`) to what the module
   has under its name. Then the bodies are copied (`cloneFnDclFill`,
   `cloneVarDclFill`), the way a generic type's members are. **So a default's
   body naming another member names the module's**: its own declaration where it
   has one, overriding the default, or the copy of another default. A name that is
   not a member keeps what it bound in the trait's scope, a private helper of the
   declaring module included. The copies arrive resolved; `ntaken` says how many,
   and `modNameRes` leaves them alone.

**A copy is the module's own declaration**: spelled after the module
(`plain.run`), private unless the trait wrote `pub`, and generated wherever the
module is. **A default global is storage of each module that took it**, so two
modules conforming to one trait count separately (`module_trait`). **The trait's
own members are generated nowhere** (`genlGlobalImpl` passes a `ModTraitTag`
over): a requirement has no body, and a default's body is the template.

**When it runs is what makes a copy a declaration to everyone who reads the
module.** `modTraitConform` is attempted at the end of the module's own fold
pass (`modFoldNames`): its namespace is complete there, and the fold pass is
dependency-first, so nothing folding from it has read it yet. A module extending
it therefore takes its copies as the base's declarations — aliases, one storage —
and an importer's `use *` folds them like any other public name. That holds
because imports form a DAG [Jon 23 Sep]. Where the attempt cannot be made there —
`is` not resolving yet, or the trait's module still mid-fold (a parent's trait,
the parent folding its child) — it is left, and `pgmNameRes` makes it after the
fold passes, reporting whatever is wrong, before any module's own names are
resolved. ⚠ **`is` naming a trait its parent declares is a child reaching up
without an import, and the module order counts no edge for it**: the ruled edges
are imports, `extends` and containment, and `is` was not among them. Whether it
should be — which would refuse a child conforming to its parent's trait — is
open.

**What the module has for each member must have the member's shape**
(`modTraitCheck`, at the start of `modTypeCheck`, once both sides have types): a
function or an overload name for a function, one candidate with exactly the
member's signature (`fnSigEqual`; no coercion, as for a struct trait); a global
for a global, of the member's type and permission. Each difference is
`ErrorModTraitMismatch` at the `is`, naming the member. A copy has the shape by
construction and is passed over, and where a difference was reported the copies
are marked checked rather than analyzed, since their bodies were written against
the trait's members and would report the difference again. An `extern`
declaration meets a requirement, as an include file will declare what a
package's object defines. Visibility is not compared.

**A path through a module trait is refused** (`fnCallNameResPath`): `Runner.run`
names a member with no code or storage of its own, `ErrorAbstractMeth`, as a
path to a struct trait's method is. `extends` naming a module trait is
`ErrorModExtends`, whose message says to write `is`.

**A module's or a module trait's name where a type is wanted is refused**
(`itypeRefuseModule`), `ErrorNotType`: neither is a type, though `nameUseGroup`
answers such a name as one. It is asked where a bare generic is
([generic.md](generic.md), "A generic named bare where a type is wanted").

**What is not built** [Jon 23 Sep]: host traits (a shell's, a web server's), the
entry glue — a trait default the host calls as `main`, running the program's
stitched init and final round the program's own `main` ("Init and final":
`initAll()` and `finalAll()` are what it would call) — and traits that require
types. Conformance declared by
structure alone does not exist and is not planned. A generic module conforms by
its `mod` line: the generic takes the defaults as any module does, and each
instance clones them with the rest of its declarations and is checked against
the trait itself ("Generic modules").

### Generic modules

**A generic module is treated as a generic type is** [Jon 23 Sep: *"if I
treated generic modules like I treated generic types, how would I treat
them?"*]. It is written with type parameters on its `mod` line, `pub mod
stack[T];`, and instantiated where it is used, `stack[i64].push(3i64)`, as
`Box[i64]` names an instance of a type. The analogy decides every detail below;
[generic.md](generic.md) is the machinery both share.

```
pub mod stack[T];

pub mut count i64 = 0i64;
pub mut top T = 0;

pub fn push(x T) {
    top = x;
    count = count + 1i64;
}
```

**Nothing of it is compiled until an instance is named.** The generic is
resolved once, in place, with its parameters hooked over its own names
(`modNameRes`), and never type checked (`modTypeCheck` returns after its
imports) or generated (`genlProgram` passes it over): its body is written
against parameters that stand for nothing. Its imports, folds and `is` are
made once, on the generic, and every instance shares them.

**An instance is a module of its own**, made by `modInstantiate` the first
time type check meets `stack[i64]` — `genericSubstitute` asks `genericMemoize`,
whose memo is the generic's `memonodes`. **Identical arguments anywhere are one
instance**, compared by `itypeIsSame`, so `stack[i64]` in two modules is one
module with one set of globals (`module_generic`). The instance is named as
the generic is, owned where it is, marked with the call that made it
(`instnode`, which `generic` points back from), and flagged `FlagGenMod`. Its
declarations are cloned the way a generic type's members are
(`cloneStructNode`): a shell for each — a function, a global, a type, an
overload name, a type alias — mapped from the generic's before anything is
copied, then every signature, body, type and initial value, so a body naming
another declaration of the module, before it or after, names the instance's.
The generic's own name is mapped to the instance, so `stack.count` or
`stack[T].count` inside the body is this instance's global, while another
argument list, `stack[f64]`, is another instance reached through the memo
(`cloneFnCallNode`). Every other name the generic's namespace binds — what its
imports bound and folded — is bound in the instance's to the same declaration.
A module trait's defaults the generic took are declarations of it, and are
cloned with the rest; the instance keeps the generic's `trait`, so
`modTraitCheck` checks each instance. It is registered before it is type checked, so a body
naming its own instance reaches it rather than instantiating it again; it is
then type checked with no function around it, which runs `modLifecycle`, so
**each instance has its own `init` and `final`** and its own `drop`.

**A path through an instance is collapsed at type check** (`fnCallTypeCheck`,
`fnCallModuleInstancePath`), since the instance does not exist at name
resolution: `stack[i64].push` looks `push` up in the instance's namespace, a
private member named from outside it is `ErrorNotPublic`, and the hop
disappears as `fnCallNameResPath` makes it for any other module. What a
generic is given that is not a list of types is refused as a generic type's is
— `ErrorNotType`, `ErrorArgCount`; nothing is inferred.

**Nothing of the generic itself has members to reach**, so its bare name is
refused wherever a member is wanted, `ErrorGenModBare`: a path, `stack.push`
(`fnCallNameResPath`; inside its own body the name means the instance); a
standalone `use stack;`; an import's `use` clause (`importNameRes`, which binds
the name and folds nothing); `extends stack`; and a default fold on its own
`mod` line (`pgmGenericModulesCheck`). An import of it binds its name, and an
instance is then written through that name.

**What an instance is cloned from is its generic's own declarations**, so what
a clone does not yet re-point is refused where the generic declares it,
`ErrorGenModBody` (`modGenericCheckBody`): a generic function or type inside it,
whose own parameters the clone would have to carry through; a trait or an enum,
a macro and a module trait; and a global's `use` clause, whose aliases are
reached through the generic's global. **A generic module's submodules are not
built** (the same code, `pgmGenericModulesCheck`): by the analogy they are
instantiated with it, as a generic type's methods are, and the flat case is
what is built. **An executable's root may not be generic** (`ErrorGenModRoot`):
nothing could instantiate the program. A library's root may, and so may a
submodule or an imported module.

**Its symbols carry the instance's type arguments on the module**, as a generic
type instance's carry them on the type: the instance's `instnode` is what
`nameOwnTypeArgs` reads, so `namePath` wraps the module's component in `I…E` —
`stack[i64].push` is `_CNvIC5stackxE4push`, `stack[i64].Entry.doubled` is
`_CNvNtIC5stackxE5Entry7doubled` — and each member's own component is its
identifier alone. [Names and Namespaces](../../../../doc/design/names-and-namespaces.md),
"Symbols", is the rule.

**An instance joins the program's modules once type check is done**
(`pgmTypeCheck`), since the module list is being walked while instances are
made; `modInstanceList` holds them until then. It is then generated like any
module the compile generates, and its place in the init order is "The module
order" below. **Every object that uses an instance defines it**, as it defines
a generic function's: in a described build `dclIsInstance` answers for every
declaration of the instance — its owners are asked up to the module, and an
instance module is an instance — so its functions *and its globals* are
`linkonce_odr` with a COMDAT of `any`, and the copies separately compiled
objects make merge into one, with one set of globals. The generic's own
package defines nothing of it; its include file carries the generic module's
full source, which is what an importer compiles the instance from
(`module_generic_link`).

### The packages folder

**`core` and `stdio` are packages, laid out as Congo lays out every package.**
The repository's root holds `packages/`, one folder per package, each holding
a manifest, `congo.toml`, and the package's source, `src/<name>.cone`. Neither
holds an include file: a program Congo builds is compiled against the one each
package's own compile generates, `build/<mode>/<name>.cone` ("Generating the
include file"). The hand-written `core.cone` and `stdio.cone` that stood at the
package roots are gone. Nothing about either is known to the
compiler but the name `core`: each is located, registered and parsed on the
path every imported module takes, and named by its file. `stdio`'s printing is
C, declared in its `pub extern` block, each function marked `@c` so that it
takes its C name rather than `stdio`'s Cone one, and supplied by `conestd`.

**A compile that finds a package on the search path wants its source**, since
it builds the package into its own object (below), so `fileFindPackage` tries
`name/src/name.cone` after `name.cone` and **before** the designated file
`name/name.cone`, where a Congo package once kept a hand-written include file,
and one left behind must not stand in for the source. The source is
then a lone file, not a designated one — `src` is not `name` — so nothing
beside it is swept: a package found this way is its one root file. Only a bare
name is looked for there. This lookup is the compiler's side of the
**package folder**, and it lives only as long as the search path does; a Congo
build names every file itself ("A described build", below).

**The packages folder is found by default.** It ends the **package search
path**, `package_search_paths` in `coneopts.c`, which `lexInit` hands to
`fileio.c` as `fileSearchPaths`: every `--path` folder in the order given, then
the packages folder. That folder is chosen in three steps, first found wins:

1. the one the `CONE_PACKAGES` environment variable names;
2. **the one that travels with `conec`**: the nearest `packages/` holding
   `core/src/core.cone` (the repository's layout) or `core/core.cone` (a
   designated file), looked for in the executable's own folder and then each
   folder above it (`coneOptExePackages`, with the executable's folder asked of
   the operating system by `fileExeFolder`, never read from `argv[0]`). One rule
   serves the build tree, where `build/x64-release/conec.exe` finds the
   repository's `packages/` two folders up, and an installed layout, where
   `<prefix>/bin/conec.exe` or `<prefix>/conec.exe` finds `<prefix>/packages/`.
   Requiring `core` rather than the bare name keeps an unrelated `packages`
   folder on the way up from being taken;
3. `CONE_PACKAGES_DIR`, the fallback compiled in: the CMake build gives it
   `${CMAKE_SOURCE_DIR}/packages/`, and a build that defines nothing looks in
   `packages/` under the current directory.

So the test runner and a direct run of a `conec` built from this repository find
`core` and `stdio` with no setup, from any directory, and a `conec` copied out
with a `packages/` folder beside or above it takes that folder with it.
`--path` adds to the search path and replaces nothing, so a package in
a `--path` folder is found ahead of the packages folder's of the same name;
`CONE_PACKAGES` replaces the packages folder itself.

**An import's name is answered in the registry first, then beside the importing
file, then on the search path** — so `import stdio` inside a submodule whose
parent holds a sister named `stdio` reaches the sister (`module_package_sister`),
and a `--path` folder's `stdio` wins over the packages folder's
(`module_package_path`).

**What is still special about `core` is that it is the prelude.** `parsePgm`
loads it before any module of the program (`parseLoadCore`), and every other
module is given a star import of it (`parseAddCorelibImport`). **It is loaded
from the search path alone, unless a build description's package line names
it**: then from that file, `core`'s generated include file, loaded as any file
an import line names is, declared and not generated ("A described build").
Missing from the search path, it ends the compile (`ExitNF`), naming where
the packages folder comes from. Its name is `core`, its folder's, so an
`import core` reaches the prelude module and binds that name; the IR dump reads
`module core`. No symbol is spelled after it, since everything it defines is
`inline` or `extern`.

**Until separate compilation lands, every module found on the search path is
compiled into this object.** That is the truth of today's single-object
compiler: a package is Cone source and nothing else supplies its definitions, so
`parseLoadAndParseModuleFile` gives `FlagGenMod` to a module `fileFindPackage`
found, `core` included, and withholds it from one `fileFindLocal` found beside its
importer. The root generates too, and a submodule generates exactly when its
parent does (`parseSubmoduleDraw` copies the parent's flag), so the one kind of
module denied it is an import reached relative to its importer, together with
every submodule of it (`module_import_submodule`).

That asymmetry is the whole of the separate-compilation gap, and both sides of
it are visible in emitted IR:

- `import stdio use *` emits `stdio.print` **and definitions** for
  `stdio.IOStream.appendStr` and its siblings, all internal, because the search
  path found it. The multi-module generation path works, and is exercised on
  every compile that prints.
- Importing a module found beside the importer emits **only `declare`s** —
  `declare i64 @_CNvC9modulesub8scaleInt(i64)`, read `modulesub.scaleInt` —
  because its bodies are never reached.
  Measured, that is the module's *public* surface whether or not the importer
  calls it: a public function nothing references is still declared, and a private
  one is declared only when a public inline body the importer generates reaches
  it. So what an import contributes today is already the
  shape of a `.h` file, derived from the imported source rather than from a
  reduced artifact.
- Compiling that same module as the root emits
  `define internal i64 @scaleInt(i64 %0)`, bare, because the root contributes
  no name to the owner chain its declarations are spelled from.

So **a symbol's identity depends on which compilation the module was the root
of**, and the two spellings never resolve against each other — unless the root
was compiled as a library from a build description, which names it and exports
its definitions ("A described build"). That is the one way an import is linked
against today: the importer's declarations resolve against the library's
object. How a symbol is spelled from its
declaration, and the linkage it gets, is
[Names and Namespaces](../../../../doc/design/names-and-namespaces.md), "Symbols".

### A described build

**Congo discovers; the compiler is told** [Jon 23 Sep]. Congo walks a package's
folders and reads each file's header, and hands the compiler a **build
description**: the module tree of one package, each module's files, and where
each module's imports are. The compiler loads exactly those files, checks what it
was told against each file's own `mod` line rather than trusting it, and never
searches. So the folder rules live in one place, Congo, and the folder sweep
above stays only until the test runner uses Congo's scanner.

**Congo's side of the contract** (`tools/congo/congo.py`, whose README is its
guide) is what makes a Congo build rely on nothing else here:

- **One description, one package, one `conec` run.** Congo writes
  `build/<mode>/<package>.conebuild` in the package being built, for it and for
  every package it imports, and runs `conec -o build/<mode> <description>` on
  each alone, the imported ones first. The object is named after the
  description, so it is `<package>.obj`. The package being built gets the
  `output` its manifest says; every package it imports is `output: library`.
- **Every path is absolute**, written with `/`. The root module holds
  `src/<name>.cone` first, then the files a folder sweep would have given it;
  each child module is written where a subfolder or a one-file module draws it.
  So the compiler's own designated-file rule is never asked about `src/`.
- **An import line names another package's generated include file**,
  `build/<mode>/<name>.cone`: every package of a Congo build is compiled into
  the one build folder of the package being built, each before what imports it,
  and each library's compile writes its include file there beside its object,
  so it is there, and current, when an importer's compile reads it. Congo
  deletes it before the compile and checks the compile wrote it. **Never a file
  written by hand**: one left at a package's root is not read, and Congo warns
  that it is not. A C package's include file is generated too, the same rule:
  its `@c` module's source with a banner ("How a C library becomes a Cone
  package", below). The line is written in the module whose file imports the
  package. A submodule's import of a sister, or of a name of its parent, gets no
  line: the registry answers it before the description is asked.
- **The package lines list the compile's whole dependency closure** [Jon 25
  Sep]: one top-level `import name: "path"` for every package the compiled
  package depends on, directly or through another package, `core` first and each
  after what it imports, each naming that package's generated include file as its import
  lines do. They are what an include file's own imports resolve against, since
  an include file is a module file like any other and may import. Congo takes
  the closure from the packages' sources, never from their include files.
- **A loop is refused before any `conec` run**, at both scales, from the header
  scan: between packages, and between the modules of one package — a sister
  imported or extended, a name of the parent imported, and each child its parent
  contains, the compiler's own edges ("The module order"). Its message names the
  loop the way `ErrorImportLoop` does. The compiler's check is the backstop for
  a direct run.
- **The prelude is described by its package line.** Every description but
  `core`'s own has one for `core`, naming the include file `core`'s compile
  generated, and `conec` loads the prelude from it (`parseLoadCore`), so a
  program is compiled against `core`'s include file as against any package's.
  An explicit `import core` names the same file, and the two are one module.
  `core`'s own description has no such line, since `core` is its root: the
  prelude is then loaded from the package search path, so Congo sets
  `CONE_PACKAGES` to the registry folder it found `core` in, and the root and
  the prelude are the very same file, one module, keyed by path. A description
  listing a *different* copy of `core` as a module's file meets the prelude's
  names as duplicates (`ErrorDupName`, one per name). Its object defines
  nothing, since all of `core` is `inline`, generic or `extern`.

`conec` takes a description where it would take a source file, told apart by its
extension, `.conebuild`:

```
build: debug
output: library
import core: "core.cone"
import geometry: "geometry.cone"
import stdio: "stdio.cone"
q: {
    "../../src/q.cone"
    "../../src/more.cone"
    import stdio: "stdio.cone"
    inner: {
        "../../src/inner.cone"
    }
}
```

(Written by Congo into `q/build/debug/`, beside the include files the earlier
compiles generated, it names every path absolutely; they are relative here only
to be read.)

**The format** is Jon's brace shape [Jon 23 Sep], read by the compiler's own
lexer (`parser/parsebuild.c`), so a comment is a Cone comment and a path is a
Cone string — a backslash begins an escape, so paths are written with `/`.

- **Settings first**, each at most once: `build` is `debug` or `release`, and
  sets what `--debug` sets, so the description is read before generation is set
  up; `output` is `executable` (the default) or `library`, and a library
  compile also writes its package's include file into the output directory
  ("Generating the include file").
- **Then the package lines**, `import name: "path"` at the top level, one per
  package of the compile's dependency closure: where each package's include
  file is, for the imports an include file writes. In the example, `q` imports
  only `stdio`, and `geometry` is there because `stdio`'s include file (say)
  imports it. The line for `core`, where there is one, is also where the prelude
  is loaded from. A package line comes before the module, as a setting does.
- **Then the package's one module**, `name: { ... }`, whose body holds three
  kinds of line: a quoted path is a file of the module, `name: { ... }` a child
  module, and `import name: "path"` where this module's `import name` is found.
  Imports are per module, since each module has its own. A relative path is
  relative to the description's folder.
- A malformed line, a name written twice, a setting or a package line after the
  module, a module's import line naming a different file from the package line
  of the same name, and a module listing no file are each `ErrorBuildDesc`, one
  code with the cause in the message. Nothing is compiled against a description
  with an error in it.

**What it builds** is the same module tree a folder gives, by a different route
(`parseBuildModuleTree`, `parseBuildSubmoduleDraw`): each module named by the
description, holding the files it lists in the order listed, its first file the
one whose `mod` line may declare it; each child owned by its parent, bound in the
parent's namespace, private to it until its own `mod` line says `pub`, drawn
before any is parsed and before the parent's own files, in the order written.
The file registry, `core`'s auto-import and every later phase are unchanged.
Nothing is swept and nothing is probed: a file beside a listed one is not
compiled.

**The root is named by the description.** Under `output: executable` it still
contributes nothing to a symbol, so `main` links; under `output: library` it
carries `DclNamesChain`, so a package built on its own spells its declarations
as an importer spells them — `q.addOne`, `_CNvC1q6addOne` — rather than bare.
That is the spelling half of separate compilation. **The linkage half is
built too:** `output: library` sets `opt->library`, so the object is
position-independent and a library compile's rule decides linkage
(`dclIsExported`, [Generation](../phases/generation.md), "Symbols, linkage
and COMDATs"). The package's own modules — the root and its children, never
`core` — export every public function and global, every function of a type an
importer can reach, and every private definition that a body an importer
expands names: an `inline`, generic or macro body, or a trait's default, whose
copy in the importer calls the private symbol it reached (the importer's side
of that is `genlFnSym` and `genlVarSym`). Everything else is internal as in a
program. So a package compiled alone links with a program compiled against its
include file: `module_build_link` builds both, links them and runs the program,
and `module_build_export` pins each case of the rule.

**A generic's instances are shared, in every described build.** A description
also sets `opt->described`, for a program as for a library, and each object then
defines every instance it uses — the package the ones it uses itself, an
importer the ones it makes from the body its include file carries, a module it
does not generate notwithstanding (`genlImportedInstances`) — as `linkonce_odr`
with a COMDAT of kind `any`, so identical instances in several objects merge
at link. The members of a generic type's instance are shared with it, and so
is every vtable, which each object coercing a type to a trait builds and whose
address pattern matching compares. `module_build_link` exercises an instance
both objects define, one only the program does, and a virtual reference built
in the program and tested in the package.

**An include file declares with `extern`.** Each function, method, operator
and global the package's object defines is written there `extern` and without a
body — the private ones an `inline` body reaches too — and, the include file's
module being Cone-named like the package's, each is spelled with the package's
Cone name, the one the object exports (`module_build_link`,
`module_extern_cone_names`). `extern` says only "defined elsewhere"; the naming
is the module's. What an importer must have the body of — an `inline` or
generic function, a macro, a trait's methods, a generic type's — is written
whole, and `extern` on it is `ErrorBadExtern`. **A global the package folds
through is declared `extern` with the source's fold clause** [Jon 25 Sep],
`pub extern mut config Config pub use *;`: the fold reads only the declared
type, so an importer reaches the folded names as the package does, each the
package's one global (`module_build_fold_extern`; the refusals where the type
is out of view, `module_build_fold_extern_nameres`).

**An import in a described module is answered by the description.** After the
registry — a sister, a module the parent bound — `parseImport` looks up the
module's import line for the name, and loads the file it names as a module named
by the import, one file swept for nothing, declared and not generated
(`parseLoadBuildImport`): it stands for another package's include file. A file
two import lines name is one module, read once. A submodule's other bare names
are its parent's, as anywhere. Anything else — a name the description gives no
line for, `stdio` included, or a quoted path — is `ErrorBuildImport`. `core` is
loaded from the package search path as always.

**An include file imports like any module file** [Jon 25 Sep: an include file
*"is in fact a mod file… Starts off mod… and then you can have [imports] after
it"*]. The module an import line loads is answered from the description's
**package lines** (`parseBuildImportModule` gives its entry those lines), so its
imports load further include files the same way, each read once by path, to
whatever depth the closure has; one the package lines do not name is
`ErrorBuildImport`, naming the missing package line. Include files that import
each other loop like any two modules (`ErrorImportLoop`,
`module_build_transitive_loop`), whether or not Congo's package-level check,
which reads the packages' sources, saw it. **The package lines answer only an
include file's imports.** The compiled package's own modules import only what
their own lines give them, so a package in the closure that the program does
not import is in view to the include files that import it and to nothing else:
the program's `import b` with only a package line for `b` is `ErrorBuildImport`,
`b` is not a name the program has, and `a.b` is `ErrorNotPublic` where `a`'s
include file imports `b` without `pub`, since a module-level import binds the
module in the importing module's namespace with the import's visibility
(`module_build_transitive_parse`, `module_build_transitive_nameres`). Values of
`b`'s types still reach the program through `a`'s functions, with their fields
and methods (`module_build_transitive`). Under Congo the difference never
shows: a program that writes `import b` gets its own line for it.

### Generating the include file

**A library compile writes its package's include file** [Jon 23 Sep: include
files are *"hand-written at first and generated soon after"*; the seven
questions ruled 25 Sep]: `<package>.cone` in the output directory [Q3], the Cone
source a program compiles against in place of the package's source. It is
written when the build description says `output: library`, or when
`--emit-include` asks, after type check and only where there were no errors
(`conec.c`). It is generated before any code is, so that what it cannot declare
fails the compile with no object written, and checked and written after.
**Congo compiles every dependent against it**, never against a file written by
hand: each library's compile writes it into `build/<mode>/`, and the importers'
descriptions name it there, `core`'s and `stdio`'s included ("A described
build"). Include files written by hand are still legal wherever a description
names one, as the suite's scenarios do.

**It is the author's own text, edited; nothing is printed from the IR**
(`ir/incfile.c`, `incFileGenerate`). By type check the parser has desugared
bodies and type check has lowered them, so the IR no longer reads as the source
did. The parser instead records where each statement sits (`ir/dclspan.h`):
one span per module-level statement, in a side list on the module, and one per
member of a type's braces, on the type — where it starts (at its `pub`), where
its keyword is, where its body or value starts and ends, and where it ends, and
for a global where its name ends and whether its type is written. `--spans`
prints the root module's, which is how they were measured. The IR then decides,
statement by statement, whether the text goes in whole, goes in cut to an
`extern` declaration, or stays out:

- **Whole**, where an importer expands the body: an `inline` or generic
  function, a macro, a trait's methods, a generic type's, an enum's own
  (`fnDclIsExpanded`).
- **Cut to `extern`**, where the object defines and exports it
  (`dclIsExported`, the rule generation follows, so the object and the include
  file cannot disagree [Q5]): `extern` is written in before the keyword, and the
  body from its `{`, or the value from its `=`, is left out. A global's fold
  clause stays [Q4], and a module-level `static` becomes `extern`.
- **Out**, anything else, with the comments directly above it, no blank line
  between. A private declaration nothing an importer expands names is not
  exported, and so not declared.

**Beyond the exported set**, three kinds go in because an importer's compile
reads them without naming them: a global the module's finalizer drops, so the
importer derives the package's `drop` as the package did ("Init and final"); a
type's `final` and `clone`, which decide what dropping or copying a value
does; and a method meeting a requirement of a trait its type is, which the
importer's type check asks for. The last two are exported too, public or not
(`fnIsTypeLifecycle`, `fnIsTraitMethod`), since an importer's drop and its
vtables call them. And **a type goes in whenever an included declaration names
it** — in a signature, a global's type, a field — so a private type a public
one holds is declared, fields and all.

**Types keep every field**, private ones included, since an importer lays the
type out; each member is decided by the rules above, and an enum's variants
likewise. **As written**: the `mod` line (its `extends`, `is` and default fold
with it), every import [Q7: all of them for now; pruning is later] — all in the
first file's header, since a module's other files have none [Jon 23 Sep] — and
each typedef, const, macro and module trait, which
declare no symbol and whose uses nothing records, so a private one stays in too.
A standalone `use` stays where what it names is in the file or another
package's, and goes where it privately folds a submodule's names.

**A global whose type was inferred gets its type written in** [Q6], the one
thing printed from the IR: a number type, a struct — an instance of a generic
one with its arguments, one of the package's own by its path from the module
declaring the global (bare, `vec.Stack`, a sister's `vec.Stack` or deeper),
another package's through its module's name, `core`'s bare — or an array,
`[2; u8]`. A type none of those is refused (`ErrorIncCheck`), asking for the
type to be written. **So is a type another package holds in a submodule**, even
where that package's root re-exports it: it is `coll.vec.Stack`, and nobody
outside `coll` may name `coll.vec`. The generator does not look for the
re-exported name; written as `coll.Stack`, the type is copied
(`module_include_infer_reject`).

**Every comment in the kept text is kept** [Q2], those between declarations
included, and a banner at the top says the file is generated, from which of
the package's files, and not to edit it. A generic module's include file is its
source whole, since every declaration of it is instantiated where it is used.

**What the root reaches in one of its submodules goes in a nested module
block** [Jon 25 Sep, Q1: *"Sure, we can go with B. The important part is we want
stuff to work correctly."*]: `mod vec { ... }`, written after the root's header,
holding the submodule's own text edited by the same rules, and nested as deep as
the submodules are — `mod map { ... mod slot { ... } ... }`. **Only what is
reached goes in**, and a submodule nothing reaches has no block. Because the
block is a submodule of the include file's module, every name in it is spelled
after its real owner — `coll.map.slot.Slot.doubled` — exactly as the package's
object spells it, and `pub use vec Stack` in the root copies as written. **The
block is private**: its `mod` line loses its `pub` and its default fold, keeping
its `@c`, `extends` and `is`, so no importer can name `coll.vec`; a submodule is
private to its package. What is reached, transitively:

- **A type** an included declaration names — in a signature, a global's type, a
  field, a type's base — wherever in the package it is declared; it goes in with
  every field, and its members by the root's rules.
- **What a body the file copies names.** Name resolution records, for each
  expanded body — inline, generic, macro, trait default — each declaration it
  names (`exportReachAdd`, `exportReachesOf`): a function, global or type, and a
  macro, typedef or const too, which have no symbol and so no
  `DclExpandReached`. It records **a parameter's default value** the same way,
  against its function, since an importer evaluates the default where it calls
  (`NameResState.sigfn`). The generator follows the record from every function,
  macro and default the file holds.
- **What a `pub use` at the root re-exports**: each name a list names, every
  public name a submodule's star fold brings, an enum and its variants. A list
  anywhere — a `use`, an import of a sister — brings what it names, so the fold
  finds it.
- **A submodule's `init`, `final` and each global its finalizer drops**, which the
  program's stitched pair calls or derives from, so a submodule's `init` now runs
  in a program that imports the package.
- **The blocks around a block**: a nested submodule's parent, a sister a block
  imports or extends. A block's import of a sister with no block is dropped, and
  so is a `use` of a submodule or enum the file does not hold.

A module conforming to a module trait (`mod x is T`) keeps every declaration,
since the trait decides which it needs. **A generic submodule's block is its
whole text**, as a generic root's file is: every declaration is instantiated
where it is used. What that text reaches in a sister is not recorded, so a
sister it imports goes in whole. The blocks come in the program's module order,
so a sister a block imports is ahead of it, and the block's import finds her in
the registry as a folder's would.

**A type the include file declares is marked `DclIncluded`**, and the export
rule treats it as it treats a type an expanded body names: its public methods,
`final`, `clone` and trait methods are exported (`dclIsExported`). An importer
holds values of every such type, through a field or a signature, whether or
not it can name the type, and calls its public methods through them
(`module_include_field_reach`: a private type held in a public field).

**The file is checked before it is written.** It is parsed as the package's
module beside the program (`parseIncludeCheck`), its blocks' modules beside it
and kept out of the program's list, its imports answered by the import lines of
the root and its submodules, and name-resolved against the program's modules as
they are (`pgmNameResAlone`, `modFoldAlone`, which folds them without folding
the program's again). A failure is the generator's, not the author's: it is
`ErrorIncCheck`, the text goes to `<package>.cone.rejected`, where what did not
resolve is reported, and no include file is written. No known package fails it
now: every reach it once caught is recorded. **Nor does it ever overwrite a
source of the compile** (`ErrorIncWrite`, checked before anything is generated,
`driver_include_over_source`).

**Where a module block may be written.** Source may not write one: a nested
module is a file or a folder ("The `mod` declaration"), and the block there is
`ErrorUnbuiltKind` as it always was. A **generated include file** may, and it is
known by two things together (`ParseState.generated`): its **role**, a file a
build description's import line names, which is an include file; and its
**banner**, whose first words are the generator's (`IncFileBanner`,
`incFileIsGenerated`). The banner alone does not make a source file an include
file, and a hand-written include file has no banner, so each is refused as
source is; and `pub` on a block is `ErrorBadPub` (`module_include_block_parse`).
A block is a submodule in every respect a drawn one is (`parseModuleBlock`):
owned by the module it is written in, bound in its namespace, given `core`,
parsed with its own namespace hooked, generated when its parent is — never, in an
include file.

`module_build_link`, `module_init_link`, `module_generic_link`,
`module_include_roundtrip`, `module_include_nested` and
`module_include_field_reach` compile programs against the include files their
packages generate, each pinned as a golden file the program's description names
(the runner's `include` key), and link and run them. `module_include_nested` is
a collections package — `vec`, `map` holding `slot`, a one-file `kinds`, a
generic `pair`, and a `util` nothing reaches — re-exported at its root.

### What an import reaches

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
  `parseLoadAndParseModuleFile` locates, registers and parses it: beside the
  importing file first, then on the package search path. That is how an external
  module is reached, and how a package is — `stdio`, from the packages folder.
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
  refused at parse (`ErrorDupImport`), as a module imported twice is. **Every
  such import is a dependency on the parent, and so a loop, refused**
  (`ErrorImportLoop`, "The module order"); the binding is still made, so that
  the loop is the one thing reported.

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
this way is a module on the package search path: a package of the packages
folder, `stdio` among them, or a module in a `--path` folder. Where the parent
answers the same name with something else, the import is `ErrorDupName` in the
pass that reports (`importCheckNamedFile`), rather than meaning whichever was
parsed first; where the parent binds the same module, the two are one.

## Name resolution

**Every module's FOLDS run before any module's body is resolved.** `pgmNameRes`
runs the folds of every module (`modFoldAll`), and only then `modNameRes` on
each. `modFoldNames` is dependency-first — for what the module extends and for
each import it folds the source module's own names first, recursively — so a module's folds are complete before
anything folds from it, which is what makes a re-export transit. Imports form a
DAG, a loop refused before the first fold runs ("The module order", below), so
dependency-first can always be had.

**A module's folds resolve in that module's names alone**, whichever module's
pass reached them. Dependency-first means one module's folds often run from
inside another's — a parent's `use` of its child, an import of a sister — and
`modHook` hooks the inner module's namespace *in place of* what is hooked, not
over it ([Name Resolution](../phases/name-resolution.md), "Hooking"), so a
`use Dir;`, a global's fold type and its value, and a type demanded from another
module see nothing of the module that got there first. That is "nothing arrives
unasked" holding inside the fold pass as it does in a body; `module_fold_scope_nameres`
pins it from a child and from a sister.

**Where a fold waits, the folds run again until they settle.** Dependency-first
orders modules, not the folds within one: a module's folds run in a fixed order
(below), so a global whose type only a later standalone `use` of the same module
brings WAITS in the first pass, and is folded in the next — and a sister that
wildcard-imported the module in the first pass takes the global's re-export only
then (`module_use_submodule`). Every module's own *declarations* are bound at
parse, so what is late is a fold and never a declaration. So `modFoldAll`
repeats the pass over the module list until one binds nothing new — a fixpoint,
the way Rust resolves glob imports. What makes that cheap is `modFoldBind`: two
bindings of the same declaration under one name are one binding, so a pass
re-binding what an earlier pass bound changes nothing, and "nothing new" is the
whole test. A pass counts as new a name it bound and a binding it made public or
made a fold (`FlagImportName` cleared), since either lets a module folding from
this one take more. A namespace only grows and a binding only becomes more
visible, so the passes end. Another pass is run only where a pass left a fold
waiting, or met a module mid-fold (`folding`) round a loop already refused, and
made progress: a program whose folds all find what they name takes one pass.
▸ **The same passes are what keep a refused loop to one diagnostic.** A loop is
not cut (unless it is `extends` alone), so round it one module reads another
mid-fold, and the passes carry each re-export round it as they carry a late
fold; nothing the loop cut short is then reported missing beside it
(`module_import_cycle`, `module_extends_back`).

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
or does not show, an unknown type or enum. So a `but`, a listed name and an `as`
are judged against the module once it is complete.

**A collision is reported once, where a single pass would have met it.** Two
different declarations under one name is final the moment it is met — neither
binding ever leaves — so it is reported then, and a star clause keeps an item
under the name so that a later pass passes it over rather than reporting it
again (`importStarHas`; a listed item is made once). A later pass can bind a
later fold's name before an earlier fold's arrives, so the collision is
reported at the fold that comes second in the order a module's folds run —
`extends`, then the imports, the globals and the enum `use`s, each in the order
written — not at whichever arrived second (`modFoldCollisionAt`). Which pass met
it does not change what is reported. **The one exception is the core import**,
which is written nowhere (`ImportNode.iscore`): where the collision would be
reported at its fold, which has no place in any source, it is reported at the
other binding instead — the declaration, the module's `mod` line, the import —
saying that core has the name (`module_builtin_name_nameres`).

**That order is what stops the file load order deciding what a name means.** A
module's folds used to run at the start of its own name resolution, and modules
are resolved in the order they were loaded — so a module resolved earlier, the
root among them, looked a folded name up before it was there, and one resolved
later found it.

**An import of a name of the parent is bound among the imports, in the order
written** (`importBindName`) [Jon 23 Sep], though it closes a loop and has been
refused already ("The module order"): binding it keeps the child's body from
reporting what the loop alone caused. It reads the parent's namespace, so
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
(`modExtendsResolve`), and then the modules are put in dependency order, a loop
refused (`pgmModuleOrder`), so that every edge the fold pass follows is known
before the first fold runs.

### The module order

**Imports form a DAG at every scale, sister modules inside a package as well as
packages** [Jon 23 Sep: *"Why would we ever want circular imports. Modules need
to have an explicit dag order."*]. `pgmModuleOrder` (`ir/stmt/program.c`) walks
every module's dependencies depth first, once `modExtendsResolve` has bound
what each `extends` names — the one edge not known at parse — and places each
module after everything it depends on, in `pgm->initorder`. **That order is the
order module `init`s run in** [Jon 23 Sep], and finalizers run in its exact
reverse (`genlStitch`, "Init and final"). A module depends on:

| Edge | From the IR |
| --- | --- |
| each module it imports — a package, a sister, a file | an import's `module` |
| the module holding a name it imports: `import Point;` in a submodule depends on the parent | an import's `binding`, to the module's owner. A module the parent binds by an import is such a name too |
| the module it extends | `extends->module` |
| each of its own submodules: a parent depends on its children | a module whose `dclinfo.owner` is this one |

**Containment is a dependency** because a program is a hierarchical
decomposition — *"That's the whole reason we have something called dependency
injection"* [Jon 23 Sep] — so a child that depends on its parent closes a loop of
two, and what they share moves into a sister both import. A module's standalone
`use` of a submodule needs no edge of its own (containment is it), and nor does
an enum's `use`. The prelude's import of itself, where `core` is the module a
build description compiles, is no edge.

**A loop is `ErrorImportLoop`**, one per edge that closes one, reported at that
edge — the import or the `extends` — or, where it closes through containment,
at the first edge round the loop that something wrote. The message names the
modules round it, each by its path from the root, and each step:

```
Import loop between modules: q -> q.inner -> q. Imports between modules must not
loop (q contains q.inner; q.inner imports Point of q at src/inner.cone:3). A module
may not depend on a module that contains it: move what they share into a sister
both import.
```

The walk goes on past a loop, so every module still gets a place. **A loop of
`extends` alone is cut** where it closed (`extends` cleared), since each module
would be adding to the other and neither has a surface to start from; any other
loop is left whole, and the fold passes carry the names round it, so the loop is
the one thing reported. Congo refuses the same loops before `conec` runs
("Congo's side of the contract"); this is the backstop for a direct run. Which
walk starts where — the program's module list, root first — decides which edge
closes a loop, and so where it is reported, never whether.

**An instance of a generic module is placed once type check has made it**
(`pgmInstanceOrder`, from `pgmTypeCheck`), since the walk runs at name
resolution, before any instance exists. It goes straight after the last of
what it depends on [Jon 23 Sep]: its generic — which the walk placed after
everything the generic imports, so the instance follows those too — each module
its type arguments come from (`modTypeArgModules`, through references, arrays
and tuples to each named type's module), and each instance made while its own
body was checked, which is one it uses. Instances that follow the same module
keep the order they were made in. So an instance runs its `init` before every
module that follows those and uses it — **except a module that supplied one of
its type arguments**, which the instance follows: in `module_generic`,
`tally[user.Tag]` runs its `init` after `user`'s. A module that uses an instance
places no edge of its own: its place was fixed at name resolution. Instances
that use one another round a loop are placed last, in the order made.

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
[Names and Namespaces](../../../../doc/design/names-and-namespaces.md), "A module extending a
module", which owns the rules.

**A global's `use` clause is a module's third fold**, and the whole of it is
`foldGlobalExpand` (`ir/stmt/fold.c`): the global's type supplies the members,
every entry is an `AliasDclNode` carrying the global as its `through`, and a use
of the name is lowered to `global.name`. The rules, the diagnostics and why it is
cheaper than a field's fold are in
[Names and Namespaces](../../../../doc/design/names-and-namespaces.md), "Folding through a
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
[Names and Namespaces](../../../../doc/design/names-and-namespaces.md), "Folding an enum's
variants into a module" and "Folding a submodule's names into a module".

**A type alias's target is resolved in a pass of its own, and then checked for a
cycle.** A forward reference to a `typedef` is ordinary, so the target has to be
bound before anything asks whether the name is a type at all; `aliasDclCheckCycle`
then reports a chain that comes back to itself and cuts it, so no later walk
loops.

`importNameRes` does nothing unless the import carries a fold clause, or writes
none and its module has a default fold. **A default fold is copied onto the
import** the first time it is folded (`importDefaultFold`), because a clause's
items are the bindings it makes and each importer makes its own: under the
import's visibility, so `pub import bigint;` re-exports the default's names with
`bigint`; positioned at the import, so a collision with a name of the importer is
the ordinary `ErrorDupName` there, whose advice (`as`, `but`) now means writing a
clause of its own; and marked `FlagUnlisted`, since the imported module wrote the
names and the importer did not, so the same declaration arriving by another route
merges with it. It is taken wherever an import names a module and writes no
clause — a sister, a module bound through the parent's registry, a file — and not
by an `extends` or a standalone `use` of a submodule, each of which carries its
own clause. **What the default names is judged once, at the `mod` line**
(`modDefaultFoldCheck`, run by `modFoldNames` in the pass that reports, after the
module's own folds): each listed name and each `but` name must be a public name
of the module — `ErrorNoMbr` for one it has not got, `ErrorNotPublic` for a
private one, `ErrorBadFold` for the module's own name, `ErrorDupName` for a name
listed twice. An import taking the default passes the same names over
(`isdefault`) rather than reporting them again at every importer.

Once an import has a clause, a star clause has `foldStarItems` make an item per public name of the source
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
  imported and listed (`import meter; import relay use meter;`), a global's
  clause listing a member twice. "If you've got two different ways of bringing in the same thing,
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
each re-export `c`'s `x` — and the core fold an extending module takes from its
base meeting its own; neither the outcome nor the
visibility depends on fold order. **Different declarations under one name collide
everywhere**: `ErrorDupName`, reported at the listed item or, for a star clause, at
its `use` (or `ErrorExtendsOverride` for `extends`, above), and so does one member
folded through two different globals, since each is reached through its own. An
overload name folds as one
node, the `FnOverloadDclNode`, with its candidates riding inside it; a public name
holds only public candidates (`ErrorPrivOverload`), so the fold carries nothing
private.

## Type check

`modTypeCheck` type checks the imported modules first, then — where the module
conforms to a module trait — what it has for each member against the member's
shape (`modTraitCheck`, "Module traits"), then every declaration
the module owns, in source order, and right after an enum that extends another,
that enum's copies of its base's variants, which the module does not own
([struct](struct.md), "An enum extending an enum"). As everywhere in this phase,
**order decides when a declaration is checked, not whether** — a name reached
from elsewhere pulls its declaration forward. See
[Type Check Phase](../phases/type-check.md). **Last, once every global's type is
settled, `modLifecycle`** finds and checks the module's `init` and `final`, and
gives it the `drop` that finalizes its globals ("Init and final", below). A
generic module stops after its imports: only its instances are checked, each as
it is made ("Generic modules").

**No program reaching type check has an import loop**: name resolution refused
it ("The module order"), and a name-resolution error ends the compile before
type check. Reuse by file in the registry is what stops the parser recursing
forever round one before that.

## Flow and generation

Flow analysis has no module concept; it runs per function body. The one
exception is a module's `init`, whose pass starts with the module's globals
without a value holding nothing ("Init and final").

`genlProgram` is two strict passes over `pgm->modules`:

1. **Symbols.** Every module, generating or not. A declaration is skipped only
   when it is private *and* its module is not generating. A public inline body
   is generated in each caller, so a private function, method or global it names
   is declared there on first use instead (`genlFnSym`, `genlVarSym`).
2. **Implementations.** Only modules flagged `FlagGenMod`.

Both passes pass a generic module over: what it compiles to is its instances,
each a module of the program by then, flagged `FlagGenMod` whatever its generic
is, since every object that uses an instance defines it ("Generic modules").

**The root is flagged `FlagGenMod`, and so is a module found on the package
search path; a module an import found beside its importer is not, nor is one a
build description's import line names, and a submodule is flagged exactly as
its parent is.** A submodule of the root is
part of the program the compiler was pointed at — its bodies belong in this
object exactly as a swept file's do — and a submodule of an import is part of
that import, declared or generated with it. A package is
compiled in until separate compilation lands ("The packages folder" above),
where a module found beside its importer is taken as supplied from elsewhere and
only declared. ▸ **So a program spanning a module TREE, or importing a package,
links and runs today, and one spanning any other import does not**, which is
what lets a folder scenario reaching two levels of submodule be a `run` scenario,
and `module_package_path` too.

`ImportTag` is an explicit no-op in `genlGlobalImpl`. A module not flagged
`FlagGenMod` still has one kind of body generated here: the instances this
compile made of its generics (`genlImportedInstances`), since its package has
none for an importer to link against. `genlLinkage` makes every definition of a
program internal except `main` and a public C-named one, and leaves an imported
module's declarations external. A library compile exports what its importers
link against (`dclIsExported`, "A described build"), and a described build
makes an instance of a generic and a vtable `linkonce_odr` with a COMDAT of
`any`, so that each object's copy merges.

The privacy filter in pass 1 assumes nothing outside a module can reach its
private names, and a public overload name cannot break that assumption: a
private candidate may not join one (`ErrorPrivOverload`), so an
`FnOverloadDclNode` generates nothing of its own, and each candidate is
generated as the module's or type's node it also is.

## Init and final

**Each module declares only its own portion, and the compiler stitches them
together** [Jon 23 Sep: *"every mod can only tell you about its own state"*]. A
module's `init` sets up its own globals and its `final` releases its own state;
neither knows about any other module's. The program runs every module's `init`,
dependencies first and the root last, before its own work, and every module's
finalizer in exactly the reverse, the root first, after it.

```
imm handler Handler;

fn @initpure init() {
    handler = Handler[1];
}

fn final() {
    ...
}
```

**The declarations** (`modLifecycle`, at the end of `modTypeCheck`) are the
module's own function `init`, written `fn @initpure init()` as the manual writes
it, and its own function `final`, written `fn final()` by analogy with a type's
`final`: no parameters, returning nothing, not generic, not `inline` and with no
overload name, since the stitched functions call each with nothing to pass and
nothing to receive. Anything else under either name at module scope is
`ErrorModLifecycle`, with the cause in the message — a function of another
shape, `init` without `@initpure`, a global named `final`. A name a fold
brought is another module's `init` and is not this module's. Either may be
`extern` (an include file's), and either may be private: the stitched functions
reach them however they are declared. A module may declare either, both or
neither. Each well-formed one is marked `DclLifecycle`.

**`@initpure`** is an attribute after `fn`, before or after `@c`: a function a
module's `init` may call, as `init` itself is. The parser records it
(`DclInitPure`, kept by `dclInfoJoin`) and **nothing checks it**: the rule that
such a function call only `pure` or `initpure` functions needs `pure`, which is
not built.

**Every global without an initial value must be assigned by its module's
`init`** (refmodule.html). The check is `init`'s own data flow pass
(`fnDclTypeCheck` asks `modInitOf`): `modInitFlowBegin` clears `VarInitialized`
on each global the module declares without a value — an `extern` one excepted,
its value being another object's — so inside `init` such a global is exactly an
uninitialized local. Its first assignment is allowed, an `imm` one's included,
and carries `FlagFirstAssign`, so nothing is released from the zeroed storage; a
second assignment of an `imm` one is `ErrorNoMut`; and a read before any
assignment is flow's own `ErrorMove`, "has not been initialized", which is the
manual's rule that `init` read no uninitialized global of its module, for `init`
itself. `modInitFlowEnd` then reports each global still unassigned as
`ErrorGlobalUninit` and puts back every global's flags, so to every other
function each global holds a value, as the parser recorded. **Assigned anywhere
in the body counts** — flow's `VarInitialized` is a whole-function summary —
which is what the manual says: assigned "at some point" in `init`. A field
assignment does not assign the global. A global without a value in a module that
declares no `init` is `ErrorGlobalUninit` too, reported by `modLifecycle`;
where the module's `init` is malformed, only that is reported. Assigning such a
global in any other function does not count, and an `imm` one may not be
assigned there at all.

**The module's finalizer** mirrors a type's ([struct](struct.md), step 8).
Where any global the module declares has a type with a drop function,
`modGiveDrop` gives the module a `drop` — owned by it, so its symbol is spelled
after it (`middle.drop`, `_CNvC6middle4drop`), appended to its `nodes` after name
resolution, built pre-lowered and never type checked or flow analyzed — calling
the module's own `final`, if it has one, and then each such global's drop
function over `&uni` the global, **in declaration order**, as a type drops its
fields. Its finalizer (`finalfn`) is that `drop`, else its own `final`. A
C-named global is C's storage and is not finalized. A global's drop runs whether
the global was given a literal or assigned by `init`, since either way it holds
a value by then. A module that needs a `drop` and declares one of its own is
`ErrorModLifecycle`: both would be one symbol. Only drop functions count: a
global that is an owning reference is not freed, as a struct field that is one
is not.

**The stitched pair** (`genlStitch`, `genllvm/genllvm.c`) is two functions of the
object being generated: `cone.initAll`, calling `initfn` of each module in
`pgm->initorder` that has one, and `cone.finalAll`, calling `finalfn` of each in
the reverse. A module with neither gets no call. Each is internal, made only
when a call asks for it (`genlStitchFn`), and built last, after every module's
bodies. **A program calls them through two compiler-provided functions,
`initAll()` and `finalAll()`** (`corelib.c`, `InitAllIntrinsic` and
`FinalAllIntrinsic`): bound as names every module reaches, as it reaches `i64`.
Only a local declaration of the same name hides them; a module-level one is
refused as a duplicate at parse (`modAddNamedNode`), as one named `i64` is —
Jon, 24 September 2026: the compiler is right, and they hold that place for now
([Name Resolution](../phases/name-resolution.md), "Some bindings are never hooked").
They stand in for the entry glue until it is built; nothing calls them
implicitly, and how an executable's C `main` is chosen is unchanged. Nothing
stops them being called twice. Where they end up, since user code should not
call them once the entry glue does, belongs to the entry-trait conversation.

**Across separately compiled packages**, a package's `init`, `final` and `drop`
keep the package's Cone names (`lib.init`, `lib.drop`), and a library compile
exports each whatever its visibility (`DclLifecycle` in `dclIsExported`). The
program's compile sees the package through its include file, which declares
them: `extern fn @initpure init();` where the package has an `init`,
`extern fn final();` where it has a `final`, and each global the package's
finalizer drops. The program then derives the package's finalizer from the
include file exactly as the package's compile derives it from its source — a
`drop` where a declared global finalizes, else `final` — and its stitched pair
calls the declared symbols (`module_init_link`). An include file that declares
less than its package has leaks rather than misbehaves: an `init` it omits is not
run, and where it omits the global a `drop` finalizes, the program calls
`final` alone; one that declares more fails to link. **A type's `final` and
`clone` are exported the same way** when an importer can reach the type, public
or not (`fnIsTypeLifecycle`): a program that drops or copies a value of the type
calls them without naming them (`module_init_link` drops a `lib.Handle`).

**An instance of a generic module has its own `init` and `final`**, stitched in
its place in the order ("The module order"). Across separately compiled
packages each object's copy of an instance's `init` is the one merged function,
and the program's stitched init calls it once, where the program names that
instance itself (`module_generic_link`). ⚠ **An instance only a package uses is
invisible to the program**: the package's include file says nothing of it, so
the program's stitched pair makes no call to its `init` or finalizer, and a
global it would assign keeps the zero it was stored with — measured at the IR,
not at run time. By the rule above it leaks rather than misbehaves; what closes
it is open.

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
layers surface them — is in [Modularity](../../../../doc/design/modularity.md), which
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
- **Package dependencies form a DAG, and so do the dependencies of the modules
  inside a package** [Jon 23 Sep], containment counting as one: a parent
  depends on its children. Built: "The module order" above.

Two consequences of the package being the compilation unit are worth stating,
because they remove work rather than adding it:

- **No declaration needs an owning source file.** A generic instance, an
  expanded macro and a cloned trait default method have use sites rather than a
  home file, and with one object per package the question never arises.
- **Deduplication happens in the IR, not in the linker.** `genericinfo`'s
  `memonodes` memoizes an instantiation on the generic's declaration, matched by
  argument types, so twenty files instantiating `Option[i32]` produce one
  instance and one symbol. `linkonce_odr` (`LLVMLinkOnceODRLinkage`, built
  for a described build) is therefore a **cross-package** mechanism only.

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
  qualifying it with the module's name. **A program's root carries a `mod` line,
  as every module's first file does** [Jon 24 Sep], and naming the root changes
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
shared by everything in the module — and a generic module holds one copy per
instantiation as a generic type's instances each hold their own. That
distinction is what makes a module the natural shape for a region or a
subsystem and a type the natural shape for a value, and it is why a module
cannot be nested inside a type. A module holding a single type is therefore
not a special construct — it is a type sitting at the package's top level.

### Source files and folders

A module's source files are the files of a folder, and the folder tree carries
the module tree:

- **A module folder holds one designated file, named for the folder** —
  `matrix/matrix.cone`. It carries the folder module's `mod` declaration. Every
  other `.cone` file in the folder belongs to that module, unless it declares a
  module of its own.
- **A subfolder is a submodule when it holds its own designated file**, and
  organizational otherwise. An organizational folder's files, at
  any depth beneath it, belong to the enclosing module. This is what lets a
  forty-file module group its files by topic without minting namespaces for
  them.
- **A file whose first statement is `mod` is a submodule of one file** [Jon 23
  Sep: "Why do a whole ceremony around folders if there's only one file in it?"].
  `lexer.cone` holding `mod lexer;` in a module's folder is in every respect the
  submodule `lexer/lexer.cone` would draw, and growing it into that folder
  changes nothing for anyone who names it. A file and a folder of one name in one
  parent are an error naming both.
- **A module must sit directly in its parent module's folder**, as a file there
  or as a direct child folder. So the module tree's *shape* mirrors the folder
  tree's. A designated file or a one-file module found beneath an organizational
  folder is an error, not a deeper submodule.
- **Every file the compiler builds as a module opens with its `mod` line** [Jon
  24 Sep] — a lone file, a module's root, a folder module's designated file, a
  one-file submodule — and **every file's imports come right after it**, ahead
  of everything else the file declares. The other files of a folder module, and
  the files of its organisational subfolders, carry no `mod` line: the folder
  names the module they join. So a file's header is comments, the `mod` line,
  the imports, which is all Congo reads.
- **A module's name is its folder's name, or its one file's**, and a name written
  in its `mod` declaration is checked against that rather than replacing it. The
  boundary is declared in code — a folder is a module because it holds a
  designated file, a file because it opens with `mod` — but the name is a
  filesystem fact, readable by a tool that cannot parse Cone.
- **The compiler is pointed at one file** and walks outward: the folder's other
  files join the module or are modules of their own, subfolders are probed for
  their designated file. It
  needs no package concept to do this. The layout convention that a package's top
  module lives in `src/` as `<package>.cone` is congo's, and the compiler never
  sees it.
- **Or it is handed a build description, and walks nothing** [Jon 23 Sep]:
  Congo applies the folder rules and tells the compiler the result, which the
  compiler checks against each file's `mod` line. The walk above retires once
  the test runner uses Congo's scanner.

Collisions follow, and each wants a diagnostic that names full paths: two
organizational subfolders can each declare the same name into the enclosing
module, and two files of one module can share a basename. ▸ **Two sibling module
folders declaring one module name is not among them**, though it was listed here
while the name was free: the folder names the module, and a filesystem gives two
children of one folder two names. The one sibling pair it does allow is a
one-file module beside a module folder of its name.

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
  compile does emit definitions for those, and `linkonce_odr` with a COMDAT of
  `any` is what lets several importers, and the package itself, each emit the
  same one (built for generic instances, 23 Sep 2026; an `inline` body and a
  macro leave no symbol). This is the whole of the exception: it does not extend
  to anything the imported package could have emitted itself. ▸ A vtable sits
  on the edge: every object coercing a type to a trait builds one — the pair is
  known only at the coercion, since a type may satisfy a trait it never names —
  and it is shared the same way, so that its address is one across objects.
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
- **A package may name its own default fold, on its `mod` line** [Jon 23 Sep]:
  `mod bigint use BigInt;` is what a bare `import bigint;` folds. See "The idiom
  for reaching a package's members" below.

### The idiom for reaching a package's members

A package named after the thing it provides would put that name in every path
twice — `bigint.BigInt`. This is not an edge case: a module with no global state
is pure namespace, and a great deal of library code needs none, so single-type
packages will be common. Go accepts `time.Time`, Rust leans on `use`, and
Python's `datetime.datetime` is the cautionary case.

**Decided [Jon 23 Sep]: a package holding one thing just becomes the thing when
imported, and the package says so on its own `mod` line.**

```
mod bigint use BigInt;
mod bigint extends base use BigInt;     // with extends, the use clause comes last
```

- **The `mod` line's `use` list is what a bare `import bigint;` folds by
  default**, however many names it holds. With one name the importer has
  `BigInt` directly, and no special case is needed; adding a name later only adds
  to what importers see.
- **An importer's own clause replaces the default entirely** — `import bigint use
  *`, `import bigint use Other`, `but` — so an importer that wants less, more or
  other names says so where it imports.
- **The module's name stays bound**, so `bigint.helper` still reaches what the
  default leaves out.
- **On the `mod` line, `use` names what importers fold**: the opposite direction
  from every other `use`, which folds into the module where it is written.
- **It is not an export list.** `pub` still governs what is reachable ("no header
  files, no export list", below); the clause only chooses the default fold, among
  names `pub` already made public.

As built, the default is honoured at every import of a module, a sister's and a
package's alike. A standalone `use sub;` of a submodule is not an import, and
keeps its meaning of every public name; whether the default should apply there
too has not been ruled on.
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
what its definitions say it is. A `mod` line's `use` clause is not one: it
chooses which public names a bare import folds by default, and makes nothing
public that `pub` did not.

- **A declaration is private to its module unless `pub`**, always — private to
  the module, not to the package. A nested module neither sees its parent's
  private names nor exposes its own to it, so nesting is a real boundary. An
  organizational subfolder is how files are grouped *without* erecting one,
  which is why the boundary needs no escape hatch: no one is forced to nest for
  layout reasons.
- **A submodule is private to its parent unless `pub`**, which is
  how a package keeps internals internal without a second visibility level.
  Written on the submodule's own `mod` declaration, since the parent declares
  nothing about a subfolder or a file.
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
folder's name. **Every module's first file opens with its `mod` line, and every
file's imports come right after it**, each refused otherwise ("A file's header"
above). **The module tree is real**: a subfolder holding its own designated
file is a submodule, private to its parent unless it writes `pub`, spelled after
its parent in every symbol, and reached from its parent by path — while a
subfolder that holds none is organisational at any depth, and a designated file
too deep to be a direct child is refused. **So is a file of the folder whose
first statement is `mod`**: a one-file submodule, named for its file, which
grows into a folder without anyone who names it seeing a change. `include` is retired and reported
(`ErrorInclude`), since the folder is what brings a file in. **A module reaches SIDEWAYS too**: it
imports a sister by name, resolved against the registry its parent is, and
because every module of a tree is compiled into one object that import *links* —
which an import of a module found beside its importer cannot do. **Never UP**: an import of a
name of its parent is a loop through containment, refused [Jon 23 Sep]. **Imports
form a DAG**: the modules are put in dependency order, a loop refused naming the
modules round it, and the order is the one the program's stitched init runs each
module's `init` in, its stitched final the finalizers in reverse ("Init and final"). The registry is the
immediate parent's namespace and no ancestor's, which is the scoped reading,
adopted provisionally. There is no nesting within a *source file*, and none is
planned — a `mod name { ... }` block there is refused, `ErrorUnbuiltKind` — no
package as a unit of compilation and no manifest. **The interface artifact is
generated**: a library compile writes its package's include file from the root's
own text ("Generating the include file"), with a private, pruned nested module
block for what the root reaches in each submodule, the one place a module block
is written, and Congo compiles every package's dependents against it. **What the compiler does
take is a build description** ("A described build"): one package's module tree
and files, each file's `mod` line checked against it, imports found only where
it says, and a library's root named from it, so a package compiled on its own
spells its symbols as its importers do and exports what they link against. What
stands in for
packages is the **packages folder**: `core` and `stdio` are folder modules there,
found on the package search path and compiled into the importing object ("The
packages folder" above).
**A module conforms to a module trait**, `mod prog is Runner;`, checked where it
is written, taking a copy of each default it does not declare ("Module traits"
above). **A module may be generic**, `mod stack[T];`, instantiated as
`stack[i64]` where it is used, one instance per argument list with its own
globals, `init` and `final`, and compiled only as its instances — the flat case,
without submodules ("Generic modules" above); `import` takes a file path where the registry has no answer, and
folds with a `use` clause — selecting, renaming and excluding as a global's
clause does. A lone file — the root, or a module an import reached by its
path — is still named after that file, and its declaration still renames it. Sections and
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
[Names and Namespaces](../../../../doc/design/names-and-namespaces.md).

**A module may name its default fold** on its `mod` line, `mod bigint use BigInt;`
[Jon 23 Sep]: a bare `import bigint;` folds `BigInt` beside binding `bigint`, an
importer's own clause replaces it, and `pub import` re-exports it. Each name must
be a public name of the module, checked at the `mod` line.

**A module imports another once.** A second import is `ErrorDupImport`, naming
both: an identical repeat [Jon 23 Sep] as much as one that differs in its clause
or its `pub`. So is a second import of one name of the parent.

**A name written twice is an error; one never written merges** [Jon 23 Sep].
Every fold into a module's namespace binds through `modFoldBind`. The same
declaration under a name already taken is `ErrorDupName` where the module's own
source wrote both bindings — listed twice, `use Colors;` twice, imported and
listed (`module_fold_written_nameres`) —
and is that binding, public if either route is, where a wildcard, an `extends` or
the core import made either one. Different declarations collide everywhere. The
diamond compiles (`module_fold_diamond`), whatever order the folds ran in.

**A module may extend another**, `mod solids extends shapes;`, reusing the base's
declarations and folds, but not its imports [Jon 23 Sep] — an alias, so the declaration, its symbol and its state stay the
base's, and as visible here as there, so its code reads the base's private names
while its importers see only the public ones [Jon 23 Sep] — and adding its own
declarations beside them. The base is a module already in reach, a sister or one
an import bound; a chain of `extends` transits, and a tree that extends links and
runs. What it does not do: extend a trait (a module conforms to a module trait
with `is`, `mod arena is Region`), or extend more than one module.

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

### How a C library becomes a Cone package, and what an interface artifact is

These are one question. A C library's package and a package's generated
interface are the same artifact — public declarations, no bodies, symbols
supplied elsewhere — one written by hand and one emitted by the compiler.

Cone code must be able to use C-API libraries, and the mechanism must produce
something `import` can name — a package — rather than declarations sprinkled
through user code. Two facts, kept apart [Jon 23 Sep]: **`extern` says a
declaration is defined elsewhere**, for C and Cone alike, and **naming is the
module's**. A module is C-named by `@c` after `mod` — `mod @c("SDL_") sdl;` —
and its functions and globals then bind to C symbols, the prefix and the name
as written; a function's own `@c("sym")` is its whole symbol, the override for a
name outside the prefix; `@c(system)` is the system calling convention
(`module_c_names`, `module_c_system`). A C binding module writes both words, and
exporting a Cone body to C writes only `pub fn @c(...)`. Symbol spelling is
[Names and Namespaces](../../../../doc/design/names-and-namespaces.md), S5.

**The hand-written form is built, as a C package** (`tools/congo/README.md`, "C
packages"). A package whose source is a C-named module, its `mod` line carrying
`@c`, is compiled as any package is, and its include file is generated as any
package's is: the declarations as written, with the banner. Congo's import line
names that file, and the importer loads it declared and not generated. It was
the package's own source until the generated include files were wired in
[25 Sep]; one rule for every package is simpler than an exception that saves
nothing, and it lifts the old limit that such a package be one file, since a
Cone helper beside the declarations is its object's to define. The package's
manifest names the C library in a `[link]` table
(`libraries`, and optionally `paths` to search, relative to the package), and
Congo puts every library the packages of a build name on the executable's link
line. The compiler is not involved in linking and needed no change: compiled on
its own, as Congo compiles every package, a declarations-only `@c` module is an
empty object, as `core`'s is, and Congo links it like any other rather than
treating a C package as a case of its own. That also keeps a `@c` module that
holds a Cone body correct, since the body is in that object.

What is still open: how `trust` is stated, how opaque types are declared,
a C package whose root reaches a C-named submodule (its block keeps the
submodule's `@c`, unmeasured), per-platform
library names (`opengl32` on Windows is `GL` elsewhere), and generating such a
package from a C header. `--safe=package`,
which exists in the option help and controls which packages may use C FFI, is
the policy half of the same question; that C naming is now written on the
module is what gives it something to check.

`core` and `stdio` still hold their C declarations inside their Cone-named
modules, each function marked `@c`, rather than in a C-named module of their
own.

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
  except `main`, a public C-named one, and — in a described build — a generic's
  instance and a vtable, which are `linkonce_odr` so that the copies other
  objects make merge with it; nothing is `hidden`. Hidden visibility
  would keep a symbol out of a shared library's export table but leave it a
  global symbol at static link, so it could still collide; internal linkage
  makes it object-local and collision-proof. The hazard that distinguishes them
  is silent: were a program's `fn log(...)` emitted as an external `@log`, a
  package calling libm's `log` would have its `declare` satisfied from the
  program and `log.o` never pulled — the wrong function with no diagnostic.
- **Private names in a *library* are internal, except what an expanded body
  reaches, which is linked against** ([Names and
  Namespaces](../../../../doc/design/names-and-namespaces.md), "Linkage", L5).
  Built in `dclIsExported`: the mangled namespace shrinks to the names that
  cross the package boundary, and a private helper an `inline`, generic or
  macro body reaches is exported once rather than re-emitted per importer.
  Hidden visibility is not set on it, as no visibility is set anywhere.
- **Build-mode defaults, or an explicit export set.** `--library` and congo's
  `exe`/`lib` targets already distinguish the modes. But a program built as a
  WebAssembly module or a DLL does export more than an entry point — the samples
  carry a `wasm.syms` listing exactly that. `pub fn @c(...)` now exports one
  Cone body under a C name from any compile, which is the per-declaration form
  of such a set; whether a build mode should also choose a default set is
  still open.

### How far the module/type convergence goes

Modules and types are meant to share namespace machinery while staying distinct
in state. **What a generic module means is settled by that convergence** [Jon 23
Sep]: a generic module is treated as a generic type is — parameters on its
`mod` line, instances named with type arguments and memoized, each instance with
its own globals, `init` and `final`, nothing compiled until an instance is named,
and across packages the full source in the include file ("Generic modules"
above). What the analogy leaves to be built is a generic module's submodules,
which it instantiates with the module as a generic type's methods are.

**Substitution was wanted before generativity, and was built first.** An entry
kind — a shell executable, a web request's receiver — is an interface a module
plugs into, and so is a region protocol, a module supplying alloc, free, alias
and dealias; module traits are that interface ("Module traits" above). A generic
module is a separate axis, and conforms to a module trait as any module does.

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
- The core package, `packages/core/src/core.cone`, implements them as
  **`struct @move so`** with `fn alloc(size usize) *u8` and no `free` method at
  all.

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
open. Both are buildable now; the second is what makes a region fully library
code, and a region protocol would also want what a module trait does not yet
hold, a type among its requirements.

That a module carries global singleton state, and a type does not, is why the
region-as-module description is the one that fits the state half; what a region
annotation on a reference names is a type.

### Consequences that follow whichever way those go

- **Symbol identity must stop depending on which module was the root.** The
  measured asymmetry above is the mechanism. A library built from a build
  description no longer has it — its root is named — while a program's root
  and a source file compiled directly still do. A generated name is the module
  path, outermost first, and there is no package component: a package
  correlates to one top-level module, whose name is what makes a public name
  distinguishable once the linker flattens every namespace into one. The rules,
  and what of the spelling is still open, are
  [Names and Namespaces](../../../../doc/design/names-and-namespaces.md), "Symbols".
- **The interface artifact must carry bodies, not signatures.** Generics
  monomorphize at the use site, macros expand at the use site, and `inline` is
  macro-shaped, so an importer needs the body of each. It exposes private
  declarations that a public generic or inline body calls — the one place a
  private declaration is needed from outside its module, since a public
  overload name may hold no private candidate.

  **The format is Cone source, not serialized IR.** **Decided by the
  author, 12 September 2026:** the artifact is **auto-generated**, with
  **hand-written as a transitional stage** — and nobody hand-writes serialized
  IR, so the format is the language itself. ▸ **This is also what makes a C
  library's package and a generated package interface one artifact with one
  loading path**, which is what that decision requires. It is generated by
  copying the author's text rather than printing the IR ("Generating the
  include file"), as Swift's textual `.swiftinterface` copies an inlinable
  body's source; Swift chose text for the same reason — a module built by one
  compiler version stays readable by a later one. **Built [25 Sep]**: Congo
  compiles dependents against the generated file, and `core` and `stdio` have
  retired their hand-written ones; the transitional stage is over for every
  package Congo builds.

  ⚠ **This paragraph previously read "the artifact is therefore serialized IR."**
  That was stated here and contradicted in the packages backlog item, with
  nothing saying which won.
- **Module `init` and `final` are built; purity and the entry are not** ("Init
  and final"). What `refmodule.html` specifies and nothing checks is the
  `initpure` rule that such a function call only `pure` or `initpure` functions,
  and that one other than `init` read no uninitialized global of its module:
  `pure` itself is unbuilt. Nothing runs the stitched init and final but a call
  to `initAll()` and `finalAll()`, until the entry glue does. ⚠ **The
  program's compile now SEES a package that only another package imports**:
  an include file imports, and the package lines list the whole closure ("A
  described build"), so every include file the program's imports reach,
  directly or through other include files, is a module of the program's
  compile. **How their `init`s run is
  still open** — Jon's chaining idea (`WI\first-congo-milestone.md`), for the
  entry-trait conversation. What happens today falls out of the stitched pair
  rather than being decided: an indirect package whose include file declares
  its `init` is in `pgm->initorder` like any module, so `initAll()` calls it
  (measured by hand, 25 Sep 2026: `b.init` setting a global to 7, reached only
  through `a`'s include file, ran from the program's `initAll()`); one whose
  include file does not declare it is not run, as for a direct import.
- **Dependency fan-out is unmeasured.** Section GC decides what reaches the
  binary; it does not decide what must resolve at link time. Archive member
  extraction precedes it, so calling one function from a package pulls its whole
  object and every undefined symbol in it enters resolution. Whether that ends
  in a hard error for a symbol only unreachable code references depends on
  linker and version. It wants an experiment once a multi-package program
  exists, not an assertion.
- **Module substitution and generativity are aims without a design.** Both
  drafts that would carry them are outlines.
  [Modularity](../../../../doc/design/modularity.md) states the aim and measures the
  distance.

## Hazards

- **A module trait's copies are the last `ntaken` of a module's `nodes`**, and
  `modNameRes` walks only the nodes before them, since the copies arrived
  resolved in the trait's scope and a second walk would bind their names again
  in the module's. Anything appended to `nodes` between `modTraitConform` and
  `modNameRes` would be taken for a copy and left unresolved. The module's
  `drop` is appended after name resolution, where nothing reads the count.
- **A `pub` `init` or `final` is folded like any public name**, so an importer's
  `use *` of a module whose `init` is public collides with the importer's own
  `init` (`ErrorDupName`). Written private, as the scenarios write them, neither
  leaves its module.
- **An `init`'s globals are cleared only round its own flow pass.** A function
  `init` calls reads the module's globals as holding values, whatever `init` has
  assigned by then: that is the unbuilt `initpure` rule, and reading one early
  that way reads zeroed storage.
- **Where a module's defaults are taken decides who sees them.** A copy made at
  the end of the module's fold pass is read by every module folding from it;
  one the fallback in `pgmNameRes` makes, after the fold passes, is not — no
  `extends` of the module and no importer's `use *` takes it, though a path
  reaches it. The fallback is taken only when the trait's module is mid-fold at
  the attempt or `is` does not resolve by then, which an acyclic program meets
  only for a trait declared by the conforming module's parent while the parent
  folds it.
- **A build description's `build` line wins over `--debug`.** The description is
  read after the options, so `build: release` optimises a compile run with
  `--debug`. A description with no `build` line leaves the option as given.
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
  be read the file is a lone file.
- **The parser survives a loop by the registry alone.** An import reaching a
  module mid-parse gets the half-parsed module back, which is complete before
  anything reads it, since name resolution runs after all parsing; the loop is
  refused only then (`pgmModuleOrder`). So a loop costs nothing at parse, and
  reports nothing there.
- **`FlagGenMod` is decided by where the import that loaded a module found its
  file**, and the first to load it decides; its submodules follow it. A file both beside one importer and
  in a `--path` folder is generated or only declared according to which import
  reached it first.
- **The packages folder a CMake build compiles in is an absolute path into the
  source tree, and it is only the fallback.** A `conec` moved away from that
  tree with no `packages/` holding `core` at or above its new folder, while the
  tree has also moved, finds no `core` and stops, unless `CONE_PACKAGES` or
  `--path` names a folder that holds one.
- **The walk up from the executable has no stopping point but the root.** A
  `conec` installed with no `packages/` of its own, under a folder that happens to
  hold `packages/core/` further up, takes that one ahead of the
  compiled-in fallback.
- **A module's public names are folded whether or not anything uses them.** A
  wildcard import walks the source's whole namespace, so a name the importer
  never mentions still takes a binding and still collides with a declaration of
  the importer's own.
- **A struct that a global's fold needs is resolved once, even mid-fold**, and
  that is where a refused loop can report more than the loop. A global's `use`
  clause needs its type's members in place, so the fold resolves that struct on
  demand (`structNameResDemand`), in its own module's scope. Where that module is
  mid-fold round a loop, a name the struct's own declaration reaches through a
  re-export not yet arrived — a field's type, say — is reported missing beside
  the loop, and the fold passes do not retry it: a struct is resolved once.
  Measured: `alpha` imports `beta`, then re-exports `delta`'s `Inner`, and
  declares `struct Box { pub v Inner use get; }`; `beta` imports `alpha` and
  writes `mut g alpha.Box = alpha.Box[delta.Inner[3i64]] use get;`. The loop is
  reported, then `Inner` unknown in `Box`, and `get` failing to fold: three
  diagnostics where one is the cause. The global's type itself waits
  for a late name (`modFoldAwaits`); what that type's declaration names does not.
  Relatedly, a global carrying a `use` clause is resolved whole in the fold pass,
  so a late name in its initial value is reported unknown; an initial value must
  be a literal, so that changes only which refusal such a program gets.
- **A loop of `extends` alone is cut where the module order closes it**, since
  neither module has a surface to start from; the other module still takes the
  one it extends. A loop an `extends` edge closes together with imports is left
  whole, and folded as any refused loop is.
- **A use of a module's name answers `isTypeNode` true** — `nameUseGroup`'s
  fallthrough for every declaration that is not a value, a macro or a generic
  parameter, not because a module is a type. `genericSubstitute` leans on it:
  that is what lets `stack[i64]` reach the generic path at all.
- **An instance's namespace is a copy of its generic's, made when the instance
  is.** Every fold has run by type check, so the copy is complete; a binding made
  in the generic's namespace after an instance exists would not reach it.
- **The root keeps every private const, macro and module trait of its own**, as
  the first version did, though what an expanded body names of them is now
  recorded (`exportReachesOf`): a const may size an array in a signature, which
  the generator does not walk, so pruning them is left alone. A submodule's are
  pruned to what is reached. A private typedef of the root goes where it names a
  type of the package the file leaves out and nothing the file holds reaches it.
- **What a generic submodule's text reaches in a sister is not recorded** — its
  functions are resolved once in place, not as expanded bodies — so a sister it
  imports goes in whole rather than pruned.
- **`DclIncluded` is written by the generator, and read by generation.** It
  decides exports only because a library compile generates its include file
  before any code. A compile that generates no include file marks nothing, and
  `--emit-include` without `output: library` exports nothing whatever it marks.
- **A method an expanded body reaches only through a receiver is exported where
  its type holds an expanded body, the body names the type, or the include file
  declares the type** (`typeHoldsExpanded`, `DclExpandReached`, `DclIncluded`).
  Before `DclIncluded`, a private type held in a public type's field had its
  public methods left internal and out of the include file, and a program
  calling one through the field failed to type check (measured,
  `module_include_field_reach`).
- **The self-check of a compile with no build description looks for its imports
  beside the root's first file**, and names its lexer
  `<folder>/<package>.include.cone`, a file that does not exist, so what it
  reports is located there.
- **A generic module's type parameters are hooked only by `modNameRes`.** A fold
  of the generic that resolves a declaration (a global's `use` clause, refused
  in a generic module) or a type of it resolved by demand from another module's
  pass (`structNameResDemand`) would meet its parameters unbound. Neither is
  reachable while a bare path through a generic is refused.

## What lives elsewhere

- The name rules a module implements — lookup, qualification, visibility,
  folding, aliases, overloading: [Names and Namespaces](../../../../doc/design/names-and-namespaces.md)
- What modularity is for, and how far Cone is from it: [Modularity](../../../../doc/design/modularity.md)
- Module loading as a parse-time activity, and the name-table hook:
  [Parse](../phases/parse.md)
- How a declaration's symbol is spelled and what linkage it gets:
  [Names and Namespaces](../../../../doc/design/names-and-namespaces.md), "Symbols"
- The lowering of those rules, linkage, COMDATs, and the allocation header:
  [Generation](../phases/generation.md)
- What an `is` list takes in, trait inheritance, and types as namespaces: [struct](struct.md)
- Instantiation, cloning and memonodes: [generic](generic.md)
