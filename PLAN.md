# Cone - Next Steps (as of August 1, 2021)

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

## Current Focus: 

Since August 2021, development is focused on Cone's distinctive reference mechanisms 
and the safety protections they require. Here is the ordered to-do list:

- Move semantics - basics [Done - Aug]
- Variables initialized before use [Done - Aug]
- Programmable region annotations:
  - Region annotations are structs [Done - Aug]
  - Allocation & initializers [ongoing]
  - Free and finalizers
  - Alias and de-alias
  - Weak references
- Move semantics (advanced): 
  - Auto-drop/dealias
  - Conditional moves
- Borrowed references
- Lifetime constraints
- Static permissions
- Lock permissions

Along the way, related features need work to support the above:

- Sum type syntax adjustment
- Generics refactor [ongoing]
- Proper modules and import with name folding [Done - Oct]
- Void and nil unit type support [Done - Sept]
- @static methods on a type [Done - Sept]
- @move and @opaque structs [Done - Aug]
- Normalize ref types [Done - Aug]
- inline functions [Done - July]
- Block/loop and exit refactor [Done - July]

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
| | so, rc, borrowed | move/borrow semantics |
| | | gc, arena, pool |
| | static permissions | runtime permissions |
| | pointers | trust block |
| **Polymorphism** | | |
| | Generics |  |
| | Macros | CTE |
