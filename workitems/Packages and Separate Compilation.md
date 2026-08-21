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

The spine is **0 → 1 → 2 → 3 → 4**, and **4 is the unblock**. 5 hangs off 4 with
a NameDef question attached. 6, 7 and 8 hang off 4 and can run in parallel. 9 is
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
- **A module may span source files**, each file belonging to exactly one module.
- **Modules and types share namespace machinery and differ in state** — a
  module's state is global and singleton with no `self`, a type's is per-instance
  through `self`. A module holding one type is therefore a type at the package's
  top level, not a special construct.
- **`import` names a package** and marks its declarations externally supplied,
  subsuming `extern` for Cone packages.
- **`include` is retired**, but not before the C-shim question below is answered.

### Still open, in the order to take them

1. **How a module's source files are found.** Folder tree authoritative (Java,
   Python, Go), a declaration in each file authoritative (.NET), or folder
   default with an override. `mod` is separately ambiguous between declaring
   *which module this file belongs to* and declaring a *nested module inline
   within a file*; Rust has both, and Cone must choose which it wants. Blocks
   step 3.
2. **How a C library becomes a Cone package.** `extern` moves rather than
   disappears: it becomes how a package declares symbols supplied by something
   the compiler cannot read. Needs a name-to-C-symbol mapping, calling
   convention, `trust`, opaque types, and a decision on whether such a package is
   hand-written Cone or generated. `--safe=package` is the policy half. Carries
   the old note that `FlagExtern` should become a collection of globals in an
   "extern" module. **Blocks retiring `include`**, since packaging an `extern`
   block into an include file is what the sample projects use it for.
3. **Where a folded name lives, and the idiom for reaching a package's
   members.** There is no file-level scope in the IR: does a source file become
   a lookup scope between block scopes and the module namespace, and what does
   that do to one-namespace-one-uniqueness-domain? Alongside it, what a caller
   writes to reach a single-type package without saying the name twice —
   `bigint::BigInt`. A module with no global state is pure namespace, and much
   library code needs none, so this shape will be common. Blocks steps 3 and 5.
4. **How far the module/type convergence goes.** Whether a module may be
   generic, may declare or implement an interface, whether a package's top-level
   module may be parameterized, and whether folding into a module and into a type
   are literally one operation — the last being delegated inheritance. Only the
   final question blocks anything before step 10.
5. **What a region is.** Module, `region` declaration, or `struct` with an
   `_alloc` — three live descriptions, and a `region` keyword the lexer interns
   and nothing parses. Most separable; its implementation is step 6 and can run
   in parallel with everything.

Smaller, and to be folded into whichever session they touch:

- **Whether folding transits.** If A folds B's publics and Z folds A's, does Z
  see B's? Posed and left open in `modules-vs-types.md`. **This is the prelude
  question**, and it belongs with item 3.
- **How a package is named and found on disk**, and how that relates to the
  `import` spelling and the manifest. Belongs with item 1.
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

- Implement whatever step 0's first item decided about finding a module's source
  files — a folder rule, a per-file declaration, or both — and add the `mod`
  node if that answer needs one.
- Separate import *file* handling from *module* handling; file handling becomes
  a dictionary keyed by path, independent of the module tree.
- Nested modules: declaration, qualified paths through them, and the visibility
  rule at each level.
- Add the file-level scope step 0's third item decided on, so two files in one
  module can hold different imports.
- Retire `include`, once step 0's second item has given the C-shim packages that
  replace its one remaining use.


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
- Turn the auto-import into a documented prelude rule, using step 0's transit
  answer.
- `Option` and `Result` become ordinary library types. They are already Cone
  source; what is left is that the compiler names `Option` in `parseexpr.c` for
  the `?T` sugar. The nullable-pointer collapse in `genltype.c` needs no change:
  it keys on shape, not on the name.

**Exit:** [[Option]] and [[Result]] are library code, and a change to either
needs no compiler rebuild.

---

## 5. Complete name folding

Carried in full by [[Using and Module Name-folding]]. Selective names, aliasing,
`except`, and the transit rule from step 0.

**Dependency to decide before starting.** A renamed fold needs a binding whose
spelling differs from the definition's while retaining origin and visibility.
Today a fold binds the *same node* into a second namespace, so a different local
spelling is not expressible. That record is what `NameDef` is. Either land
[[Namedef Refactor]] stages 1–5 first, or build a narrow alias node and accept
doing this twice. [[__Top Priority]] currently orders this item ahead of
[[IR refactor]], so the collision is already implicit and should be made
explicit.

---

## 6. Regions as genuine library code

Depends on 4 for placement; the hard part is independent of packages and can run
in parallel. Carried by [[Regions]].

- Replace the `isRegion(..., rcName)` / `soName` name dispatch — seven sites
  across `ir/flow.c`, `genllvm/genlalloc.c`, `genllvm/genlexpr.c` and
  `ir/exp/arraylit.c` — with a region protocol.
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

Carried by [[Module generics and traits]]: generic modules and modtrait — module
substitution and generativity, which [[modularity|Modularity]] names as the two
missing strategies at the module layer.

Genuinely new design; both drafts that would carry it are outlines. **Nothing in
0–9 waits on it.** Step 0 need only avoid foreclosing it.
