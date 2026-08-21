
Internal constants
- Compiler options
- Defines

If conditional generation
- Eval of constant expression

Looping (`each`) generation over some AST collection

## Global data sourced from a file at compile time

A global data structure whose contents come from a file in the source tree,
embedded at compile time either as the raw bytes or in a pre-processed form —
JSON parsed and re-emitted as the encoded structure it describes, say. Prior art
is Rust's `include_bytes!`/`include_str!`, Zig's `@embedFile` and C++23's
`#embed`; the pre-processed variant is closer to Rust's `build.rs` generating
source, and is the more interesting half.

To decide: whether pre-processing is a compiler-resident format list or
programmable from Cone, what the embedded value's type is and how it is
declared, and what the file path is relative to — the declaring source file is
the least surprising base.

Its one consequence for [[Packages and Separate Compilation]]: a package's source
set is not `**/*.cone`, so whatever describes a package's contents has to account
for files the parser never reads.