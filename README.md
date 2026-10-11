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
 
This repository also holds the Congo build tool (`tools/congo/`) and
example Cone programs (`samples/`), both brought in with their history from
the former [Cone home repository](https://github.com/jondgoodwin/conehome).
The language reference's source is in `doc/reference/`.

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

The build depends on [LLVM 23.1][llvm], built with the X86, WebAssembly and
SPIR-V targets (the test suite compiles for all three), and uses CMake with the Ninja generator from a VS 2022 x64 prompt:

	cmake -S . -B build\x64-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR=<llvm root>\lib\cmake\llvm
	cmake --build build\x64-release

Building LLVM itself on Windows, from `llvm-project-23.1.2.src.tar.xz` at
https://github.com/llvm/llvm-project/releases, with the same generator:

	cmake -S llvm -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD="X86;WebAssembly;SPIRV" -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DCMAKE_INSTALL_PREFIX=<llvm root>
	cmake --build build --target install

Only the `llvm`, `cmake` and `third-party` folders of the source are needed.

## Building (Linux and WSL in Windows)

To build on Linux (x86-64; Ubuntu 24.04 is the one measured), install LLVM 23.1
with the X86, WebAssembly and SPIR-V targets from [apt.llvm.org][aptllvm] (its
`llvm.sh 23` adds the repository, then `sudo apt-get install llvm-23-dev`), and
`cmake`, `ninja-build` and `build-essential`:

	cmake -S . -B build/x64-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR=/usr/lib/llvm-23/lib/cmake/llvm
	cmake --build build/x64-release
	python3 test/run.py --conec build/x64-release/conec

A plain program compiles, links and runs, and so do threads, the `sync`
package's locks and channels, and actors (the `thread`, `sync` and `actors`
packages bind pthreads and the futex); what is built on the rest of the OS
(`iocore`, `process`, `fs` and `posix` beyond `libc`) is still Windows only.
`conec` makes position-independent
code by default on Linux, as the usual `cc` links position-independent
executables (`--nopic` opts out). Linking LLVM's static components takes about
850 MB of memory; on a small machine, changing
`target_link_libraries(conec ${llvm_libs})` in `CMakeLists.txt` to
`target_link_libraries(conec LLVM m)` links the shared `libLLVM.so` instead
(about 100 MB), a change to keep local.

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
See LICENSE and COPYRIGHT for details. The two Phacelle noise files in
`packages/noise/src` (`phacelle.*`) are under the Mozilla Public License 2.0.

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
[aptllvm]: https://apt.llvm.org/

[hello]: http://cone.jondgoodwin.com/play/index.html?gist=f55a8caa2605a11223437167730c53af
[pi]: http://cone.jondgoodwin.com/play/index.html?gist=4510655502edcde9d50d185cfd7f3c2e
[perm]: http://cone.jondgoodwin.com/play/index.html?gist=96ecaecb4827c2b9e6aaad35feb2bfd1
[struct]: http://cone.jondgoodwin.com/play/index.html?gist=cd702c7c1ffc8f97d7762735d04fd9de
