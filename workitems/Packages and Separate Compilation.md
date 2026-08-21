Modules, packages, compile units and namespace folding.

**Why this is the top priority.** Until it is solved, there is no core library —
a core library *is* a package. And without a core library the language cannot
grow in the directions that matter: `Option` and `Result` should be library code
with macros as methods rather than compiler-resident, and so should regions, and
so should everything that follows them.

The mechanism as it stands, and the model as it has been decided, are in
`design/nodes/module.md` ([[module|Module]]). This item does not restate them.

**The sequencing insight.** Separate compilation and multi-module linking have
been treated as one thing here. They are not. **A core library needs multi-module
linking, which mostly exists already; it does not need separate compilation.**
`import stdio::*` today emits `@stdio_print` and full definitions for the
`IOStream` methods, because `stdio` is the one imported module granted
`FlagGenMod`. Every other imported module is denied it by a `strcmp` on its
filename. So step 2 below is small, and it is what unblocks the core library —
step 9, the expensive one, can wait until the model has been exercised.

The spine is **0 → 1 → 2 → 3 → 4**, and **4 is the unblock**. 5 hangs off 4 with
a NameDef question attached. 6, 7 and 8 hang off 4 and can run in parallel. 9 is
a second spine that only starts once 4 is real. 10 is independent and
deliberately last.

---

## 0. Settle the model, and write it down

No code. Each item below is a decision that two documents currently answer
differently, or that the implementation answers by accident. Each gets its own
discussion session; the outcome lands in `design/nodes/module.md`, replacing the
corresponding entry in its "What the model has not decided" section.

Recommended order, most upstream first:

1. **What the compilation unit is, and what an object file is.** Challenging
   "the module is the compilation unit, emitting per-source-file object files
   for selective linking." The semantic claim and the build claim may be
   separable; section-level linker collection may make per-file objects
   unnecessary; a generic instance or expanded macro has no home file. Possible
   reframing: a codegen partition is a build-performance knob, not a semantic
   boundary. Reaches upward into whether `package == module` at all — if a
   package may hold several modules, the compilation unit is the package.
   **Take this first: it constrains 2, 3 and 4 below.**
2. **Whether a module may contain a module.** Single-level per the post; "even
   other named modules" per `refmodule.html` and `module.h`. Separate *a
   namespace containing a binding to* a module from *a module declaring* one.
   Then answer the question this is really about: **how does a core library
   subdivide** — submodules, types-as-namespaces, or a package layer? The
   existing position is that a package can only be a module and never a type,
   that a module cannot be imported into a type while types may namespace other
   types and macros, that a module has vars and functions rather than fields
   and methods, and that global variables do not support delegated inheritance
   the way fields do. Confirm or revise, and record it as the answer rather
   than as an aside.
3. **Where a folded name lives when a module spans files.** Folding is
   per-source-file by decision; there is no file-level scope in the IR. Does a
   source file become a lookup scope between block scopes and the module
   namespace, and what does that do to one-namespace-one-uniqueness-domain?
   Decide `include`'s fate in the same session — real multi-file modules do
   properly what `include` does by injection, so what remains for it is
   packaging an `extern` block, which is item 4's question.
4. **What `extern` is for.** "Get rid of this, because an extern is a name in an
   imported package" versus [[names-and-namespaces|Names and Namespaces]]'s
   design in which matching `extern`s and one implementation merge into a single
   canonical binding, which is how a serialized interface would be spelled.
   FFI declaration, interface declaration, or both. Needs item 1's answer about
   the artifact. Carries the old note that `FlagExtern` should become a
   collection of globals in an "extern" module.
5. **What a region is.** Module, `region` declaration, or `struct` with an
   `_alloc` — three live descriptions, and a `region` keyword the lexer interns
   and nothing parses. Most separable of the five; its implementation is item 6
   and can run in parallel with everything.

Also to be decided here, each smaller than the five above:

- **Whether folding transits.** If A folds B's publics and Z folds A's, does Z
  see B's? Posed and left open in `modules-vs-types.md`. **This is the prelude
  question** and item 4 cannot be designed without it.
- **Where a module's name comes from**, and how it relates to the file or
  directory it was found in, to the `import` spelling, and to the package
  manifest. Today it is the filename.
- **Compiler options** the model needs — the existing `--pkg-path` and
  `--safe=package` currently name a concept the language does not have.

**Exit:** `design/nodes/module.md` has no "not decided" section left that blocks
steps 1–4, and `refmodule.html`, `refinclude.html` and `module.h`'s comment
agree with it.

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
- Make generic instance names deterministic across compilation units, so that
  `LLVMLinkOnceAnyLinkage` actually merges them rather than accumulating
  near-duplicates. Confirm `linkonce` works correctly for generic functions.
- Handle `genname` correctly regardless of aliasing, cross-module reference and
  ownership — the same requirement [[IR refactor]] item 2.1 states for
  `genericdef`.

**Verification:** entirely `llvmir` checks in the `module` test group. No new
syntax, no new nodes.

---

## 2. Whole-program multi-module compile and link

Generate bodies for every module in the program, not only the root and `stdio`.

- Remove the filename `strcmp` that grants `FlagGenMod`; replace it with
  whatever step 0.1 decided the rule is.
- Revisit the privacy filter in `genlProgram` accordingly — its assumption that
  nothing outside a module reaches a private name is already broken by public
  overload names, and generating every module changes what it is protecting.
- Import cycle detection: name resolution in dependency order, and an error on a
  recursive cycle. The DAG rule is decided and nothing enforces it.

**Exit:** a program spanning modules links and runs. The `module` test group's
scenarios can be promoted from `compile` to `run`, and its `cases.toml` note
explaining why nothing there runs comes out.

---

## 3. `mod` declaration, multi-file modules, file scope

- Add the `mod` node and parse it in source files.
- Separate import *file* handling from *module* handling; file handling becomes
  a dictionary. Module names come from parsing `mod`, not from the filename.
- Add the file-level scope step 0.3 decided on, so two files in one module can
  hold different imports.
- Mark each `fn` and `var` with whether it belongs to the current source file,
  to the extent step 0.1's answer still needs it — if the codegen partition is
  not the source file, this bookkeeping may not be needed at all.
- `include`: implement whatever step 0.3 decided, including removing it.

---

## 4. Move the core library out of the compiler

**This is the unblock.** `corelib` and `stdio` are C string literals today —
`corelibSource` in `corelib.c` and `stdiolib` in `parsemod.c`.

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
- Implement `region` syntax or delete the keyword, per step 0.5.

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
