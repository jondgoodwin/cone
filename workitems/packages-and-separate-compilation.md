Packages, modules, compile units and namespace folding.

**Why this is the top priority.** Until it is solved there is no core library —
a core library *is* a package. And without one the language cannot grow in the
directions that matter: `Option` and `Result` should be library code with macros
as methods rather than compiler-resident, and so should regions, and so should
everything that follows them.

**The model is in `design/nodes/module.md`** ([[module|Module]]). This item does
not restate it; it sequences the work that gets there.

**The unblock is stage 13**, where core and stdio become real packages. Getting
there does not need separate compilation: a package is parsed for its
declarations and linked against its prebuilt object, and the interface artifact
of Phase 6 only replaces source-parsing with digest-loading. It buys build speed,
not the ability to link.

**Every stage lands with its notes and its tests**, per the change discipline in
`CLAUDE.md`. Where a stage contradicts `refmodule.html`, `refinclude.html`,
`module.h`'s comment, `PLAN.md` or [[__top-priority|__Top Priority]], that stage fixes them.

---

## Still to decide

Each gets its own discussion session; the outcome lands in
`design/nodes/module.md`, replacing the corresponding entry in its "What the
model has not decided" section.

1. **How a C library becomes a Cone package.** `extern` moves rather than
   disappears: it becomes how a package declares symbols supplied by something
   the compiler cannot read. Needs a name-to-C-symbol mapping, calling
   convention, `trust`, opaque types, and whether such a package is hand-written
   or generated. Carries the old note that `FlagExtern` should become a
   collection of globals in an "extern" module.

   **Decide the interface-artifact format here too.** A C library's package and
   a generated package interface are the same artifact — public declarations, no
   bodies, symbols supplied elsewhere — one written by hand and one emitted by
   the compiler. One format, one loading path.

   **On the critical path**: gates stages 11–13.
2. **The idiom for reaching a package's members** without saying the name twice —
   `bigint::BigInt`. Fold at import, name the top-level module independently of
   the package, convention, or a shortcut rule. A module with no global state is
   pure namespace and much library code needs none, so this shape is common.
   Gates stage 10's detail and the manual, no structure.
3. **How far the module/type convergence goes.** Whether a module may declare or
   implement a module trait, whether a package's top-level module may be
   parameterized, and whether folding into a module and into a type are one
   operation — the last being delegated inheritance. **Substitution before
   generativity**: the region protocol wants module traits, not generic modules.
   Gates module traits, and whether stage 7's grammar is stage 10's.
4. **What a region is.** Module, `region` declaration, or `struct` with an
   `_alloc` — three live descriptions, and a `region` keyword the lexer interns
   and nothing parses. Also whether its protocol is a contract the compiler holds
   structurally or one written in Cone. Gates the regions track only.

Deferred by decision: **the manifest** — what defines a package's name and
contents, including files the parser never reads (see [[metaprogramming|Metaprogramming]] on
compile-time embedded data). No stage before 13 needs it. **Compiler options**
`--pkg-path` and `--safe=package` name a concept the language does not have yet;
they gain meaning in stage 12.

---

## Phase 1 — make multi-package programs link

Nothing here waits on an open decision.

### 1. Generate every module, not just root and `stdio`

What a compile generates is everything in *this* package; an import is declared,
never defined.

- Delete the filename `strcmp` in `parseLoadAndParseModuleFile` that grants
  `FlagGenMod`.
- Revisit `genlProgram`'s privacy filter — it assumes nothing outside a module
  reaches a private name, which public overload names already break, and
  generating a whole package changes what it protects.
- Import cycle detection: resolve in dependency order, error on a cycle. The DAG
  rule is decided and nothing enforces it.

**Lands with:** the `module` group promoting from `compile` to `run`, and its
`cases.toml` note explaining why nothing there runs coming out. Smallest change
with the highest value, and it gives every later stage a real multi-module test
bed.

### 2. Function-per-section and COMDAT

So the linker's inclusion granularity is the function rather than the object
file.

- Emit a section and a COMDAT per function in `genlGloFnName`. `LLVMSetSection`
  and the `Comdat.h` API are both in LLVM 13's C API; `LLVMCreateTargetMachine`
  takes no `TargetOptions`, so there is no `FunctionSections` switch to flip.
  The COFF path needs a spike confirming the hand-rolled COMDAT matches `/Gy`.
- Teach congo `--gc-sections` on ELF and wasm, `/OPT:REF` on COFF — which is off
  by default whenever `/DEBUG` is on.
- **Measure dependency fan-out.** Section GC decides what reaches the binary, not
  what must resolve at link time: archive extraction precedes it, so calling one
  function from a package pulls its whole object and every undefined symbol in it
  enters resolution. Whether that hard-errors for a symbol only unreachable code
  references depends on linker and version. Stage 1 is what makes it testable.

**Lands with:** `llvmir` checks on section and COMDAT emission, and the fan-out
measurement recorded in `design/nodes/module.md`.

### 3. Package-rooted symbol identity

A symbol's spelling is a function of the package, never of which module was
`argv[1]`. Measured: the same module emits `@scaleInt` compiled as root and
`@modulesub_scaleInt` compiled as an import.

- Give the root module a name and a prefix by the same rule everything else
  uses; stop keying "no prefix" on the empty string in `nameGenFnName` and
  `nameGenVarName`. **`main` must still emit as `@main`** — measured, and the C
  runtime links against exactly that — so the entry point needs an explicit
  exemption alongside the one `FlagExtern` already has.
- **Settle what a package exports**, which decides how much of the name is
  needed. A generated name is package plus module path; a program needs the
  module path and not the package component, because nothing imports a program.
  Options to weigh: `LLVMInternalLinkage` versus today's `LLVMHiddenVisibility`
  for what is not exported; whether library privates go internal too; and
  build-mode defaults versus an explicit export set that also covers wasm and
  DLL exports and gives the C ABI its hook. `--library` and congo's `exe`/`lib`
  targets already distinguish the modes. The option space is in
  `design/nodes/module.md`, "What a package exports".
- Decide the generated-name separator: `_` vs `:`. Fewer names go through it if
  only cross-package names are mangled.
- Overloaded functions: how a concrete candidate's real name is spelled, given
  the overload name has no symbol of its own.
- Make generic instance names deterministic **across packages** so `linkonce`
  merges them. Within a package `memonodes` already dedups in the IR, so this is
  purely cross-package.
- Handle `genname` correctly regardless of aliasing and cross-package reference —
  the same requirement [[ir-refactor|IR refactor]] item 2.1 states for `genericdef`.

**Here rather than earlier** because whole-program linking works without it. It
is separate compilation that needs it, so it carries no risk for stages 1–2.

**Lands with:** `llvmir` checks in the `module` group. No new syntax, no new
nodes.

---

## Phase 2 — classification

Independent of Phase 1 and can run beside it. Carried by
[[tag-group-and-name-aliasing-refactor|Tag Group and Name Aliasing Refactor]], which holds the detail.

### 4. Capability predicates, alongside the existing retagging

`isExpNode`/`isTypeNode`/`isMetaNode` answer from a node's characteristics,
following name use through alias to declaration, rather than from the tag's group
bits. They answer correctly whether or not a node was retagged, so nothing breaks
while consumers still dispatch on tags.

**Lands with:** a scenario for a generic instantiation used as a type, which
today needs `itypeIsGenericType` as a hand-written special case.

### 5. Move consumers off the retagged tags

Family at a time. Measured reads to migrate: 26 of `TypeNameUseTag`, 44 of
`VarNameUseTag`, 17 of `MbrNameUseTag`.

### 6. Stop retagging

`NameUseTag` becomes one tag; the five destinations go, one of which
(`GenericNameTag`) is assigned zero times today.

---

## Phase 3 — binding

### 7. NameAlias and the visibility bit, in structs first

Delegated inheritance does not exist yet, so this is additive and lands with its
own scenarios rather than replacing working behavior.

- `NameAliasNode`: its own `namesym`, its own flags including the private bit, a
  target `INode*`.
- The private bit on every named declaration family. `0x0100` looks free in
  `flags` across all of them, but `flags` is not one namespace and a collision
  has no diagnostic, so confirm.
- Route the six visibility decisions off the spelling and onto the bit.
  `nameUseNameRes`, `fnCallLowerMethod`, `typeLitStructReorder` and
  `genlGloVarName` read `namestr == '_'` directly; only `genlProgram` and
  `importNameRes` go through `inodeIsPrivate`, which inverts into the initializer
  that stamps the bit.

**Needs 4–6 first.** While `isTypeNode` is a mask test, an alias to a struct
answers wrong and does so silently.

### 8. Module folding onto the alias

Replacement, now against a mechanism structs have already exercised.

**Lands with:** the transit scenario. Today whether a folded name is reachable as
`A::name` depends on module load order — `modNameRes` folds a module's imports at
the start of *that module's* resolution and `pgmNameRes` walks modules in load
order, so a root module naming `mid::plain` is rejected while the same reference
from a module loaded after the folding one compiles. Both measured. Neither is
the decided answer, so the scenario lands with the fix rather than pinning
today's behavior.

---

## Phase 4 — modules as designed

### 9. `mod` block, folder walk, nesting

- Parse `mod` as a block, with generic parameters accepted and unimplemented.
  Inline nested `mod` blocks parse too.
- The folder walk: from the file the compiler is given, sibling `.cone` files
  auto-include into that module; each subfolder is probed for a designated file
  declaring a `mod` and recursed into as a submodule, or its contents at any
  depth join the enclosing module. Reject a designated file found beneath an
  organizational folder — silently treating it as ordinary files is the confusing
  failure.
- Separate import *file* handling from *module* handling; file handling becomes a
  dictionary keyed by path, independent of the module tree.
- Nested modules: qualified paths through them, and `_`-named submodules private
  to their parent.

**De-risked by making it opt-in**: a file with no `mod` behaves exactly as today,
so every existing source keeps compiling while the walk is built.

### 10. `import` names packages, and `use` folds

- `import` stops accepting paths; `parseFilename`'s string-literal form and the
  `fileName` reduction serve path-based import and `include`, and go with them.
- One import per package per module: ignore an identical repeat, reject a
  differing one. Identity is the resolved package plus the normalized fold spec.
- `use` as a clause of `import` and standing alone: selective names, `as`,
  `except` against a wildcard, block form, `pub` re-export.
- Switch the lexer's reserved `using` to `use`, keeping `using` reserved so it is
  diagnosed with a suggestion.

Detail carried by [[using-and-module-name-folding|Using and Module Name-folding]].

**Lands with:** diagnostics that name every file involved — a duplicate import, a
module-wide name collision, and two sibling folders declaring one module name all
involve more than the last file parsed.

---

## Phase 5 — the unblock

### 11. Retire `include`

Needs decision 1, since packaging an `extern` block into an include file is what
the sample projects use it for.

### 12. C-shim packages

Decision 1 made real. `--safe=package` gains meaning here.

### 13. core and stdio become packages

`corelib` and `stdio` are C string literals today — `corelibSource` in
`corelib.c` and `stdiolib` in `parsemod.c`.

- Make them Cone source files in a `core` package, and refactor how Conehome
  holds them.
- The auto-import becomes a documented prelude rule. A fold is private to the
  module that made it, so a prelude is re-exported deliberately with `use pub`.
- `Option` and `Result` become ordinary library types. They are already Cone
  source; what remains is that the compiler names `Option` in `parseexpr.c` for
  the `?T` sugar. The nullable-pointer collapse in `genltype.c` needs no change —
  it keys on shape, not on the name.

**Exit:** [[option|Option]] and [[result|Result]] are library code, and changing either needs
no compiler rebuild.

---

## Phase 6 — separate compilation

### 14. The package interface artifact

Only worth building once 13 proves the model, and once re-parsing the core
library is a measured cost rather than an assumed one.

- The format is decided in decision 1, alongside the C shim, because they are the
  same artifact.
- **It cannot be signatures only.** Generics monomorphize at the use site, macros
  expand at the use site, `inline` is macro-shaped, and trait defaults are cloned
  into implementers, so the importer needs those bodies.
- Which drags in the awkward case: a **private** helper called from a public
  inline or generic body has to appear in the artifact, be emitted by nobody, and
  stay uncallable by the importer. That wants a marker or a rule, and it is the
  same assumption the overload privacy filter already broke once.
- Emitting it needs a printer that produces *valid Cone* rather than the current
  `--ir` debug dump — real work with independent value as a formatter.

### 15. Package build files and congo

- The manifest: what names a package and what it contains.
- Congo builds and uses libraries, and links them into executables.
- Auto-generate a C header for foreign consumers — see [[c-abi-generation|C-ABI Generation]].

---

## Tracks that run beside this

Each depends on Phase 5 for placement and on nothing else here.

- **Regions as library code** — [[regions|Regions]]. Replace the
  `isRegion(..., rcName)`/`soName` dispatch across `ir/flow.c`,
  `genllvm/genlalloc.c`, `genllvm/genlexpr.c` and `ir/exp/arraylit.c` with a
  protocol; generalize the allocation header, which today assumes `rc` has one
  `usize` field and `so` an empty region struct. The protocol can start as a
  contract the compiler holds structurally, so this does not wait on module
  traits. Needs decision 4. **Exit:** an arena can be written in Cone.
- **Macros as type methods** — [[macro-and-inline|Macro and Inline]], so `?`, `??`, `?.` and the
  error-handling forms on `Option` and `Result` are library-defined rather than
  parser-lowered. See [[option|Option]], [[result|Result]], [[error-handling|Error Handling]].
- **Module `init` and `final`** — [[init-and-final|Init and Final]]. `refmodule.html` specifies
  `initpure` and dependency-ordered initialization; `initpure` appears nowhere in
  the source. Required before any package with global state, so before arenas,
  pools or a collector.
- **Module polymorphism** — [[module-generics-and-traits|Module generics and traits]]. Modtraits first: the
  demand comes from regions, where the protocol is an interface. Generic modules
  bring per-instantiation state, per-instantiation `init`, mangling that encodes
  the instantiation, and cloning every declaration across every file of a module.
  The syntax parses from stage 9, so the semantics rewrite no source.
