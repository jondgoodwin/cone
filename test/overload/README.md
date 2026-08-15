# Overload test fixtures

These Cone sources are committed fixtures for the overload refactor described in
`workitems/Overload Refactor.md`. They are not temporary comparison files: they
stay in the repository and are re-run in later overload work.

`test/test.cone` remains the broad compiler smoke input. The files here are small
enough to compile independently so a failure points at one overload behavior.

Every function and method has a namespace-unique concrete name. A declaration may
additionally join an overload set by naming it:

```cone
fn areaOf overload area(self &) f32:
  w * h
```

`areaOf` names this one function. `area` names the set of every declaration that
overloads it. An overload name may only be used as the name being called; the
concrete name is what you use for anything else.

Selection tests every candidate and requires exactly one of them to accept the
call. An exact match is not preferred over a coercible one, fewer coercions do not
win, declaration order does not break ties, and return types take no part. A
caller resolves an ambiguity by calling a concrete name or by converting its
arguments so only one candidate accepts them.

## Positive fixtures

| File | Covers |
| --- | --- |
| `methods.cone` | Value, reference, and mutable-reference receivers under one overload name; a direct concrete method call; a coercible (widening) argument; a defaulted parameter; overloaded methods reached through a `&<Shape` virtual reference; a trait requirement satisfied by one overload candidate (`Rect`, `Circle`) and by a directly named method (`Square`); the five private `<-` candidates of the compiler-embedded `stdio` stream. |
| `operators.cone` | A user-declared operator overload set (`+` on `Vec2`) plus non-overloaded operator methods (`+=`, `()`, `[]`); a direct call to an operator's concrete name; pointer comparison, arithmetic, difference and op-assign, where the compiler declares two `-` candidates; reference comparison operators; the numeric intrinsics, where `-` names both negation and subtraction. |
| `globals.cone` | Module-level overload sets selected by exact and coercible arguments; direct calls to each concrete name; a private concrete candidate selected through a public overload name; a defaulted parameter on one candidate; static (non-method) overloaded functions inside a type, called as `Maker::make(...)`. |
| `imports.cone` | An overload name folded into another module by a wildcard import, selecting a public candidate and a private one; a public concrete name called directly across the import. Its supporting module is `importsub.cone`, which is imported rather than compiled on its own. |

`imports.cone` covers a case the single-file fixtures cannot reach. A private
concrete candidate is reachable from another module only through a public
overload name, so its symbol must be generated even though the importing compile
does not generate that module's function bodies. Selecting one used to crash LLVM
generation with a null `llvmvar`.

## Negative fixtures

Each file below must fail to compile with the diagnostics listed. Follow-on
diagnostics, which are consequences of the primary error rather than separate
findings, are marked.

| File | Expected diagnostics |
| --- | --- |
| `bad-nomatch.cone` | `Error 1056: No method declared by `add` accepts the call's arguments.` at 15:5 |
| `bad-two-exact.cone` | `Error 1057: More than one function declared by pick accepts these arguments. Call a concrete name or convert the arguments.` at 14:7 |
| `bad-exact-plus-coercible.cone` | `Error 1057: More than one function declared by widen accepts these arguments. Call a concrete name or convert the arguments.` at 13:8 |
| `bad-two-coercible.cone` | `Error 1057: More than one function declared by widen accepts these arguments. Call a concrete name or convert the arguments.` at 13:8 |
| `bad-overload-as-value.cone` | `Error 1058: The overload name show may only be used as the name being called. Use a concrete name for its value.` at 13:12, 16:11, and 19:3; follow-on `Error 1034: Borrowed reference cannot obtain this permission` at 13:11 and `Error 1013: Return expression type does not match return type on function` at 19:3 with `Error 1013: This is the declared function's return type` at 18:15 |
| `bad-duplicate-concrete.cone` | `Error 1011: Global name is already defined. Duplicates not allowed.` at 11:1 with `Error 1011: This is the conflicting definition for that name.` at 8:1, and `Error 1011: Duplicate name twice: every function/method needs its own name.` at 19:3 |
| `bad-overload-name-clash.cone` | `Error 1054: Overload name label is already declared as something that is not an overload name.` at 10:1 and `Error 1054: Overload name size is already declared as something that is not an overload name.` at 18:3 |
| `bad-equal-signatures.cone` | `Error 1055: sameFloat accepts the same arguments as sameInt, so overload same could never choose between them.` at 11:1 and `Error 1055: dupTwo accepts the same arguments as dupOne, so overload dup could never choose between them.` at 19:3 |
| `bad-generic-overload.cone` | `Error 1053: A generic function may not declare the overload name pick` at 7:32 |
| `bad-malformed-overload.cone` | `Error 1052: Expected the overload name that follows 'overload'` at 6:25, `Error 1052: A declaration's overload name must differ from its own name same` at 9:22, and `Error 1052: An anonymous function may not declare an overload name` at 13:26 |
| `bad-trait-mismatch.cone` | `Error 1013: Type declares area, but none of what it declares has the signature Shape requires` at 11:8 |

The overload diagnostic codes are: 1052 malformed `overload` declaration, 1053
generic declaration with an overload name, 1054 overload name already bound to
another kind of declaration, 1055 two candidates accepting the same arguments,
1056 no candidate accepting the call, 1057 more than one candidate accepting the
call, and 1058 an overload name used outside the callee position.

## Running

From the repository root, with a built `conec` (see the build notes in
`.github/copilot-instructions.md`). The output directory must already exist.

```powershell
# Windows
build\x64-release\conec.exe --ir --llvmir -o <outdir> test\overload\methods.cone
build\x64-release\conec.exe --ir --llvmir -o <outdir> test\overload\operators.cone
build\x64-release\conec.exe --ir --llvmir -o <outdir> test\overload\globals.cone
build\x64-release\conec.exe --ir --llvmir -o <outdir> test\overload\imports.cone
build\x64-release\conec.exe --ir --llvmir -o <outdir> test\test.cone
# each of these must fail with the diagnostics listed above
Get-ChildItem test\overload\bad-*.cone | ForEach-Object {
    build\x64-release\conec.exe -o <outdir> $_.FullName
}
```

```bash
# Linux/WSL
conec --ir --llvmir -o <outdir> test/overload/methods.cone
conec --ir --llvmir -o <outdir> test/overload/operators.cone
conec --ir --llvmir -o <outdir> test/overload/globals.cone
conec --ir --llvmir -o <outdir> test/overload/imports.cone
conec --ir --llvmir -o <outdir> test/test.cone
for f in test/overload/bad-*.cone; do conec -o <outdir> "$f"; done
```

Use an output directory outside the repository (or a git-ignored one such as
`build/`), because each run writes several files.

## Expected result

Every positive command prints `Compile finished ...` with `0 warnings detected`
and exits with status 0. No diagnostic output is expected from any positive
fixture. Every `bad-*.cone` fixture exits with status 1 after printing the
diagnostics listed above.

Each positive run writes, into `<outdir>`:

| File | Contents |
| --- | --- |
| `init.ast` | IR/AST dump for the whole program (`--ir`) |
| `<source>.preir` | LLVM IR before optimization (`--llvmir`) |
| `<source>.ir` | LLVM IR after optimization (`--llvmir`) |
| `<source>.obj` | generated object file |

Useful things to confirm across a change:

- In `init.ast`, every call is lowered to a concrete name: `addValue(pt)`,
  `addRef(&...)`, `addOther(pt, other)`, `scaleWide(s, (cast, f64, 2.5f32))`,
  `scaleCount(s, 4i32, 2i32)` with the default argument inserted once, and
  `showInt((cast, i64, 7i32))` with the coercion inserted once. No call keeps an
  overload name as its callee.
- In `init.ast`, an overload name prints as its own binding, listing each
  candidate's concrete name and signature, for example
  `overload show showInt fn(imm n i64) void showFloat fn(imm n f64) void ...`.
- In the LLVM IR, each concrete method generates one uniquely named symbol from
  its own source name with no parameter mangling, for example `@Point_addValue`,
  `@Point_addRef`, `@Point_addOther`, `@Rect_areaOf`, and the private
  `@stdio_IOStream__appendInt`. No symbol is generated for an overload name.
- In the LLVM IR, `@"Rect->Shape:Vtable"` and `@"Circle->Shape:Vtable"` record
  the selected concrete method (`@Rect_areaOf`, `@Circle_areaOf`), never an
  overload node.
- In `imports.preir`, every candidate reachable through the imported overload
  name is declared, including the private one:
  `declare i64 @importsub_scaleInt(i64)` and
  `declare hidden double @importsub__scaleFloat(double)`. The candidates of the
  private overload name `_hidden` are not folded by the import and get no
  declaration.
