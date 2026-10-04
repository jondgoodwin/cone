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
congo run -D FAST -D N=2   a constant for isDefined and definedInt (provisional)
```

`-D NAME` and `-D NAME=123`, given to `build`, `run` or `test`, are passed to
every `conec` compile of the build, the packages it imports included, where
core's `isDefined` and `definedInt` read them. They are a provisional
mechanism whose final design is open (`doc/reference/refintrinsic.html`,
"Constants of the build").

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
    gpu/                optional: modules that run on the GPU too (below, "A package's gpu/ folder")
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
  examples of `packages/geomath` run as they stand, while a package in no
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
| `targets` | what the package may be compiled for: `"native"`, the machine Congo runs on, and `"gpu"`. `["native", "gpu"]` marks a library for the GPU, all of it (below, "Kernels for the GPU"); a package with GPU code beside the rest puts it in its `gpu/` folder instead, and names no target for it. Optional: `["native"]` where it is not written |

A package that needs C libraries linked adds a `[link]` table (below, "C
packages"):

```toml
[link]
libraries = ["SDL3", "user32"]
paths = ["clib"]
runtime = ["SDL3"]
```

| Key | Value |
| --- | --- |
| `libraries` | the C libraries to link, each by its bare name: `SDL3`, not `SDL3.lib` or `libSDL3.a` |
| `paths` | folders the linker searches for them first; a relative one is relative to the package folder. Optional |
| `runtime` | the DLLs a program using the package loads when it runs, each by its bare name: `SDL3` for `SDL3.dll`. Congo copies them beside every program it links (below, "Runtime libraries"). Optional |

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
   `sdl`, `vulkan`, `gpu`, `geomath`, `mesh`, `sculpt`, `noise`, `testing`, `collections`, `arena`, `pool`, `collector`, `thread`, `render`, `window` and `frame` are
   there), then
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
  No importer can name the submodule itself;
- a comment such as `//#line 24 "loc.cone"` marks each line where the file's
  lines stop being the source's, so an error or a panic in a body a program
  compiles from the include file names the package's source file and line.

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
  the program's first, then what it imports. On Windows `SDL3` becomes `SDL3.lib`
  and each path `/LIBPATH:`; elsewhere `-lSDL3` and `-L`. The linker looks in
  `[link] paths` first, then where it always looks: on Windows the folders the
  `LIB` environment variable lists, which Visual Studio's environment sets to
  the Windows SDK's (so `user32`, `shlwapi` or `synchronization` need no path).
- **Any package may have `[link]`**, a program's too. A program importing a C
  package names nothing itself: `frame/examples/spin.cone` imports `sdl`
  (through `window`), and its manifest names `SDL3`.

A library that is not installed is the linker's to report, and Congo adds which
package named which library.

**The packages folder holds two C packages**, `libc` (the ISO C library) and
`posix` (the POSIX functions beyond it, built on `libc`): raw bindings, C names,
C types and C strings, Windows first. They name no `[link]` library, since the C
runtime they bind is on every link line already. `core` imports `libc` for its
allocator, so every build compiles `libc` first. `samples/oslayer` is a tour of
both, and each package's source says what it binds and how. `geomath`, 2-D
and 3-D math in Cone, is built on `libc` for its trigonometry and on
`collections` for the lists its polygon functions return, and its example is
`packages/geomath/examples/tour.cone`. `mesh`, over `geomath`, holds the
surfaces: indexed triangles, the editable half-edge mesh, their generators
and `.obj` export; its example is `packages/mesh/examples/cage.cone`.
`sculpt`, over `mesh`, builds solids procedurally: profiles, paths with
rotation-minimizing frames, extrude, lathe and sweep, and the bend, twist,
taper and curve deformers, and Catmull-Clark subdivision; its examples are
`packages/sculpt/examples/pipe.cone`, `pipedemo.cone` (the pipe smoothed at
three levels of detail) and `creasedcube.cone`. `noise`, over
`geomath`, is coherent noise seeded by integer hashing (PCG hashes, value,
gradient and cellular noise, fractal sums, domain warp, Phacelle stripes),
with a Slang twin, `noise.slang`, for shaders; its example is
`packages/noise/examples/images.cone`, which writes BMPs, and its README
holds the determinism rules.

One more binds a library beyond the C runtime, and names it: `sdl` (SDL3: a
window for Vulkan, its events and clocks, and loading Vulkan; `[link]` names
`SDL3`, which is not part of the Windows SDK, so the `lib\x64` folder of
SDL3's development kit must be on `LIB`; its `runtime` names `SDL3` too, so
Congo finds `SDL3.dll` in that folder and copies it beside every program that
imports `sdl`, at any depth). `window` opens its window through
`sdl`, and `frame`, the loop a world runs in, drives it, so building
`frame`'s example, as `congo test` does, needs `SDL3.lib` on `LIB`, and
running it needs a display. The tests of `sdl`, `window` and `frame` push
events through SDL's own queue and need `SDL3.dll` but no display.

`vulkan` binds Vulkan 1.3 and links nothing: every function is found at run
time through the `vkGetInstanceProcAddr` SDL hands out, so no Vulkan SDK is
needed. `gpu`, the thin WebGPU-shaped layer over it, draws into a window
`window` opens for Vulkan, and `render` draws lit meshes through `gpu`.
Their layout, constants, loader, camera, image and LOD tests run anywhere;
`vulkan`'s `runtime` test, all of `gpu`'s (`headless`, `pipelines`,
`offscreen`), `render`'s `offscreen` and `noise`'s `parity` run against the real Vulkan loader
with no window, so testing `packages/` needs a GPU driver with Vulkan 1.3
and SDL3's `lib\x64` folder on `LIB`, but no display. The shaders of `gpu` and `render`
are Slang compiled ahead of time to SPIR-V, committed and embedded in the
Cone source (`tools/shaders/`), so building and testing needs no Vulkan SDK
either; where the SDK's validation layer is installed, the tests run under
it, synchronization validation included, and fail on any message.
`render`'s example `pipevk.cone`, the pipe demo, imports `sculpt` and
`window` besides.

`thread`, OS threads and the futex, is Cone code over `extern` declarations
of the C runtime and Windows, not a C package, and its `[link]` names
`synchronization`: the Windows SDK's `Synchronization.lib`, which defines
`WaitOnAddress` and its wakes and is not on the C runtime's default link line.

## Runtime libraries

A program that calls into a DLL needs the DLL where Windows looks when the
program starts: beside the program, first. A package's `[link] runtime` list
names the DLLs its importers need, and **Congo copies each one into the
folder of every executable it links**: `congo build` and `congo run`, a lone
file's build, and each test and example `congo test` builds (each in a folder
of its own, so the DLLs go beside each). A library's own build links nothing
and copies nothing.

**An importer inherits the list.** The DLLs copied are those of every package
of the build, at any depth, each once, in the order the libraries are linked:
a program that imports `window` gets `SDL3.dll`, which `sdl` names, and a
program importing a TLS package gets OpenSSL's DLLs, naming nothing itself.

**Where Congo looks for a DLL**, `<name>.dll`, the first copy found being the
one copied:

1. the folders the build's `[link] paths` name, in the order the linker is
   given them;
2. each folder in the **fetched-dependencies folder** (`deps/` at the
   repository's root, or the folder `CONE_DEPS` names), by name: where
   `python tools/deps/fetch.py` puts the prebuilt libraries it fetches,
   pinned and verified (`deps/openssl/` holds OpenSSL 3.5's `libssl-3.dll`
   and `libcrypto-3.dll`, with their import libraries; the script's header
   says what it fetches and how it checks it). Git ignores the folder;
3. the folders `LIB` lists, where an import library's own DLL usually sits
   beside it (SDL3's development kit keeps `SDL3.dll` beside `SDL3.lib`);
4. the folders `PATH` lists.

**A DLL found nowhere is an error** naming the DLL, the package whose list
names it, and the places searched; the program is not linked, and one an
earlier build left is removed:

```
congo: error: tls's [link] runtime names libssl-3, but libssl-3.dll is in none of
the folders Congo looks in: [link] paths (none), then each folder in C:\src\cone\deps
(python tools/deps/fetch.py fills it), then the folders LIB lists, then the folders
PATH lists
```

**A DLL is copied only when it has changed**: where the copy beside the
program is the same, byte for byte, it is left alone. Each copy made prints a
`Copying` line naming where it came from (a test's or an example's does not).

Windows only: elsewhere the list is read and checked, and nothing is copied.

## Kernels for the GPU

**GPU compatibility is declared per package, and Congo enforces it before
compiling** [Jon 3 Oct 2026]. A library marks itself in its manifest:

```toml
[package]
name = "gpusample"
version = "0.1.0"
output = "library"
targets = ["native", "gpu"]
```

The spelling is a placeholder. `targets` must name `"native"` (a package for
the GPU alone is not built yet), and only a library may name `"gpu"`: a
program has a `main`, which a GPU does not run, so a program's kernels go in
its `gpu/` folder (below), or in a library it imports. Marking suits a library
that is GPU-safe throughout (`noise`, `geomath`, `sdf`, `libc`); a package
whose GPU code sits beside code that is not uses a `gpu/` folder instead.

**A marked package's kernels are its compute entry points**, written in its
`src/` like any function (`fn @compute(64) shade(inv Invocation, ...)`; the
reference manual's *GPU Compute* page). There is no list of them: Congo reads
a marked package's source to its end, with the header scan's tokens, for
`fn @compute`, so one in a comment or a string does not count. Compiled for
the CPU, the package is what it always was, each entry point an ordinary
function, the kernel's twin. A marked package with entry points is also
**compiled for the GPU, into `build/<mode>/<name>.spv`**, beside its object:
one SPIR-V module for Vulkan 1.3 (`conec --triple=spirv1.6-unknown-vulkan1.3`)
holding every kernel of the package, each named as its function is. A marked
package with none, such as `geomath`, has nothing to build for the GPU.

```
> congo build
   Compiling libc v0.1.0 (C:\src\cone\packages\libc)
   Compiling core v0.1.0 (C:\src\cone\packages\core)
   Compiling geomath v0.1.0 (C:\src\cone\packages\geomath)
   Compiling gpusample v0.1.0 (C:\src\cone\packages\gpusample)
   Compiling gpusample v0.1.0 for the GPU (C:\src\cone\packages\gpusample)
    Finished debug library object build\debug\gpusample.obj and kernels build\debug\gpusample.spv
```

**Every package a GPU build imports must be marked**, at any depth. Before
anything is compiled, Congo refuses each one that is not, with the imports
that pulled it in:

```
congo: error: kern is compiled for the GPU, and so is every package it imports, each of
which must be marked for the GPU with targets = ["native", "gpu"] in its congo.toml; these
are not:
    plain: kern imports mid at src\kern.cone:3; mid imports plain at C:\pkgs\mid\src\mid.cone:3
    loose: kern imports loose at src\kern.cone:4
```

The check is made for each package compiled for the GPU, and for a marked
package being built, kernels or none, so a marking that cannot hold is found
where it is written. `core`, the prelude, is imported by nobody and is not
asked. `libc` is marked, since `geomath`, `noise` and `sdf` import it for their
maths: its functions are declarations with no bodies, and which of them a
kernel may call is the compiler's to say (its maths, as a GPU's built-ins; the
rest refused). Inside a marked package, what a GPU cannot do is the compiler's
to refuse too, as an error where it is written; Congo decides only which
packages may be compiled for the GPU. `libc`, `geomath`, `noise`, `sdf` and
`gpusample` are marked.

**Each package a kernel imports is compiled from its source, not against its
include file.** On a GPU every function is inlined into the kernel that calls
it (the memory-kinds ruling, 3 Oct), so a kernel's build needs the bodies of
what it calls, and the include file has none. `conec` compiles from source
every package it finds on its package search path (`compiler/c/doc/nodes/module.md`,
"The packages folder"), so the GPU build is the one compile Congo does not
describe: it writes `build/<mode>/gpu/<name>/<name>_gpu.cone`, a module of one
line that imports the package, and compiles that alone for the GPU, with
`--path` naming the folder each package of the build is in (the packages
folder aside, which `conec` searches last anyway) and `CONE_PACKAGES` the one
`core` came from. Only what a kernel reaches is generated, since everything
else is inlined nowhere and dropped. `conec` finds a package by its folder's
name, so Congo checks first that it will find each one where Congo did, which
needs a package compiled for the GPU to be in a folder named for it:

```
congo: error: kern's GPU build compiles kern from its source, which conec finds by its name
on its package search path (C:\pkgs, C:\src\cone\packages): there it finds no kern, not
C:\pkgs\kernels\src\kern.cone; it looks for a package in a folder named for it, and kern is in
C:\pkgs\kernels
```

**A program finds the kernels beside it.** Every program Congo links gets the
`.spv` of each package of its build that has kernels, at any depth, copied
into its folder as a runtime DLL is (above): `congo build` and `congo run` of a
package or a lone file, and each test and example `congo test` builds. A
program's build compiles those packages' kernels as it compiles the packages,
so a program that imports `gpusample` needs nothing more. `gpuwork`'s
`readSpirv` reads one from the program's own folder, wherever the program is
run from:

```
imm spirv = readSpirv("gpusample.spv");
mut kernel = Kernel.make(&device, spirv.view(), "shade", &[READ, READ, READ_WRITE]);
```

A kernel binds its buffers in the order of its parameters, and its error
buffer after them (the *GPU Compute* page), which gpuwork gives each dispatch
and reads itself. `packages/gpusample` is the
sample: a kernel over `geomath`, and a test, `tests/dispatch.cone`, that loads
it, runs it on the GPU and as its twin on the CPU, and compares the two.

**`congo test` validates a package's kernels**, where it has them, with the
Vulkan SDK's `spirv-val --target-env vulkan1.3`, and counts that as a test:
`kernels gpusample.spv ... valid`, or `FAILED` with what `spirv-val` said.
Where `spirv-val` is not on `PATH` the kernels are only built, and the line
says so; that is never a failure. A name filter (`congo test vec`) skips it.

### A package's gpu/ folder

**A package's GPU half goes in a `gpu/` folder beside `src/`** [Jon 3 Oct
2026]. A part of a 3-D world is often both: code that runs on the CPU and
calls into the GPU, and the kernels it dispatches. The CPU half is `src/`, as
always. The GPU half is `gpu/`, and **being in the folder is what marks it**:
no manifest key, and no marker on any function. Congo checks everything in
`gpu/` against the GPU's rules, compiles it into the package for the CPU, so
that `src/` calls the same functions and both halves share one declaration of
each buffer's struct, and compiles it for the GPU, without `src/`, into the
package's one `.spv`. A program can have one as well as a library: its `main`
stays in `src/`, which never reaches the GPU build.

```
gpupart/
    congo.toml              output = "executable"; no targets
    src/gpupart.cone        the root: mod gpupart; its imports; 'use pattern Settings, Cell;'
    src/main.cone           main: calls pattern.cell on the CPU, dispatches 'fill' on the GPU
    gpu/pattern.cone        mod pattern; import noise ...; cell, and fn @compute(64) fill
```

```
> congo run
   ...
   Compiling gpupart v0.1.0 (C:\src\cone\packages\gpupart)
   Compiling gpupart v0.1.0 for the GPU (C:\src\cone\packages\gpupart\gpu)
     Linking build\debug\gpupart.exe
```

**`gpu/` holds submodules of the package's root**, laid out as they would be
in `src/`: a file opening with its own `mod` line is a module of one file
(`gpu/pattern.cone`, the module `pattern`), and a subfolder holding its
designated file is a module folder (`gpu/mesher/mesher.cone` and the files
beside it), under the usual rules inside. `src/` names them as it names any
submodule (the reference manual's *Modules* page): by path, `pattern.cell(i,
s)`, or folded, `use pattern Settings, Cell;`, and a library re-exports them
to its importers with `pub use`. A module of `src/` may import one of `gpu/`'s,
its sister, by name. So nothing new is written to reach `gpu/`, and the
package's root file is still the one place the root's imports are written.

The rules, each checked before anything is compiled:

- **A file directly in `gpu/` opens with its own `mod` line, and a folder
  there holds its designated file.** A file with none would join no module,
  and a folder with none would organise nothing; each is refused, naming it.
  A module of `gpu/` named as one of `src/`'s is refused too.
- **`gpu/` may not use `src/`**: a module of `gpu/` that imports or extends a
  module of `src/`, or imports a name of the root, is refused, since `gpu/` is
  compiled for the GPU without `src/`. Its modules import one another as
  sisters, and `src/` may use all of them.

  ```
  congo: error: gpu\kern.cone:3: import helpers: helpers is a module of lib's src/
  (src\helpers.cone), and gpu/ may not use src/: gpu/ is compiled for the GPU without
  src/. Move what both need into gpu/, which src/ may use
  ```

- **Every package `gpu/` imports must be marked for the GPU**, at any depth,
  as every package a marked package imports must be (above). So I/O, threads,
  actors, locks and collections, whose packages are not marked, are refused
  where `gpu/` imports them, while `src/` imports what it likes:

  ```
  congo: error: lib's gpu/ folder is compiled for the GPU, and so is every package it
  imports, each of which must be marked for the GPU with targets = ["native", "gpu"] in
  its congo.toml; these are not:
      stdio: lib imports stdio at gpu\kern.cone:4
  ```

- **What a GPU cannot do inside `gpu/` is the compiler's to refuse**, as it is
  inside a marked package: everything in `gpu/` is compiled for the GPU,
  called by a kernel or not (a function calling itself, say, is `conec`'s
  error 1256).
- **A package marked for the GPU has no `gpu/` folder**: all of it is compiled
  for the GPU already, so the folder is refused there.

**The kernels are the compute entry points of `gpu/`**, all of them in one
`build/<mode>/<name>.spv`, copied beside every program of the build as a
marked package's is (above), and validated by `congo test` the same way. A
`gpu/` folder with no entry point builds no `.spv`; its imports are checked
all the same when the package is the one being built.

**How the GPU build finds `gpu/`.** It is the marked package's build (above)
with `gpu/` in place of the package: Congo writes
`build/<mode>/gpu/<name>/<name>_gpu.cone`, a module that imports each module
of `gpu/` by its name, each after those of them it imports, and compiles it
for the GPU with `gpu/` first on `--path`, so that `conec` finds each module
there and compiles it from source, with the marked packages they import. (The
order matters: a sister `conec` meets first beside its importer's file, rather
than on the search path, is declared and not compiled.) A module of `gpu/`
named as a package that build compiles (`libc`, say) would hide it, and is
refused with the folder-name message above.

`packages/gpupart` is the sample, a program: `gpu/pattern.cone` hashes each
cell's index with `noise`'s PCG and makes floats from the hash by additions
and multiplications, and `main` runs its kernel `fill` on the GPU through
`gpuwork` and the same `cell` function on the CPU, and compares every word:
all 12,288 are identical. A test program cannot import a program, so
`test_congo.py` runs it, where SDL3's `lib` folder is on `LIB`.

## Testing a package

**This is the first phase of testing, and deliberately thin** [Jon 26 Sep]:
enough to test a package through its interface today, deciding nothing about
how tests will be scaffolded, mocked or asserted later.

```
packages/geomath/
    src/geomath.cone
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
   for at most 60 seconds. On Windows it runs as Congo's debuggee, so that a
   crash, a panic's fail-fast above all, ends at once with the exit status
   Windows would have given it, rather than being held by Windows Error
   Reporting, which takes many seconds when many programs crash at once
   (`congo.py`'s `Debuggee`; the `process` package's "A crash"). A test that
   runs out of time shows what it wrote to stderr.
3. **Compares** what the test printed with `tests/<name>.out`, line ends and
   trailing blank lines aside, and its exit status with 0, or with the number
   in `tests/<name>.exit` where there is one. A test that writes to stderr on
   purpose -- a panic's line, whose exit status is abort's, 3221226505 on
   Windows -- pins that too, in `tests/<name>.err`, written by hand; without
   one, stderr is not compared. A mismatch prints a diff of the output, or
   both statuses.
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
  out from the math catches what a copied run cannot (`geomath`'s first test
  found two errors in the Pegasus code it was ported from that way).
- **At a folder of packages**, such as the repository's `packages/`, `congo
  test` tests each package in it in turn, and ends with a summary of them all.
  Each package's builds go in its own `build/` folder, as a build of it would.
- **A test of an executable package** cannot import it, since only a library
  can be imported; `congo test` says so and fails the tests.

### Writing checks with `testing`

A test may print whatever it likes for its `.out` file to pin, as
`geomath`'s `operations` test does. Or it may check values itself with the
`testing` package, an ordinary library in `packages/`, and print only what
fails:

```
mod rotations;

import testing use *;
import geomath use *;

fn main() i32 {
  imm q = Quat.angleAxis(pi / 2., new Vec3(0., 0., 1.));
  requireFloat(q.dot(q), 1., 0.00001d, "a unit quaternion");
  expectFloat(q.rotate(new Vec3(1., 0., 0.)).y, 1., 0.00001d, "x turns to y");
  expectInt(new IRect(10, 20, 640, 480).w, 640, "width");
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
2. **Scan**: walk `src/`, and `gpu/` where there is one, read each file's
   header, and make the module tree.
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
5. **Compile the kernels** of each package marked for the GPU whose source
   holds compute entry points, and of each package whose `gpu/` folder holds
   them, into `build/<mode>/<name>.spv`, once its imports are known to be
   marked (above, "Kernels for the GPU" and "A package's gpu/ folder").
6. **Link** the objects, the program's first, with `conestd`, the C libraries
   the packages' `[link]` tables name, and the C runtime,
   into `build/<mode>/<name>.exe`, and copy beside it the DLLs their `[link]
   runtime` lists name (above, "Runtime libraries") and the packages'
   kernels. A library stops at its object, `build/<mode>/<name>.obj`, and its
   kernels.

Everything is rebuilt every time.

## What Congo needs on the machine

- **`conec`**: the one `CONEC` names; else the repository's own build,
  `build/x64-release/conec.exe` (Congo warns when it is older than the
  compiler's sources — a stale compiler fails good code); else `conec` on
  `PATH`.
- **`conestd`**, the runtime library: the one `CONESTD` names, else the one
  built beside `conec` (`conestd.lib`, or `libconestd.a`).
- **A linker.** On Windows, Microsoft's `link.exe`: Congo uses the one on `PATH`
  when it is Microsoft's and links for x64, as `conec`'s objects are (an x64
  Developer Command Prompt), and otherwise finds Visual Studio's `vcvars64.bat`
  and takes its environment, so no Developer Command Prompt is needed. What a
  `link.exe` links for is its folder's name, `bin\Host<host>\<target>`, else
  `VSCMD_ARG_TGT_ARCH`. Visual Studio's default Developer Command Prompt links
  for x86: from there Congo, saying nothing, takes out of `LIB` the folders
  under the Visual Studio and Windows SDK folders that environment names,
  keeping the rest (SDL3's `lib`, say), and runs `vcvars64.bat` over it, which
  puts its x64 folders in front of what `LIB` still lists. When no Microsoft
  `link.exe` is found even so, the error names the `vcvars64.bat` run, the end
  of what it printed, and the folders searched. Elsewhere, `cc` or `gcc`.

What these tools print, Congo reads in the encoding each writes: `conec`'s
messages, and `vswhere`'s answer (asked with `-utf8`), as UTF-8; what
`link.exe` and `vcvars64.bat` print, in the code page of the console Congo
shares with them (the OEM code page, 437 or 850, say, of the console Windows
makes for them when Congo has none), which cannot hold every character, so a
path beyond it shows as `?` where the linker wrote one; and the environment
`vcvars64.bat` leaves, which a nested `cmd /u` lists in UTF-16, whole.
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
(shlwapi), a C library built in the test and found through `[link] paths`, a
DLL built in the test and copied beside the program by `[link] runtime` (a
program that cannot start without it, the copy made again only when the DLL
has changed, an importer inheriting the list, the four places searched in
order, a missing DLL refused, and the copies beside each test and example),
`libc` built before `core` with no prelude line, the `samples/oslayer` tour of
`libc` and `posix` (Windows), `geomath`'s example run where it stands (Windows),
the loop refusals between packages and between
modules, the manifest's checks, `congo test` itself (a passing test, a
failing output with its diff, an exit status, a filter, bless, an example that
does not build, a package with no tests, and a folder of packages), and the
GPU build (`targets` checked; a marked package's kernel over `geomath`
compiled into a module whose entry point is read back, in debug and release,
and copied beside a lone file, a package's program, a test and an example;
its validation by `spirv-val`, and the note where it is not on `PATH`;
`fn @compute` in a comment or a string, or in a package not marked, building
nothing; an import not marked refused with its chain, before any compile; and
a package in a folder not named for it refused), and the `gpu/` folder (a
library's and a program's, a module of one file and a module folder, one
importing the other, a module of `src/` importing one of `gpu/`; the
description listing them, the GPU build's module importing them in order, the
entry point read back, and the `.spv` beside an importer; `gpu/` refused its
use of `src/` three ways, an unmarked import, a function calling itself, and
the layout's refusals; no `.spv` without an entry point; and, where SDL3's
`lib` folder is on `LIB`, `packages/gpupart` run on the GPU, every word
identical).
Each program is compiled against the include files its packages' compiles
generated. The test suite (`test/run.py`) does not run Congo, and does not run
the packages' tests: `congo test` in `packages/` does.

## Congo in Cone

Congo is being ported to Cone, in stages, so that nothing needs Python. The
port is a package here, beside `congo.py`: `tools/congo/congo.toml`, its root
`src/congo.cone` (the only file that imports) and the files joining its module
(`commands.cone`, `header.cone`, `paths.cone`, `registry.cone`, `modules.cone`,
`order.cone`, `linker.cone`, `build.cone`, `gpu.cone`, `test.cone`, `sha1.cone`, `util.cone`). `congo.py`
builds it first:

```
cd tools/congo
python congo.py build
```

which writes `tools/congo/build/debug/congo.exe` (`--release`,
`build/release/congo.exe`). After that it builds itself, `build\debug\congo.exe
build` in `tools/congo`, into the same place. Windows will not remove a
running program's file but will rename it, so when the executable a build
links is the Congo running, that Congo is first moved aside to
`congo.exe.old` (one left there before is removed). It cannot `clean` its own
build folder while it runs from it; `congo.py clean` can. Windows only, as its
`process` package and its linking are for now.

**It does everything `congo.py` does:** `congo new` (the manifest and the
template, byte for byte), `congo build` (debug and `--release`, `-D`, a
program linked or a library's object, the build folder, the descriptions),
`congo run` of the current package or a lone file, `congo test` (above, "Testing a
package": the package built, each test built, run within its time and
compared with its `.out`, `.exit` and `.err`, `--bless`, the name filter, the
examples built, a folder of packages), a marked package's kernels and a
`gpu/` folder's (above, "Kernels for the GPU"), and `congo clean` of either, matching
`congo.py` message for message, file for file and exit status for exit status
— the current package's manifest found by walking up and checked as
`congo.py` checks it, the header scan, the package's folder module tree and its
loop refusals, the registries (the repository's `packages/` and the machine
config's folders, every manifest in them read and checked), the build order
and its loop refusals, `conec`, `[link]`, the linker found through
`vcvars64.bat`, and the program run with its exit status passed back. Like
`congo.py`, it builds the whole of a build every time. The repository is the
nearest folder above the executable holding `packages/core`, as `conec` finds
its packages folder.

Cone has no exceptions, so what `congo test` goes on after (a source it
cannot scan or resolve, a compile or link that fails, no `conec` or linker)
is answered as a value, a `Result` whose `Error` is the message `congo.py`'s
`CongoError` carries; `build` and `run` end on the first, as `congo.py` does.
As in `congo.py`, a test's build shares one session with the package's: each
package is compiled into the build folder once, and the linker is found once.

`CONGO_EXE` points `test_congo.py` at it:

```
$env:CONGO_EXE = "tools\congo\build\debug\congo.exe"; python tools/congo/test_congo.py
```

runs every scenario against the Cone Congo.

Where it differs from `congo.py`:

- A manifest or machine config outside the TOML subset the `toml` package reads
  is refused with that package's reason (`line 2: a list may hold only
  strings`), where `tomllib` either reads it or words its error its own way.
- A lone file's build folder is named from its path lower-cased in ASCII only;
  `congo.py` lower-cases the whole of Unicode, so a path with an upper-case
  letter beyond ASCII (`Ü`) gets another folder from each.
- The header scan reads every byte beyond ASCII as a letter of a name, where
  Python's `isalpha()` takes most such characters but not all.
- A path is made absolute from its text (`..` read off), and a symbolic link,
  junction or short (8.3) name in it is kept as written, where `Path.resolve()`
  follows it; and a source file's time is in whole seconds when Congo asks
  whether `conec` is stale.
- Where `congo.py` stops with a Python traceback (a folder or file it cannot
  make, write, read or remove, a test program it cannot start), it says
  `congo: error:` and why, and exits 1.
- `congo test` puts a folder's tests and examples in order by their names
  lower-cased in ASCII only, as it lower-cases a lone file's path.
- A `.exit` file is read as an optional sign and the digits 0 to 9 (with
  `_` between two of them), between spaces and control characters: Python's
  `int()` and `strip()` take other Unicode digits (`٣`) and spaces (a
  no-break space) too, which the Cone Congo refuses as not an integer.
- An expected `.out` or `.err` file that is not UTF-8 is compared byte for
  byte, where `congo.py` stops with a decoding traceback.
- A failing test's diff is `textdiff`'s minimal edit. Where several edits
  are equally small, or `difflib`'s is not the smallest, the two can pair
  lines differently and print other hunks; where the edit is forced, as for
  one changed, added or removed run of lines, they print the same.

## Not built yet

- Runtime libraries anywhere but Windows, where the dynamic loader finds a
  shared library by its own rules (`rpath`, `LD_LIBRARY_PATH`).
- A package of C sources, which Congo would compile; a C package whose root
  reaches a C-named submodule (untried); library names per platform (`opengl32` on Windows is `GL`
  elsewhere); and link folders in the machine config, where a machine's own
  install location belongs.
- Anything of testing past its first phase, above: how a test is scaffolded,
  what it may reach of a package beyond its interface, mocks and integration
  environments, checks the compiler knows of (the `testing` package's are a
  library), and running examples. That is a design of its own, not yet made.
- WebAssembly (`--target`), which the prototype's `web` mode did.
- Of the GPU build: a package for the GPU alone (`targets = ["gpu"]`); a
  module per kernel, or a choice of which kernels to build (a package's one
  `.spv` holds them all, and those of every marked package it imports that
  has kernels too, since each is compiled into it from source); the GPU build
  described to `conec` like every other compile, rather than found on its
  search path; and another GPU target than Vulkan 1.3's SPIR-V (WebGPU's
  WGSL). Of the `gpu/` folder: one package's `gpu/` using another's (a
  part's kernel over a mesher in `sdfmesh/gpu/`, say). Only a marked package
  can be imported into GPU code today, and a marked package has no `gpu/`.
- A static library file (`.lib`/`.a`) for a library package; it builds an object.
- An internet registry, downloads, a lockfile and version selection.
- Incremental builds: Congo rebuilds everything, every time.
