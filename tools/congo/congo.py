#!/usr/bin/env python3
"""Congo, the Cone build tool, version 1.

    congo new <name> [--lib]          make a package folder
    congo build [--release]           build the package the current folder is in
    congo run [--release] [file.cone] [-- args...]
                                      build it (or one lone file) and run it
    congo clean [file.cone]           delete what a build wrote

CONGO DISCOVERS, THE COMPILER IS TOLD [Jon 23 Sep 2026]. Congo walks each
package's folders and reads each source file's header -- its leading comments,
its 'mod' line and its 'import' lines, and nothing after them; a module's
imports are those of its designated file, the one with the 'mod' line. It resolves every
import that names another package through the package-folder registries, orders
the packages so that each is built after what it imports (refusing an import
loop between packages, or between the modules of one), writes each package's BUILD DESCRIPTION into
build/<mode>/, compiles each package on its own with conec, and links the
objects with conestd, and with the C libraries the packages' [link] tables name,
into build/<mode>/<name>.exe. The folder rules live here, in one place: conec
never searches for a file of a Congo build.

A package is a folder holding congo.toml and src/<name>.cone, the root module's
designated file. A package that others import is compiled as a library, and
that compile GENERATES its INCLUDE FILE, build/<mode>/<name>.cone beside its
object: what an importer is compiled against in place of the package's source,
written by conec from the package's own text. Every dependent is compiled
against that generated file, core's and a C package's included, and never
against a file written by hand. An include file is a module file like any
other and may import [Jon 25 Sep]; each description's package lines, the
compile's whole dependency closure, are where those imports are found.

Python 3.11 or later, standard library only. README.md beside this file is the
user's guide.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

IS_WINDOWS = os.name == "nt"
CONGO_DIR = Path(__file__).resolve().parent
# Congo lives at tools/congo/ in the Cone repository, beside packages/ and the
# compiler's build tree
REPO = CONGO_DIR.parent.parent
REPO_PACKAGES = REPO / "packages"
REPO_CONEC = REPO / "build" / "x64-release" / ("conec.exe" if IS_WINDOWS else "conec")

MANIFEST = "congo.toml"
OBJ_EXT = ".obj" if IS_WINDOWS else ".o"
EXE_EXT = ".exe" if IS_WINDOWS else ""
PRELUDE = "core"

NAME_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
VERSION_RE = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\Z")
OUTPUTS = ("executable", "library")


class CongoError(Exception):
    """A failure Congo reports as one message and exit status 1."""


def say(verb: str, rest: str) -> None:
    """One progress line, Cargo's shape: a right-aligned verb, then what."""
    print(f"{verb:>12} {rest}", flush=True)


def shown(path: Path) -> str:
    """A path as a message shows it: relative to here where it is below here."""
    try:
        return str(path.relative_to(Path.cwd()))
    except ValueError:
        return str(path)


# ---------------------------------------------------------------------------
# The manifest
# ---------------------------------------------------------------------------

@dataclass
class Package:
    """One package of a build: from its manifest, or a lone file standing as one."""
    name: str
    version: str | None
    output: str
    root: Path            # the package folder; a lone file's own folder
    src: Path             # the root module's designated file
    lone: bool = False
    libraries: list[str] = field(default_factory=list)    # [link] libraries
    link_paths: list[Path] = field(default_factory=list)  # [link] paths, absolute

    @property
    def hand_include(self) -> Path:
        """Where an include file written by hand used to sit, <name>.cone at the
        package root. Congo no longer reads one, and says so where it finds one."""
        return self.root / f"{self.name}.cone"

    def label(self) -> str:
        return f"{self.name} v{self.version}" if self.version else self.name


# A C library as the linker is told it, without its prefix or suffix: SDL2 is
# SDL2.lib on Windows and -lSDL2 (libSDL2.a or .so) elsewhere
LIBRARY_RE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.+-]*\Z")
LIBRARY_SUFFIXES = (".lib", ".a", ".so", ".dylib", ".dll")


def read_link(path: Path, table: object) -> tuple[list[str], list[Path]]:
    """[link]: the C libraries a program using this package must be linked with,
    and folders to search for them, relative to the package folder."""
    if not isinstance(table, dict):
        raise CongoError(f"{path}: [link] must be a table, with libraries and paths")
    for key in table:
        if key not in ("libraries", "paths"):
            raise CongoError(f"{path}: '{key}' is not a [link] key; the keys are"
                             " libraries and paths")
    libraries, paths = table.get("libraries", []), table.get("paths", [])
    for key, value in (("libraries", libraries), ("paths", paths)):
        if not isinstance(value, list) or not all(isinstance(v, str) for v in value):
            raise CongoError(f"{path}: [link] {key} must be a list of strings")
    for lib in libraries:
        if not LIBRARY_RE.match(lib) or lib.lower().endswith(LIBRARY_SUFFIXES):
            raise CongoError(f"{path}: [link] library \"{lib}\" must be the library's bare"
                             " name, such as \"SDL2\": no folder, prefix or suffix, which"
                             " Congo adds for the linker (and paths says where to look)")
    folders = []
    for entry in paths:
        folder = Path(os.path.expandvars(os.path.expanduser(entry)))
        folders.append((folder if folder.is_absolute() else path.parent / folder).resolve())
    return libraries, folders


def read_manifest(path: Path) -> Package:
    """congo.toml: a [package] table with name, version and output, and, for a
    package that needs C libraries linked, a [link] table.

    There is no dependencies section: the imports in the source are the
    dependency list, and the registries say where each one is [Jon 23 Sep]."""
    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, tomllib.TOMLDecodeError) as exc:
        raise CongoError(f"{path}: cannot read the manifest: {exc}") from None
    for key in data:
        if key not in ("package", "link"):
            extra = (" There is no dependencies section: a package's imports are its"
                     " dependencies." if key == "dependencies" else "")
            raise CongoError(f"{path}: [{key}] is not part of a Congo manifest, which"
                             f" holds [package] and [link].{extra}")
    table = data.get("package")
    if not isinstance(table, dict):
        raise CongoError(f"{path}: the manifest needs a [package] table with name,"
                         " version and output")
    for key in table:
        if key not in ("name", "version", "output"):
            raise CongoError(f"{path}: '{key}' is not a [package] key; the keys are"
                             " name, version and output")
    name, version, output = table.get("name"), table.get("version"), table.get("output")
    if not isinstance(name, str) or not NAME_RE.match(name):
        raise CongoError(f"{path}: [package] name must be a Cone name, such as \"hello\"")
    if not isinstance(version, str) or not VERSION_RE.match(version):
        raise CongoError(f"{path}: [package] version must be MAJOR.MINOR.PATCH, such as"
                         " \"0.1.0\"")
    if output not in OUTPUTS:
        raise CongoError(f"{path}: [package] output must be \"executable\" or \"library\"")
    libraries, link_paths = read_link(path, data["link"]) if "link" in data else ([], [])
    root = path.parent.resolve()
    return Package(name, version, output, root, root / "src" / f"{name}.cone",
                   libraries=libraries, link_paths=link_paths)


def find_manifest(start: Path) -> Path | None:
    """The nearest congo.toml in this folder or one above it."""
    for folder in (start, *start.parents):
        candidate = folder / MANIFEST
        if candidate.is_file():
            return candidate
    return None


# ---------------------------------------------------------------------------
# The header scan
# ---------------------------------------------------------------------------

@dataclass
class Import:
    name: str | None      # None for a quoted path
    written: str          # as written, for messages
    file: Path
    line: int

    def where(self) -> str:
        return f"{shown(self.file)}:{self.line}"


@dataclass
class Header:
    mod: str | None       # the name on the 'mod' line, or None where there is none
    imports: list[Import]
    extends: Import | None = None   # 'mod a extends b': b, where the line says so
    c: bool = False       # 'mod @c ...': a C-named module


class HeaderTokens:
    """Just enough of Cone's lexer for a file's header: names, strings and single
    punctuation, with white space and comments (line, and nested block) passed
    over. A malformed token ends the header, and the compiler reports it."""

    def __init__(self, text: str):
        self.text = text.lstrip("\ufeff")
        self.pos = 0
        self.line = 1
        self.ahead: list[tuple[str, str, int]] = []

    def _skip(self) -> None:
        text, n = self.text, len(self.text)
        while self.pos < n:
            ch = text[self.pos]
            if ch == "\n":
                self.line += 1
                self.pos += 1
            elif ch in " \t\r\f\v":
                self.pos += 1
            elif text.startswith("//", self.pos):
                end = text.find("\n", self.pos)
                self.pos = n if end < 0 else end
            elif text.startswith("/*", self.pos):
                nest, self.pos = 1, self.pos + 2
                while self.pos < n and nest:
                    if text.startswith("*/", self.pos):
                        nest, self.pos = nest - 1, self.pos + 2
                    elif text.startswith("/*", self.pos):
                        nest, self.pos = nest + 1, self.pos + 2
                    else:
                        if text[self.pos] == "\n":
                            self.line += 1
                        self.pos += 1
            else:
                return

    def _read(self) -> tuple[str, str, int]:
        self._skip()
        text, start, line = self.text, self.pos, self.line
        if start >= len(text):
            return ("eof", "", line)
        ch = text[start]
        if ch.isalpha() or ch == "_":
            end = start + 1
            while end < len(text) and (text[end].isalnum() or text[end] == "_"):
                end += 1
            self.pos = end
            return ("name", text[start:end], line)
        if ch == '"':
            end = start + 1
            while end < len(text) and text[end] not in '"\n':
                end += 2 if text[end] == "\\" else 1
            self.pos = min(end + 1, len(text))
            return ("string", text[start + 1:end], line)
        self.pos = start + 1
        return ("punct", ch, line)

    def peek(self, k: int = 0) -> tuple[str, str, int]:
        while len(self.ahead) <= k:
            self.ahead.append(self._read())
        return self.ahead[k]

    def next(self) -> tuple[str, str, int]:
        tok = self.peek()
        self.ahead.pop(0)
        return tok

    def is_name(self, word: str, k: int = 0) -> bool:
        kind, value, _ = self.peek(k)
        return kind == "name" and value == word

    def skip_statement(self) -> None:
        """Pass over the rest of a statement: up to its ';', with any brackets in
        it (a 'use { ... }' block) balanced."""
        depth = 0
        while True:
            kind, value, _ = self.next()
            if kind == "eof":
                return
            if kind == "punct":
                if value in "({[":
                    depth += 1
                elif value in ")}]":
                    depth -= 1
                elif value == ";" and depth <= 0:
                    return


def scan_header(path: Path) -> Header:
    """A source file's header: comments, then its 'mod' line, then its 'import'
    lines, and then Congo stops reading.

    The 'mod' line is 'mod name', optionally 'pub' before and '@c' or '@c(...)'
    after the keyword, and a generic module's '[T, ...]' after the name, then
    whatever clauses follow: 'extends' is read, since it
    is a dependency like an import, and 'is' and 'use' are passed over. 'mod
    trait' declares a module trait and is not a 'mod' line. An
    'import' is 'import name' or 'import "path"', optionally with 'pub', and
    whatever 'use' clause follows."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise CongoError(f"cannot read {path}: {exc}") from None
    toks = HeaderTokens(text)
    mod = None
    extends = None
    c_named = False
    k = 1 if toks.is_name("pub") else 0
    if toks.is_name("mod", k) and not toks.is_name("trait", k + 1):
        for _ in range(k + 1):
            toks.next()
        if toks.peek()[:2] == ("punct", "@"):
            toks.next()
            c_named = toks.next()[:2] == ("name", "c")
            if toks.peek()[:2] == ("punct", "("):
                while toks.peek()[0] != "eof" and toks.next()[:2] != ("punct", ")"):
                    pass
        kind, value, _ = toks.peek()
        if kind == "name":
            mod = value
            toks.next()
            # A generic module's type parameters, 'mod stack[T]', come before
            # its clauses
            if toks.peek()[:2] == ("punct", "["):
                while toks.peek()[0] != "eof" and toks.next()[:2] != ("punct", "]"):
                    pass
            if toks.is_name("extends"):
                toks.next()
                kind, value, line = toks.peek()
                if kind == "name":
                    extends = Import(value, value, path, line)
        toks.skip_statement()
    imports: list[Import] = []
    while True:
        k = 1 if toks.is_name("pub") else 0
        if not toks.is_name("import", k):
            break
        for _ in range(k + 1):
            toks.next()
        kind, value, line = toks.peek()
        if kind == "name":
            imports.append(Import(value, value, path, line))
        elif kind == "string":
            imports.append(Import(None, f'"{value}"', path, line))
        toks.skip_statement()
    return Header(mod, imports, extends, c_named)


# ---------------------------------------------------------------------------
# The module tree of one package
# ---------------------------------------------------------------------------

@dataclass
class Module:
    name: str
    files: list[Path] = field(default_factory=list)
    children: list["Module"] = field(default_factory=list)
    imports: list[Import] = field(default_factory=list)
    extends: Import | None = None     # from the 'mod' line of its first file

    def add_imports(self, header: Header) -> None:
        if header.extends is not None and self.extends is None:
            self.extends = header.extends
        for imp in header.imports:
            if not any(imp.written == seen.written for seen in self.imports):
                self.imports.append(imp)

    def walk(self, parent: "Module | None" = None):
        yield self, parent
        for child in self.children:
            yield from child.walk(self)


def cone_files(folder: Path) -> tuple[list[Path], list[Path]]:
    """A folder's .cone files and its subfolders, each by name."""
    entries = sorted(folder.iterdir(), key=lambda p: p.name)
    files = [p for p in entries if p.is_file() and p.suffix == ".cone"]
    folders = [p for p in entries if p.is_dir()]
    return files, folders


def scan_folder_module(name: str, folder: Path, designated: Path) -> Module:
    """A module folder, by the rules the compiler's own folder sweep follows
    (compiler/c/doc/nodes/module.md, "The folder tree"): the designated file,
    then the folder's other files by name, then each organisational subfolder's
    files at any depth. A file that opens with its own 'mod' line is a one-file
    submodule, and a subfolder holding its designated file is a submodule."""
    module = Module(name, [designated])
    module.add_imports(scan_header(designated))
    files, folders = cone_files(folder)
    for file in files:
        if file == designated:
            continue
        header = scan_header(file)
        if header.mod is not None:
            child = Module(file.stem, [file])
            child.add_imports(header)
            module.children.append(child)
        else:
            # No 'mod' line, so no imports: a module's imports are all in its
            # designated file [Jon 23 Sep], and the compiler refuses any here
            module.files.append(file)
    for sub in folders:
        if sub.name == "build":
            continue
        inner = sub / f"{sub.name}.cone"
        if inner.is_file():
            module.children.append(scan_folder_module(sub.name, sub, inner))
        else:
            scan_organisational(module, sub)
    module.children.sort(key=lambda m: m.name)
    return module


def scan_organisational(module: Module, folder: Path) -> None:
    """An organisational folder's files join the module around it, at any depth.
    A module cannot sit there, since a module sits directly in its parent's folder."""
    files, folders = cone_files(folder)
    for file in files:
        header = scan_header(file)
        if header.mod is not None:
            raise CongoError(f"{file}: a file opening with 'mod' is a module, and a module"
                             f" must sit directly in its parent module's folder, not in"
                             f" the organisational folder {folder}")
        # Like any file of the module but its designated one, it has no imports
        module.files.append(file)
    for sub in folders:
        if (sub / f"{sub.name}.cone").is_file():
            raise CongoError(f"{sub / (sub.name + '.cone')}: a module must sit directly in"
                             f" its parent module's folder, not in the organisational"
                             f" folder {folder}")
        scan_organisational(module, sub)


def scan_package(pkg: Package) -> Module:
    """The package's module tree. A package's root module is src/, whose
    designated file is src/<name>.cone -- the package's name standing in for the
    folder's [Jon 23 Sep], which the compiler's own sweep would not see, since
    'src' is not 'name'. Congo lists the files, so the compiler never sweeps.
    A lone file is a module of its own and nothing beside it joins."""
    if not pkg.src.is_file():
        raise CongoError(f"package {pkg.name} has no {shown(pkg.src)}: a package's root"
                         f" module is src/<name>.cone, named as its manifest names it")
    if pkg.lone:
        module = Module(pkg.name, [pkg.src])
        module.add_imports(scan_header(pkg.src))
        return module
    return scan_folder_module(pkg.name, pkg.src.parent, pkg.src)


# ---------------------------------------------------------------------------
# The registries, and the order packages are built in
# ---------------------------------------------------------------------------

def congo_home() -> Path:
    home = os.environ.get("CONGO_HOME")
    return Path(home) if home else Path.home() / ".congo"


def config_path() -> Path:
    return congo_home() / "config.toml"


def registry_folders() -> list[Path]:
    """The package folders searched, in order: the Cone repository's packages/
    first, then each folder the machine config lists.

    The machine config is <Congo home>/config.toml, the home being CONGO_HOME or
    ~/.congo:

        [registry]
        folders = ["D:/cone/mypackages", "~/cone/vendor"]

    A relative folder is relative to the config file's own folder."""
    folders = [REPO_PACKAGES] if REPO_PACKAGES.is_dir() else []
    config = config_path()
    if config.is_file():
        try:
            data = tomllib.loads(config.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, tomllib.TOMLDecodeError) as exc:
            raise CongoError(f"{config}: cannot read the machine config: {exc}") from None
        listed = data.get("registry", {}).get("folders", [])
        if not isinstance(listed, list) or not all(isinstance(f, str) for f in listed):
            raise CongoError(f"{config}: [registry] folders must be a list of folder paths")
        for entry in listed:
            folder = Path(os.path.expandvars(os.path.expanduser(entry)))
            if not folder.is_absolute():
                folder = config.parent / folder
            folders.append(folder.resolve())
    return folders


class Registry:
    """Package name -> its folder, answered from the registry folders in order.
    A package folder is a folder whose subfolders are packages, each found by its
    congo.toml and named by it, not by the subfolder."""

    def __init__(self, folders: list[Path]):
        self.folders = folders
        self.packages: dict[str, Package] = {}
        for folder in folders:
            if not folder.is_dir():
                print(f"warning: registry folder {folder} does not exist", file=sys.stderr)
                continue
            for sub in sorted(folder.iterdir(), key=lambda p: p.name):
                manifest = sub / MANIFEST
                if not manifest.is_file():
                    continue
                try:
                    pkg = read_manifest(manifest)
                except CongoError as exc:
                    print(f"warning: skipped: {exc}", file=sys.stderr)
                    continue
                self.packages.setdefault(pkg.name, pkg)

    def find(self, name: str) -> Package | None:
        return self.packages.get(name)

    def searched(self) -> str:
        return ", ".join(str(f) for f in self.folders) or "(no registry folders)"


@dataclass
class Unit:
    """One package of the build, scanned, with what its modules import resolved."""
    pkg: Package
    tree: Module
    deps: dict[str, Package]                          # other packages, first-seen order
    lines: dict[int, dict[str, Package]]              # id(module) -> import name -> package


def include_for(pkg: Package, out: Path) -> Path:
    """What an importer of pkg is compiled against: the include file pkg's own
    compile generated, <name>.cone in the build folder, beside its object. Every
    package of a build is compiled into that one folder, each before what imports
    it, so the file is there, and current, when an importer's compile reads it.

    That holds for core, the prelude, whose include file the description names
    in a package line that conec loads the prelude from; and for a C PACKAGE,
    whose source is a C-named module ('mod @c ...') declaring what a C library
    defines: its generated include file is that source with a banner, and one
    rule for every package is simpler than an exception that saves nothing."""
    return out / f"{pkg.name}.cone"


def resolve_imports(pkg: Package, tree: Module, registry: Registry) -> Unit:
    """Which of each module's imports name another package. A submodule's import
    of a sister is answered inside the package, and one of a name of its parent
    too: those get no line. An import of 'core' names the prelude, which the
    compiler loads from the same include file its package line names."""
    deps: dict[str, Package] = {}
    lines: dict[int, dict[str, Package]] = {}
    for module, parent in tree.walk():
        mine = lines.setdefault(id(module), {})
        sisters = {m.name for m in parent.children} if parent else set()
        for imp in module.imports:
            if imp.name is None:
                raise CongoError(f"{imp.where()}: import {imp.written}: a Congo build"
                                 f" imports a package by its name, not by a path")
            if imp.name in sisters:
                continue
            if imp.name == PRELUDE:
                # The prelude is loaded already; an explicit import of it names
                # the very file it was loaded from, so the two are one module
                core = registry.find(PRELUDE)
                if core is not None and pkg.name != PRELUDE:
                    mine[imp.name] = core
                continue
            found = registry.find(imp.name)
            if found is None:
                if parent is not None:
                    continue      # a name of the parent, which the compiler resolves
                raise CongoError(f"{imp.where()}: import {imp.name}: no package named"
                                 f" '{imp.name}' in the registries searched:"
                                 f" {registry.searched()}")
            deps.setdefault(found.name, found)
            mine[imp.name] = found
    check_module_loops(pkg, tree, registry)
    return Unit(pkg, tree, deps, lines)


def check_module_loops(pkg: Package, tree: Module, registry: Registry) -> None:
    """Imports are a DAG between the modules of a package too [Jon 23 Sep]. A
    module depends on each sister it imports or extends, on its parent where it
    imports a name of the parent, and on each of its own submodules: so a child
    that imports a name of its parent closes a loop of two. A loop is refused,
    naming it, as the compiler would refuse it (ErrorImportLoop); the compiler's
    own check is the backstop for a direct conec run."""
    parents: dict[int, Module | None] = {id(m): p for m, p in tree.walk()}

    def qualified(module: Module) -> str:
        parent = parents[id(module)]
        return f"{qualified(parent)}.{module.name}" if parent else module.name

    def edges(module: Module):
        """(kind, module depended on, the Import that wrote it or None)."""
        parent = parents[id(module)]
        sisters = {m.name: m for m in parent.children} if parent else {}
        if module.extends is not None and module.extends.name in sisters:
            yield "extends", sisters[module.extends.name], module.extends
        for imp in module.imports:
            if imp.name is None or imp.name == PRELUDE:
                continue
            if imp.name in sisters:
                yield "imports", sisters[imp.name], imp
            elif registry.find(imp.name) is None and parent is not None:
                yield "imports-name", parent, imp
        for child in module.children:
            yield "contains", child, None

    state: dict[int, str] = {}
    path: list[tuple[Module, str, Module, Import | None]] = []

    def step_text(frm: Module, kind: str, to: Module, imp: Import | None) -> str:
        if kind == "contains":
            return f"{qualified(frm)} contains {qualified(to)}"
        verb = {"imports": "imports", "extends": "extends"}.get(kind, f"imports {imp.name} of")
        return f"{qualified(frm)} {verb} {qualified(to)} at {imp.where()}"

    def visit(module: Module) -> None:
        state[id(module)] = "visiting"
        for kind, to, imp in edges(module):
            path.append((module, kind, to, imp))
            if state.get(id(to)) == "visiting":
                at = next(i for i, (m, *_) in enumerate(path) if m is to)
                loop = path[at:]
                chain = " -> ".join([qualified(m) for m, *_ in loop] + [qualified(to)])
                steps = "; ".join(step_text(*s) for s in loop)
                hint = (" A module may not depend on a module that contains it: move what"
                        " they share into a sister both import."
                        if any(k == "contains" for _, k, _, _ in loop) else "")
                raise CongoError(f"import loop between modules of package {pkg.name}:"
                                 f" {chain}. Imports between modules must not loop"
                                 f" ({steps}).{hint}")
            if state.get(id(to)) is None:
                visit(to)
            path.pop()
        state[id(module)] = "done"

    visit(tree)


def first_import(unit: Unit, name: str) -> Import:
    for module, _ in unit.tree.walk():
        for imp in module.imports:
            if imp.name == name:
                return imp
    raise AssertionError(name)


def build_order(top: Package, registry: Registry) -> list[Unit]:
    """Every package the build needs, each after what it imports. Imports are a
    DAG between packages [Jon 23 Sep]: a loop is refused, naming it. The prelude,
    core, comes first of all, since every package uses it -- after the packages
    core itself imports (libc), which are C packages, given no prelude by the
    compiler, and so built before it. A package core imports, built on its own,
    needs no core at all."""
    order: list[Unit] = []
    state: dict[str, str] = {}
    units: dict[str, Unit] = {}
    stack: list[Unit] = []

    def visit(pkg: Package) -> None:
        state[pkg.name] = "visiting"
        unit = resolve_imports(pkg, scan_package(pkg), registry)
        units[pkg.name] = unit
        stack.append(unit)
        for name, dep in unit.deps.items():
            if state.get(name) == "visiting":
                at = next(i for i, u in enumerate(stack) if u.pkg.name == name)
                loop = stack[at:]
                chain = " -> ".join([u.pkg.name for u in loop] + [name])
                steps = "; ".join(
                    f"{u.pkg.name} imports {nxt} at {first_import(u, nxt).where()}"
                    for u, nxt in zip(loop, [u.pkg.name for u in loop[1:]] + [name]))
                raise CongoError(f"import loop between packages: {chain}. Imports between"
                                 f" packages must not loop ({steps}).")
            if state.get(name) is None:
                visit(dep)
        stack.pop()
        state[pkg.name] = "done"
        order.append(unit)

    if top.name != PRELUDE:
        core = registry.find(PRELUDE)
        if core is None:
            raise CongoError(f"no package named '{PRELUDE}', the prelude, in the"
                             f" registries searched: {registry.searched()}")
        visit(core)
        if top.name in state:
            # One of core's own imports: built with what it imports, and no core
            order.clear()
            state.clear()
    visit(top)
    return order


def prelude_needs(units: dict[str, Unit]) -> set[str]:
    """The packages core imports, directly or not: the C packages the prelude
    rests on (libc). Each is built before core and gets no prelude, so its
    description has no package line for core, whose include file does not exist
    yet when it is compiled."""
    needs: set[str] = set()

    def visit(name: str) -> None:
        for dep in units[name].deps:
            if dep not in needs:
                needs.add(dep)
                visit(dep)

    if PRELUDE in units:
        visit(PRELUDE)
    return needs


# ---------------------------------------------------------------------------
# The build description
# ---------------------------------------------------------------------------

def cone_path(path: Path) -> str:
    """A path as a build description writes it: absolute, with '/', since the
    description is read as Cone source and a backslash begins an escape."""
    return str(path.resolve()).replace("\\", "/").replace('"', '\\"')


def closure(unit: Unit, units: dict[str, Unit], registry_core: Package | None) -> list[Package]:
    """Every package in the unit's dependency closure, direct or indirect, in
    build order (each after what it imports), the prelude first, after what the
    prelude itself imports. These are the description's PACKAGE LINES: an include
    file is a module file like any other and may import [Jon 25 Sep], and what
    its imports reach is found there -- so core's include file's import of libc
    is answered by libc's line, in every description that has core's. The
    prelude's line is also where conec loads the prelude from. A package core
    imports gets neither: it is built before core, and has no prelude."""
    seen: dict[str, Package] = {}

    def visit(pkg: Package) -> None:
        for name, dep in units[pkg.name].deps.items():
            if name not in seen:
                visit(dep)
                seen[name] = dep

    if (registry_core is not None and unit.pkg.name != PRELUDE
            and PRELUDE in units and unit.pkg.name not in prelude_needs(units)):
        visit(registry_core)
        seen[PRELUDE] = registry_core
    visit(unit.pkg)
    return list(seen.values())


def description(unit: Unit, mode: str, output: str, out: Path,
                packages: list[Package] = ()) -> str:
    pkg = unit.pkg
    lines = [
        f"// The build description of {pkg.label()}, written by Congo for conec;",
        "// rewritten on every build, so edit the source, not this.",
        "",
        f"build: {mode}",
        f"output: {output}",
        "",
    ]
    # The package lines: where each package of the closure is found, for an
    # include file's own imports and, for core, for the prelude. Each is the
    # include file that package's compile generated into 'out'. The package's
    # own modules import only what their own lines, below, give them
    for dep in packages:
        lines.append(f'import {dep.name}: "{cone_path(include_for(dep, out))}"')
    if packages:
        lines.append("")

    def emit(module: Module, depth: int) -> None:
        pad = "    " * depth
        lines.append(f"{pad}{module.name}: {{")
        for file in module.files:
            lines.append(f'{pad}    "{cone_path(file)}"')
        for name, dep in unit.lines.get(id(module), {}).items():
            lines.append(f'{pad}    import {name}: "{cone_path(include_for(dep, out))}"')
        for child in module.children:
            emit(child, depth + 1)
        lines.append(f"{pad}}}")

    emit(unit.tree, 0)
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# The compiler, conestd and the linker
# ---------------------------------------------------------------------------

def find_conec() -> Path:
    """CONEC names the compiler; else the Cone repository's own build,
    build/x64-release/conec.exe; else 'conec' on PATH."""
    named = os.environ.get("CONEC")
    if named:
        path = Path(named)
        if not path.is_file():
            raise CongoError(f"CONEC names {path}, which does not exist")
        return path
    if REPO_CONEC.is_file():
        warn_if_stale(REPO_CONEC)
        return REPO_CONEC
    on_path = shutil.which("conec")
    if on_path:
        return Path(on_path)
    raise CongoError(f"no conec: set CONEC to the compiler, build it at {REPO_CONEC},"
                     " or put conec on PATH")


def warn_if_stale(conec: Path) -> None:
    """A stale conec fails good sources in ways that look like a language
    regression. The repository's build is checked against its sources, as the
    test runner checks it."""
    stamp = conec.stat().st_mtime
    sources = REPO / "compiler" / "c"
    for path in sources.rglob("*"):
        if path.suffix in (".c", ".h") and path.stat().st_mtime > stamp:
            print(f"warning: {conec} is older than {path.relative_to(REPO)}; rebuild it"
                  f" (python test/run.py --build) before trusting what it reports",
                  file=sys.stderr)
            return


def find_conestd(conec: Path) -> Path:
    """CONESTD names the runtime library; else it is the one built beside conec."""
    named = os.environ.get("CONESTD")
    path = Path(named) if named else conec.parent / ("conestd.lib" if IS_WINDOWS
                                                     else "libconestd.a")
    if not path.is_file():
        raise CongoError(f"no conestd library at {path}; build it with conec, or set"
                         f" CONESTD")
    return path


def find_vcvars() -> str | None:
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) \
        / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.exists():
        result = subprocess.run(
            [str(vswhere), "-latest", "-products", "*",
             "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-property", "installationPath"],
            capture_output=True, text=True)
        root = result.stdout.strip().splitlines()
        if root:
            candidate = Path(root[0]) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
            if candidate.exists():
                return str(candidate)
    for edition in ("Community", "Professional", "Enterprise", "BuildTools"):
        candidate = Path(r"C:\Program Files\Microsoft Visual Studio\2022") / edition \
            / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
        if candidate.exists():
            return str(candidate)
    return None


def is_msvc_linker(tool: str) -> bool:
    """Git for Windows ships a coreutils link.exe, which is not a linker at all,
    so an inherited link.exe is trusted only when it says it is Microsoft's."""
    try:
        banner = subprocess.run([tool, "/?"], capture_output=True, text=True,
                                errors="replace", stdin=subprocess.DEVNULL, timeout=20)
    except (OSError, subprocess.SubprocessError):
        return False
    return "Microsoft" in banner.stdout + banner.stderr


class Linker:
    """The system linker, with the environment it needs. On Windows that is
    Microsoft's link.exe in the environment vcvars64.bat sets up, which Congo
    produces itself, so no Developer Command Prompt is needed."""

    def __init__(self, conestd: Path):
        self.conestd = conestd
        self.env = dict(os.environ)
        if IS_WINDOWS:
            inherited = shutil.which("link.exe")
            if not (inherited and is_msvc_linker(inherited)):
                self.env = self.vs_environment()
            tool = shutil.which("link.exe", path=self.env.get("PATH", ""))
            if tool is None or not is_msvc_linker(tool):
                raise CongoError("no Microsoft link.exe: install Visual Studio's C++ tools"
                                 " (Congo runs vcvars64.bat to find it)")
            self.tool = tool
        else:
            tool = shutil.which("cc") or shutil.which("gcc")
            if tool is None:
                raise CongoError("no cc or gcc on PATH to link with")
            self.tool = tool

    @staticmethod
    def vs_environment() -> dict[str, str]:
        vcvars = find_vcvars()
        if vcvars is None:
            raise CongoError("no Visual Studio C++ tools (vcvars64.bat) to link with")
        # A single string, not a list: list2cmdline would escape the quotes
        # around the batch path, and cmd would not recognise it
        result = subprocess.run(f'cmd /c ""{vcvars}" >nul && set"',
                                capture_output=True, text=True, errors="replace")
        if result.returncode != 0:
            raise CongoError(f"{vcvars} failed:\n{result.stdout}{result.stderr}")
        env = dict(os.environ)
        for line in result.stdout.splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                env[key] = value
        return env

    def command(self, objs: list[Path], exe: Path, libraries: list[str] = (),
                paths: list[Path] = ()) -> list[str]:
        """The link line: the objects, conestd, then the C libraries the packages'
        [link] tables name, each searched for first in their [link] paths."""
        if IS_WINDOWS:
            return [self.tool, "/NOLOGO", *map(str, objs), str(self.conestd),
                    *(f"/LIBPATH:{p}" for p in paths), *(f"{lib}.lib" for lib in libraries),
                    f"/OUT:{exe}", "/SUBSYSTEM:CONSOLE", "msvcrt.lib",
                    "legacy_stdio_definitions.lib"]
        return [self.tool, *map(str, objs), str(self.conestd),
                *(f"-L{p}" for p in paths), *(f"-l{lib}" for lib in libraries),
                "-o", str(exe), "-lm"]


# ---------------------------------------------------------------------------
# Building
# ---------------------------------------------------------------------------

def compile_unit(conec: Path, unit: Unit, out: Path, mode: str, top: bool,
                 env: dict[str, str], packages: list[Package] = ()) -> Path:
    """Write the package's build description into build/<mode>/ and compile the
    package on its own. Every package but the one being built is a library;
    the one being built is what its manifest says. 'packages' is the unit's
    dependency closure, which the description lists for its include files.
    A library's compile writes its include file beside its object, which is
    what every package compiled after it that imports it is compiled against."""
    output = unit.pkg.output if top else "library"
    desc = out / f"{unit.pkg.name}.conebuild"
    desc.write_text(description(unit, mode, output, out, packages), encoding="utf-8")
    # A library's include file is written afresh, so one left by an earlier
    # build never stands in for it
    include_for(unit.pkg, out).unlink(missing_ok=True)
    say("Compiling", f"{unit.pkg.label()} ({unit.pkg.root})")
    result = subprocess.run([str(conec), "-o", str(out), str(desc)], env=env,
                            capture_output=True, text=True, errors="replace")
    chatter = [line for line in (result.stdout + result.stderr).splitlines()
               if line.strip() and not line.startswith("Compile finished")]
    if chatter:
        print("\n".join(chatter), flush=True)
    if result.returncode != 0:
        raise CongoError(f"could not compile {unit.pkg.name} (build description:"
                         f" {desc})")
    obj = out / f"{unit.pkg.name}{OBJ_EXT}"
    if not obj.is_file():
        raise CongoError(f"conec wrote no object at {obj}")
    include = include_for(unit.pkg, out)
    if output == "library" and not include.is_file():
        raise CongoError(f"conec wrote no include file for {unit.pkg.name} at {include}")
    return obj


def build_folder(pkg: Package, mode: str) -> Path:
    return lone_build_root(pkg.src) / mode if pkg.lone else pkg.root / "build" / mode


def lone_build_root(file: Path) -> Path:
    """A lone file's build goes under the Congo home, not beside the file: the
    folder a lone file sits in is nobody's package, and may well hold a 'build'
    folder of its own that 'congo clean' must not delete."""
    spelled = str(file.resolve())
    digest = hashlib.sha1((spelled.lower() if IS_WINDOWS else spelled).encode()).hexdigest()[:10]
    return congo_home() / "lone" / f"{file.stem}-{digest}"


def build(pkg: Package, mode: str) -> Path:
    """Build pkg and everything it imports; return the executable, or for a
    library its object."""
    registry = Registry(registry_folders())
    order = build_order(pkg, registry)
    conec = find_conec()
    out = build_folder(pkg, mode)
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    # Every package after core loads the prelude from core's generated include
    # file, which its package line names. core's own compile has no such line,
    # and loads the prelude from the package search path, which must give the
    # very file core is compiled from, or the two are two modules named core: so
    # the packages folder conec takes it from is the registry folder Congo found
    # core in
    core = next((u.pkg for u in order if u.pkg.name == PRELUDE), None)
    if core is not None:
        env["CONE_PACKAGES"] = str(core.root.parent)
    # An include file written by hand at a package's root is no longer read:
    # say so, once, so that nobody edits it expecting it to count
    for unit in order:
        if not unit.pkg.lone and unit.pkg.hand_include.is_file():
            print(f"warning: {unit.pkg.hand_include} is not used: a package's importers"
                  f" compile against the include file its own compile generates,"
                  f" build/<mode>/{unit.pkg.name}.cone", file=sys.stderr)
    by_name = {unit.pkg.name: unit for unit in order}
    objs = [compile_unit(conec, unit, out, mode, unit.pkg is pkg, env,
                         closure(unit, by_name, core)) for unit in order]
    if pkg.output == "library":
        say("Finished", f"{mode} library object {shown(objs[-1])}")
        return objs[-1]
    exe = out / f"{pkg.name}{EXE_EXT}"
    linker = Linker(find_conestd(conec))
    # The program's object first, then the packages it imports, last built first;
    # and the C libraries every one of them names, in the same order, each once.
    # A C package's own object is linked like any other: it defines nothing
    # (as core's does not), and so needs no case of its own
    units = [order[-1], *reversed(order[:-1])]
    libraries = list(dict.fromkeys(lib for u in units for lib in u.pkg.libraries))
    paths = list(dict.fromkeys(p for u in units for p in u.pkg.link_paths))
    command = linker.command([objs[-1], *reversed(objs[:-1])], exe, libraries, paths)
    say("Linking", shown(exe))
    result = subprocess.run(command, env=linker.env, capture_output=True, text=True,
                            errors="replace")
    if result.returncode != 0:
        named = "; ".join(f"{u.pkg.name} names {', '.join(u.pkg.libraries)}"
                          for u in units if u.pkg.libraries)
        where = ("the folders the LIB environment variable lists" if IS_WINDOWS
                 else "the linker's own search path")
        hint = (f"\nC libraries linked ({named}): the linker looks for each in the"
                f" folders [link] paths names, then in {where}" if named else "")
        raise CongoError(f"link failed:\n{' '.join(command)}\n{result.stdout}"
                         f"{result.stderr}{hint}")
    say("Finished", f"{mode} {shown(exe)}")
    return exe


def lone_package(file: Path) -> Package:
    """A lone file with no manifest is a program of one module, named by its
    'mod' line. A file with none is named by the file here, and the compiler
    refuses it for the missing line (ErrorNoModDcl)."""
    file = file.resolve()
    if not file.is_file():
        raise CongoError(f"no file {file}")
    header = scan_header(file)
    name = header.mod or file.stem
    if not NAME_RE.match(name):
        raise CongoError(f"{file}: '{name}' is not a Cone name; give the file a 'mod' line")
    return Package(name, None, "executable", file.parent, file, lone=True)


def current_package() -> Package:
    manifest = find_manifest(Path.cwd())
    if manifest is None:
        raise CongoError(f"no {MANIFEST} in this folder or any above it; 'congo new <name>'"
                         f" makes a package, and 'congo run <file.cone>' runs a lone file")
    return read_manifest(manifest)


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

PROGRAM_TEMPLATE = """\
// {name}: a program. 'congo run' builds it and runs it.

mod {name};

import stdio;

fn main() i32 {{
  stdio.print <- "Hello, world!\\n";
  0i32;
}}
"""

LIBRARY_TEMPLATE = """\
// {name}: a library package. A program imports it by name, and is compiled
// against its include file, which compiling {name} generates from this source:
// build/<mode>/{name}.cone, beside its object.

mod {name};

pub fn answer() i64 {{
  42i64;
}}
"""


def cmd_new(args: argparse.Namespace) -> int:
    name = args.name
    if not NAME_RE.match(name):
        raise CongoError(f"'{name}' is not a Cone name: a letter or '_', then letters,"
                         " digits or '_'")
    root = Path(args.path or name).resolve()
    if root.exists():
        raise CongoError(f"{root} already exists")
    output = "library" if args.lib else "executable"
    (root / "src").mkdir(parents=True)
    (root / "tests").mkdir()
    (root / MANIFEST).write_text(
        f'[package]\nname = "{name}"\nversion = "0.1.0"\noutput = "{output}"\n',
        encoding="utf-8")
    template = LIBRARY_TEMPLATE if args.lib else PROGRAM_TEMPLATE
    (root / "src" / f"{name}.cone").write_text(template.format(name=name), encoding="utf-8")
    say("Created", f"{output} package {name} ({shown(root)})")
    return 0


def cmd_build(args: argparse.Namespace) -> int:
    build(current_package(), "release" if args.release else "debug")
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    pkg = lone_package(Path(args.file)) if args.file else current_package()
    if pkg.output != "executable":
        raise CongoError(f"{pkg.name} is a library, which has nothing to run;"
                         f" 'congo build' builds it")
    exe = build(pkg, "release" if args.release else "debug")
    say("Running", shown(exe))
    return subprocess.run([str(exe), *args.args]).returncode


def cmd_clean(args: argparse.Namespace) -> int:
    if args.file:
        target = lone_build_root(Path(args.file))
    else:
        target = current_package().root / "build"
    if target.exists():
        shutil.rmtree(target)
        say("Removed", shown(target))
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(prog="congo", description="The Cone build tool.")
    sub = parser.add_subparsers(dest="command", required=True, metavar="command")
    new = sub.add_parser("new", help="make a package folder")
    new.add_argument("name", help="the package's name")
    new.add_argument("path", nargs="?", help="the folder to make (default: the name)")
    new.add_argument("--lib", action="store_true",
                     help="a library package, whose include file its build generates")
    new.set_defaults(func=cmd_new)
    for name, func, text in (("build", cmd_build, "build this package"),
                             ("run", cmd_run, "build this package or a lone file, and run it")):
        p = sub.add_parser(name, help=text)
        p.add_argument("--release", action="store_true", help="an optimised build")
        if name == "run":
            p.add_argument("file", nargs="?", help="a lone .cone file, run with no manifest")
            p.epilog = "Arguments after '--' are passed to the program."
        p.set_defaults(func=func)
    clean = sub.add_parser("clean", help="delete what a build wrote")
    clean.add_argument("file", nargs="?", help="a lone .cone file whose build to delete")
    clean.set_defaults(func=cmd_clean)
    # What follows '--' is the program's, and argparse never sees it
    program_args: list[str] = []
    if "--" in argv:
        at = argv.index("--")
        argv, program_args = argv[:at], argv[at + 1:]
    args = parser.parse_args(argv)
    args.args = program_args
    return args


def main(argv: list[str] | None = None) -> int:
    if sys.version_info < (3, 11):
        print("congo: needs Python 3.11 or later", file=sys.stderr)
        return 1
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        return args.func(args)
    except CongoError as exc:
        print(f"congo: error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
