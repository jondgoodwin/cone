# Test Suite

## Requirements

The compiler has no test runner. Every check is run by hand, which means a clean
compile is the only routine evidence a change is correct, and a clean compile is
not evidence of correct runtime behavior. This work item builds the missing
infrastructure.

To be filled in. The shape it needs to cover:

1. **Positive compiles.** A source file compiles clean, for the native and
   WebAssembly targets, with no diagnostics and no warnings.
2. **Negative compiles.** A source file must fail, with an expected diagnostic
   code, message text, and source line:column for each expected error, and a
   way to distinguish a primary error from the follow-on diagnostics that are
   consequences of it. Expected output has to be updatable in bulk when
   diagnostic wording changes intentionally.
3. **Generated output.** Assertions against the IR/AST dump (`--ir`) and the
   LLVM IR (`--llvmir`) — that a call lowered to the concrete symbol it should
   have, that a vtable records what it should, that a declaration was or was
   not emitted.
4. **Runtime behavior.** Compile, link against `conestd`, run, and compare
   output against expectation. Until [[Packages and Separate Compilation]]
   lands, a runnable program cannot span modules, so a runtime case must live
   in a single source file.
5. **Diagnostic code stability.** `ErrorCode` values are unnamed enum positions
   in `src/c-compiler/shared/error.h`, so inserting a code renumbers every code
   below it and silently invalidates expected output. Either give the codes
   explicit values or have the suite verify the mapping.

## Preserved fixtures

The [[Overload Refactor]] work built a fixture suite that already exercises
most of the above by hand. It is preserved on branch **`overload-fixtures`**,
whose tip is the head of the overload refactor PR. The fixtures were removed
from that PR to keep it focused, and are to be restored by the PR that does
this work.

Under `test/overload/` on that branch:

- `README.md` — the specification. For each negative fixture it records the
  expected diagnostic code, the exact rendered message text, the source
  line:column, and which follow-on diagnostics to expect. It also documents
  what the positive runs must show in `init.ast` and the LLVM IR. This is the
  acceptance criteria the new suite should encode; the compiler source has the
  codes and message templates, but nothing else records the expected result.
- `methods.cone`, `operators.cone`, `globals.cone` — positive fixtures.
- `imports.cone` with `importsub.cone` — positive fixture, and the regression
  test for the access violation fixed by commit `34ca637`. It crashes the
  compiler without that fix.
- Eleven `bad-*.cone` — negative fixtures covering diagnostics 1052-1058.

Also on that branch, `CLAUDE.md` documents the build environment, the fixture
suite, and the link-and-run recipe.

Not in the repository: a single-file runtime check of overloaded functions,
methods, defaults and operators was written and run during the overload
refactor, and every value matched expectation. It was never committed, so it
would have to be rebuilt as part of requirement 4.
