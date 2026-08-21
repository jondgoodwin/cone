# Cone - Next Steps (as of August-September 2026)

After four dormant years the project is active again.

## Current Capability

The Cone compiler supports an ever-growing set of language features.
Enough that it is now possible to write programs that render 3D objects
as OpenGL-based native executables
or [WebGL-based WebAssembly modules](http://cone.jondgoodwin.com/example/index.html).

At this point, Cone's implemented features are largely richer than C's core features (although there are still gaps).
There are several ways to find out what it does, and does not, support:

- The "Feature Status" section below gives a high-level view of which features are working or not.
- A more detailed understanding can be gleaned from the robust language reference documentation.
  Each page describes both current and planned features in detail.
  At the top of each page is a summary of which features have not yet been implemented.
- The [Cone Playground](http://cone.jondgoodwin.com/play/index.html)
  examples demonstrate many of the language's currently supported features.

## Current Focus: Modules, Packages and Libraries

Everything the language needs next sits behind one gate. `Option` and `Result` live
inside the compiler as source compiled into it, and so do the `so` and `rc` regions.
Nothing about them changes, and nothing joins them, without rebuilding `conec`.
A core library is a package, so packages come first.

What that delivers:

- **Programs that span source files.** A program spanning modules cannot be linked today.
- **Libraries built once and linked into many programs**, with congo building them,
  publishing their interface, and consuming them.
- **A core library that grows on its own schedule** — `Option`, `Result`, error handling,
  collections — with no compiler release in the way.
- **Memory strategies written in Cone.** Arenas, pools and tracing collectors need a package
  to live in and global state to hold. Single-owner and reference counting are the only
  regions today because they are the only two the compiler implements itself.
- **Namespaces that hold up in a large program** — modules nested inside a package,
  imports that state what they bring in, names folded or renamed where they collide.

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
