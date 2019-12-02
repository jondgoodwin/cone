# Cone - Next Steps (as of October 20, 2019)

## Current Capability

The Cone compiler supports an ever-growing set of language features.
Enough that it is now possible to write programs that render 3D objects
as OpenGL-based native executables
or [WebGL-based WebAssembly modules](http://cone.jondgoodwin.com/example/index.html).

Cone's implemented features (see below) are roughly comparable to C.
It supports most control flow structures, expressions, and types in some fashion, 
although a lot of refinement is still necessary, including around safety constraints. 
The [Cone Playground](http://cone.jondgoodwin.com/play/index.html)
examples demonstrate some of the language's currently supported features.

The language documentation is quite robust, describing both current and planned features.
Headers on documentation pages highlight what is not yet implemented.

## Current Focus: Generics, Option, Result

- Generic functions, structs and traits, able to use traits as bounds on types
- Option and Result generic types for nullable references, exception handling and closures/iterators

## Feature Status

This table illustrates the current status of Cone's key features:

| Feature | Implemented | Planned |
| --- | --- | --- |
| **Control Flow** | Functions | closures |
| | Methods for types | Constructors, finalizers, set methods |
| | Return (+implicit) | |
| | do, with blocks. 'this' operators | build blocks |
| | if & block (expressions) match | partial pattern match |
| | while, loop, break, continue | each |
| **Names** | Global, local, extern variables | |
| | Module & type namespaces | |
| | include | import |
| **Operators** | +, -, *, /, % | |
| | == < <= > >= | ~~ |
| | = : << | |
| | ++, --, +=, *=, etc. | |
| | . () [] * & | |
| | & \| ^ ~ | |
| | and, or, not/! | |
| **Types** | u8, u16, u32, u64, i8, i16, i32, i64 | |
| | f32, f64 | |
| | Bool: true, false | |
| | struct, traits, and tuples | |
| | array, array refs | slices, collections |
| | variant types | enum |
| | references (incl. nullable) | safety guards |
| | own, rc, borrowed | move/borrow semantics |
| | | gc, arena, pool |
| | static permissions | runtime permissions |
| | pointers | trust block |
| **Polymorphism** | | |
| | | Generics |
| | Macros | CTE |
