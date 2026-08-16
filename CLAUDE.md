# Cone repository instructions

## Project overview

Cone is an in-development systems programming language. This repository
contains its C compiler (`conec`) and a small standard-library component
(`conestd`). The compiler targets LLVM and currently depends on LLVM 13.

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
- `design/`: design notes for compiler and language subsystems. Use
  `design/_index.md` to find the relevant topic, then page in only the notes
  needed for the task.
- `workitems/`: active and backlog compiler/language work. Its
  `workitems/_index.md` plan summarizes the work and acts as the manifest for
  the individual work-item notes; `workitems/__Top Priority.md` identifies the
  current priority sequence.
- `test/test.cone`: broad compiler smoke-test input.
- `test/submod.cone`: imported module used by the smoke-test input.

## Documentation context

- Treat source code as the truth for current compiler behavior.
- Consult `design/_index.md` when a task needs design intent or subsystem
  context. Design notes complement the implementation and may describe
  incomplete or planned behavior.
- Consult `workitems/_index.md` for planned work, dependencies, and links to
  detailed active or backlog items.
- Consult `conesite/public/coneref/index.html` for the language reference
  page index and the surrounding `conesite/` files when changing published
  language documentation or playground behavior.

## Compiler pipeline

`src/c-compiler/conec.c` defines the high-level pipeline:

1. Parse source into heterogeneous `INode` IR nodes.
2. Resolve names.
3. Type-check and infer types while lowering syntactic sugar.
4. Run data-flow analysis from function type checking.
5. Generate LLVM IR and output.

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
WebAssembly targets, the Ninja generator, and the VS 2022 x64 toolchain. On the
current development machine `LLVM_DIR` is `C:\LLVM\13\lib\cmake\llvm`.

```powershell
cmake -S . -B build\x64-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build\x64-release
```

`cl.exe` needs the Visual Studio environment. From a shell that does not
already have it, wrap the build:

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

**There is no automated test runner, and compiling clean is not evidence of
correct runtime behavior.** Treat a successful compile as the first check, not
the last one.

Useful `conec` options: `--ir` writes an IR/AST dump, `--llvmir` writes LLVM IR
before and after optimization, and `--wasm` targets WebAssembly. The output
directory must already exist, and each run writes several files, so use a
git-ignored directory such as `build/`.

For a compiler change:

1. Build `conec` and compile `test/test.cone`, adding focused Cone cases near
   related coverage in that file.
2. Run any fixture suite covering the affected feature. There is no committed
   fixture suite yet; see `workitems/Add test suite.md`, which also names the
   branch holding the overload fixtures until they are restored.
3. Inspect the generated IR when the change affects lowering or symbols.
4. When the change affects runtime behavior, link and run a program.

`conec` emits only an object file. To produce a runnable executable on Windows,
link it against `conestd` and the C runtime from a VS environment:

```powershell
link prog.obj build\x64-release\conestd.lib /OUT:prog.exe /SUBSYSTEM:CONSOLE msvcrt.lib legacy_stdio_definitions.lib
```

A program that spans modules cannot be linked yet. Compiling a module on its
own emits root-module symbol names (`@scaleInt`), while an importing module
references namespaced ones (`@mymod_scaleInt`), so the two never resolve. See
`workitems/Packages and Separate Compilation.md`. Runtime checks must therefore
live in a single source file.

## Change discipline

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
