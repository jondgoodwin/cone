# Cone - 0.1.0 -> 0.2.0 Project Plan (as of December 8, 2018)

## Current Capability

The Cone compiler supports a decent core set of language features.
Enough that it is now possible to write programs that render 3D objects
as OpenGL-based native executables
or [WebGL-based WebAssembly modules](http://cone.jondgoodwin.com/example/index.html).

Cone's implemented features (see below) are roughly comparable to C.
It is missing macros, conditional compilation, switch, enums and unions.
However, it does support methods, namespaces, tuples, permissions, and forward references.
The [Cone Playground](http://cone.jondgoodwin.com/play/index.html)
examples demonstrate some of the language's currently supported features.

## Current Focus: Add more core features

In order to support more complex 3D programs, we need:

- const variables
- import packages
- each blocks
- initializer and finalizer methods
- builder blocks

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
