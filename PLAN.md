# Cone - Next Steps (as of June 1, 2021)

## Current Capability

The Cone compiler supports an ever-growing set of language features.
Enough that it is now possible to write programs that render 3D objects
as OpenGL-based native executables
or [WebGL-based WebAssembly modules](http://cone.jondgoodwin.com/example/index.html).

At this point, Cone's implemented features are largely richer than C's core features (although there are still gaps).
There are several ways to gain a sense of what it supports and does not support:

- The "Feature Status" section below gives a high-level view of which features are working or not.
- A more detailed understanding can be gleaned from the robust language reference documentation.
  Each page describes both current and planned features in detail.
  At the top of each page is a summary of which features have not yet been implemented.
- The [Cone Playground](http://cone.jondgoodwin.com/play/index.html)
  examples demonstrate many of the language's currently supported features.

## Current Focus: Modules, Packages and Libraries

The language is becoming progressively more solid. As more features come online, bugs get squashed, and 
the design improves for versatility and ease-of-use, it becomes attractive to write larger 
and more interesting programs. As these programs grow bigger, so grows the need for packaging reusable
module and type functionality into libraries.

To make this easier:

- Do not implementation values/code, but still generate public names, for imported modules
- Improve name mangling for modules, types, generic and overloaded functions
- Improve the congo build tool to build the standard and other libraries, and then link them in to executable programs
- Implement full name folding behavior for modules, including special-case handling for core library and single-type modules
- Add some module capabilities to types (e.g., include, macro methods, typedef, subtypes)

## Feature Status

This table illustrates the current status of Cone's key features:

| Feature | Implemented | Planned |
| --- | --- | --- |
| **Control Flow** | Functions | closures |
| | Methods for types | Constructors, finalizers, set methods |
| | Return (+implicit) | |
| | do, with blocks. 'this' operators | build blocks |
| | if & block (expressions) match | partial pattern match |
| | while, break, continue | each |
| **Names** | Global, local, extern variables | |
| | Module & type namespaces | |
| | include, import | |
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
| | struct, traits, and tuples | inheritance |
| | array, array refs | slices, collections |
| | union & trait variant types | |
| | references (incl. nullable) | safety guards |
| | so, rc, borrowed | move/borrow semantics |
| | | gc, arena, pool |
| | static permissions | runtime permissions |
| | pointers | trust block |
| **Polymorphism** | | |
| | Generics |  |
| | Macros | CTE |
