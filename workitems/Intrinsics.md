
Enrich the support for LLVM intrinsics as part of Intrinsic Nodes, including language syntax that allows direct specification of an intrinsic (including primitive operations)

- Lexer change ##intrinsics?
- Ensure all types have a default behavior for `clone`/copy and `hash` (32-bit)

## Float intrinsics own the number method surface

`.sqrt()` and the other float functions belong here, as future capability rather
than as coverage to be written now. The old smoke input carried
`(pt.x).sqrt()`, and when [[Add test suite]] decomposed that file into coverage
groups this was the one construct with no owning group: `core` owns number types
and [[Type Inference and Coercion]] owns conversion, but neither owns the
intrinsic method surface, and no reference-manual chapter describes it.

Test coverage follows the capability. When float intrinsics are built out, the
question R6.1 asks is whether they need their own group and manual chapter —
`design/Test Suite.md` records that a feature with no group means both.
