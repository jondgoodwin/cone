# Cone Programming Language
Cone is a statically-type, object-oriented programming language, 
used to build realistic, interactive, [3D web][3dweb] worlds.
It is designed for real-time performance and responsiveness, compile-time safety, 
and programmer flexibility and convenience.

The Cone compiler is currently under development and in very early stages.
The current status and next steps are documented in [PLAN.md][plan].

## Documentation and Examples

 - [Cone Language Reference][coneref]
 - [Showcase][showcase]
 - [Web-based playground][playground], using pre-built gists:
   - [Hello, world!][hello]
   - [Calculating pi][pi]
   - [Permissions][perm]
   - [Structs][struct]

Note: Cone resembles its dynamically-typed cousin [Acorn][acorn],
which has its own [reference documentation][acornref].

## Language Features

When finished, Cone will support these features:

- Readable, modular marriage of 3D content and behavior:
  - Simple, outline-like declarative syntax for content
  - Procedural generation and behavior interwoven in content
  - Snap-together, Internet-hosted, url-located parts
- Compile-time memory, type, and concurrency safety checks
- [Gradual memory management][gmm]: safely manage memory your way
  - Lexical, single-owner strategy for performance
  - Ref-counted or tracing GC for flexibility
  - Lifetime-constrained references for performance/simplicity
  - Custom allocators (pools/arenas) for performance
- Lightweight concurrency
  - Co-routines, threads and actors
  - Lockless and locked permissions for safe data sharing
- Robust type system
  - Sum types, structs, arrays, slices, ranges, aliases
  - struct subtyping via trait, interface, & parent inheritance
  - Attach methods to any type
- Modules, macros, templates and meta-programming
- Extensible pattern matching
  - Type-defined '~~' match operator
  - 'match' blocks using custom match methods
  - Content extraction during matching
- Functions, Methods and Closures
  - Multiple return values and implicit return
  - Computed properties
- 'do' block for context management
- Concise, readable code:
  - 'this'-implied prefix operators for method cascading, etc.
  - Operator overloading
  - Control clauses for 'if', 'while', 'each'
  - Type inference
  - Parallel assignment
  - Auto-detected off-side rule
  - Blocks and 'if' are expressions
- Unicode-aware (UTF8) text strings and variable names
- Fast compilation and convenient packaging

## Building (Windows)

A Visual Studio C++ solution can be created using the Cone.vcxproj project file.
The generated object and executable files are created relative to the location of the 
solutions file. The build depends on [LLVM 5][llvm] being installed and available at $(LLVMDIR).

## Building (Linux)

To build on Linux:

	sudo apt-get install llvm-5.0-dev
	cmake .
	make

## Building (Mac OS)

To build on Mac OS:

	brew install --with-toolchain llvm
	llvm-config --bindir

Modify CMakeLists.txt so that LLVM_HOME points to LLVM's path
(e.g., "/usr/local/Cellar/llvm/5.0.1" without the /bin) and 
modify LLVM_LIB to "libLLVM.dylib".

	cmake .
	make

## License

The Cone programming language compiler is distributed under the terms of the MIT license. 
See LICENSE and COPYRIGHT for details.

[3dweb]: http://web3d.jondgoodwin.com/faq.html
[gmm]: http://jondgoodwin.com/pling/gmm.pdf
[plan]: https://github.com/jondgoodwin/cone/blob/master/PLAN.md
[coneref]: http://web3d.jondgoodwin.com/cone
[showcase]: http://web3d.jondgoodwin.com/cone/showcase.html
[playground]: http://playcone.jondgoodwin.com
[examples]: http://github.com/jondgoodwin/cone/tree/master/text
[acorn]: https://github.com/jondgoodwin/acornvm
[acornref]: http://web3d.jondgoodwin.com/acorn
[llvm]: https://llvm.org/

[hello]: http://playcone.jondgoodwin.com/?gist=f55a8caa2605a11223437167730c53af
[pi]: http://playcone.jondgoodwin.com/?gist=4510655502edcde9d50d185cfd7f3c2e
[perm]: http://playcone.jondgoodwin.com/?gist=96ecaecb4827c2b9e6aaad35feb2bfd1
[struct]: http://playcone.jondgoodwin.com/?gist=cd702c7c1ffc8f97d7762735d04fd9de