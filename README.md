# Cone Programming Language
Cone is a fast, fit, friendly, and safe systems programming language.
It features:

- Do-it-your-way memory management
- Versatile type system (incl. variant types and slices)
- Memory, thread & type safe
- Extensive code reuse features
- Lean, native runtime
- Concise, readable syntax

The Cone compiler is currently under development.
The current status and next steps are documented in [PLAN.md][plan].

## Documentation and Other Resources

 - [Cone web site](http://cone.jondgoodwin.com)
 - [Web-based playground][playground], offering pre-built examples in a drop-down
 - [Cone Language Reference][coneref] documentation
 - [Programming Linguistics blog](http://pling.jondgoodwin.com)
 
The [Cone home repository](https://github.com/jondgoodwin/conehome)
offers a rudimentary build environment for Cone programs,
including the Congo build tool and additional example Cone programs.

## Language Features

When finished, Cone will support these features:

- Safely manage memory your way
  - Lexical, single-owner strategy for performance
  - Ref-counted or tracing GC for flexibility
  - Lifetime-constrained references for performance/simplicity
  - Custom allocators (pools/arenas) for performance
- Lightweight concurrency
  - Co-routines, threads and actors
  - Lockless and locked permissions for safe data sharing
- Compile-time memory, type, and concurrency safety
- Robust type system
  - Sum types, structs, arrays, slices, ranges, aliases
  - struct subtyping via trait, & delegated inheritance
  - Attach methods to any nominal type
- Modules, macros, templates and meta-programming
- Extensible pattern matching
  - 'match' blocks using custom match methods
  - Content extraction during matching
- Functions, Methods and Closures
  - Multiple return values and implicit return
  - Computed properties
- 'with' block for context management
- Concise, readable code:
  - 'this'-implied prefix operators for method cascading, etc.
  - Operator overloading
  - Type inference
  - Parallel assignment
  - Auto-detected off-side rule
  - Blocks and 'if' are expressions
- Unicode-aware (UTF8) text strings and variable names
- Fast compilation and convenient packaging

## Building (Windows)

A Visual Studio C++ solution can be created using the Cone.vcxproj project file.
The generated object and executable files are created relative to the location of the 
solutions file. 

The build depends on [LLVM 13][llvm] being installed and libs/includes found at $(LLVMDIR).

Building LLVM on Windows can be a pain. Some notes:

1. Download llvm-13.0.0.src.tar.xz from https://github.com/llvm/llvm-project/releases 
2. Extract the src to a path having no spaces (e.g., D:\libs\llvm-13.0.0.src)
3. Make sure you have a recent version of CMake (which has CMake-GUI)
4. Use CMake-GUI to configure. Fill in the source folder (above) and a new target folder (D:\libs\llvm-13.0.0.build) and hit Configure button
   - When it asks if build folder should be created, say Yes
   - When it prompts for a generator, specify the Visual Studio version installed (e.g., 15).
   - In next drop down, select "Win32" to build 32-bit compiler.
5. In CMake-Gui, change defaults if you want (e.g., 32-bit target triple is i686-pc-win32), and then hit "Generate" button.
6. From build folder, double clicked on llvm.sln file to open Visual Studio. 
   - Select Release, and then build ALL_BUILD
   - Select Debug, and then build ALL_BUILD. (fails if not enough memory, shut all pgms off)
7. Copy llvm-c from llvm-13.0.0.src/include to llvm-13.0.0.build/include
8) In command prompt: setx LLVMDIR d:\libs\llvm-13.0.0.build\

## Building (Linux and WSL in Windows)

To build on Linux:

	sudo apt-get install llvm-13-dev
	cmake .
	make

Note: Sometimes, it is necessary to custom-build LLVM, e.g.:

	mkdir llvm
	cd llvm
	svn co http://llvm.org/svn/llvm-project/llvm/trunk llvm-src
	cd llvm-src/tools
	svn co http://llvm.org/svn/llvm-project/cfe/trunk clang
	svn co http://llvm.org/svn/llvm-project/lld/trunk lld
	cd ../..
	mkdir llvm-build
	cd llvm-build
	CC=clang CXX=clang++ cmake -G "Unix Makefiles" -DLLVM_BUILD_LLVM_DYLIB=ON -DCMAKE_INSTALL_PREFIX=/llvm/wasm -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=WebAssembly /llvm/llvm-src
	make
	make install

## Building (Mac OS)

To build on Mac OS:*

	brew install --with-toolchain llvm
	llvm-config --bindir

CMake will auto-detect LLVM, so all you should need to do:

	cmake .
	make

More detailed instructions: https://github.com/git-yledu/cone-misc/blob/main/install_cone_on_mac.md

## License

The Cone programming language compiler is distributed under the terms of the MIT license. 
See LICENSE and COPYRIGHT for details.

[3dweb]: http://cone.jondgoodwin.com/web3d.html
[gmm]: http://jondgoodwin.com/pling/gmm.pdf
[plan]: https://github.com/jondgoodwin/cone/blob/master/PLAN.md
[coneref]: http://cone.jondgoodwin.com/coneref/index.html
[showcase]: http://cone.jondgoodwin.com/coneref/showcase.html
[playground]: http://cone.jondgoodwin.com/play/index.html
[examples]: http://github.com/jondgoodwin/cone/tree/master/text
[acorn]: https://github.com/jondgoodwin/acornvm
[acornref]: http://web3d.jondgoodwin.com/acorn
[llvm]: https://llvm.org/

[hello]: http://cone.jondgoodwin.com/play/index.html?gist=f55a8caa2605a11223437167730c53af
[pi]: http://cone.jondgoodwin.com/play/index.html?gist=4510655502edcde9d50d185cfd7f3c2e
[perm]: http://cone.jondgoodwin.com/play/index.html?gist=96ecaecb4827c2b9e6aaad35feb2bfd1
[struct]: http://cone.jondgoodwin.com/play/index.html?gist=cd702c7c1ffc8f97d7762735d04fd9de
