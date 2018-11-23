# Cone - 0.1.0 -> 0.2.0 Project Plan (as of November 23, 2018)

## Current Capability

The compiler works:
It compiles Cone programs to native or WebAssembly object files.

Cone's implemented features are now comparable to C (see below).
It is missing macros, conditional compilation, switch, enums and unions.
However, it does support methods, namespaces, tuples, permissions, and forward references.
The [Cone Playground](http://cone.jondgoodwin.com/play/index.html)
demonstrates the language's currently supported features.

## Current Focus: Control structures & References

Add more control and data structures:

- [DONE] this blocks & this operators
- [DONE] struct literals
- each blocks
- match blocks and matching operator

Enriching the language's reference support is also a key focus.
Cone's reference capabilities are mostly designed and
[documented](http://cone.jondgoodwin.com/coneref/refrefs.html).

References are Cone's most unique feature,
They make it possible for a single program
to use and safely integrate multiple memory management strategies:
Rust-like single owner (RAII with escape analysis), reference counting,
tracing garbage collection, arenas and pools.

References are usable today, but safety is not yet enforced.
The [data flow analysis pass](http://pling.jondgoodwin.com/post/data-flow-analysis/)
is being built to close this gap.
When this work is done, the compiler will:

- [DONE] Drop and free (or de-alias) variables at the end of their declared scope.
- Initializers and finalizers (including struct/type literals)
- Allow unique references to (conditionally) “escape” their current scope, thereby delaying when to drop and free/de-alias them.
- [DONE] Track when copies (aliases) are made of a reference
- Ensure that lifetime-constrained borrowed references always outlive their containers.
- Deactivate variable bindings as a result of “move” semantics or for the lifetime of their borrowed references.
- Enforce reference (and variable) mutability and aliasing permissions
- Track whether every variable has been initialized and used

## Feature Status

This table illustrates the current status of Cone's key features:

| Feature | Implemented | Planned |
| --- | --- | --- |
| **Control Flow** | Functions | Anon. functions, closures |
| | Methods for types | Constructors, finalizers, set methods |
| | Return (+implicit) | |
| | do, this blocks. 'this' operators | build blocks |
| | if & block (expressions) | match |
| | while, break, continue | each |
| **Names** | Global, local, extern variables | |
| | Module & type namespaces | |
| | include | import |
| **Operators** | +, -, *, /, % | |
| | == < <= > >= | ~~ |
| | = : << | +=, *=, etc. |
| | . () [] * & | |
| | & \| ^ ~ | |
| | and, or, not/! | |
| **Types** | u8, u16, u32, u64, i8, i16, i32, i64 | |
| | f32, f64 | |
| | Bool: true, false | |
| | struct and tuples | |
| | array, array refs | slices, collections |
| | | variant types |
| | references (incl. nullable) | safety guards |
| | own, rc, borrowed | move/borrow semantics |
| | | gc, arena, pool |
| | static permissions | runtime permissions |
| | pointers | trust block |
| **Polymorphism** | | Interfaces, Traits |
| | | Generics |
| | | Macros, CTE |
