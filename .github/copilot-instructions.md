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
- `test/test.cone`: broad compiler smoke-test input.
- `test/submod.cone`: imported module used by the smoke-test input.

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
  or placeholder values.
- Add comments only where compiler lowering, ownership behavior, or LLVM
  representation is not self-evident.

## Build and validation

### Windows

The verified Windows configuration uses a 64-bit LLVM 13 installation with
the X86 and WebAssembly targets. Set `LLVM_DIR` to LLVM's CMake package
directory, then build with the VS 2022 x64 environment:

```powershell
cmake -S . -B build\x64-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build\x64-release
```

On the current development machine, `LLVM_DIR` is
`C:\LLVM\13\lib\cmake\llvm`. The checked-in Visual Studio projects use the
Windows 10 SDK and the VS 2022 `v143` toolset, but CMake is the verified path
for the minimal X86/WebAssembly LLVM build.

### CMake, Linux, and WSL

`CMakeLists.txt` uses `find_package(LLVM 13 REQUIRED CONFIG)` and defines the
`conec` executable and `conestd` library. Configure and build with the
repository's existing CMake setup; do not change the LLVM major version
without updating source compatibility and both build systems.

There is currently no configured automated test runner. For compiler changes,
build `conec` and compile `test/test.cone`; add focused Cone cases near related
coverage in that file. Treat successful compilation alone as insufficient
when a change affects generated runtime behavior: exercise or inspect the
generated result as appropriate.

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
