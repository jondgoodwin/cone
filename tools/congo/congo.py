#!/usr/bin/env python3
"""Congo, the Cone build tool, version 1.

    congo new <name> [--lib]          make a package folder
    congo build [--release]           build the package the current folder is in
    congo run [--release] [file.cone] [-- args...]
                                      build it (or one lone file) and run it
    congo test [name] [--release] [--bless]
                                      build the package, run each program in
                                      its tests/ and build each in examples/
    congo clean [file.cone]           delete what a build wrote

build, run and test also take -D NAME or -D NAME=123 (TEMPORARY, a provisional
mechanism whose final design is open), passed to every conec compile.

CONGO DISCOVERS, THE COMPILER IS TOLD [Jon 23 Sep 2026]. Congo walks each
package's folders and reads each source file's header -- its leading comments,
its 'mod' line and its 'import' lines, and nothing after them; a module's
imports are those of its designated file, the one with the 'mod' line. It resolves every
import that names another package through the package-folder registries, orders
the packages so that each is built after what it imports (refusing an import
loop between packages, or between the modules of one), writes each package's BUILD DESCRIPTION into
build/<mode>/, compiles each package on its own with conec, and links the
objects with conestd, and with the C libraries the packages' [link] tables name,
into build/<mode>/<name>.exe, beside which it copies the DLLs their [link]
runtime lists name. The folder rules live here, in one place: conec never
searches for a file of a Congo build.

A package marked for the GPU (targets = ["native", "gpu"]) whose source holds
compute entry points ('fn @compute') is also compiled for the GPU [Jon 3 Oct
2026], into build/<mode>/<name>.spv, which is copied beside every program that
imports it; Congo first refuses any package it imports that is not marked.
A package's gpu/ folder, beside src/, is marked by being there: its modules
are submodules of the root, compiled into the package for the CPU and, without
src/, which they may not use, into the same .spv. The GPU compile is the one
exception to the rule above: every function on a GPU
is inlined into its kernel, so the packages a kernel calls into are compiled
from their source, which conec finds on its package search path.

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
import codecs
import difflib
import hashlib
import os
import re
import shutil
import subprocess
import sys
import threading
import time
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
# The fetched-dependencies folder, which tools/deps/fetch.py fills and git
# ignores: one folder per dependency (deps/openssl/), where Congo looks for a
# runtime DLL after [link] paths. CONE_DEPS names another
REPO_DEPS = REPO / "deps"

MANIFEST = "congo.toml"
OBJ_EXT = ".obj" if IS_WINDOWS else ".o"
EXE_EXT = ".exe" if IS_WINDOWS else ""
PRELUDE = "core"
# What Windows ends a program with when a DLL it needs is not found
STATUS_DLL_NOT_FOUND = 0xC0000135


def hard_errors_to_status() -> None:
    """Windows only, once at start-up: the loader's missing-DLL dialog, and
    the other hard-error and file-open boxes, become an exit status.

    A program started without a DLL it needs would otherwise stop at a modal
    dialog on the desktop, waiting for a person. A child process inherits this
    process's error mode, so every program Congo starts (a test, an example,
    'congo run', the compiler) ends with the status instead (see 'dll_note').
    Running as the debuggee does not prevent the dialog; the error mode does.
    OR-ed into the mode already set, as SetErrorMode's documentation advises;
    no launch passes CREATE_DEFAULT_ERROR_MODE, which would undo this."""
    if not IS_WINDOWS:
        return
    import ctypes
    k = ctypes.WinDLL("kernel32")
    k.SetErrorMode.argtypes = [ctypes.c_uint]
    k.SetErrorMode.restype = ctypes.c_uint
    SEM_FAILCRITICALERRORS, SEM_NOOPENFILEERRORBOX = 0x1, 0x8000
    k.SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | k.SetErrorMode(0))


def dll_note(code: int | None, exe: Path | str) -> str:
    """A line saying that the program, by its full path, did not start
    because a DLL it needs was not found, if it ended with
    STATUS_DLL_NOT_FOUND; empty for any other status. With the loader's dialog
    gone, this line is how a person learns of it."""
    if IS_WINDOWS and code is not None and code & 0xFFFFFFFF == STATUS_DLL_NOT_FOUND:
        return (f"\n{os.path.abspath(exe)} could not start: a DLL it needs was not found"
                " (0xC0000135)")
    return ""


# TEMPORARY, a provisional mechanism whose final design is open: each '-D NAME'
# or '-D NAME=123' given to build, run or test, passed to every conec compile of
# the build, the packages it imports included (conec's isDefined, definedInt)
DEFINES: list[str] = []

NAME_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
VERSION_RE = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\Z")
OUTPUTS = ("executable", "library")
# What [package] targets may name: the machine Congo runs on, and the GPU
TARGETS = ("native", "gpu")
# A GPU build's target: SPIR-V for Vulkan 1.3, whose compute entry points are
# the kernels gpuwork loads
GPU_TRIPLE = "spirv1.6-unknown-vulkan1.3"
SPV_EXT = ".spv"
# A package's folder of GPU modules, beside src/: the folder is the marking
GPU_FOLDER = "gpu"

# The words the compiler's lexer never reads as a name: its keywords and the
# words it reserves for features not built yet (compiler/c/parser/lexer.c,
# keywordInit), and the permissions (compiler/c/corelib/corelib.c). None can
# name a package or a module. The language refuses one on a 'mod' line, and a
# build description writes each module's name bare, read by the compiler's
# lexer, so one there makes the description malformed (ErrorBuildDesc) instead
# of being reported against the file. test_congo.py checks these against the
# compiler's own lists
KEYWORDS = frozenset((
    "include import extern pub static macro fn overload const alias typedef struct mod"
    " actor trait extends mixin use but enum return with if elif else case match"
    " while each in by break continue not or and as is into inline where new trynew await selfactor async do"
    " void nil null true false undef").split())
RESERVED = frozenset((
    "baseurl context local selfmethod using wait yield throw catch spawn"
    ).split())
PERMISSIONS = frozenset("uni mut imm ro mut1 opaq".split())


def name_fault(name: str) -> str | None:
    """Why a word cannot name a package or a module, or None where it can."""
    if not NAME_RE.match(name):
        return "is not a Cone name: a letter or '_', then letters, digits or '_'"
    if name in KEYWORDS:
        return "is a Cone keyword, and a keyword cannot name a package or a module"
    if name in RESERVED:
        return ("is reserved for a Cone feature not built yet, and cannot name a package"
                " or a module")
    if name in PERMISSIONS:
        return "is a Cone permission, and a permission cannot name a package or a module"
    return None


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
    runtime: list[str] = field(default_factory=list)      # [link] runtime: DLLs, bare names
    gpu: bool = False     # [package] targets names "gpu": it may be compiled for the GPU

    @property
    def hand_include(self) -> Path:
        """Where an include file written by hand used to sit, <name>.cone at the
        package root. Congo no longer reads one, and says so where it finds one."""
        return self.root / f"{self.name}.cone"

    def label(self) -> str:
        return f"{self.name} v{self.version}" if self.version else self.name


# A C library as the linker is told it, without its prefix or suffix: SDL3 is
# SDL3.lib on Windows and -lSDL3 (libSDL3.a or .so) elsewhere
LIBRARY_RE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.+-]*\Z")
LIBRARY_SUFFIXES = (".lib", ".a", ".so", ".dylib", ".dll")


def read_link(path: Path, table: object) -> tuple[list[str], list[Path], list[str]]:
    """[link]: the C libraries a program using this package must be linked with,
    folders to search for them, relative to the package folder, and the DLLs
    the program needs beside it to run, which Congo copies there."""
    if not isinstance(table, dict):
        raise CongoError(f"{path}: [link] must be a table, with libraries, paths and"
                         " runtime")
    for key in table:
        if key not in ("libraries", "paths", "runtime"):
            raise CongoError(f"{path}: '{key}' is not a [link] key; the keys are"
                             " libraries, paths and runtime")
    libraries, paths = table.get("libraries", []), table.get("paths", [])
    runtime = table.get("runtime", [])
    for key, value in (("libraries", libraries), ("paths", paths), ("runtime", runtime)):
        if not isinstance(value, list) or not all(isinstance(v, str) for v in value):
            raise CongoError(f"{path}: [link] {key} must be a list of strings")
    for lib in libraries:
        if not LIBRARY_RE.match(lib) or lib.lower().endswith(LIBRARY_SUFFIXES):
            raise CongoError(f"{path}: [link] library \"{lib}\" must be the library's bare"
                             " name, such as \"SDL3\": no folder, prefix or suffix, which"
                             " Congo adds for the linker (and paths says where to look)")
    for dll in runtime:
        if not LIBRARY_RE.match(dll) or dll.lower().endswith(LIBRARY_SUFFIXES):
            raise CongoError(f"{path}: [link] runtime \"{dll}\" must be the DLL's bare"
                             " name, such as \"SDL3\" for SDL3.dll: no folder or suffix,"
                             " which Congo adds")
    folders = []
    for entry in paths:
        folder = Path(os.path.expandvars(os.path.expanduser(entry)))
        folders.append((folder if folder.is_absolute() else path.parent / folder).resolve())
    return libraries, folders, runtime


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
        if key not in ("name", "version", "output", "targets"):
            raise CongoError(f"{path}: '{key}' is not a [package] key; the keys are"
                             " name, version, output and targets")
    name, version, output = table.get("name"), table.get("version"), table.get("output")
    if not isinstance(name, str) or not NAME_RE.match(name):
        raise CongoError(f"{path}: [package] name must be a Cone name, such as \"hello\"")
    if name_fault(name):
        raise CongoError(f"{path}: [package] name '{name}' {name_fault(name)}; choose another")
    if not isinstance(version, str) or not VERSION_RE.match(version):
        raise CongoError(f"{path}: [package] version must be MAJOR.MINOR.PATCH, such as"
                         " \"0.1.0\"")
    if output not in OUTPUTS:
        raise CongoError(f"{path}: [package] output must be \"executable\" or \"library\"")
    gpu = read_targets(path, table.get("targets", ["native"]), output)
    libraries, link_paths, runtime = (read_link(path, data["link"]) if "link" in data
                                      else ([], [], []))
    root = path.parent.resolve()
    return Package(name, version, output, root, root / "src" / f"{name}.cone",
                   libraries=libraries, link_paths=link_paths, runtime=runtime, gpu=gpu)


def read_targets(path: Path, targets: object, output: str) -> bool:
    """[package] targets: what the package may be compiled for, "native" (the
    machine Congo runs on) and "gpu"; ["native"] where it is not written.
    Whether it names "gpu": GPU compatibility is declared per package, and a
    GPU build refuses a package that is not marked [Jon 3 Oct 2026]. The
    spelling is a placeholder."""
    if not isinstance(targets, list) or not all(isinstance(t, str) for t in targets):
        raise CongoError(f"{path}: [package] targets must be a list of targets, such as"
                         " [\"native\", \"gpu\"]")
    for target in targets:
        if target not in TARGETS:
            raise CongoError(f"{path}: [package] targets names \"{target}\", which is not a"
                             " target; the targets are \"native\" and \"gpu\"")
    if "native" not in targets:
        raise CongoError(f"{path}: [package] targets must name \"native\": a package built"
                         " for the GPU alone is not built yet")
    if "gpu" in targets and output != "library":
        raise CongoError(f"{path}: [package] targets names \"gpu\", and an executable is"
                         " not built for the GPU: put its kernels in its gpu/ folder, or in a"
                         " library package it imports")
    return "gpu" in targets


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
    whatever 'as' and 'use' clause follow: the name read is the package's,
    whatever name 'as' binds it under."""
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


def holds_kernel(path: Path) -> bool:
    """Whether a source file declares a compute entry point, 'fn @compute':
    read to its end with the header scan's tokens, so that one written in a
    comment or a string does not count."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise CongoError(f"cannot read {path}: {exc}") from None
    toks = HeaderTokens(text)
    before = [("eof", ""), ("eof", "")]
    while True:
        kind, value, _ = toks.next()
        if kind == "eof":
            return False
        if before == [("name", "fn"), ("punct", "@")] and (kind, value) == ("name", "compute"):
            return True
        before = [before[1], (kind, value)]


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
    gpu: bool = False     # in the package's gpu/ folder, at any depth: compiled for the GPU too

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
            if name_fault(file.stem):
                raise CongoError(f"{file}: a one-file module is named by its file:"
                                 f" '{file.stem}' {name_fault(file.stem)}; rename the file"
                                 f" and its 'mod' line")
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
            if name_fault(sub.name):
                raise CongoError(f"{inner}: a module folder names its module:"
                                 f" '{sub.name}' {name_fault(sub.name)}; rename the folder,"
                                 f" its designated file and its 'mod' line")
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
    tree = scan_folder_module(pkg.name, pkg.src.parent, pkg.src)
    if (pkg.root / GPU_FOLDER).is_dir():
        scan_gpu_folder(pkg, tree)
    return tree


def scan_gpu_folder(pkg: Package, tree: Module) -> None:
    """A package's gpu/ folder, beside src/ [Jon 3 Oct 2026]: Cone source that
    runs on the GPU, the folder itself the marking. It holds submodules of the
    package's root module, each laid out as it would be in src/: a file opening
    with its own 'mod' line is a module of one file, named for the file, and a
    subfolder holding its designated file is a module folder, scanned by the
    usual rules. src/ reaches them as it reaches any submodule, by name; they
    are compiled for the CPU as part of the package, and for the GPU, without
    src/, into the package's one .spv. A file with no 'mod' line would join no
    module there, and a folder with no designated file would organise
    nothing, so each is refused."""
    folder = pkg.root / GPU_FOLDER
    if pkg.gpu:
        raise CongoError(f"{folder}: {pkg.name} is marked for the GPU (targets = [\"native\","
                         f" \"gpu\"]), so all of it is compiled for the GPU already; a gpu/"
                         f" folder is for a package that is not: move gpu/'s modules into"
                         f" src/, or take \"gpu\" out of targets")
    files, folders = cone_files(folder)
    found: list[Module] = []
    for file in files:
        header = scan_header(file)
        if header.mod is None:
            raise CongoError(f"{file}: a file in gpu/ opens with its own 'mod' line, as a"
                             f" module of one file named for the file: gpu/ holds the"
                             f" package's GPU modules, and a file with no 'mod' line would"
                             f" join none")
        if name_fault(file.stem):
            raise CongoError(f"{file}: a one-file module is named by its file:"
                             f" '{file.stem}' {name_fault(file.stem)}; rename the file"
                             f" and its 'mod' line")
        module = Module(file.stem, [file])
        module.add_imports(header)
        found.append(module)
    for sub in folders:
        inner = sub / f"{sub.name}.cone"
        if not inner.is_file():
            raise CongoError(f"{sub}: a folder in gpu/ is a module folder, holding its"
                             f" designated file, {sub.name}.cone: gpu/ holds the package's"
                             f" GPU modules, and a folder with none would be part of no"
                             f" module")
        if name_fault(sub.name):
            raise CongoError(f"{inner}: a module folder names its module:"
                             f" '{sub.name}' {name_fault(sub.name)}; rename the folder,"
                             f" its designated file and its 'mod' line")
        found.append(scan_folder_module(sub.name, sub, inner))
    in_src = list(tree.children)
    for module in found:
        clash = next((m for m in in_src if m.name == module.name), None)
        if clash is not None:
            raise CongoError(f"{module.files[0]}: {pkg.name} has two submodules named"
                             f" '{module.name}', this one in gpu/ and {clash.files[0]} in"
                             f" src/; rename one")
        for inner_module, _ in module.walk():
            inner_module.gpu = True
        tree.children.append(module)
    tree.children.sort(key=lambda m: m.name)


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
    kernels: bool = False     # what is compiled for the GPU holds compute entry points
    # The packages its gpu/ folder imports, first-seen order; None where it has no gpu/
    gpu_deps: dict[str, Package] | None = None


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
        sisters = {m.name: m for m in parent.children} if parent else {}
        # A module of gpu/ is compiled for the GPU without src/, so it may not
        # use src/: not a sister there, and not a name of the root [Jon 3 Oct]
        from_gpu = module.gpu and parent is not None and not parent.gpu
        if from_gpu and module.extends is not None:
            sister = sisters.get(module.extends.name)
            if sister is not None and not sister.gpu:
                raise gpu_uses_src(pkg, module.extends, "extends", sister)
        for imp in module.imports:
            if imp.name is None:
                raise CongoError(f"{imp.where()}: import {imp.written}: a Congo build"
                                 f" imports a package by its name, not by a path")
            if imp.name in sisters:
                if from_gpu and not sisters[imp.name].gpu:
                    raise gpu_uses_src(pkg, imp, "import", sisters[imp.name])
                continue
            if from_gpu and imp.name != PRELUDE and registry.find(imp.name) is None:
                raise gpu_uses_src(pkg, imp, "import", None)
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
    # Only what is compiled for the GPU has its kernels built, so only its
    # source is read past the headers: a marked package's, or its gpu/ folder's
    kernels = any(holds_kernel(file) for module, _ in tree.walk()
                  if pkg.gpu or module.gpu for file in module.files)
    gpu_deps = None
    if any(module.gpu for module, _ in tree.walk()):
        gpu_deps = {}
        for module, _ in tree.walk():
            if module.gpu:
                for dep in lines[id(module)].values():
                    if dep.name != PRELUDE:
                        gpu_deps.setdefault(dep.name, dep)
    return Unit(pkg, tree, deps, lines, kernels, gpu_deps)


def gpu_uses_src(pkg: Package, imp: Import, verb: str, sister: Module | None) -> CongoError:
    """The refusal of a gpu/ module's import or extends of src/: a sister
    there, or a name of the root module, whose designated file is in src/."""
    what = (f"{sister.name} is a module of {pkg.name}'s src/ ({shown(sister.files[0])})"
            if sister is not None else
            f"{imp.name} is no package, so it would be a name of {pkg.name}'s root module,"
            f" in src/")
    return CongoError(f"{imp.where()}: {verb} {imp.name}: {what}, and gpu/ may not use src/:"
                      f" gpu/ is compiled for the GPU without src/. Move what both need into"
                      f" gpu/, which src/ may use")


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


def first_import(unit: Unit, name: str, gpu_only: bool = False) -> Import:
    """The first import of name in the package, in walk order; of its gpu/
    folder only, where asked."""
    for module, _ in unit.tree.walk():
        if gpu_only and not module.gpu:
            continue
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
    """The build description of one package; 'out' is the folder the include
    files of the packages it imports are in."""
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


def newlines(text: str) -> str:
    """Every line end made '\\n', as text=True's universal newlines make '\\r\\n'
    and a lone '\\r'."""
    return text.replace("\r\n", "\n").replace("\r", "\n")


def utf8_text(data: bytes) -> str:
    """What conec, or vswhere with -utf8, printed: UTF-8."""
    return newlines(data.decode("utf-8", "replace"))


def console_text(data: bytes) -> str:
    """What a console program Congo starts printed, link.exe and cmd among
    them: text in the code page of the console it shares with Congo, or, when
    Congo has none, of the console Windows makes for it, whose code page is
    the OEM one. Read through Windows' own conversion, as the Cone Congo
    reads it."""
    if not IS_WINDOWS:
        return utf8_text(data)
    import ctypes
    kernel32 = ctypes.windll.kernel32
    page = kernel32.GetConsoleOutputCP() or kernel32.GetOEMCP()
    if page == 65001:
        return utf8_text(data)
    return newlines(codecs.code_page_decode(page, data, "replace", True)[0])


def find_vcvars() -> str | None:
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) \
        / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.exists():
        result = subprocess.run(
            [str(vswhere), "-latest", "-products", "*",
             "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-property", "installationPath", "-utf8"],
            capture_output=True)
        root = utf8_text(result.stdout).strip().splitlines()
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
        banner = subprocess.run([tool, "/?"], capture_output=True,
                                stdin=subprocess.DEVNULL, timeout=20)
    except (OSError, subprocess.SubprocessError):
        return False
    return "Microsoft" in console_text(banner.stdout) + console_text(banner.stderr)


def env_value(env: dict[str, str], name: str) -> str | None:
    """A variable by its name in any case, as Windows names it: os.environ spells
    WindowsSdkDir WINDOWSSDKDIR."""
    name = name.upper()
    return next((v for k, v in env.items() if k.upper() == name), None)


def with_set_output(base: dict[str, str], printed: str) -> dict[str, str]:
    """base with the variables cmd's set printed, one entry per variable in
    base's spelling of its name: a Command Prompt's own PATH is Path, which set
    prints, and os.environ, so base, spells it PATH."""
    env = dict(base)
    spelled = {k.upper(): k for k in env}
    for line in printed.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            env[spelled.get(key.upper(), key)] = value
    return env


def msvc_target(tool: str, env: dict[str, str]) -> str | None:
    """The machine a Microsoft link.exe links for, lower-cased (x64, x86, arm64),
    or None when nothing says. Its folder says first, bin\\Host<host>\\<target>,
    since that is the linker that would run; else what VsDevCmd.bat and
    vcvars*.bat record, VSCMD_ARG_TGT_ARCH."""
    folder = Path(tool).parent
    if folder.parent.name.lower().startswith("host"):
        return folder.name.lower()
    target = env_value(env, "VSCMD_ARG_TGT_ARCH")
    return target.lower() if target else None


def without_vs_libs(env: dict[str, str]) -> dict[str, str]:
    """env with the folders a Visual Studio environment put on LIB taken out --
    each entry under the Visual Studio or Windows SDK folder the environment
    names -- and every other entry, the user's own, kept in order. vcvars64.bat
    puts its own folders in front of what LIB already lists, and an x86
    environment's left behind them would still be searched. (PATH keeps its
    x86 folders: vcvars64.bat's link.exe is found first.)"""
    roots = [os.path.normcase(os.path.normpath(root)) + os.sep
             for root in (env_value(env, k) for k in
                          ("VSINSTALLDIR", "WindowsSdkDir", "UniversalCRTSdkDir"))
             if root]

    def put_there(entry: str) -> bool:
        where = os.path.normcase(os.path.normpath(entry)) + os.sep
        return any(where.startswith(root) for root in roots)

    env = dict(env)
    for key in env:
        if key.upper() == "LIB":
            env[key] = os.pathsep.join(entry for entry in env[key].split(os.pathsep)
                                       if entry and not put_there(entry))
    return env


class Linker:
    """The system linker, with the environment it needs. On Windows that is
    Microsoft's link.exe in the environment vcvars64.bat sets up, which Congo
    produces itself, so no Developer Command Prompt is needed. An inherited
    environment is used only when its link.exe is Microsoft's and links for x64,
    as conec's objects are: Visual Studio's default Developer Command Prompt
    links for x86."""

    def __init__(self, conestd: Path):
        self.conestd = conestd
        self.env = dict(os.environ)
        if IS_WINDOWS:
            ran: tuple[str, str] | None = None     # vcvars64.bat, and what it printed
            inherited = shutil.which("link.exe")
            if not (inherited and is_msvc_linker(inherited)):
                self.env, ran = self.run_vcvars(self.env)
            elif msvc_target(inherited, self.env) not in (None, "x64"):
                # Silently, as when there is no linker: nothing is the user's to fix
                self.env, ran = self.run_vcvars(without_vs_libs(self.env))
            path = env_value(self.env, "PATH") or ""
            tool = shutil.which("link.exe", path=path)
            if tool is None or not is_msvc_linker(tool):
                raise CongoError(self.no_linker(tool, path, ran))
            self.tool = tool
        else:
            tool = shutil.which("cc") or shutil.which("gcc")
            if tool is None:
                raise CongoError("no cc or gcc on PATH to link with")
            self.tool = tool

    @staticmethod
    def no_linker(tool: str | None, path: str, ran: tuple[str, str] | None) -> str:
        """Why no Microsoft link.exe was found: which vcvars64.bat ran and how its
        output ended, and the folders searched."""
        lines = ["no Microsoft link.exe: install Visual Studio's C++ tools"
                 " (Congo runs vcvars64.bat to find it)"]
        if tool is not None:
            lines.append(f"the link.exe found, {tool}, is not Microsoft's")
        if ran is not None:
            vcvars, printed = ran
            tail = printed.strip().splitlines()[-15:]
            lines.append(f"ran {vcvars}, which printed "
                         + ("nothing" if not tail else "(the end of it):"))
            lines += [f"    {line}" for line in tail]
        lines.append("searched for link.exe in PATH's folders:")
        lines += [f"    {folder}" for folder in path.split(os.pathsep) if folder]
        return "\n".join(lines)

    @staticmethod
    def vs_environment(base: dict[str, str] | None = None) -> dict[str, str]:
        """vcvars64.bat's environment, run in base (by default Congo's own). It
        keeps what LIB already lists, behind its own folders."""
        return Linker.run_vcvars(dict(os.environ) if base is None else base)[0]

    @staticmethod
    def run_vcvars(base: dict[str, str]) -> tuple[dict[str, str], tuple[str, str]]:
        """vcvars64.bat's environment, run in base, and the batch file with what
        it printed, which a failure to find link.exe shows."""
        vcvars = find_vcvars()
        if vcvars is None:
            raise CongoError("no Visual Studio C++ tools (vcvars64.bat) to link with")
        # A single string, not a list: list2cmdline would escape the quotes
        # around the batch path, and cmd would not recognise it. The marker
        # divides what vcvars64.bat prints, in the console's code page, from
        # the environment, which 'cmd /u' prints in UTF-16 so that every value
        # arrives whole.
        marker = "--congo: vcvars64.bat's environment--"
        result = subprocess.run(f'cmd /c ""{vcvars}" && echo {marker}&& cmd /u /c set"',
                                env=base, capture_output=True)
        before, found, after = result.stdout.partition(marker.encode())
        printed = console_text(before)
        variables = ""
        if found:
            # After the line end echo writes
            after = after[2:] if after.startswith(b"\r\n") else after
            variables = newlines(after.decode("utf-16-le", "replace"))
        err = console_text(result.stderr)
        if result.returncode != 0 or not variables:
            raise CongoError(f"{vcvars} failed:\n{printed}{err}")
        return with_set_output(base, variables), (vcvars, printed + err)

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
# Runtime libraries: the DLLs a program needs beside it
# ---------------------------------------------------------------------------
#
# A package's [link] runtime list names the DLLs a program using it loads at
# run time (SDL3, OpenSSL's libssl-3 and libcrypto-3). Every executable Congo
# links gets the DLLs of every package of its build beside it, copied into
# its folder: so a program inherits them from whatever it imports, at any
# depth, and names nothing itself [Jon 3 Oct]. Windows only: elsewhere the
# list is read and nothing is copied.

def deps_folder() -> Path:
    """The fetched-dependencies folder: CONE_DEPS, else deps/ in the repository,
    which tools/deps/fetch.py fills."""
    named = os.environ.get("CONE_DEPS")
    return Path(named).resolve() if named else REPO_DEPS


def env_folders(name: str) -> list[Path]:
    """The folders an environment variable lists, ';'-separated on Windows."""
    return [Path(entry) for entry in os.environ.get(name, "").split(os.pathsep) if entry]


def runtime_folders(paths: list[Path]) -> list[Path]:
    """Where a runtime DLL is looked for, in order: the build's [link] paths,
    in the order the linker is given them; each folder in the fetched-
    dependencies folder, by name; the folders LIB lists, where an import
    library's own DLL usually sits beside it (SDL3's development kit); then
    the folders PATH lists. The first copy found is the one copied."""
    deps = deps_folder()
    fetched = (sorted((p for p in deps.iterdir() if p.is_dir()), key=lambda p: p.name)
               if deps.is_dir() else [])
    return [*paths, *fetched, *env_folders("LIB"), *env_folders("PATH")]


def find_runtime(name: str, pkg: Package, folders: list[Path], paths: list[Path]) -> Path:
    """The DLL a [link] runtime list names, where runtime_folders finds it first;
    one that is nowhere is an error naming it and the package that asked."""
    for folder in folders:
        candidate = folder / f"{name}.dll"
        if candidate.is_file():
            return candidate
    listed = ", ".join(str(p) for p in paths) or "none"
    raise CongoError(f"{pkg.name}'s [link] runtime names {name}, but {name}.dll is in none"
                     f" of the folders Congo looks in: [link] paths ({listed}), then each"
                     f" folder in {deps_folder()} (python tools/deps/fetch.py fills it),"
                     f" then the folders LIB lists, then the folders PATH lists")


def copy_runtime(source: Path, into: Path, name: str) -> bool:
    """Copy a runtime DLL into a program's folder, unless the copy there is the
    same already, byte for byte; whether it copied."""
    return copy_if_changed(source, into / f"{name}.dll")


def copy_if_changed(source: Path, target: Path) -> bool:
    """Copy a file, unless the copy at target is the same already, byte for
    byte; whether it copied."""
    if (target.is_file() and target.stat().st_size == source.stat().st_size
            and target.read_bytes() == source.read_bytes()):
        return False
    try:
        shutil.copyfile(source, target)
    except OSError as exc:
        raise CongoError(f"cannot copy {source} to {target}: {exc}") from None
    return True


# ---------------------------------------------------------------------------
# Building
# ---------------------------------------------------------------------------

def compile_unit(conec: Path, unit: Unit, out: Path, mode: str, top: bool,
                 env: dict[str, str], packages: list[Package] = (),
                 includes: Path | None = None, announce: bool = True) -> Path:
    """Write the package's build description into build/<mode>/ and compile the
    package on its own. Every package but the one being built is a library;
    the one being built is what its manifest says. 'packages' is the unit's
    dependency closure, which the description lists for its include files.
    A library's compile writes its include file beside its object, which is
    what every package compiled after it that imports it is compiled against.
    'includes' is the folder those include files are in, where it is not 'out'
    itself: a test program is compiled into a folder of its own, against the
    include files in the package's build folder."""
    output = unit.pkg.output if top else "library"
    desc = out / f"{unit.pkg.name}.conebuild"
    desc.write_text(description(unit, mode, output, includes or out, packages),
                    encoding="utf-8")
    # A library's include file is written afresh, so one left by an earlier
    # build never stands in for it
    include_for(unit.pkg, out).unlink(missing_ok=True)
    if announce:
        say("Compiling", f"{unit.pkg.label()} ({unit.pkg.root})")
    if not run_conec([str(conec), "-o", str(out), str(desc)], env):
        raise CongoError(f"could not compile {unit.pkg.name} (build description:"
                         f" {desc})")
    obj = out / f"{unit.pkg.name}{OBJ_EXT}"
    if not obj.is_file():
        raise CongoError(f"conec wrote no object at {obj}")
    include = include_for(unit.pkg, out)
    if output == "library" and not include.is_file():
        raise CongoError(f"conec wrote no include file for {unit.pkg.name} at {include}")
    return obj


def run_conec(command: list[str], env: dict[str, str]) -> bool:
    """Run conec, the -D defines put after the compiler, and show what it said
    but its closing 'Compile finished' line; whether it succeeded."""
    defines = [arg for define in DEFINES for arg in ("-D", define)]
    result = subprocess.run([command[0], *defines, *command[1:]], env=env,
                            capture_output=True)
    said = utf8_text(result.stdout) + utf8_text(result.stderr)
    chatter = [line for line in said.splitlines()
               if line.strip() and not line.startswith("Compile finished")]
    if chatter:
        print("\n".join(chatter), flush=True)
    return result.returncode == 0


# ---------------------------------------------------------------------------
# The GPU build: a package's kernels
# ---------------------------------------------------------------------------
#
# GPU compatibility is declared per package [Jon 3 Oct 2026]: [package]
# targets = ["native", "gpu"]. A package so marked whose source holds compute
# entry points ('fn @compute(64) name(...)') is compiled for the GPU as well,
# into build/<mode>/<name>.spv, a SPIR-V module for Vulkan whose kernels are
# its entry points, each named as its function is. Building for the GPU,
# Congo refuses any package it imports that is not marked, before anything
# is compiled. Every function on a GPU target is inlined into the kernel that
# calls it (memory kinds, question 2 (a)), so a kernel's build needs the
# bodies of what it calls: the packages it imports are compiled from their
# source, not against their include files. conec does that for a package it
# finds on its package search path, so the GPU build is a direct compile of
# a module written here that imports the package, with --path naming the
# folders the packages are in. Which of a marked package's features a GPU
# refuses is the compiler's to say; Congo decides only which packages may be
# compiled for the GPU.

def check_gpu_marks(unit: Unit, units: dict[str, Unit]) -> None:
    """Refuse every package unit imports, at any depth, that is not marked for
    the GPU, naming the imports that pulled each one in: every package a
    marked package imports, or, for a package with a gpu/ folder, every one
    that folder imports. The prelude, which no package imports, is not asked;
    what it holds that a GPU lacks is the compiler's to refuse where it is
    used."""
    unmarked: dict[str, str] = {}
    seen: set[str] = set()
    folder = unit.gpu_deps is not None and not unit.pkg.gpu

    def visit(here: Unit, chain: list[str]) -> None:
        start = here is unit and folder
        for name, dep in (here.gpu_deps if start else here.deps).items():
            step = (f"{here.pkg.name} imports {name} at"
                    f" {first_import(here, name, gpu_only=start).where()}")
            if not dep.gpu:
                unmarked.setdefault(name, "; ".join([*chain, step]))
            elif name not in seen:
                seen.add(name)
                visit(units[name], [*chain, step])

    visit(unit, [])
    if unmarked:
        listed = "\n".join(f"    {name}: {chain}" for name, chain in unmarked.items())
        what = f"{unit.pkg.name}'s gpu/ folder" if folder else unit.pkg.name
        raise CongoError(f"{what} is compiled for the GPU, and so is every package it"
                         f" imports, each of which must be marked for the GPU with targets ="
                         f" [\"native\", \"gpu\"] in its congo.toml; these are not:\n{listed}")


def found_on_search_path(name: str, folders: list[Path]) -> Path | None:
    """The file conec loads for 'import name' on its package search path, the
    folders in order: in each, name.cone, then a package's source root,
    name/src/name.cone, then name/name.cone (fileFindPackage)."""
    for folder in folders:
        for candidate in (folder / f"{name}.cone", folder / name / "src" / f"{name}.cone",
                          folder / name / f"{name}.cone"):
            if candidate.is_file():
                return candidate
    return None


def gpu_search_folders(unit: Unit, packages: list[Package], core: Package,
                       lead: list[Path]) -> list[Path]:
    """The folders a GPU build hands conec's --path: 'lead' first (a gpu/
    folder, whose modules the build imports by name), then the folder each
    package it compiles is in, but the packages folder (core's), which conec
    searches last anyway. Checked: conec must find each package where Congo
    found it, which needs each in a folder named for it, and none hidden by
    another of its name earlier on the path (a gpu/ module among them)."""
    packages_folder = core.root.parent
    folders: list[Path] = list(lead)
    for pkg in packages:
        folder = pkg.root.parent
        if folder != packages_folder and folder not in folders:
            folders.append(folder)
    searched = [*folders, packages_folder]
    for pkg in packages:
        found = found_on_search_path(pkg.name, searched)
        if found is None or os.path.normcase(found.resolve()) != os.path.normcase(pkg.src.resolve()):
            there = f"finds {found}" if found is not None else f"finds no {pkg.name}"
            hint = (f"; it looks for a package in a folder named for it, and {pkg.name} is in"
                    f" {pkg.root}" if pkg.root.name != pkg.name else "")
            raise CongoError(f"{unit.pkg.name}'s GPU build compiles {pkg.name} from its source,"
                             f" which conec finds by its name on its package search path"
                             f" ({', '.join(str(f) for f in searched)}): there it {there},"
                             f" not {pkg.src}{hint}")
    return folders


GPU_MODULE = """\
// The GPU build of {name}, written by Congo for conec; rewritten on every
// build. {name}'s compute entry points are the kernels of {name}{ext}.

mod {module};

import {name};
"""


GPU_FOLDER_MODULE = """\
// The GPU build of {name}'s gpu/ folder, written by Congo for conec; rewritten
// on every build. Its compute entry points are the kernels of {name}{ext}. Each
// module of gpu/ is imported, after the modules of gpu/ it imports, and found
// on the package search path, which gpu/ leads.

mod {module};
{imports}"""


def gpu_closure(unit: Unit, units: dict[str, Unit], core: Package) -> list[Package]:
    """What a gpu/ folder's GPU build compiles from source: the packages the
    folder imports and everything they import, each after what it imports,
    the prelude first, after what it imports itself (libc)."""
    seen: dict[str, Package] = {}

    def visit(pkg: Package) -> None:
        for name, dep in units[pkg.name].deps.items():
            if name not in seen:
                visit(dep)
                seen[name] = dep

    visit(core)
    seen[PRELUDE] = core
    for name, dep in (unit.gpu_deps or {}).items():
        if name not in seen:
            visit(dep)
            seen[name] = dep
    return list(seen.values())


def gpu_module_order(tree: Module) -> list[Module]:
    """The modules of gpu/, the root's submodules drawn from it, each after
    those of them it extends or imports: the order the GPU build imports them
    in, so that each is compiled into the module, not only declared (an
    import conec meets first beside its importer's file is declared alone)."""
    mine = {m.name: m for m in tree.children if m.gpu}
    order: list[Module] = []
    done: set[str] = set()

    def visit(module: Module) -> None:
        done.add(module.name)
        needs = [module.extends] if module.extends is not None else []
        for imp in [*needs, *module.imports]:
            if imp.name in mine and imp.name not in done:
                visit(mine[imp.name])
        order.append(module)

    for module in mine.values():
        if module.name not in done:
            visit(module)
    return order


def compile_gpu(conec: Path, unit: Unit, units: dict[str, Unit], core: Package, out: Path,
                mode: str, env: dict[str, str]) -> Path:
    """Compile a package's kernels into out/<name>.spv: a direct conec compile,
    for SPIR-V's Vulkan form, in out/gpu/<name>/, of a module written here,
    so that what it imports is found on the package search path and compiled
    from its source. For a marked package, the module imports the package;
    for a package with a gpu/ folder, it imports each module of gpu/ by name,
    gpu/ leading the search path, and src/ is never compiled for the GPU."""
    pkg = unit.pkg
    module = f"{pkg.name}_gpu"
    if unit.gpu_deps is not None:
        packages = gpu_closure(unit, units, core)
        gpu_folder = pkg.root / GPU_FOLDER
        folders = gpu_search_folders(unit, packages, core, [gpu_folder])
        imports = "".join(f"\nimport {m.name};" for m in gpu_module_order(unit.tree))
        text = GPU_FOLDER_MODULE.format(name=pkg.name, module=module, ext=SPV_EXT,
                                        imports=imports + "\n" if imports else "")
        shown_root = gpu_folder
    else:
        packages = closure(unit, units, core)
        folders = gpu_search_folders(unit, [unit.pkg, *packages], core, [])
        text = GPU_MODULE.format(name=pkg.name, module=module, ext=SPV_EXT)
        shown_root = pkg.root
    work = out / "gpu" / pkg.name
    work.mkdir(parents=True, exist_ok=True)
    source = work / f"{module}.cone"
    source.write_text(text, encoding="utf-8")
    spv = out / f"{pkg.name}{SPV_EXT}"
    spv.unlink(missing_ok=True)
    written = work / f"{module}{SPV_EXT}"
    written.unlink(missing_ok=True)
    say("Compiling", f"{pkg.label()} for the GPU ({shown_root})")
    command = [str(conec), f"--triple={GPU_TRIPLE}"]
    if mode == "debug":
        command.append("--debug")
    if folders:
        command.append("--path=" + ";".join(str(f) for f in folders))
    if not run_conec([*command, "-o", str(work), str(source)], env):
        raise CongoError(f"could not compile {pkg.name} for the GPU ({source})")
    if not written.is_file():
        raise CongoError(f"conec wrote no SPIR-V module at {written}")
    os.replace(written, spv)
    return spv


def validate_kernels(spv: Path) -> tuple[bool | None, str]:
    """spirv-val's verdict on a package's kernels, for Vulkan 1.3, and what it
    said; None where spirv-val (the Vulkan SDK's) is not on PATH."""
    validator = shutil.which("spirv-val")
    if validator is None:
        return None, ""
    result = subprocess.run([validator, "--target-env", "vulkan1.3", str(spv)],
                            capture_output=True)
    return result.returncode == 0, utf8_text(result.stdout) + utf8_text(result.stderr)


def build_folder(pkg: Package, mode: str) -> Path:
    return lone_build_root(pkg.src) / mode if pkg.lone else pkg.root / "build" / mode


def lone_build_root(file: Path) -> Path:
    """A lone file's build goes under the Congo home, not beside the file: the
    folder a lone file sits in is nobody's package, and may well hold a 'build'
    folder of its own that 'congo clean' must not delete."""
    spelled = str(file.resolve())
    digest = hashlib.sha1((spelled.lower() if IS_WINDOWS else spelled).encode()).hexdigest()[:10]
    return congo_home() / "lone" / f"{file.stem}-{digest}"


class Session:
    """The compiles of one build folder, and its link. A package is compiled at
    most once in a session: 'congo test' builds the package, then each test
    program against the objects and include files that build left, compiling
    only what a test imports that the package does not (stdio, say)."""

    def __init__(self, registry: Registry, out: Path, mode: str):
        self.registry = registry
        self.out = out
        self.mode = mode
        self.objs: dict[str, Path] = {}      # each library compiled into 'out'
        self.spvs: dict[str, Path] = {}      # each package's kernels compiled into 'out'
        self.gpu_checked: set[str] = set()   # packages whose imports were checked for the GPU
        self.warned: set[str] = set()
        self._conec: Path | None = None
        self._linker: Linker | None = None

    @property
    def conec(self) -> Path:
        """Found at the first compile, after the imports are resolved, so that an
        error in the source is reported before anything about the machine."""
        if self._conec is None:
            self._conec = find_conec()
        return self._conec

    def compile(self, order: list[Unit], top: Package, top_out: Path | None = None,
                announce: bool = True) -> list[Path]:
        """Compile every unit of the order not compiled already, each after what
        it imports; return the objects in the order's order. 'top' gets the
        output its manifest says, into 'top_out' where that is given (a test
        program's own folder); every other package is a library, into 'out'."""
        conec = self.conec
        self.out.mkdir(parents=True, exist_ok=True)
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
            if (not unit.pkg.lone and unit.pkg.hand_include.is_file()
                    and unit.pkg.name not in self.warned):
                self.warned.add(unit.pkg.name)
                print(f"warning: {unit.pkg.hand_include} is not used: a package's importers"
                      f" compile against the include file its own compile generates,"
                      f" build/<mode>/{unit.pkg.name}.cone", file=sys.stderr)
        by_name = {unit.pkg.name: unit for unit in order}
        # Before anything is compiled: a package compiled for the GPU, and a
        # marked package being built, import only packages marked for it
        for unit in order:
            if ((unit.kernels or (unit.pkg is top and (unit.pkg.gpu
                                                        or unit.gpu_deps is not None)))
                    and unit.pkg.name not in self.gpu_checked):
                check_gpu_marks(unit, by_name)
                self.gpu_checked.add(unit.pkg.name)
        objs = []
        for unit in order:
            is_top = unit.pkg is top
            if not is_top and unit.pkg.name in self.objs:
                objs.append(self.objs[unit.pkg.name])
                continue
            into = top_out if is_top and top_out is not None else self.out
            into.mkdir(parents=True, exist_ok=True)
            obj = compile_unit(conec, unit, into, self.mode, is_top, env,
                               closure(unit, by_name, core), self.out,
                               announce or not is_top)
            if into == self.out and (not is_top or unit.pkg.output == "library"):
                self.objs[unit.pkg.name] = obj
            objs.append(obj)
        # Then the kernels of each marked package that has them, once a
        # session, into the build folder, beside its object
        for unit in order:
            if unit.kernels and core is not None and unit.pkg.name not in self.spvs:
                self.spvs[unit.pkg.name] = compile_gpu(conec, unit, by_name, core, self.out,
                                                       self.mode, env)
        return objs

    def link(self, order: list[Unit], objs: list[Path], exe: Path,
             announce: bool = True) -> None:
        if self._linker is None:
            self._linker = Linker(find_conestd(self.conec))
        linker = self._linker
        # The program's object first, then the packages it imports, last built first;
        # and the C libraries every one of them names, in the same order, each once.
        # A C package's own object is linked like any other: it defines nothing
        # (as core's does not), and so needs no case of its own
        units = [order[-1], *reversed(order[:-1])]
        libraries = list(dict.fromkeys(lib for u in units for lib in u.pkg.libraries))
        paths = list(dict.fromkeys(p for u in units for p in u.pkg.link_paths))
        # The runtime DLLs every one of them names, each once, found before the
        # link: a missing one stops the build with no program linked, an
        # earlier build's removed, as a failed link leaves none
        runtime: dict[str, Path] = {}
        if IS_WINDOWS:
            folders = runtime_folders(paths)
            try:
                for u in units:
                    for name in u.pkg.runtime:
                        if name not in runtime:
                            runtime[name] = find_runtime(name, u.pkg, folders, paths)
            except CongoError:
                try:
                    exe.unlink(missing_ok=True)
                except OSError:
                    pass
                raise
        command = linker.command([objs[-1], *reversed(objs[:-1])], exe, libraries, paths)
        if announce:
            say("Linking", shown(exe))
        exe.unlink(missing_ok=True)
        result = subprocess.run(command, env=linker.env, capture_output=True)
        if result.returncode != 0:
            named = "; ".join(f"{u.pkg.name} names {', '.join(u.pkg.libraries)}"
                              for u in units if u.pkg.libraries)
            where = ("the folders the LIB environment variable lists" if IS_WINDOWS
                     else "the linker's own search path")
            hint = (f"\nC libraries linked ({named}): the linker looks for each in the"
                    f" folders [link] paths names, then in {where}" if named else "")
            raise CongoError(f"link failed:\n{' '.join(command)}\n{console_text(result.stdout)}"
                             f"{console_text(result.stderr)}{hint}")
        for name, source in runtime.items():
            if copy_runtime(source, exe.parent, name) and announce:
                say("Copying", shown(source))
        # The kernels of every package of the build, each beside the program
        # as a runtime DLL is, where gpuwork's readSpirv finds it by its name
        for u in units:
            spv = self.spvs.get(u.pkg.name)
            if spv is not None and spv.parent != exe.parent:
                if copy_if_changed(spv, exe.parent / spv.name) and announce:
                    say("Copying", shown(spv))


def build(pkg: Package, mode: str, session: Session | None = None) -> Path:
    """Build pkg and everything it imports; return the executable, or for a
    library its object."""
    if session is None:
        session = Session(Registry(registry_folders()), build_folder(pkg, mode), mode)
    order = build_order(pkg, session.registry)
    objs = session.compile(order, pkg)
    if pkg.output == "library":
        spv = session.spvs.get(pkg.name)
        kernels = f" and kernels {shown(spv)}" if spv is not None else ""
        say("Finished", f"{mode} library object {shown(objs[-1])}{kernels}")
        return objs[-1]
    exe = session.out / f"{pkg.name}{EXE_EXT}"
    session.link(order, objs, exe)
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
    if header.mod is None and name_fault(name):
        raise CongoError(f"{file}: with no 'mod' line it is named by its file:"
                         f" '{name}' {name_fault(name)}; give the file a 'mod' line")
    if name_fault(name):
        raise CongoError(f"{file}: 'mod {name}': '{name}' {name_fault(name)}; rename the"
                         f" module, and the file where it is named for it")
    return Package(name, None, "executable", file.parent, file, lone=True)


def current_package_manifest() -> Path:
    manifest = find_manifest(Path.cwd())
    if manifest is None:
        raise CongoError(f"no {MANIFEST} in this folder or any above it; 'congo new <name>'"
                         f" makes a package, and 'congo run <file.cone>' runs a lone file")
    return manifest


def current_package() -> Package:
    return read_manifest(current_package_manifest())


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
    if name_fault(name):
        raise CongoError(f"'{name}' {name_fault(name)}")
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
    code = subprocess.run([str(exe), *args.args]).returncode
    note = dll_note(code, exe)
    if note:
        print(note.lstrip("\n"), file=sys.stderr)
    return code


def cmd_clean(args: argparse.Namespace) -> int:
    if args.file:
        target = lone_build_root(Path(args.file))
    else:
        target = current_package().root / "build"
    if target.exists():
        shutil.rmtree(target)
        say("Removed", shown(target))
    return 0


# ---------------------------------------------------------------------------
# congo test
# ---------------------------------------------------------------------------
#
# The first phase of package testing, and deliberately thin [Jon 26 Sep]: each
# program in a package's tests/ folder is built as a program that imports the
# package by name, as any user's program does -- the package compiled on its
# own, the test compiled against the include file that compile generated, the
# two linked -- then run, its output and exit status compared with the expected
# files beside it. Every program in examples/ is built. Nothing here decides how
# tests are scaffolded, mocked or asserted, or whether one may reach a private
# name: a test sees what any importer sees.

TESTS = "tests"
EXAMPLES = "examples"
TEST_TIMEOUT = 60     # seconds a test program may run before it fails


@dataclass
class Tally:
    passed: int = 0
    failed: int = 0
    skipped: int = 0      # debug-only tests, not built or run in a release test
    built: int = 0
    unbuilt: int = 0
    broken: int = 0       # packages that did not build, so nothing of theirs ran
    failures: list[str] = field(default_factory=list)

    def add(self, other: "Tally") -> None:
        self.passed += other.passed
        self.failed += other.failed
        self.skipped += other.skipped
        self.built += other.built
        self.unbuilt += other.unbuilt
        self.broken += other.broken
        self.failures += other.failures

    def ok(self) -> bool:
        return not self.failed and not self.unbuilt and not self.broken

    def summary(self) -> str:
        def count(n: int, word: str) -> str:
            return f"{n} {word}{'' if n == 1 else 's'}"
        text = (f"{count(self.passed + self.failed + self.skipped, 'test')}:"
                f" {self.passed} passed, {self.failed} failed")
        # Said only where a test was skipped, so a report without one reads
        # as it always has
        if self.skipped:
            text += f", {self.skipped} skipped (debug only)"
        text += (f"; {count(self.built + self.unbuilt, 'example')}:"
                 f" {self.built} built, {self.unbuilt} failed to build")
        if self.broken:
            text += f"; {count(self.broken, 'package')} did not build"
        return text


def normalized_lines(text: str) -> list[str]:
    """Output as it is compared: line ends made '\\n' (git may check an expected
    file out with CRLF), and trailing blank lines dropped, as the compiler's
    suite compares a run's output."""
    lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    while lines and not lines[-1].strip():
        lines.pop()
    return lines


def expected_exit(program: Path) -> int:
    """tests/<name>.exit holds the exit status the program must end with, where
    it is not 0: one integer, as the compiler suite's 'program_exit' is."""
    path = program.with_suffix(".exit")
    if not path.is_file():
        return 0
    text = path.read_text(encoding="utf-8").strip()
    try:
        return int(text)
    except ValueError:
        raise CongoError(f"{shown(path)} must hold one integer, the exit status the"
                         f" test ends with; it holds {text!r}") from None


def debug_only(program: Path) -> bool:
    """tests/<name>.debug, whatever it holds, marks a test debug-only: a test of
    a check made in a debug build alone (assertDebug, assertDebugMsg), which
    has nothing to say in a release build. A release test skips it."""
    return program.with_suffix(".debug").is_file()


def indent(text: str, pad: str = "        ") -> str:
    return "\n".join(pad + line for line in text.rstrip("\n").split("\n"))


def programs(folder: Path, name_filter: str | None) -> list[Path]:
    """The .cone programs directly in a tests/ or examples/ folder, by name,
    those whose name contains the filter where one is given."""
    if not folder.is_dir():
        return []
    found = sorted(p for p in folder.iterdir() if p.is_file() and p.suffix == ".cone")
    return [p for p in found if name_filter is None or name_filter in p.stem]


def build_program(session: Session, file: Path, into: Path) -> Path:
    """A test or example program, built as a lone file of its own that imports
    the package by name: its imports resolved through the session's registry,
    where the package under test is found first; the packages it imports
    compiled into the package's build folder (once each, most already there),
    and the program itself into a folder of its own, then linked."""
    pkg = lone_package(file)
    order = build_order(pkg, session.registry)
    objs = session.compile(order, pkg, top_out=into, announce=False)
    exe = into / f"{pkg.name}{EXE_EXT}"
    session.link(order, objs, exe, announce=False)
    return exe


class Debuggee:
    """Windows only: a test program run as Congo's debuggee, so that its crash
    ends here rather than in Windows Error Reporting (test/run.py's Debuggee,
    the same).

    A panic ends through the C library's 'abort', a fail-fast on Windows, and
    Windows hands every fail-fast (and every other crash) to WER, which holds
    the dying process, and its executable locked, while it collects a report.
    WER is one service for the whole machine: forty panics at once took up to
    1.5s each to exit, and a hundred and fifty up to 18s. Neither a job
    object's DIE_ON_UNHANDLED_EXCEPTION nor SetErrorMode keeps a fail-fast
    out of WER; a debugger does. A crash reaches the debugger as a
    second-chance exception before WER, and Congo ends the process there,
    with the exception's own code, which is the exit status Windows would
    have given it: 3221226505 (0xC0000409) for a panic. Everything else is
    passed on as no debugger would have seen it: first-chance exceptions to
    the program's handlers, the loader's breakpoint aside. And the program
    gets the heap it would have had: started under a debugger, Windows gives
    a process its checking debug heap unless _NO_DEBUG_HEAP is set.

    The thread that started the process is the one that must wait for its
    debug events, which 'pump' does; until it does, the program is stopped.
    """

    DEBUG_ONLY_THIS_PROCESS = 0x2
    ENVIRONMENT = {"_NO_DEBUG_HEAP": "1"}
    EXCEPTION_DEBUG_EVENT, CREATE_PROCESS_DEBUG_EVENT = 1, 3
    EXIT_PROCESS_DEBUG_EVENT, LOAD_DLL_DEBUG_EVENT = 5, 6
    DBG_CONTINUE, DBG_EXCEPTION_NOT_HANDLED = 0x00010002, 0x80010001
    STATUS_BREAKPOINT = 0x80000003

    _kernel32 = None

    @classmethod
    def kernel32(cls):
        if cls._kernel32 is None:
            import ctypes
            from ctypes import wintypes as w

            class DebugEvent(ctypes.Structure):
                # DEBUG_EVENT: the union is read as words, at its 8-byte
                # alignment; an exception's code is the low half of word 0,
                # and dwFirstChance the low half of word 19, after the
                # 152-byte EXCEPTION_RECORD; a file handle is word 0
                _fields_ = [("code", w.DWORD), ("pid", w.DWORD), ("tid", w.DWORD),
                            ("u", ctypes.c_uint64 * 20)]

            k = ctypes.WinDLL("kernel32", use_last_error=True)
            k.WaitForDebugEvent.argtypes = [ctypes.POINTER(DebugEvent), w.DWORD]
            k.WaitForDebugEvent.restype = w.BOOL
            k.ContinueDebugEvent.argtypes = [w.DWORD, w.DWORD, w.DWORD]
            k.ContinueDebugEvent.restype = w.BOOL
            k.TerminateProcess.argtypes = [w.HANDLE, w.UINT]
            k.TerminateProcess.restype = w.BOOL
            k.CloseHandle.argtypes = [w.HANDLE]
            k.CloseHandle.restype = w.BOOL
            k.DebugEvent = DebugEvent
            cls._kernel32 = k
        return cls._kernel32

    def __init__(self, process: subprocess.Popen):
        self.process = process
        self.k = self.kernel32()
        self.event = self.k.DebugEvent()
        self.seen_breakpoint = False
        self.exited = False

    def pump(self, wait: float) -> bool:
        """Handle the debug events that come within wait seconds; whether
        the process has exited."""
        k, ev = self.k, self.event
        ms = max(0, int(wait * 1000))
        while not self.exited and k.WaitForDebugEvent(ev, ms):
            ms = 0
            status = self.DBG_CONTINUE
            if ev.code == self.EXCEPTION_DEBUG_EVENT:
                code = ev.u[0] & 0xFFFFFFFF
                first_chance = ev.u[19] & 0xFFFFFFFF
                if first_chance and code == self.STATUS_BREAKPOINT and not self.seen_breakpoint:
                    self.seen_breakpoint = True          # the loader's, for a debugger
                elif first_chance:
                    status = self.DBG_EXCEPTION_NOT_HANDLED
                elif not k.TerminateProcess(int(self.process._handle), code):
                    status = self.DBG_EXCEPTION_NOT_HANDLED
            elif ev.code in (self.CREATE_PROCESS_DEBUG_EVENT, self.LOAD_DLL_DEBUG_EVENT):
                if ev.u[0]:
                    k.CloseHandle(ev.u[0])               # the image's file, the debugger's to close
            elif ev.code == self.EXIT_PROCESS_DEBUG_EVENT:
                self.exited = True
            k.ContinueDebugEvent(ev.pid, ev.tid, status)
        return self.exited


def run_program(exe: Path, cwd: Path, timeout: float) -> subprocess.CompletedProcess:
    """Run a test program with no input, what it writes captured, for at most
    timeout seconds: a returncode of None is one stopped for its time, with
    what it wrote until then. On Windows it runs as Congo's debuggee
    (Debuggee), so that a crash, a panic's above all, ends at once."""
    if not IS_WINDOWS:
        try:
            return subprocess.run([str(exe)], cwd=cwd, capture_output=True,
                                  stdin=subprocess.DEVNULL, timeout=timeout)
        except subprocess.TimeoutExpired as e:
            return subprocess.CompletedProcess(e.cmd, None, e.output or b"", e.stderr or b"")
    process = subprocess.Popen([str(exe)], cwd=cwd, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               env=dict(os.environ, **Debuggee.ENVIRONMENT),
                               creationflags=Debuggee.DEBUG_ONLY_THIS_PROCESS)
    debugger = Debuggee(process)
    # Its outputs are read on a thread of their own, both at once, while this
    # one, the debugger, waits for its events
    captured: list[bytes] = []
    reader = threading.Thread(target=lambda: captured.extend(process.communicate()))
    reader.start()
    deadline = time.monotonic() + timeout
    timed_out = False
    while not debugger.pump(0.05):
        if time.monotonic() > deadline:
            timed_out = True
            process.kill()
            # Once killed it ends at once, its last events aside
            stop = time.monotonic() + 10
            while not debugger.pump(0.05) and time.monotonic() < stop:
                pass
            break
    reader.join()
    code = None if timed_out else process.wait()
    stdout, stderr = captured if len(captured) == 2 else (b"", b"")
    return subprocess.CompletedProcess([str(exe)], code, stdout, stderr)


def run_test(session: Session, file: Path, bless: bool) -> tuple[bool, str]:
    """Build, run and compare one test program: (passed, what to show)."""
    into = session.out / TESTS / file.stem
    exe = build_program(session, file, into)
    # Run in its own build folder, so a file it writes by a relative name
    # lands there and not in the package's source
    ran = run_program(exe, into, TEST_TIMEOUT)
    if ran.returncode is None:
        # What it wrote to stderr says how far it got: a panic's line, say,
        # before an end that never came
        err = ran.stderr.decode("utf-8", errors="replace")
        return False, (f"timed out after {TEST_TIMEOUT} seconds"
                       + (f"\nstderr:\n{indent(err, '  ')}" if err.strip() else ""))
    # A program that could not start has nothing to compare, and this line is
    # how a person learns why (no dialog says so)
    not_started = dll_note(ran.returncode, exe)
    if not_started:
        return False, not_started.lstrip("\n")
    stdout = ran.stdout.decode("utf-8", errors="replace")
    stderr = ran.stderr.decode("utf-8", errors="replace")
    expected_path = file.with_suffix(".out")
    if not expected_path.is_file():
        if not bless:
            return False, (f"no expected output, {shown(expected_path)}: write it by"
                           f" hand, or run 'congo test {file.stem} --bless' and check"
                           f" what it writes against the source")
        # Blessed only where there is nothing yet: an expected file is written
        # by hand, or blessed once and then checked by hand, and never rewritten
        # from a run, which would pin whatever the package does today
        expected_path.write_text("\n".join(normalized_lines(stdout)) + "\n",
                                 encoding="utf-8", newline="\n")
        note = f"blessed {shown(expected_path)}"
        if ran.returncode != 0:
            file.with_suffix(".exit").write_text(f"{ran.returncode}\n", encoding="utf-8",
                                                 newline="\n")
            note += f" and {shown(file.with_suffix('.exit'))} ({ran.returncode})"
        return True, note + ": check it by hand against the source"
    problems = []
    want = expected_exit(file)
    if ran.returncode != want:
        problems.append(f"exited {ran.returncode}, expected {want}")
    expected = normalized_lines(expected_path.read_text(encoding="utf-8"))
    actual = normalized_lines(stdout)
    if actual != expected:
        diff = difflib.unified_diff(expected, actual, fromfile=shown(expected_path),
                                    tofile="actual", lineterm="", n=2)
        problems.append("output differs:\n" + indent("\n".join(diff), "  "))
    # tests/<name>.err, written by hand, holds what the program must write to
    # stderr, where it writes there on purpose: a panic's line. Without one,
    # stderr is not compared
    err_path = file.with_suffix(".err")
    err_checked = err_path.is_file()
    if err_checked:
        expected_err = normalized_lines(err_path.read_text(encoding="utf-8"))
        actual_err = normalized_lines(stderr)
        if actual_err != expected_err:
            diff = difflib.unified_diff(expected_err, actual_err, fromfile=shown(err_path),
                                        tofile="actual", lineterm="", n=2)
            problems.append("stderr differs:\n" + indent("\n".join(diff), "  "))
    if problems and stderr.strip() and not err_checked:
        problems.append("stderr:\n" + indent(stderr, "  "))
    return not problems, "\n".join(problems)


def test_package(pkg: Package, mode: str, name_filter: str | None, bless: bool) -> Tally:
    """Build the package, then run each test and build each example."""
    tally = Tally()
    say("Testing", f"{pkg.label()} ({pkg.root})")
    # The package under test is found first, under its own name, so a test
    # imports this copy -- whether or not it is in a registry, and whatever
    # else a registry holds of that name
    registry = Registry(registry_folders())
    registry.packages[pkg.name] = pkg
    session = Session(registry, build_folder(pkg, mode), mode)
    tests = programs(pkg.root / TESTS, name_filter)
    examples = programs(pkg.root / EXAMPLES, name_filter)
    try:
        build(pkg, mode, session)
    except CongoError as exc:
        print(f"error: {exc}", file=sys.stderr)
        tally.broken += 1
        tally.failures.append(f"{pkg.name}: the package does not build")
        say("Result", f"{pkg.name}: the package does not build")
        return tally
    # Its kernels, where it has them, checked by the Vulkan SDK's validator
    # for Vulkan 1.3, counted as a test; where the SDK is not installed they
    # are only built, and that is said
    spv = session.spvs.get(pkg.name)
    if spv is not None and name_filter is None:
        valid, said = validate_kernels(spv)
        if valid is None:
            print(f"     kernels {spv.name} ... built, not validated: spirv-val, the Vulkan"
                  f" SDK's, is not on PATH", flush=True)
        else:
            print(f"     kernels {spv.name} ... {'valid' if valid else 'FAILED'}", flush=True)
            if not valid:
                if said:
                    print(indent(said), flush=True)
                tally.failed += 1
                tally.failures.append(f"{pkg.name}: kernels {spv.name}")
            else:
                tally.passed += 1
    if not (pkg.root / TESTS).is_dir():
        say("Tests", f"none: {pkg.name} has no {TESTS} folder")
    elif pkg.output != "library" and tests:
        print(f"error: {pkg.name} is an executable package, and a test imports the"
              f" package it tests; only a library can be imported, so its tests"
              f" cannot run yet", file=sys.stderr)
        tally.failed += len(tests)
        tally.failures += [f"{pkg.name}: test {t.stem}" for t in tests]
        tests = []
    elif not tests and name_filter is None:
        say("Tests", f"none: {pkg.name}'s {TESTS} folder holds no .cone program")
    for file in tests:
        # Not built or run, so nothing is blessed for it either
        if mode == "release" and debug_only(file):
            print(f"        test {file.stem} ... skipped (debug only)", flush=True)
            tally.skipped += 1
            continue
        try:
            passed, detail = run_test(session, file, bless)
        except CongoError as exc:
            passed, detail = False, str(exc)
        print(f"        test {file.stem} ... {'ok' if passed else 'FAILED'}", flush=True)
        if detail:
            print(indent(detail), flush=True)
        if passed:
            tally.passed += 1
        else:
            tally.failed += 1
            tally.failures.append(f"{pkg.name}: test {file.stem}")
    for file in examples:
        try:
            build_program(session, file, session.out / EXAMPLES / file.stem)
            passed, detail = True, ""
        except CongoError as exc:
            passed, detail = False, str(exc)
        print(f"     example {file.stem} ... {'built' if passed else 'FAILED to build'}",
              flush=True)
        if detail:
            print(indent(detail), flush=True)
        if passed:
            tally.built += 1
        else:
            tally.unbuilt += 1
            tally.failures.append(f"{pkg.name}: example {file.stem}")
    say("Result", f"{pkg.name}: {tally.summary()}")
    return tally


def package_manifests(folder: Path) -> list[Path]:
    """The manifests of a folder of packages, a registry folder such as packages/."""
    return [sub / MANIFEST for sub in sorted(folder.iterdir(), key=lambda p: p.name)
            if (sub / MANIFEST).is_file()]


def cmd_test(args: argparse.Namespace) -> int:
    mode = "release" if args.release else "debug"
    here = Path.cwd()
    # A folder of packages (a registry folder such as packages/) that is not a
    # package itself: each package in it is tested
    manifests = [] if (here / MANIFEST).is_file() else package_manifests(here)
    if not manifests:
        manifests = [current_package_manifest()]
    total = Tally()
    for manifest in manifests:
        total.add(test_package(read_manifest(manifest), mode, args.name, args.bless))
    if args.name is not None and not (total.passed + total.failed + total.skipped
                                      + total.built + total.unbuilt + total.broken):
        raise CongoError(f"no test or example has '{args.name}' in its name")
    if len(manifests) > 1:
        say("Result", f"{len(manifests)} packages: {total.summary()}")
    if not total.ok():
        print("failed:\n" + indent("\n".join(total.failures), "    "), file=sys.stderr)
        return 1
    return 0


def add_define_argument(parser: argparse.ArgumentParser) -> None:
    """TEMPORARY, a provisional mechanism whose final design is open: '-D',
    handed to conec as it is given, which checks it."""
    parser.add_argument("-D", "--define", action="append", default=[], metavar="NAME[=INT]",
                        help="define NAME (1) or NAME=INT for isDefined and definedInt in every"
                             " compile of the build (provisional)")


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
        add_define_argument(p)
        if name == "run":
            p.add_argument("file", nargs="?", help="a lone .cone file, run with no manifest")
            p.epilog = "Arguments after '--' are passed to the program."
        p.set_defaults(func=func)
    test = sub.add_parser("test", help="build this package, run its tests and build its"
                                       " examples")
    test.add_argument("name", nargs="?",
                      help="only the tests and examples whose file name contains this")
    test.add_argument("--release", action="store_true", help="an optimised build")
    add_define_argument(test)
    test.add_argument("--bless", action="store_true",
                      help="write the expected output of a test that has none, from a"
                           " run; check it by hand against the source")
    test.set_defaults(func=cmd_test)
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


def utf8_streams() -> None:
    """Congo writes UTF-8, as Cone programs and their expected output are.
    Python's own choice for a stream that is not a console is the locale's code
    page (1252 on most Windows machines), which cannot hold most of Unicode: a
    test's failure diff with a character outside it raised UnicodeEncodeError
    when stdout was a pipe. A console is unaffected, as Python already writes
    it UTF-8; 'replace' covers what UTF-8 cannot encode, a lone surrogate."""
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")


def main(argv: list[str] | None = None) -> int:
    if sys.version_info < (3, 11):
        print("congo: needs Python 3.11 or later", file=sys.stderr)
        return 1
    utf8_streams()
    hard_errors_to_status()
    args = parse_args(sys.argv[1:] if argv is None else argv)
    DEFINES[:] = getattr(args, "define", [])
    try:
        return args.func(args)
    except CongoError as exc:
        print(f"congo: error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
