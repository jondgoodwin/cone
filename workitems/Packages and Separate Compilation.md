Packages, modules, compile units and namespace folding.

**Why this is the top priority.** Until it is solved, there is no core library —
a core library *is* a package. And without a core library the language cannot
grow in the directions that matter: `Option` and `Result` should be library code
with macros as methods rather than compiler-resident, and so should regions, and
so should everything that follows them.

The mechanism as it stands, and the model as it has been decided, are in
`design/nodes/module.md` ([[module|Module]]). This item does not restate them.

**The sequencing insight.** Separate compilation and multi-package linking have
been treated as one thing here. They are not. **A core library needs
multi-package linking, which mostly exists already; it does not need separate
compilation.** `import stdio::*` today emits `@stdio_print` and full definitions
for the `IOStream` methods, because `stdio` is the one imported module granted
`FlagGenMod`. Every other imported module is denied it by a `strcmp` on its
filename. So step 2 below is small, and it is what unblocks the core library —
step 9, the expensive one, buys build speed rather than the ability to link, and
can wait until the model has been exercised.

The spine is **0 → 1 → 2 → 3 → 4**, and **4 is the unblock**. 5 hangs off 4 and
depends on [[Tag Group and Name Aliasing Refactor]]. 6, 7 and 8 hang off 4 and can run in parallel. 9 is
a second spine that only starts once 4 is real. 10 is independent and
deliberately last.

---

## 0. Settle the model, and write it down

No code. Each item is a decision that two documents answer differently, or that
the implementation answers by accident. Each gets its own discussion session,
and the outcome lands in `design/nodes/module.md`, replacing the corresponding
entry in its "What the model has not decided" section.

### Decided

- **The package is the unit of distribution, the semantic unit and the
  compilation unit**, emitting one object file, with a function-per-section
  COMDAT so the linker's inclusion granularity is the function. A codegen
  partition is a build knob with no semantic content; splitting a large codebase
  into packages is the recommended coarser form of the same lever. No
  declaration needs an owning source file.
- **A module is a namespace, and modules nest** by path within a package, which
  keeps package names themselves flat. A package correlates to one top-level
  module.
- **Modules and types share namespace machinery and differ in state** — a
  module's state is global and singleton with no `self`, a type's is per-instance
  through `self`. A module holding one type is therefore a type at the package's
  top level, not a special construct.
- **`mod` is a block**, carried by one designated file per module folder, named
  for the folder; sibling files auto-include into it. A subfolder is a submodule
  only if it holds its own designated file declaring a `mod`, and a module
  folder must be a direct child of its parent module's folder. The module's name
  comes from the `mod` declaration and is conventionally, not necessarily, the
  folder's. The compiler is pointed at one file and walks outward; `src/` and the
  package name are congo's layout convention and the compiler never sees them.
- **`mod X[T]:` parses from the start.** Generic-module semantics land with
  module polymorphism, and accepting the syntax now means no source is rewritten
  when they do.
- **`import` names a package**, resolved through the search path rather than by
  file path, and marks its declarations externally supplied — subsuming `extern`
  for Cone packages. A module imports a package at most once: an identical
  repeat is ignored, a differing one is an error.
- **Folding accumulates into the one module namespace.** No file-level scope; the
  namespace's existing uniqueness rule reports collisions. A spelling cannot be
  aliased two ways within one module.
- **`use` states what to fold** — `import opengl use setColor, sub::* except
  green` for a package, and standalone `use matrix::*` for a namespace already
  in scope. `using` stays reserved so it can be diagnosed.
- **`include` is retired**, but not before the C-shim question below is answered.
- **`_` is private to its module**, never to the package; a `_`-named submodule
  is private to its parent. Nesting is a real boundary, and an organizational
  subfolder is how files group without one.
- **A folded or imported name is private to the module that folded it**, with
  `pub` opting in — `import pub B use pub c as d`. Folding never widens
  visibility beyond the origin. `pub` marks a binding and `_` marks a
  declaration; `pub` never appears on a declaration and is contextual to
  `import`/`use`.
- **Visibility is a bit on the binding**, written once from the `_` spelling for
  a declared name and read by every check thereafter. Access asks the binding
  the caller traversed, linkage asks the declaring binding, and diagnostics want
  both ends.

### Still open, in the order to take them

1. **How a C library becomes a Cone package.** `extern` moves rather than
   disappears: it becomes how a package declares symbols supplied by something
   the compiler cannot read. Needs a name-to-C-symbol mapping, calling
   convention, `trust`, opaque types, and a decision on whether such a package is
   hand-written Cone or generated. `--safe=package` is the policy half. Carries
   the old note that `FlagExtern` should become a collection of globals in an
   "extern" module. **Blocks retiring `include`, and blocks step 4**, since
   `stdio`'s methods call into `conestd`'s C.
2. **The idiom for reaching a package's members** without saying the name twice
   — `bigint::BigInt`. Fold at import, name the top-level module independently of
   the package, convention, or a shortcut rule. A module with no global state is
   pure namespace and much library code needs none, so this shape will be common.
3. **How far the module/type convergence goes.** Whether a module may declare or
   implement a module trait, whether a package's top-level module may be
   parameterized, and whether folding into a module and into a type are one
   operation — the last being delegated inheritance. **Substitution before
   generativity**: the region protocol wants module traits, not generic modules.
4. **What a region is.** Module, `region` declaration, or `struct` with an
   `_alloc` — three live descriptions, and a `region` keyword the lexer interns
   and nothing parses. Also whether its protocol is a contract the compiler holds
   structurally or one written in Cone as a module trait. Its implementation is
   step 6 and can run in parallel with everything.

Smaller, and to be folded into whichever session they touch:

- **The manifest** — what defines a package's name and contents, including files
  the parser never reads (see [[Metaprogramming]] on compile-time embedded data).
  Deferred to package support by decision; the compiler needs none of it for
  steps 1–4.
- **Compiler options** the model needs — `--pkg-path` and `--safe=package` name
  a concept the language does not have yet.

**Exit:** `design/nodes/module.md` has no open question left that blocks steps
1–4, and `refmodule.html`, `refinclude.html` and `module.h`'s comment agree with
it.

---

## 1. Symbol identity independent of the compilation root

A symbol's spelling must be a function of the package, always — not of which
module happened to be `argv[1]`. Measured today: the same module emits
`@scaleInt` compiled as root and `@modulesub_scaleInt` compiled as an import,
and the two never resolve.

- Give the root module a name and a non-empty prefix; stop keying "no prefix" on
  the empty string in `nameGenFnName` and `nameGenVarName`.
- Decide the namespace separator for generated names: `_` vs `:`.
- Overloaded function names: how a concrete candidate's real name is spelled,
  given the overload name has no symbol of its own.
- Make generic instance names deterministic **across packages**, so that
  `LLVMLinkOnceAnyLinkage` actually merges them. Within a package `memonodes`
  already dedups in the IR, so this is purely a cross-package concern. Confirm
  `linkonce` works correctly for generic functions.
- Handle `genname` correctly regardless of aliasing, cross-module reference and
  ownership — the same requirement [[IR refactor]] item 2.1 states for
  `genericdef`.

**Verification:** entirely `llvmir` checks in the `module` test group. No new
syntax, no new nodes.

---

## 2. Whole-program multi-module compile and link

Generate bodies for every module in the program, not only the root and `stdio`.

- Remove the filename `strcmp` that grants `FlagGenMod`. What a compile
  generates is now "everything in this package"; what an `import` brings in is
  declared, never defined.
- Revisit the privacy filter in `genlProgram` accordingly — its assumption that
  nothing outside a module reaches a private name is already broken by public
  overload names, and generating a whole package changes what it is protecting.
- **Emit a section and a COMDAT per function** in `genlGloFnName`, so the
  linker's inclusion granularity is the function rather than the object file.
  `LLVMSetSection` and the `Comdat.h` API are both in LLVM 13's C API;
  `LLVMCreateTargetMachine` takes no `TargetOptions`, so there is no
  `FunctionSections` switch to flip. The COFF path needs a spike confirming the
  hand-rolled COMDAT matches what `/Gy` emits.
- **Teach congo the matching link flags** — `--gc-sections` on ELF and wasm,
  `/OPT:REF` on COFF, which is off by default whenever `/DEBUG` is on.
- Import cycle detection: name resolution in dependency order, and an error on a
  recursive cycle. The DAG rule is decided and nothing enforces it.
- **Measure dependency fan-out.** Section GC decides what reaches the binary,
  not what must resolve at link time: archive extraction precedes it, so calling
  one function from a package pulls its whole object and every undefined symbol
  in it enters resolution. Whether that hard-errors for a symbol only
  unreachable code references depends on linker and version. This step is the
  first point at which a real multi-package program exists to test it on.

**Exit:** a program spanning modules links and runs. The `module` test group's
scenarios can be promoted from `compile` to `run`, and its `cases.toml` note
explaining why nothing there runs comes out.

---

## 3. Modules that nest and span files

- Add the `mod` node and parse it as a block, with optional generic parameters
  accepted and unimplemented. Inline nested `mod` blocks parse too.
- Implement the folder walk: from the file the compiler is given, sibling
  `.cone` files auto-include into that module; each subfolder is probed for a
  designated file declaring a `mod` and recursed into as a submodule, or its
  contents at any depth join the enclosing module. Reject a designated file
  found beneath an organizational folder — that is the illegal
  module-under-non-module case, and silently treating it as ordinary files is
  the confusing failure.
- Separate import *file* handling from *module* handling; file handling becomes
  a dictionary keyed by path, independent of the module tree.
- Nested modules: qualified paths through them, and the visibility rule step 0's
  third item settles.
- `import` stops accepting paths. `parseFilename`'s string-literal form and the
  `fileName` reduction exist to serve path-based import and `include`; both go.
- Enforce one import per package per module: ignore an identical repeat, reject
  a differing one. Both diagnostics, and the module-wide name collision, must
  name every file involved and not just the last one parsed.
- Retire `include`, once step 0's first item has given the C-shim packages that
  replace its one remaining use.
- Scenario for the transit rule. Today whether a folded name is reachable as
  `A::name` depends on module load order: `modNameRes` folds a module's imports
  at the start of *that module's* resolution and `pgmNameRes` walks modules in
  load order, so a root module naming `mid::plain` is rejected while the same
  reference from a module loaded after the folding one compiles. Both measured.
  Neither answer is the decided one, so the scenario lands with the fix rather
  than pinning today's behavior.


---

## 4. Move the core library out of the compiler

**This is the unblock.** `corelib` and `stdio` are C string literals today —
`corelibSource` in `corelib.c` and `stdiolib` in `parsemod.c`.

- **Build the C-shim package mechanism first.** `stdio` cannot become a package
  without it: its `IOStream` methods call `printStr` and friends, which
  `conestd` implements in C. This is step 0's second open item made real, and it
  gates this step as much as it gates retiring `include`.
- Make them real Cone source files in a `core` package, and refactor how
  Conehome holds them.
- Turn the auto-import into a documented prelude rule. A fold is private to the
  module that made it, so a prelude is re-exported deliberately with `use pub`
  rather than transiting by default.
- `Option` and `Result` become ordinary library types. They are already Cone
  source; what is left is that the compiler names `Option` in `parseexpr.c` for
  the `?T` sugar. The nullable-pointer collapse in `genltype.c` needs no change:
  it keys on shape, not on the name.

**Exit:** [[Option]] and [[Result]] are library code, and a change to either
needs no compiler rebuild.

---

## 5. Complete name folding

Carried in full by [[Using and Module Name-folding]]. The `use` clause and its
standalone form: selective names, `as` renaming, `except` against a wildcard, the
block form for long lists, and `pub` re-export. Switch the lexer's reserved
`using` to `use`, keeping `using` reserved so it can be diagnosed with a
suggestion rather than rejected as an unknown statement.

Route all six visibility decisions through the binding's bit rather than the
spelling — `nameUseNameRes`, `fnCallLowerMethod`, `typeLitStructReorder` and
`genlGloVarName` read `namestr == '_'` directly today; only `genlProgram` and
`importNameRes` go through `inodeIsPrivate`, which inverts from a query into the
initializer that stamps the bit.

**Prerequisite: a binding record, and structs are the place to build it.** A
folded name needs its own spelling, visibility and origin while pointing at a
declaration owned elsewhere. Today a module fold inserts the *same node* into a
second namespace, so there is nowhere to put any of that.

The same record is what name-folding into a **type** needs, and that is the
better first target: delegated inheritance is a capability structs do not have,
so it can be built beside the existing clone-based mixin without regressing
anything, while changing module folding is a replacement of working behavior.
Build it there, exercise it, then apply it to modules.

What transfers whole: the record, namespace entries becoming bindings, the
visibility bit, collision checks against the receiving namespace, and local
re-spelling. What structs need and modules never will: retargeting `self`
through the delegating field, field cloning for layout, vtable slots. What
modules need and structs never exercise: code-generation provenance — whether
this compile defines a name or only declares it — and a cross-package origin for
symbol naming. Leave room for those even though the struct work will not fill
them.

The record is a `NameAliasNode` rather than a universal binding: declarations
keep their representation, and only the entries that bind something declared
elsewhere become nodes of their own. [[Tag Group and Name Aliasing Refactor]] carries the shape, the
capability predicates it depends on, and the staging.

---

## 6. Regions as genuine library code

Depends on 4 for placement; the hard part is independent of packages and can run
in parallel. Carried by [[Regions]].

- Replace the `isRegion(..., rcName)` / `soName` name dispatch — seven sites
  across `ir/flow.c`, `genllvm/genlalloc.c`, `genllvm/genlexpr.c` and
  `ir/exp/arraylit.c` — with a region protocol. It can start as a contract the
  compiler holds structurally, so this step does not wait on module traits;
  step 10 later lets the same contract be written in Cone.
- Generalize the allocation header. `genlRcCounter` finding the count at
  `((usize*)ref) - 1` holds only because `rc` has one `usize` field and a
  zero-sized permission; `genlDealiasOwn` freeing the reference directly holds
  only because `so`'s region struct is empty.
- Implement `region` syntax or delete the keyword, per step 0's fifth open item.

**Exit:** a region the compiler has never heard of — an arena — can be written
in Cone.

---

## 7. Macros as type methods

Carried by [[Macro and Inline]]. So that `?`, `??`, `?.` and the error-handling
forms on `Option` and `Result` are library-defined rather than parser-lowered.
Depends on 4. See [[Option]], [[Result]] and [[Error Handling]] for what each
needs to express.

---

## 8. Module init and final

Carried by [[Init and Final]]. `refmodule.html` specifies an `init` marked
`initpure`, its constraints, compiler verification that every uninitialized
global is assigned there, and dependency-ordered initialization across modules.
`initpure` appears nowhere in the source.

Required before any package with global state — which means before arenas, pools
or a tracing collector, so before the interesting half of step 6.

---

## 9. Separate compilation: the package artifact

Deliberately after 2–4, because the artifact has to serialize whatever the model
turned out to need, and designing the format before the model is exercised
serializes the wrong thing.

- **The interface must carry bodies, not signatures.** Generics monomorphize at
  the use site, macros expand at the use site, `inline` is macro-shaped. So this
  is serialized IR, and it exposes private declarations reached from a public
  generic or inline body.
- Package build files and the manifest. Today `congo.toml` has `[dependencies]`
  and nothing else.
- Give `congo` the ability to build and use libraries, and to link them into
  executable programs.
- Give `--pkg-path` and `--safe=package` real meanings.
- Auto-generate a library header file for C consumers — see [[C-ABI Generation]].
- Does LLVM's reusable "precompiled header" support help here, or is the format
  ours?

---

## 10. Module polymorphism

Carried by [[Module generics and traits]]: modtrait and generic modules — module
substitution and generativity, which [[modularity|Modularity]] names as the two
missing strategies at the module layer.

**Modtraits first.** The demand comes from regions: a region protocol is an
interface, so substitution is what makes a region fully library code, while a
generic module is a separate axis. Nothing forces that order technically.

Generic modules bring per-instantiation global state, per-instantiation `init`,
mangling that encodes the instantiation, and cloning every declaration across
every file of a module rather than just a type's methods. The syntax parses from
step 3, so adding the semantics rewrites no source.

Genuinely new design; both drafts that would carry it are outlines. **Nothing in
0–9 waits on it**, though step 6 delivers a compiler-held region contract that
modtraits would later replace with one written in Cone.
