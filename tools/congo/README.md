# Congo, the Cone build tool

Congo builds Cone packages and programs. It reads a package's manifest, finds
every package the source imports, compiles each package **on its own** with
`conec`, and links the objects into an executable. It is version 1: Python 3.11
or later, standard library only, living in the Cone repository beside
`packages/` and the compiler.

```
congo new hello            make a program package, hello/
congo new geometry --lib   make a library package, with its include file
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
    hello.cone          a library's include file, written by hand (below)
    build/debug/        what a build writes; build/release/ for --release
```

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
  description, each package's object, and the executable,
  `build/debug/<name>.exe`. The packages a program imports are compiled into
  the program's `build/<mode>/` too, so a registry folder is only ever read.

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

That is the whole manifest; any other key or table is an error. **There is no
dependencies section**: the `import` lines in the source are the dependency
list, and the registries say where each dependency is.

`output` decides executable versus library. A module trait on the root's `mod`
line (`mod hello is Shell;`) says something else — that the program conforms to
the framework that calls it — and Congo does not read it.

## Imports, the registries, and the machine config

Congo reads the **header** of every source file: its leading comments, its
`mod` line, its `import` lines, and nothing after them. So a package's imports
must come right after its `mod` line (or at the top of a file with none); an
`import` further down is not seen by Congo, and the compiler reports it as an
import the build description gives no line for.

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

1. the Cone repository's own `packages/` (`core` and `stdio` are there), then
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
file**: `<name>.cone` at the package root, beside `congo.toml`. It is Cone
source, the package's root module as an importer needs to see it:

- each function, method, operator and global the package's object defines is
  declared `extern` and without a body, and spelled with the package's Cone
  name, which is the name its object exports;
- its types are written with their fields, and what an importer must have the
  body of — an `inline` or generic function, a generic type's methods, a macro —
  is written whole;
- a private name that such a body reaches is declared too.

`packages/stdio/stdio.cone` is the example. Include files are written by hand
for now, so one must be kept in step with the package's source: a declaration
the source no longer defines is a link error in every program that uses it.
`congo new --lib` writes a starting pair. The reference manual's *import and
extern* page, "Declaring a Cone package", is the rule.

## What a build does

1. **Find the package**: the nearest `congo.toml` in this folder or one above
   it; or, for `congo run file.cone`, the lone file, a program of one module
   named by its `mod` line or else by the file.
2. **Scan**: walk `src/`, read each file's header, and make the module tree.
3. **Resolve**: answer each import through the registries, scan each package
   found the same way, and order them all, `core` first, each after what it
   imports.
4. **Describe and compile**: for each package, write its **build description**,
   `build/<mode>/<package>.conebuild`, and run `conec` on it alone. The one
   being built gets the `output` its manifest says; every package it imports is
   a `library`. Each description lists the package's modules and their files
   and, per module, where each import's include file is. The format is the
   compiler's: `compiler/c/doc/nodes/module.md`, "A described build".
5. **Link** the objects, the program's first, with `conestd` and the C runtime,
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
- **The prelude.** `conec` loads `core` itself, from its packages folder; Congo
  sets `CONE_PACKAGES` to the registry folder it found `core` in, so the prelude
  the compiler loads is the very file Congo compiles `core` from.

A lone file's build goes in the Congo home, `lone/<file>-<hash>/<mode>/`, not
beside the file.

## Checking Congo

```
python tools/congo/test_congo.py
```

builds real programs in a temporary folder with the repository's `conec` (build
it first, `python test/run.py --build`): `congo new` then `congo run`, a
package with submodules printing through `stdio`, a lone file, a library from a
registry folder that itself imports `stdio`, the loop refusals between packages
and between modules, and the manifest's checks. The test suite (`test/run.py`) does not run Congo.

## Not built yet

- Generating the include file from the package's source, into `build/`.
- A C package naming the C libraries to link (the old prototype's `opengl` and
  `sdl2` lines), and a package of C sources.
- `congo test`, and anything reading `tests/`.
- WebAssembly (`--target`), which the prototype's `web` mode did.
- A static library file (`.lib`/`.a`) for a library package; it builds an object.
- An internet registry, downloads, a lockfile and version selection.
- Incremental builds: Congo rebuilds everything, every time.
