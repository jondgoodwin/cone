# Congo, the Cone build tool

Congo builds Cone packages and programs. It reads a package's manifest, finds
every package the source imports, compiles each package **on its own** with
`conec`, and links the objects into an executable. It is version 1: Python 3.11
or later, standard library only, living in the Cone repository beside
`packages/` and the compiler.

```
congo new hello            make a program package, hello/
congo new geometry --lib   make a library package
congo build                build the package this folder is in (debug)
congo build --release      the same, optimised
congo run                  build it and run it
congo run hello.cone       build and run one lone file, no manifest needed
congo run -- a b           arguments after '--' go to the program
congo clean                delete the package's build/ folder
congo clean hello.cone     delete a lone file's build
```

Run it as `tools/congo/congo` (a shell script) or `tools\congo\congo.bat`, or
as `python tools/congo/congo.py`. Put `tools/congo/` on `PATH` to type `congo`.

```
> congo new hello
     Created executable package hello (hello)
> cd hello
> congo run
   Compiling libc v0.1.0 (C:\src\cone\packages\libc)
   Compiling core v0.1.0 (C:\src\cone\packages\core)
   Compiling stdio v0.1.0 (C:\src\cone\packages\stdio)
   Compiling hello v0.1.0 (C:\work\hello)
     Linking build\debug\hello.exe
    Finished debug build\debug\hello.exe
     Running build\debug\hello.exe
Hello, world!
```

## A package

A package is a folder holding a manifest and `src/`. Nothing else is required.

```
hello/
    congo.toml          the manifest
    src/hello.cone      the root module's designated file, named for the package
    src/...             the root module's other files, and its submodules
    tests/              the package's tests (congo new makes it; nothing runs them yet)
    build/debug/        what a build writes; build/release/ for --release
```

There is no include file to write: a library's build generates it (below,
"Include files").

- **`src/<name>.cone` is the root**, where `<name>` is the package's name as the
  manifest gives it. The outer folder's name does not matter.
- **Under `src/` the module rules are the compiler's, unchanged** (the reference
  manual's *Modules* page): the other `.cone` files in `src/` are files of the
  root module, a file opening with its own `mod` line is a one-file submodule, a
  subfolder holding its own designated file (`src/fmt/fmt.cone`) is a submodule,
  and any other subfolder is organisational, its files joining the module around
  it.
- **The compiler never sweeps a Congo package's folders.** Its own folder sweep
  starts only from a designated file named for its folder, `<folder>/<folder>.cone`,
  and `src/hello.cone` is not that — `src` is not `hello`. So Congo walks the
  folders itself and hands the compiler every file by name in the build
  description, and the root's name comes from the manifest, not the folder.
- **Build output goes in the package, in `build/<mode>/`**: each package's build
  description, each package's object, each library's generated include file,
  and the executable, `build/debug/<name>.exe`. The packages a program imports
  are compiled into the program's `build/<mode>/` too, so a registry folder is
  only ever read. `congo clean` deletes the folder, include files and all.

## The manifest: `congo.toml`

```toml
[package]
name = "hello"
version = "0.1.0"
output = "executable"
```

| Key | Value |
| --- | --- |
| `name` | the package's name, a Cone name; the root is `src/<name>.cone` |
| `version` | `MAJOR.MINOR.PATCH`. Recorded and checked for form; never compared yet |
| `output` | `"executable"` or `"library"`: what the package builds into |

A package that needs C libraries linked adds a `[link]` table (below, "C
packages"):

```toml
[link]
libraries = ["SDL2", "opengl32"]
paths = ["clib"]
```

| Key | Value |
| --- | --- |
| `libraries` | the C libraries to link, each by its bare name: `SDL2`, not `SDL2.lib` or `libSDL2.a` |
| `paths` | folders the linker searches for them first; a relative one is relative to the package folder. Optional |

That is the whole manifest; any other key or table is an error. **There is no
dependencies section**: the `import` lines in the source are the dependency
list, and the registries say where each dependency is.

`output` decides executable versus library. A module trait on the root's `mod`
line (`mod hello is Shell;`) says something else — that the program conforms to
the framework that calls it — and Congo does not read it.

## Imports, the registries, and the machine config

Congo reads the **header** of every module's designated file: its leading
comments, its `mod` line, its `import` lines, and nothing after them. That is
the language's rule too: a module's designated file opens with its `mod` line,
and the module's imports come right after it, ahead of everything else. A file
of a folder module other than its designated file has no `mod` line, and so no
imports: Congo reads its header only to see that it has no `mod` line. An
`import` further down, or in such a file, is not seen by Congo; the compiler
refuses it (`ErrorImportLate`), and reports it too as an import the build
description gives no line for.

An import names a module. Congo answers each one this way:

1. `core` is the prelude, which every module has without importing it.
2. In a submodule, the name of a sister module is answered inside the package;
   Congo leaves it to the compiler. Any other name no registry answers is taken
   as a name of the parent, which is a loop (below).
3. Otherwise it is **another package**, looked up by name in the registries.
   An import of the root module that no registry answers is an error naming the
   folders searched. An import by quoted path is an error: a Congo build imports
   packages by name.

**A registry is a folder of package folders**, each subfolder with a
`congo.toml` being one package, known by the name its manifest gives. Congo
searches, in order:

1. the Cone repository's own `packages/` (`core`, `stdio`, `libc` and `posix`
   are there), then
2. each folder the **machine config** lists.

The machine config is `config.toml` in the Congo home, which is the folder
`CONGO_HOME` names, else `~/.congo` (`%USERPROFILE%\.congo` on Windows):

```toml
[registry]
folders = ["D:/cone/mypackages", "~/cone/vendor"]
```

A relative folder is relative to the config file. The first package of a name
found wins. There is no download and no version choice yet: the copy the
registries find is the one used.

**Imports between packages must not loop.** Congo orders the packages so that
each is built after everything it imports, and refuses a loop, naming it:

```
congo: error: import loop between packages: ping -> pong -> ping. Imports between
packages must not loop (ping imports pong at ...\ping.cone:3; pong imports ping at ...).
```

**Nor between the modules of a package** [Jon 23 Sep]. From the same header
scan, a module depends on each sister it imports or extends (`mod a extends b`
is read from the `mod` line), on its parent where it imports a name no registry
answers — a name of the parent — and on each of its own submodules. So a child
importing a name of its parent is a loop of two:

```
congo: error: import loop between modules of package tree: tree -> tree.parser -> tree.
Imports between modules must not loop (tree contains tree.parser; tree.parser imports
shared of tree at src\parser\parser.cone:3). A module may not depend on a module that
contains it: move what they share into a sister both import.
```

The compiler refuses the same loops with the same shape of message
(`ErrorImportLoop`), which is what a direct `conec` run meets.

## Include files

**Every package is compiled on its own, and imported only through its include
file**, which that compile **generates** from the package's own source:
`build/<mode>/<name>.cone`, beside the package's object, in the build folder of
the package being built. Congo compiles each package before whatever imports
it, and every importer's build description names that generated file, never one
written by hand; `core`'s and `stdio`'s are generated the same way. A
`<name>.cone` left at a package's root from before is not read, and Congo warns
that it is not. The include file is Cone source, the package's root module as
an importer needs to see it, with a banner saying it is generated:

- each function, method, operator and global the package's object defines is
  declared `extern` and without a body, and spelled with the package's Cone
  name, which is the name its object exports;
- its types are written with their fields, and what an importer must have the
  body of — an `inline` or generic function, a generic type's methods, a macro —
  is written whole;
- a private name that such a body reaches is declared too;
- a global whose members the package folds into its namespace
  (`pub mut config Config = ... pub use *;`) is declared `extern` with the same
  clause (`pub extern mut config Config pub use *;`), so an importer reaches
  the folded names as the package does;
- what the root reaches in one of the package's submodules — a type a public
  function names, what an `inline` body calls, what a `pub use` re-exports — is
  declared in a private nested block, `mod vec { ... }`, holding only that, so
  a package laid out as a root re-exporting its submodules' API works as it is.
  No importer can name the submodule itself.

**An include file is a module file like any other** [Jon 25 Sep]: it opens with
its `mod` line, and its imports follow. So a package whose public functions
take or return another package's types imports that package in its include
file, as its source does:

```
mod a;

import b;

pub extern fn start(n i64) b.Counter;
```

A program that imports `a` alone can then use the `b.Counter` that `start`
returns, its fields and its methods, without importing `b`. It cannot name `b`
itself: `b` is a name of `a`'s include file, private to it unless the import
says `pub`. A program that wants `b`'s names writes `import b`.

A build of any program that prints leaves `build/<mode>/stdio.cone` to read as
the example. Since it is generated on every build from the source, it cannot
fall out of step with it. The reference manual's *import and extern* page,
"Declaring a Cone package", is the rule, and `compiler/c/doc/nodes/module.md`,
"Generating the include file", is how it is made.

## C packages

A **C package** wraps a C library, so that a program imports it by name and
writes no C declarations of its own. Its source is `src/<name>.cone`, a C-named
module (`@c` on its `mod` line) declaring what the library defines, and its
manifest names the library:

```
winstr/
    congo.toml          [package] ... and [link] libraries = ["shlwapi"]
    src/winstr.cone     mod @c("Str") winstr;  pub extern { fn ToIntA(s *u8) i32; }
```

- **Its include file is generated, as any package's is.** A C library's
  declarations are already an include file's shape (public, no bodies, the
  symbols supplied elsewhere), so the generated file is the source with its
  banner, `@c` line and all. It used to be the source itself; one rule for every
  package is simpler, and a C package may now hold a Cone helper beside its
  declarations, which its own object defines.
- **It is compiled and linked like any package.** Compiled on its own, a module
  of `extern` declarations is an empty object, as `core`'s is; Congo links it
  rather than skipping it, which keeps it right when the `@c` module holds a Cone
  body too.
- **Every library a package of the build names is linked into the executable**,
  once each, after the objects and `conestd`, in the order the objects are:
  the program's first, then what it imports. On Windows `SDL2` becomes `SDL2.lib`
  and each path `/LIBPATH:`; elsewhere `-lSDL2` and `-L`. The linker looks in
  `[link] paths` first, then where it always looks: on Windows the folders the
  `LIB` environment variable lists, which Visual Studio's environment sets to
  the Windows SDK's (so `opengl32`, `user32` or `shlwapi` need no path).
- **Any package may have `[link]`**, a program's too: `samples/opengl` binds
  SDL2 and OpenGL in submodules of its own and names both libraries in its own
  manifest.

A library that is not installed is the linker's to report, and Congo adds which
package named which library.

**The packages folder holds two C packages**, `libc` (the ISO C library) and
`posix` (the POSIX functions beyond it, built on `libc`): raw bindings, C names,
C types and C strings, Windows first. They name no `[link]` library, since the C
runtime they bind is on every link line already. `core` imports `libc` for its
allocator, so every build compiles `libc` first. `samples/oslayer` is a tour of
both, and each package's source says what it binds and how.

## What a build does

1. **Find the package**: the nearest `congo.toml` in this folder or one above
   it; or, for `congo run file.cone`, the lone file, a program of one module
   named by its `mod` line or else by the file.
2. **Scan**: walk `src/`, read each file's header, and make the module tree.
3. **Resolve**: answer each import through the registries, scan each package
   found the same way, and order them all, each after what it imports: `core`
   first, after what `core` itself imports (`libc`).
4. **Describe and compile**: for each package, write its **build description**,
   `build/<mode>/<package>.conebuild`, and run `conec` on it alone. The one
   being built gets the `output` its manifest says; every package it imports is
   a `library`, and each library's compile writes its include file,
   `build/<mode>/<package>.cone`, which Congo checks is there. Each description
   lists the package's modules and their files and, per module, where each
   import's include file is — the one that package's compile generated; and,
   at its top, a **package line** for every package in the package's
   dependency closure, direct or indirect, `core` first and each after what it
   imports, which is where an include file's own imports are found, and, for
   `core`, where the prelude is loaded from. The package's own modules import
   only what their own lines give them. The format is the compiler's:
   `compiler/c/doc/nodes/module.md`, "A described build".
5. **Link** the objects, the program's first, with `conestd`, the C libraries
   the packages' `[link]` tables name, and the C runtime,
   into `build/<mode>/<name>.exe`. A library stops at its object,
   `build/<mode>/<name>.obj`.

Everything is rebuilt every time.

## What Congo needs on the machine

- **`conec`**: the one `CONEC` names; else the repository's own build,
  `build/x64-release/conec.exe` (Congo warns when it is older than the
  compiler's sources — a stale compiler fails good code); else `conec` on
  `PATH`.
- **`conestd`**, the C runtime library: the one `CONESTD` names, else the one
  built beside `conec` (`conestd.lib`, or `libconestd.a`).
- **A linker.** On Windows, Microsoft's `link.exe`: Congo uses the one on `PATH`
  when it is Microsoft's (a Developer Command Prompt), and otherwise finds Visual
  Studio's `vcvars64.bat` and takes its environment, so no Developer Command
  Prompt is needed. Elsewhere, `cc` or `gcc`.
- **The prelude.** Every package compiled after `core` loads the prelude from
  `core`'s generated include file, which its description's package line for
  `core` names, after a line for `libc`, which `core`'s include file imports.
  `libc` itself is compiled before `core`, with no line for it: a C-named
  module gets no prelude, so `libc` needs nothing of `core`, and its compile
  loads `core` from the packages folder as `core`'s own compile does. Built on
  its own, `libc` is the whole build. `core`'s own compile has no such line, and `conec` loads the
  prelude from its packages folder: Congo sets `CONE_PACKAGES` to the registry
  folder it found `core` in, so the prelude is the very file Congo compiles
  `core` from.

A lone file's build goes in the Congo home, `lone/<file>-<hash>/<mode>/`, not
beside the file.

## Checking Congo

```
python tools/congo/test_congo.py
```

builds real programs in a temporary folder with the repository's `conec` (build
it first, `python test/run.py --build`): `congo new` then `congo run`, a
package with submodules printing through `stdio`, a lone file, a library from a
registry folder that itself imports `stdio` (beside a stale hand-written include
file, which is not read), three packages chained through an include file that
imports another package's, a library of submodules re-exported at its root whose
include file holds nested blocks, a C package linking a Windows system library
(shlwapi), a C library built in the test and found through `[link] paths`,
`libc` built before `core` with no prelude line, the `samples/oslayer` tour of
`libc` and `posix` (Windows), the loop refusals between packages and between
modules, and the manifest's checks.
Each program is compiled against the include files its packages' compiles
generated. The test suite (`test/run.py`) does not run Congo.

## Not built yet

- A package of C sources, which Congo would compile; a C package whose root
  reaches a C-named submodule (untried); library names per platform (`opengl32` on Windows is `GL`
  elsewhere); and link folders in the machine config, where a machine's own
  install location belongs.
- `congo test`, and anything reading `tests/`.
- WebAssembly (`--target`), which the prototype's `web` mode did.
- A static library file (`.lib`/`.a`) for a library package; it builds an object.
- An internet registry, downloads, a lockfile and version selection.
- Incremental builds: Congo rebuilds everything, every time.
