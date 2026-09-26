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
congo test                 build the package, run its tests, build its examples
congo test vec             only the tests and examples with 'vec' in their name
congo test --bless         write a new test's expected output from a run
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
    tests/              the package's tests, one program each (congo test runs them)
    examples/           programs showing the package's API, each a lone file
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
- **A package's example programs live in its `examples/` folder**, which no
  build of the package reads. Each is a lone file, run with
  `congo run examples/<file>.cone`, and imports the package by name like any
  other program, so the package is found through the registries: the
  examples of `packages/math3d` run as they stand, while a package in no
  registry cannot yet run its own that way (`congo test` builds them, below,
  since it finds the package under test first). `congo new` does not make the
  folder.
- **A package's tests live in its `tests/` folder**, which no build of the
  package reads either: below, "Testing a package".

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

1. the Cone repository's own `packages/` (`core`, `stdio`, `libc`, `posix`,
   `math3d` and `testing` are there), then
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
both, and each package's source says what it binds and how. `math3d`, 3D math
in Cone, is built on `libc` for its trigonometry, and its example is
`packages/math3d/examples/tour.cone`.

## Testing a package

**This is the first phase of testing, and deliberately thin** [Jon 26 Sep]:
enough to test a package through its interface today, deciding nothing about
how tests will be scaffolded, mocked or asserted later.

```
packages/math3d/
    src/math3d.cone
    tests/operations.cone     one test: a program
    tests/operations.out      what it must print
    examples/tour.cone        built by congo test, not run
```

`congo test`, in a package's folder (or any folder below it, as for `build`):

1. **Builds the package** as `congo build` does. A package that does not build
   is reported, and nothing of it is tested.
2. **Runs each test.** A test is one program, `tests/<name>.cone`, a lone file
   with its own `mod` line and a `main`, that **imports the package by name**
   as any user's program does. So it is compiled against the package's
   generated include file and linked with the package's object, compiled on
   its own: the path every importer takes, reaching only what the package
   makes `pub`. The package under test is found first under its name, before
   the registries, so a package in no registry can be tested where it stands,
   and a test reaches the copy being tested. Each test is built in
   `build/<mode>/tests/<name>/` and run there, with no arguments and no input,
   for at most 60 seconds.
3. **Compares** what the test printed with `tests/<name>.out`, line ends and
   trailing blank lines aside, and its exit status with 0, or with the number
   in `tests/<name>.exit` where there is one. A mismatch prints a diff of the
   output, or both statuses.
4. **Builds each example**, `examples/<name>.cone`, the same way, and does not
   run it. An example that does not build is a failure.

Each result prints as `test <name> ... ok` or `FAILED`, then a summary line;
`congo test` exits 1 if anything failed. A package with no `tests/` folder says
so, and still builds its examples.

- **`congo test <text>`** runs only the tests and examples whose file name
  contains the text. One that matches nothing is an error.
- **`--bless`** writes `tests/<name>.out` (and `.exit`, if the status is not 0)
  from a run, for a test that has **no** expected file yet. It never rewrites
  one that exists: to change an expected file, edit it, or delete it and bless
  again. **Blessed output is a claim that the package is right; check every
  line of it by hand against the source before committing it.** A value worked
  out from the math catches what a copied run cannot (`math3d`'s test found two
  errors in the code it was ported from that way).
- **At a folder of packages**, such as the repository's `packages/`, `congo
  test` tests each package in it in turn, and ends with a summary of them all.
  Each package's builds go in its own `build/` folder, as a build of it would.
- **A test of an executable package** cannot import it, since only a library
  can be imported; `congo test` says so and fails the tests.

### Writing checks with `testing`

A test may print whatever it likes for its `.out` file to pin, as
`math3d`'s `operations` test does. Or it may check values itself with the
`testing` package, an ordinary library in `packages/`, and print only what
fails:

```
mod rotations;

import testing use *;
import math3d use *;

fn main() i32 {
  imm q = Quat.angleAxis(pi / 2., Xyz[0., 0., 1.]);
  requireFloat(q.dot(q), 1., 0.00001d, "a unit quaternion");
  expectFloat(q.rotate(Xyz[1., 0., 0.]).y, 1., 0.00001d, "x turns to y");
  expectInt(Rect[10, 20, 640, 480].w, 640, "width");
  done();
}
```

- **`expect(cond, label)`**, and **`expectInt`**, **`expectUInt`**,
  **`expectFloat`** (with a tolerance), **`expectStr`** and **`expectBool`**,
  each `(got, want, label)`, `expectFloat` `(got, want, within, label)`. A
  check that passes prints nothing; one that fails prints
  `label: expected W, got G` (`x turns to y: expected 1.000000 within 1e-05, got 0.000000`)
  and the test goes on, so a run reports every failing check.
- **`require…`**, the same six, stops the test when its check fails: it prints
  the failure, then `stopped:` and the summary, and exits with status 1
  through the C library's `exit`.
- **`done()`** prints `3 checks passed` or `3 checks, 1 failed` and returns the
  exit status, 0 or 1, for `main` to return. So a passing test's `.out` is its
  one summary line, which pins how many checks ran as well; a test expected to
  fail says `1` in its `.exit`.

Integers print exactly, floats to six decimal places, strings in double
quotes. The package's source, `packages/testing/src/testing.cone`, documents
each function, and `packages/testing/examples/sums.cone` is a test to read.

It is **the first phase** [Jon 26 Sep]: a library, nothing known to the
compiler. Deferred to the design of testing: a failure naming the expression
it checked and its line, which needs the compiler to hand a function its
caller's expression text and location; test functions marked in source
(`@test`) and a test build; a test reaching a package's private submodules;
fixtures, setup and teardown; and mocks.

### Testing the repository's packages

The repository's packages are tested with

```
cd packages
python ../tools/congo/congo.py test
```

beside the compiler's suite, `python test/run.py`, which tests the compiler.

## What a build does

1. **Find the package**: the nearest `congo.toml` in this folder or one above
   it; or, for `congo run file.cone`, the lone file, a program of one module
   named by its `mod` line or else by the file.
2. **Scan**: walk `src/`, read each file's header, and make the module tree.
   Each module's name comes from a name Congo can read without parsing: the
   manifest's for the root, a one-file module's file, a module folder, and a
   lone file's `mod` line, or else the file. Each must be a Cone name that is
   not a keyword, a reserved word or a permission: the language refuses one on
   a `mod` line, and the build description writes each name bare, where the
   compiler's lexer would not read it as a name. So Congo refuses it, naming
   the file.
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
`libc` and `posix` (Windows), `math3d`'s example run where it stands (Windows),
the loop refusals between packages and between
modules, the manifest's checks, and `congo test` itself (a passing test, a
failing output with its diff, an exit status, a filter, bless, an example that
does not build, a package with no tests, and a folder of packages).
Each program is compiled against the include files its packages' compiles
generated. The test suite (`test/run.py`) does not run Congo, and does not run
the packages' tests: `congo test` in `packages/` does.

## Not built yet

- A package of C sources, which Congo would compile; a C package whose root
  reaches a C-named submodule (untried); library names per platform (`opengl32` on Windows is `GL`
  elsewhere); and link folders in the machine config, where a machine's own
  install location belongs.
- Anything of testing past its first phase, above: how a test is scaffolded,
  what it may reach of a package beyond its interface, mocks and integration
  environments, checks the compiler knows of (the `testing` package's are a
  library), and running examples. That is a design of its own, not yet made.
- WebAssembly (`--target`), which the prototype's `web` mode did.
- A static library file (`.lib`/`.a`) for a library package; it builds an object.
- An internet registry, downloads, a lockfile and version selection.
- Incremental builds: Congo rebuilds everything, every time.
