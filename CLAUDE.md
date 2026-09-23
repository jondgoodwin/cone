# Cone repository instructions

## Project overview

Cone is an in-development systems programming language. This repository
contains its C compiler (`conec`) and a small standard-library component
(`conestd`). The compiler targets LLVM and currently depends on LLVM 13.

This file is a map and a set of working rules. What the compiler does, and why,
lives in `design/`; do not describe compiler behaviour here, because a sentence
here goes stale without anyone noticing.

## Repository layout

- `src/c-compiler/parser/`: lexer and parser; converts Cone source into IR.
- `src/c-compiler/ir/`: shared IR plus semantic analysis.
  - `exp/`: expression nodes and lowering.
  - `stmt/`: declaration and statement nodes.
  - `types/`: type representation and type rules.
  - `meta/`: generics and macros.
- `src/c-compiler/corelib/`: compiler-defined core language types and methods.
- `src/c-compiler/genllvm/`: LLVM type, statement, expression, and allocation
  generation.
- `src/c-compiler/shared/`: diagnostics, memory, file, option, timer, and UTF-8
  utilities.
- `src/conestd/`: the C implementation of the standard-library component.
- `conesite/`: source and static content for
  [cone.jondgoodwin.com](https://cone.jondgoodwin.com), including the
  playground, examples, and language documentation. The reference
  documentation manifest is `conesite/public/coneref/index.html`.
- `design/`: design notes, grouped as `topics/` (what Cone is aiming at and
  how far the compiler is — the notes that would survive a rewrite), `phases/`
  (one per compiler phase, plus the naming rules), `nodes/` (what is true of
  every IR node, plus per-node notes), `compiler/` (how `conec` itself is built
  and stays fast), and `diagnostics/` (measuring, error codes, test suite).
  `design/_index.md` is the entry point.
- `workitems/`: the plan and backlog are kept by the project owner outside this
  repository; `workitems/_index.md` says so. `workitems/done/` holds completed
  items.
- `test/run.py`: the test suite runner. `design/diagnostics/test-suite.md` is
  its authoring guide.
- `test/cases/<group>/`: one directory per coverage group, each with a
  `cases.toml` listing its scenarios. A scenario is one `.cone` file, or a folder
  named for it holding the files of one module — and, where the case is about the
  module tree, the subfolders that draw submodules of it.
- `test/codes.toml`: the pinned `ErrorCode` name-to-number table the runner
  checks `error.h` against before any case runs.

## Where to look

- Treat source code as the truth for current compiler behaviour.
- `src/c-compiler/conec.c` defines the pipeline: parse, name resolution, type
  check and lowering, flow analysis, LLVM generation. `design/_index.md` maps
  each phase to its note. Each phase note carries its principles, what the
  phase deliberately does *not* do, its hazards, and a file-and-function map
  into the code. Read the one that owns the problem before changing it;
  `design/nodes/_index.md` covers what is true of every IR node regardless of
  phase. Design notes may describe planned behaviour and say so.
- `conesite/public/coneref/index.html` is the language reference page index.
  Consult it and the surrounding `conesite/` files when changing published
  language documentation or playground behaviour. Its chapter list is also the
  spine of the test suite's group organization: adding a chapter implies asking
  whether a coverage group is needed, and a feature with no chapter has nowhere
  to be tested.

IR nodes may be replaced or lowered during name resolution and type checking.
When changing a language feature, trace and update every affected phase:
lexer/parser, IR node construction, name resolution, type checking/lowering,
flow analysis when ownership or borrowing is involved, LLVM generation, and
the Cone smoke-test input.

## Code conventions

- Follow the existing C style and nearby naming patterns.
- Node structs share `INode` headers and are dispatched primarily by `tag`.
- Use existing node constructors, traversal macros, interned names, namespace
  lookup, type comparison, coercion, and error-reporting helpers.
- Preserve source location data when injecting or replacing nodes so
  diagnostics remain useful.
- Keep type safety explicit; do not hide invalid IR states with unchecked casts
  or placeholder values. Pass and return the declared enum (`TypeCompare`,
  `SubtypeConstraint`, `OverloadMatch`) rather than a bare `int`.
- Add comments only where compiler lowering, ownership behavior, or LLVM
  representation is not self-evident.
- Give a diagnostic its own `ErrorCode`. Do not reuse an unrelated code for a
  new condition; lookup failures, visibility errors, and no-match errors must
  stay distinguishable.

## Build

### Windows

The verified configuration uses a 64-bit LLVM 13 installation with the X86 and
WebAssembly targets, the Ninja generator, and the VS 2022 x64 toolchain. Pass
`-DLLVM_DIR=<llvm root>\lib\cmake\llvm` when LLVM is not on CMake's search path.

```powershell
cmake -S . -B build\x64-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build\x64-release
```

`cl.exe` needs the Visual Studio environment. From a shell that does not
already have it, wrap the build (adjust the path for your edition):

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build\x64-release'
```

That prints a harmless `'vswhere.exe' is not recognized` line before the build
output; it does not affect the result.

The checked-in Visual Studio projects use the Windows 10 SDK and the VS 2022
`v143` toolset, but CMake is the verified path for the minimal
X86/WebAssembly LLVM build.

### CMake, Linux, and WSL

`CMakeLists.txt` uses `find_package(LLVM 13 REQUIRED CONFIG)` and defines the
`conec` executable and `conestd` library. Configure and build with the
repository's existing CMake setup; do not change the LLVM major version
without updating source compatibility and both build systems.

## Validating a change

**Compiling clean is not evidence of correct runtime behavior.** Treat a
successful compile as the first check, not the last one.

Run the test suite. It builds nothing and needs no installation:

```powershell
python test/run.py
```

It compiles every scenario under `test/cases/`, asserts what each one's category
and inline `//~` annotations claim, links and runs the `run` scenarios, and
reports tier 0 first. `--list` prints what would run; a group, scenario, check
name or `tag:<phase>` narrows it. `design/diagnostics/test-suite.md` is the
authoring guide: which group to touch, what to assert, and how expectations are
written.

**A stale `conec` fails good sources in ways indistinguishable from a language
regression.** The runner refuses to run against a binary older than a compiler
source, and `--build` builds first (it finds the Visual Studio environment
itself); outside the runner, build before believing any failure.

Useful `conec` options: `--ir` writes an IR/AST dump, `--llvmir` writes LLVM IR
before and after optimization, and `--wasm` targets WebAssembly. The output
directory must already exist, and each run writes several files, so use a
git-ignored directory such as `build/`.

For a compiler change:

1. Build `conec` and run `python test/run.py`.
2. Add coverage for the change: a scenario in the owning group under
   `test/cases/`, following `design/diagnostics/test-suite.md`. A fix for a
   crash or a miscompile lands with the case that fails without it, and a new
   `ErrorCode` lands with the scenario that provokes it.
3. Inspect the generated IR when the change affects lowering or symbols.
4. When the change affects runtime behavior, link and run a program.

`conec` emits only an object file. To produce a runnable executable on Windows,
link it against `conestd` and the C runtime from a VS environment:

```powershell
link prog.obj build\x64-release\conestd.lib /OUT:prog.exe /SUBSYSTEM:CONSOLE msvcrt.lib legacy_stdio_definitions.lib
```

A program that spans an `import` cannot be linked yet, because an imported
module's bodies are declared and never generated. So runtime checks live in what
one compile defines: one source file, the files of one folder, or that folder and
the submodules its subfolders draw, whose bodies are generated like the rest of
the program's. `design/nodes/module.md` explains why.

## Change discipline

- **A code change is not finished until its notes are.** Every PR that changes
  compiler behavior lands with the matching updates in the same commit:
  - a test scenario under `test/cases/` in the owning group,
  - the `design/` note for each phase whose mechanism, invariant, or contract
    moved — the phase notes and any per-node note the change touches,
  - the **implementation-status annotations** on every design note the change
    touches — a `[planned]` that is now built is deleted, and a `[differs]` whose
    divergence is closed goes with it. `design/_index.md`, "Implementation
    status", carries the scheme,
  - the **reference page** under `conesite/public/coneref/` for any feature whose
    built status changed, including the italic status note at the top of that
    page and any marking on its examples,
  - and, when you have access to the owner's plan, the work item that owns the
    subsystem, so what the change closed stops being listed as owed and what it
    opened is recorded.

  **The dependency runs one way: a work item may point at a design note, a
  design note never points at a work item.** That is what makes closing an item
  prompt the question of which notes, tests and reference pages now need
  updating; a note that pointed back would simply go stale.

  Where the change contradicts something a note asserts, fix the note in that
  commit rather than leaving it to be discovered later. A design note that has
  gone stale is worse than a missing one, because it gets trusted. If a note
  needs no change, that is the normal case for a tactical fix — say so in the PR
  description rather than silently skipping the check.

  **Fixing a note means deleting what stopped being true, not annotating it.**
  A note describes the compiler as it is; it is not a changelog of itself. Do not
  leave behind "was measured, then fixed", "this note used to list", "now
  reports", or a hazard entry rewritten to say the hazard is gone. Git holds the
  history, and a reader asking how the compiler behaves is not asking for it.
  `design/_index.md`, "Conventions", is the full rule.
- Keep `CMakeLists.txt`, `Cone.vcxproj`, and `Conestd.vcxproj` synchronized when
  adding, removing, or renaming C source files or changing shared toolchain
  requirements.
- LLVM values often use optimized representations such as pointers, fat
  pointers, or allocation headers. Verify the actual layout and pointer level
  before generating casts, GEPs, loads, or stores.
- Reference permissions, regions, ownership, borrowing, alias accounting, and
  drops span type checking, flow analysis, and LLVM generation; changes to one
  stage usually require corresponding changes in the others.
- Do not commit generated Visual Studio state from `.vs/` or build outputs.
