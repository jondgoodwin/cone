# Cone repository instructions

Cone is an in-development systems programming language. This repository holds its C compiler
(`conec`, targeting LLVM 23.1), the Cone packages (standard library, runtime, domain libraries),
the build tool, and the language's tests and documents.

This file is a manifest: where things are, how to build, and the working rules. It describes no
compiler or package behaviour, because a sentence here goes stale unnoticed. Read the owning document.

## Layout

- `compiler/c/`: the C compiler (`parser/`, `ir/`, `corelib/`, `genllvm/`, `shared/`); only
  `genllvm/` includes LLVM headers. `compiler/c/doc/` holds its internals notes.
- `packages/`: the Cone packages, one folder each: `congo.toml`, `src/<name>.cone` (the root, the
  only file that imports), `tests/`, optionally `examples/`. Also the compiler's default package
  search folder. `packages/conestd/` is the runtime every native program links; it is built with
  the compiler and is not a Congo package.
- `tools/`: `congo/` (build tool), `shaders/` (Slang to SPIR-V), `deps/` (pinned prebuilt C
  libraries), `flowbench/` (flow-analysis cost).
- `test/`: the language test suite (`run.py`, `cases/<group>/`, `codes.toml`).
- `doc/`: `reference/` (reference manual) and `design/` (design notes).
- `samples/`: sample programs. `conesite/`: cone.jondgoodwin.com content. `workitems/`: a pointer
  only; the plan and backlog live outside this repository.

## Where the truth lives

- Source code is the truth for current compiler behaviour.
- `doc/reference/index.html`: the reference manual. Its chapter list is also the spine of the test
  suite's groups: adding a chapter implies asking whether a coverage group is needed, and a feature
  with no chapter has nowhere to be tested. Consult it and `conesite/` when changing published
  language documentation or playground behaviour.
- `doc/design/_index.md`: the design notes, mapped to each compiler phase. A phase note carries its
  principles, what the phase deliberately does not do, its hazards, and a file-and-function map;
  read the one that owns the problem before changing it. Notes may describe planned behaviour and
  say so.
- `compiler/c/doc/` (see its `README.md`): compiler internals. `nodes/_index.md` is what is true of
  every IR node. `compiler/c/conec.c` defines the pipeline: parse, name resolution, type check and
  lowering, flow analysis, LLVM generation.
- A package's `README.md` (where it has one) and its source header comments: package specifics.
  `tools/congo/README.md`: Congo. `compiler/c/doc/nodes/module.md`, "The packages folder": how
  packages are found.

IR nodes may be replaced or lowered during name resolution and type checking. When changing a
language feature, trace and update every affected phase: lexer/parser, IR node construction, name
resolution, type checking/lowering, flow analysis when ownership or borrowing is involved, LLVM
generation, and the Cone smoke-test input.

## Code conventions (C compiler)

- Follow the existing C style and nearby naming. Node structs share `INode` headers and are
  dispatched by `tag`.
- Use the existing node constructors, traversal macros, interned names, namespace lookup, type
  comparison, coercion, and error-reporting helpers.
- Preserve source location data when injecting or replacing nodes, so diagnostics stay useful.
- Keep type safety explicit: no unchecked casts or placeholder values hiding invalid IR states. Pass
  and return the declared enum (`TypeCompare`, `SubtypeConstraint`, `OverloadMatch`), not a bare `int`.
- Comment only where lowering, ownership behaviour, or LLVM representation is not self-evident.
- Give a diagnostic its own `ErrorCode`; never reuse an unrelated one. Lookup failures, visibility
  errors, and no-match errors must stay distinguishable.

## Build

### Windows

Verified: 64-bit LLVM 23.1 (Release, `/MD`) with the X86, WebAssembly and SPIRV targets, Ninja, the
VS 2022 x64 toolchain. Add `-DLLVM_DIR=<llvm root>\lib\cmake\llvm` if LLVM is not on CMake's path.

```powershell
cmake -S . -B build\x64-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build\x64-release
```

`cl.exe` needs the Visual Studio environment. From a shell without it, wrap the build (adjust the
path for your edition; the harmless `'vswhere.exe' is not recognized` line does not matter):

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build\x64-release'
```

`Cone.sln` opens `Cone.vcxproj` (`conec`) and `Conestd.vcxproj` (`conestd`), toolset `v143`, Debug|x64
and Release|x64, output in `x64\<Configuration>\`, against the same LLVM (`$(LLVMDIR)`, default
`C:\LLVM\23\`). Debug turns optimization off and keeps `assert` but links the release CRT (`/MD`),
since LLVM's libraries are Release. The test runner takes that compiler with
`--conec x64\Release\conec.exe`. The projects are hand-kept copies of `CMakeLists.txt` (its sources,
defines, and LLVM library list: the unique set on the link line in
`build\x64-release\build.ninja`); `Cone.vcxproj.filters` groups sources by folder. Update them in the
same change as `CMakeLists.txt`.

### Linux and WSL

On x86-64 Ubuntu, LLVM 23.1 with the X86, WebAssembly and SPIR-V targets comes from apt.llvm.org
(`llvm-23-dev`; its `llvm.sh` adds the repository). With `cmake`, `ninja-build`, `build-essential`:

```sh
cmake -S . -B build/x64-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR=/usr/lib/llvm-23/lib/cmake/llvm
cmake --build build/x64-release
python3 test/run.py --conec build/x64-release/conec
```

- `CMakeLists.txt` uses `find_package(LLVM 23.1 REQUIRED CONFIG)` and defines `conec` and `conestd`;
  `conestd`'s Cone is compiled by the `conec` the same build makes. Do not change the LLVM major
  version without updating source compatibility and both build systems.
- Linking LLVM statically peaks near 850 MB and makes a 155 MB `conec`. On a small machine, change
  `target_link_libraries(conec ${llvm_libs})` to `target_link_libraries(conec LLVM m)` (shared
  `libLLVM.so`; a 1.6 MB `conec`). Keep that change local; never commit it.
- On Linux `conec` makes position-independent code by default (Ubuntu's `cc` links PIE); `--nopic`
  opts out.
- Congo runs on Linux (`python3 tools/congo/congo.py test` in a package's folder, `conec` at
  `build/x64-release/conec`). Packages built on Windows alone (`sdl` and what draws) do not build or
  run there; a package's README says what it needs.
- A test whose output differs by system has `name.linux.out` beside `name.out` (Congo reads
  `name.<host>.out` first). Write tests to print the same everywhere where possible, asking
  `isWindows()` for the values that differ.

## Validating a change

**Compiling clean is not evidence of correct runtime behaviour.** It is the first check, not the last.

**A stale `conec` fails good sources in ways indistinguishable from a language regression.** The test
runner refuses a binary older than a compiler source, and `--build` builds first (it finds the Visual
Studio environment itself). Outside the runner, build before believing any failure.

```powershell
python test/run.py                            # compiler suite: builds nothing, needs no install
cd packages; python ../tools/congo/congo.py test   # packages' own tests (tools/congo/README.md, "Testing a package")
```

The suite compiles every scenario under `test/cases/`, asserts what each one's category and inline
`//~` annotations claim, links and runs the `run` scenarios, and reports tier 0 first. `--list`
prints what would run; a group, scenario, check name or `tag:<phase>` narrows it.
`compiler/c/doc/diagnostics/test-suite.md` is the authoring guide.

`sdl` and everything over it (`window`, `gpu`, `render`, `input`, `controls`, `noise`'s `parity` and
`bake`, `vulkan`'s `runtime` test) link `SDL3.lib` and run with `SDL3.dll`: put the `lib\x64` folder
of SDL3's development kit (here `C:\libs\SDL3-3.4.16\lib\x64`) on `LIB` before `congo test` or
`congo run`; Congo copies `SDL3.dll` beside each program.

For a compiler change:

1. Build `conec` and run `python test/run.py`. A change to `tools/congo/`, to `packages/`, or to how
   `conec` reads a build description or finds a package also runs
   `python tools/congo/test_congo.py` and `congo test` in `packages/`; a shader change also runs
   `python tools/shaders/shaders.py --check`.
2. Add coverage: a scenario in the owning group under `test/cases/`. A fix for a crash or a
   miscompile lands with the case that fails without it; a new `ErrorCode` lands with the scenario
   that provokes it.
3. Inspect the generated IR when the change affects lowering or symbols.
4. When the change affects runtime behaviour, link and run a program.

Useful `conec` options: `--ir` (IR/AST dump), `--llvmir` (LLVM IR before and after optimization),
`--wasm`, `--triple=spirv64-unknown-unknown` (OpenCL form) or `--triple=spirv1.6-unknown-vulkan1.3`
(Vulkan form) for a SPIR-V `.spv` (`compiler/c/doc/phases/generation.md`, section 7, says how little
it covers), `--path=<dir>` (a package folder searched before `packages/`). A source path ending in
`.conebuild` is a build description (`compiler/c/doc/nodes/module.md`, "A described build"). The
output directory must already exist and each run writes several files: use a git-ignored directory
such as `build/`.

`conec` emits only an object file. For a runnable program use `congo run prog.cone` (compiles a lone
file and every package it imports, links, runs), or link by hand. Windows, from a VS environment
(add `synchronization.lib` for a program importing `sync` or `thread`); on Linux use `cc` with
`-lm -lpthread` (the futex is in `libconestd.a`):

```powershell
link prog.obj build\x64-release\conestd.lib /OUT:prog.exe /SUBSYSTEM:CONSOLE msvcrt.lib legacy_stdio_definitions.lib
```

A program spanning an `import` links only in two cases, because an imported module's bodies are
declared and never generated: a package found on the package search path (such as `stdio`) is
compiled into the importing object; or a package compiled on its own from a build description
saying `output: library` exports what its importers need, so its object links beside theirs (a
scenario's `link` key does it in the suite; Congo does it for every package it builds). Otherwise
runtime checks live in what one compile defines: one source file, the files of one folder, or that
folder and the submodules its subfolders draw, plus the packages it imports.
`compiler/c/doc/nodes/module.md` explains why.

## Change discipline

- **A code change is not finished until its notes are.** Every PR that changes compiler behaviour
  lands with these updates in the same commit:
  - a test scenario under `test/cases/` in the owning group;
  - the design note for each phase whose mechanism, invariant, or contract moved (the phase notes
    and any per-node note the change touches);
  - the **implementation-status annotations** on every design note the change touches: a `[planned]`
    now built is deleted, a `[differs]` whose divergence is closed goes with it
    (`doc/design/_index.md`, "Implementation status", has the scheme);
  - the **reference page** under `doc/reference/` for any feature whose built status changed. The
    italic status note at the top of a page says only what is still missing: update it, delete it
    when nothing is missing, never append what was built. Something a reader would look for by name
    gets its own heading (h2/h3) or page with an entry in `doc/reference/index.html`, never only a
    status note or a corner of another feature's section; a minor change or extension goes into the
    existing section's prose. Update any marking on the page's examples too;
  - when you have access to the owner's plan, the work item that owns the subsystem, so what the
    change closed stops being listed as owed and what it opened is recorded.
- **The dependency runs one way: a work item may point at a design note; a design note never points
  at a work item.** That is what makes closing an item prompt the question of which notes, tests and
  reference pages need updating.
- Where the change contradicts something a note asserts, fix the note in that commit. A stale note is
  worse than a missing one, because it gets trusted. If a note needs no change (the normal case for a
  tactical fix), say so in the PR description rather than silently skipping the check.
- **Fixing a note means deleting what stopped being true, not annotating it.** A note describes the
  compiler as it is; it is not a changelog of itself. Do not leave "was measured, then fixed", "this
  note used to list", "now reports", or a hazard entry rewritten to say the hazard is gone. Git holds
  the history. `doc/design/_index.md`, "Conventions", is the full rule.
- Package specifics go in that package's `README.md` or source header comments, not in this file.
- **Determinism rules for compute.** Whatever a compute shader computes that shapes the world
  (geometry, collision, anything a decision reads or another machine must reproduce) keeps
  `packages/noise/README.md`, "Determinism": integer hashes, never a float hash or a GPU random
  number; `// slangc: -fp-mode precise`; add, subtract, multiply and `floor` only, constants exact in
  binary, no division or transcendental on the decision path; unsigned remainders of non-negative
  values; a fixed reduction order (a tree whose shape does not depend on timing, or one invocation
  looping in a fixed order), never float atomics or whichever invocation finishes first (integer
  atomics, a count or an or, are order-free and allowed). Such a result has a parity test against the
  Cone package on a real GPU, every element compared (`noise`'s `parity` and `bake`, `sdf`'s
  `parity`). Visual-only work (particles, fog, lighting, post-processing) is exempt. A compute entry
  point is `[numthreads(x, y, z)]` within WebGPU's limits (`tools/shaders/shaders.py` checks).
- Keep `CMakeLists.txt`, `Cone.vcxproj` (and `.filters`), and `Conestd.vcxproj` synchronized when
  adding, removing, or renaming C source files or changing shared toolchain requirements.
- LLVM values often use optimized representations (pointers, fat pointers, allocation headers).
  Verify the actual layout and pointer level before generating casts, GEPs, loads, or stores.
- Reference permissions, regions, ownership, borrowing, alias accounting, and drops span type
  checking, flow analysis, and LLVM generation; a change to one stage usually needs matching changes
  in the others.
- Do not commit generated Visual Studio state from `.vs/` or build outputs.
