# Overload test fixtures

These Cone sources are committed characterization fixtures for the overload
refactor described in `workitems/Overload Refactor.md`. They are not temporary
comparison files: they stay in the repository and are re-run in Phase 2 and in
later overload work.

`test/test.cone` remains the broad compiler smoke input. The files here are
small enough to compile independently so a failure points at one overload
behavior.

## Phase 1 fixtures

| File | Covers |
| --- | --- |
| `phase1-methods.cone` | Same-named `Point.add` methods with value and reference receivers and different arities, overloaded `area` methods selected through a `&<Shape` virtual reference and its vtables, overloaded `bump` methods on a mutable reference receiver, and the five same-named `<-` methods of the compiler-embedded `stdio` stream. |
| `phase1-operators.cone` | User-declared operator methods (`+` overloaded twice, `+=`, `()`, `[]`), pointer comparison/arithmetic/difference/op-assign operators, reference comparison operators, and the overloaded numeric intrinsic operators. |

Phase 1 changes only the compiler's internal representation, so every fixture
must keep compiling with no errors and no warnings, and must keep producing the
same AST and LLVM IR as before the change.

## Running

From the repository root, with a built `conec` (see the build notes in
`.github/copilot-instructions.md`):

```powershell
# Windows
build\x64-release\conec.exe --ir --llvmir -o <outdir> test\overload\phase1-methods.cone
build\x64-release\conec.exe --ir --llvmir -o <outdir> test\overload\phase1-operators.cone
build\x64-release\conec.exe --ir --llvmir -o <outdir> test\test.cone
```

```bash
# Linux/WSL
conec --ir --llvmir -o <outdir> test/overload/phase1-methods.cone
conec --ir --llvmir -o <outdir> test/overload/phase1-operators.cone
conec --ir --llvmir -o <outdir> test/test.cone
```

Use an output directory outside the repository (or a git-ignored one such as
`build/`), because each run writes several files.

## Expected result

Every command above prints `Compile finished ...` with `0 warnings detected`
and exits with status 0. No diagnostic output is expected from any Phase 1
fixture.

Each run writes, into `<outdir>`:

| File | Contents |
| --- | --- |
| `init.ast` | IR/AST dump for the whole program (`--ir`) |
| `<source>.preir` | LLVM IR before optimization (`--llvmir`) |
| `<source>.ir` | LLVM IR after optimization (`--llvmir`) |
| `<source>.obj` | generated object file |

Useful things to compare across a change:

- `add(pt)`, `add(&...)`, and `add(pt, other)` in `init.ast` show which
  same-named candidate each call selected.
- `Point_add:Point` versus `Point_add:+ro Point` in the LLVM IR show that the
  selected candidates keep their distinct generated symbols.
- `Rect->Shape:Vtable` and `Circle->Shape:Vtable` in the LLVM IR show that
  trait/vtable construction still records the concrete method.

## Phase 2

Phase 2 rewrites these fixtures to unique concrete names plus `overload <name>`
declarations and adds positive and negative fixtures with their expected
diagnostics.
