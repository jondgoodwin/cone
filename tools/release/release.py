#!/usr/bin/env python3
"""Build the Cone downloads from ONE commit: the Windows zip, the Linux tarball,
the install scripts, the checksums and the Downloads page.

    python tools/release/release.py export  [--hash REV]      the source of REV, as a tarball
    python tools/release/release.py windows [--hash REV] ...  conec for Windows, and cone-<hash8>-windows-x64.zip
    python tools/release/release.py linux   [--hash REV] ...  conec for Linux x86-64, and cone-<hash8>-linux-x86_64.tar.gz
    python tools/release/release.py stage   [--hash REV] ...  checksums, install.ps1, install.sh and download.html
                                            (--no-windows: a Linux-only release, without install.ps1)
    python tools/release/release.py fetch-python              the pinned Windows embeddable Python (a download)

Everything is built from `git archive REV`, never from a working tree, so the
files that go in an archive are the commit's own (line ends included), whatever
the builder's checkout settings. REV is origin/master unless --hash says
otherwise. The commit is recorded in the archive (VERSION) and in every file
name (the first 8 hexadecimal digits).

Nothing here uploads anything. The output goes to --out (default
build/release/<hash8>/ under the repository); publishing is a separate act.
`windows` runs on Windows and `linux` on Linux x86-64, so the two archives are
built on two machines: copy one into the other's output folder, or into any
folder, and run `stage --out` over both. `--source-tar` takes the tarball
`export` made, for a machine that has no clone of the repository.

Windows needs LLVM 23 (default C:\\LLVM\\23), CMake, Ninja and the Visual
Studio 2022 C++ tools, SDL3's development kit (--sdl3), the fetched OpenSSL
(--openssl), and Python's embeddable zip (--python-zip; see `fetch-python`).
Linux needs apt's llvm-23-dev, cmake, ninja-build and build-essential.

Python 3.11 or later, standard library only.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
import tomllib
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
IS_WINDOWS = os.name == "nt"
SITE = "https://cone.jondgoodwin.com"

# Seconds-since-epoch of 1 Jan 1980, the earliest time a zip entry can carry
ZIP_EPOCH_FLOOR = 315532800


class ReleaseError(Exception):
    pass


def say(text: str) -> None:
    print(f"release: {text}", flush=True)


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def run(command: list, cwd: Path | None = None, env: dict | None = None) -> None:
    say("run: " + " ".join(str(c) for c in command))
    result = subprocess.run([str(c) for c in command], cwd=cwd, env=env)
    if result.returncode != 0:
        raise ReleaseError(f"{command[0]} exited {result.returncode}")


# ---------------------------------------------------------------------------
# The commit
# ---------------------------------------------------------------------------

def git(*args: str) -> str:
    result = subprocess.run(["git", "-C", str(REPO), *args], capture_output=True, text=True)
    if result.returncode != 0:
        raise ReleaseError(f"git {' '.join(args)}: {result.stderr.strip()}")
    return result.stdout.strip()


class Source:
    """An exported tree of one commit: its folder, its full hash, its commit time."""

    def __init__(self, folder: Path, full: str, epoch: int):
        self.folder = folder
        self.full = full
        self.short = full[:8]
        self.epoch = epoch


def export_source(rev: str, work: Path) -> Source:
    """`git archive` of REV into work/src-<hash8>, with RELEASE_INFO (hash, then the
    commit time in seconds) beside it."""
    full = git("rev-parse", "--verify", f"{rev}^{{commit}}")
    epoch = int(git("show", "-s", "--format=%ct", full))
    folder = work / f"src-{full[:8]}"
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    archive = subprocess.run(["git", "-C", str(REPO), "archive", "--format=tar", full],
                             capture_output=True)
    if archive.returncode != 0:
        raise ReleaseError(f"git archive: {archive.stderr.decode(errors='replace')}")
    with tarfile.open(fileobj=io.BytesIO(archive.stdout)) as tar:
        tar.extractall(folder, filter="data")
    (folder / "RELEASE_INFO").write_text(f"{full}\n{epoch}\n", encoding="utf-8")
    say(f"exported {full} to {folder}")
    return Source(folder, full, epoch)


def unpack_source(tarball: Path, work: Path) -> Source:
    with tarfile.open(tarball) as tar:
        info = tar.extractfile("RELEASE_INFO")
        if info is None:
            raise ReleaseError(f"{tarball} has no RELEASE_INFO: it is not an `export` tarball")
        full, epoch = info.read().decode().split()[:2]
        folder = work / f"src-{full[:8]}"
        if folder.exists():
            shutil.rmtree(folder)
        folder.mkdir(parents=True)
        tar.extractall(folder, filter="data")
    say(f"unpacked {full} from {tarball}")
    return Source(folder, full, int(epoch))


def get_source(args: argparse.Namespace) -> Source:
    work = Path(args.work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    if args.source_tar:
        return unpack_source(Path(args.source_tar), work)
    return export_source(args.hash, work)


def output_folder(args: argparse.Namespace, source: Source) -> Path:
    out = Path(args.out).resolve() / source.short
    out.mkdir(parents=True, exist_ok=True)
    return out


# ---------------------------------------------------------------------------
# What goes in an archive, whichever the system
# ---------------------------------------------------------------------------

def copy_tree(src: Path, dest: Path) -> None:
    shutil.copytree(src, dest, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))


def stage_common(source: Source, root: Path, platform: str) -> None:
    """The parts of the archive that are the commit's own files: Congo, every
    package, the Cone licence and the notices. `root` is the archive's top folder."""
    src = source.folder
    root.mkdir(parents=True)
    copy_tree(src / "packages", root / "packages")
    tools = root / "tools" / "congo"
    tools.mkdir(parents=True)
    for name in ("congo.py", "README.md"):
        shutil.copy2(src / "tools" / "congo" / name, tools / name)
    lic = root / "licenses"
    lic.mkdir()
    shutil.copy2(src / "LICENSE", lic / "LICENSE-Cone-MIT.txt")
    shutil.copy2(src / "COPYRIGHT", lic / "COPYRIGHT-Cone.txt")
    shutil.copy2(HERE / "NOTICES.txt", root / "NOTICES.txt")
    (root / "VERSION").write_text(
        f"Cone, commit {source.full}\n"
        f"commit time {time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(source.epoch))} UTC\n"
        f"platform {platform}\n", encoding="utf-8", newline="\n")


def llvm_license(path: str | None) -> Path:
    """LLVM's licence text (Apache 2.0 with the LLVM exceptions): the copy kept
    in tools/release/licenses, from LLVM's LICENSE.TXT, or --llvm-license's."""
    chosen = Path(path) if path else HERE / "licenses" / "LLVM-LICENSE.TXT"
    if not chosen.is_file():
        raise ReleaseError(f"no LLVM licence text at {chosen}")
    return chosen


# ---------------------------------------------------------------------------
# Archives: reproducible, so the same commit and inputs give the same bytes
# ---------------------------------------------------------------------------

def make_zip(root: Path, dest: Path, epoch: int) -> None:
    stamp = time.gmtime(max(epoch, ZIP_EPOCH_FLOOR))[:6]
    files = sorted(p for p in root.rglob("*") if p.is_file())
    part = dest.with_name(dest.name + ".part")     # never a half-written archive under its name
    with zipfile.ZipFile(part, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for path in files:
            name = f"{root.name}/{path.relative_to(root).as_posix()}"
            info = zipfile.ZipInfo(name, stamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            zf.writestr(info, path.read_bytes(), compresslevel=9)
    part.replace(dest)


def make_tar_gz(root: Path, dest: Path, epoch: int, executable: set[str]) -> None:
    """`executable` holds the archive-relative names that get mode 755."""
    files = sorted(p for p in root.rglob("*") if p.is_file())
    part = dest.with_name(dest.name + ".part")
    with part.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0, compresslevel=9) as gz:
            with tarfile.open(fileobj=gz, mode="w", format=tarfile.PAX_FORMAT) as tar:
                for folder in sorted({p.parent for p in files} | {root}):
                    info = tarfile.TarInfo(f"{root.name}/" + folder.relative_to(root).as_posix()
                                           if folder != root else root.name)
                    info.type = tarfile.DIRTYPE
                    info.mode = 0o755
                    info.mtime = epoch
                    tar.addfile(info)
                for path in files:
                    rel = path.relative_to(root).as_posix()
                    info = tarfile.TarInfo(f"{root.name}/{rel}")
                    info.size = path.stat().st_size
                    info.mode = 0o755 if rel in executable else 0o644
                    info.mtime = epoch
                    info.uname = info.gname = ""
                    with path.open("rb") as handle:
                        tar.addfile(info, handle)
    part.replace(dest)


# ---------------------------------------------------------------------------
# Windows
# ---------------------------------------------------------------------------

CONGO_CMD = """@echo off
rem Congo, the Cone build tool, run with the Python and the compiler of this
rem installation. Nothing outside this folder is needed except Microsoft's C++
rem Build Tools (the linker).
setlocal
set "CONEC=%~dp0conec.exe"
"%~dp0..\\python\\python.exe" "%~dp0..\\tools\\congo\\congo.py" %*
exit /b %ERRORLEVEL%
"""

README_WINDOWS = """Cone, for Windows x64
=====================

Put the bin folder on your PATH (install.ps1 does), then:

    congo new hello
    cd hello
    congo run

congo builds with conec, Cone's compiler, and links with Microsoft's linker, so
Microsoft's C++ Build Tools (Visual Studio Build Tools, the "Desktop development
with C++" workload) must be installed, and git must be on the PATH (Congo fetches
packages by name with it). Everything else is in this folder:

    bin\\        conec.exe, conestd.lib (the runtime every program links), congo.cmd
    python\\     Python's embeddable distribution, which runs congo (nothing to install)
    tools\\congo Congo itself (README.md is its guide)
    packages\\   every Cone package: the standard library, and the domain libraries.
                The ones wrapping a C library carry its prebuilt Windows files in
                their clib folder (sdl: SDL3; tls: OpenSSL)
    licenses\\   the licence texts; NOTICES.txt says what each covers

Windows may warn about an unrecognised app the first time conec.exe runs: the
executables are not code-signed.

https://cone.jondgoodwin.com/download.html
"""


def vs_environment(source: Source) -> dict:
    """The Visual Studio x64 environment, found by the release's own Congo."""
    sys.path.insert(0, str(source.folder / "tools" / "congo"))
    import congo  # noqa: E402  (the exported copy: it knows how to find vcvars64.bat)
    return congo.Linker.vs_environment()


def build_conec_windows(source: Source, llvm: Path) -> Path:
    if not (llvm / "lib" / "cmake" / "llvm").is_dir():
        raise ReleaseError(f"no LLVM at {llvm} (lib\\cmake\\llvm is missing); pass --llvm")
    env = vs_environment(source)
    build = source.folder / "build" / "x64-release"
    run(["cmake", "-S", ".", "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         f"-DLLVM_DIR={llvm / 'lib' / 'cmake' / 'llvm'}"], cwd=source.folder, env=env)
    run(["cmake", "--build", build], cwd=source.folder, env=env)
    return build


def add_clib(package: Path, files: list[Path], licence: Path | None, licence_name: str) -> None:
    clib = package / "clib"
    clib.mkdir()
    for file in files:
        if not file.is_file():
            raise ReleaseError(f"missing {file}")
        shutil.copy2(file, clib / file.name)
    if licence is not None:
        shutil.copy2(licence, clib / licence_name)


def edit_manifest(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if text.count(old) != 1:
        raise ReleaseError(f"{path} no longer has exactly one {old!r}: update tools/release/release.py")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="")


def build_windows(args: argparse.Namespace) -> None:
    if not IS_WINDOWS:
        raise ReleaseError("the windows archive is built on Windows")
    source = get_source(args)
    out = output_folder(args, source)
    work = Path(args.work).resolve()
    if not args.python_zip:
        raise ReleaseError("pass --python-zip: the python.org embeddable distribution"
                           " (`release.py fetch-python` fetches the pinned one)")
    python_zip = Path(args.python_zip)
    pinned = args.python_sha256 or pins()["python"]["sha256"]
    if not pinned:
        raise ReleaseError("tools/release/pins.toml pins no SHA-256 for Python's zip yet: check the"
                           " file against python.org's published checksum, then pin it (or pass"
                           " --python-sha256)")
    if sha256_of(python_zip) != pinned.lower():
        raise ReleaseError(f"{python_zip} is not the pinned Python zip (SHA-256 differs)")

    build = build_conec_windows(source, Path(args.llvm))
    root = work / "stage-windows" / "cone"
    if root.parent.exists():
        shutil.rmtree(root.parent)
    stage_common(source, root, "windows-x64")

    bin_ = root / "bin"
    bin_.mkdir()
    shutil.copy2(build / "conec.exe", bin_ / "conec.exe")
    shutil.copy2(build / "conestd.lib", bin_ / "conestd.lib")
    (bin_ / "congo.cmd").write_text(CONGO_CMD.replace("\n", "\r\n"), encoding="ascii", newline="")
    (root / "README.txt").write_text(README_WINDOWS.replace("\n", "\r\n"), encoding="utf-8", newline="")

    # Python's embeddable distribution, as it comes (its own LICENSE.txt included)
    with zipfile.ZipFile(python_zip) as zf:
        zf.extractall(root / "python")
    if not (root / "python" / "python.exe").is_file():
        raise ReleaseError(f"{python_zip} holds no python.exe: it is not the embeddable distribution")
    shutil.copy2(root / "python" / "LICENSE.txt", root / "licenses" / "LICENSE-Python-PSF.txt")

    # The C libraries a package wraps travel inside that package (clib), found by
    # the package's own manifest, so a program needs no LIB and no DLL path of its own
    sdl3 = Path(args.sdl3)
    sdl3_lib = sdl3 / "lib" / "x64"
    add_clib(root / "packages" / "sdl", [sdl3_lib / "SDL3.lib", sdl3_lib / "SDL3.dll"],
             sdl3 / "LICENSE.txt", "LICENSE-SDL3-zlib.txt")
    edit_manifest(root / "packages" / "sdl" / "congo.toml",
                  'libraries = ["SDL3"]\n', 'libraries = ["SDL3"]\npaths = ["clib"]\n')
    shutil.copy2(sdl3 / "LICENSE.txt", root / "licenses" / "LICENSE-SDL3-zlib.txt")

    openssl = Path(args.openssl)
    add_clib(root / "packages" / "tls",
             [openssl / n for n in ("libssl.lib", "libcrypto.lib", "libssl-3.dll", "libcrypto-3.dll")],
             openssl / "LICENSE.txt", "LICENSE-OpenSSL-Apache-2.0.txt")
    edit_manifest(root / "packages" / "tls" / "congo.toml",
                  'paths = ["../../deps/openssl"]', 'paths = ["clib"]')
    shutil.copy2(openssl / "LICENSE.txt", root / "licenses" / "LICENSE-OpenSSL-Apache-2.0.txt")

    shutil.copy2(llvm_license(args.llvm_license), root / "licenses" / "LICENSE-LLVM-Apache-2.0-with-LLVM-exception.txt")

    (root / "BUILD-INFO.txt").write_text(
        f"commit {source.full}\n"
        f"python zip {python_zip.name} sha256 {sha256_of(python_zip)}\n"
        f"SDL3.dll sha256 {sha256_of(sdl3_lib / 'SDL3.dll')}\n"
        f"libssl-3.dll sha256 {sha256_of(openssl / 'libssl-3.dll')}\n"
        f"libcrypto-3.dll sha256 {sha256_of(openssl / 'libcrypto-3.dll')}\n"
        f"conec.exe sha256 {sha256_of(bin_ / 'conec.exe')}\n", encoding="utf-8", newline="\n")

    dest = out / f"cone-{source.short}-windows-x64.zip"
    make_zip(root, dest, source.epoch)
    say(f"{dest} {dest.stat().st_size} bytes, sha256 {sha256_of(dest)}")


# ---------------------------------------------------------------------------
# Linux x86-64
# ---------------------------------------------------------------------------

CONGO_SH = """#!/bin/sh
# Congo, the Cone build tool, run with this installation's compiler. Needs
# python3 (3.11 or later) and a C compiler to link with: cc.
here=$(dirname "$(readlink -f "$0")")
CONEC="$here/conec"
export CONEC
exec python3 "$here/../tools/congo/congo.py" "$@"
"""

README_LINUX = """Cone, for Linux x86-64
======================

Put the bin folder on your PATH (install.sh links conec and congo into
~/.local/bin), then:

    congo new hello
    cd hello
    congo run

Prerequisites, which this archive does not carry (on Ubuntu 24.04, the tested
system, or another distribution with glibc 2.38 or later):

    sudo apt install build-essential python3 git
    sudo apt install libssl-dev        # only for programs using the tls package

python3 must be 3.11 or later (git lets Congo fetch packages by name). The packages that wrap Windows-only C libraries
(sdl, and what draws with it) do not build on Linux.

    bin/         conec (LLVM linked in statically), libconestd.a (the runtime
                 every program links), congo
    tools/congo  Congo itself (README.md is its guide)
    packages/    every Cone package
    licenses/    the licence texts; NOTICES.txt says what each covers

https://cone.jondgoodwin.com/download.html
"""


def build_linux(args: argparse.Namespace) -> None:
    if sys.platform != "linux":
        raise ReleaseError("the linux archive is built on Linux x86-64")
    source = get_source(args)
    out = output_folder(args, source)
    work = Path(args.work).resolve()
    build = source.folder / "build" / "x64-release"
    # The build CLAUDE.md gives, unchanged: LLVM linked statically, so conec has no
    # libLLVM to find (155 MB; the playground's image links libLLVM.so instead)
    run(["cmake", "-S", ".", "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         f"-DLLVM_DIR={args.llvm_dir}"], cwd=source.folder)
    run(["cmake", "--build", build, "-j", str(args.jobs)], cwd=source.folder)

    root = work / "stage-linux" / "cone"
    if root.parent.exists():
        shutil.rmtree(root.parent)
    stage_common(source, root, "linux-x86_64")
    bin_ = root / "bin"
    bin_.mkdir()
    shutil.copy2(build / "conec", bin_ / "conec")
    shutil.copy2(build / "libconestd.a", bin_ / "libconestd.a")
    run(["strip", "--strip-all", bin_ / "conec"])
    (bin_ / "congo").write_text(CONGO_SH, encoding="utf-8", newline="\n")
    (root / "README.txt").write_text(README_LINUX, encoding="utf-8", newline="\n")
    shutil.copy2(llvm_license(args.llvm_license), root / "licenses" / "LICENSE-LLVM-Apache-2.0-with-LLVM-exception.txt")
    (root / "BUILD-INFO.txt").write_text(
        f"commit {source.full}\n"
        f"built on {subprocess.run(['lsb_release', '-ds'], capture_output=True, text=True).stdout.strip()}\n"
        f"llvm {subprocess.run(['llvm-config-23', '--version'], capture_output=True, text=True).stdout.strip()} (static)\n"
        f"glibc {subprocess.run(['ldd', '--version'], capture_output=True, text=True).stdout.splitlines()[0]}\n"
        f"conec sha256 {sha256_of(bin_ / 'conec')}\n", encoding="utf-8", newline="\n")

    dest = out / f"cone-{source.short}-linux-x86_64.tar.gz"
    make_tar_gz(root, dest, source.epoch, {"bin/conec", "bin/congo"})
    say(f"{dest} {dest.stat().st_size} bytes, sha256 {sha256_of(dest)}")


# ---------------------------------------------------------------------------
# Checksums, install scripts, the page
# ---------------------------------------------------------------------------

def human(size: int) -> str:
    return f"{size / (1 << 20):.1f} MB"


def fill(text: str, values: dict[str, str]) -> str:
    for key, value in values.items():
        text = text.replace(f"@{key}@", value)
    left = re.findall(r"@[A-Z0-9_]+@", text)
    if left:
        raise ReleaseError(f"unfilled placeholders: {sorted(set(left))}")
    return text


def stage(args: argparse.Namespace) -> None:
    full = git("rev-parse", "--verify", f"{args.hash}^{{commit}}") if not args.full_hash else args.full_hash
    short = full[:8]
    out = Path(args.out).resolve() / short
    win = out / f"cone-{short}-windows-x64.zip"
    lin = out / f"cone-{short}-linux-x86_64.tar.gz"
    needed = (lin,) if args.no_windows else (win, lin)
    missing = [p for p in needed if not p.is_file()]
    if missing:
        raise ReleaseError("missing: " + ", ".join(str(p) for p in missing))
    base = f"{SITE}/downloads/{short}"
    values = {
        "HASH": full, "HASH8": short, "SITE": SITE, "BASE": base,
        "LINUX_FILE": lin.name, "LINUX_SHA256": sha256_of(lin), "LINUX_SIZE": human(lin.stat().st_size),
    }
    sums = [f"{values['LINUX_SHA256']}  {lin.name}"]
    if args.no_windows:
        # Linux-only release: the page says the Windows compiler is coming, install.ps1 is
        # not staged and a leftover Windows zip is ignored, so the sums list only what ships
        values["WINDOWS_COMPILER"] = (HERE / "windows-compiler-soon.html.in").read_text(encoding="utf-8")
        values["WIN_ROW"] = ""
    else:
        values.update(WIN_FILE=win.name, WIN_SHA256=sha256_of(win), WIN_SIZE=human(win.stat().st_size))
        sums.insert(0, f"{values['WIN_SHA256']}  {win.name}")
        values["WINDOWS_COMPILER"] = fill((HERE / "windows-compiler.html.in").read_text(encoding="utf-8"), values)
        values["WIN_ROW"] = fill((HERE / "windows-file-row.html.in").read_text(encoding="utf-8"), values)
    (out / "SHA256SUMS").write_text("\n".join(sums) + "\n", encoding="utf-8", newline="\n")
    for name in ("install.sh",) if args.no_windows else ("install.ps1", "install.sh"):
        template = (HERE / f"{name}.in").read_text(encoding="utf-8")
        text = fill(template, values)
        newline = "\r\n" if name.endswith(".ps1") else "\n"
        (out / name).write_text(text, encoding="utf-8", newline=newline)
    page = (REPO / "conesite" / "public" / "download.html").read_text(encoding="utf-8")
    (out / "download.html").write_text(fill(page, values), encoding="utf-8", newline="\n")
    say(f"staged {out}: SHA256SUMS " + ("" if args.no_windows else "install.ps1 ") + "install.sh download.html")
    for line in sums:
        say(line)


# ---------------------------------------------------------------------------
# Python's embeddable distribution (a download)
# ---------------------------------------------------------------------------

def pins() -> dict:
    return tomllib.loads((HERE / "pins.toml").read_text(encoding="utf-8"))


def fetch_python(args: argparse.Namespace) -> None:
    pin = pins()["python"]
    if not pin["sha256"]:
        raise ReleaseError("pins.toml pins no SHA-256 for the Python zip: fetch it by hand once,"
                           " compare it with the checksum python.org publishes for that file,"
                           " and pin it")
    dest = Path(args.dest) / Path(pin["url"]).name
    dest.parent.mkdir(parents=True, exist_ok=True)
    say(f"fetching {pin['url']}")
    with urllib.request.urlopen(pin["url"]) as response:
        data = response.read()
    if hashlib.sha256(data).hexdigest() != pin["sha256"].lower():
        raise ReleaseError("the downloaded file's SHA-256 is not the pinned one: not kept")
    dest.write_bytes(data)
    say(f"{dest} {len(data)} bytes, sha256 verified")


# ---------------------------------------------------------------------------

def default_sdl3() -> str:
    return os.environ.get("SDL3_DIR", r"C:\libs\SDL3-3.4.16")


def default_openssl() -> str:
    deps = os.environ.get("CONE_DEPS")
    return str(Path(deps) / "openssl") if deps else str(REPO / "deps" / "openssl")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)

    def common(p: argparse.ArgumentParser) -> None:
        p.add_argument("--hash", default="origin/master", help="the commit (default origin/master)")
        p.add_argument("--source-tar", help="the tarball `export` made, instead of this clone's git")
        p.add_argument("--work", default=str(REPO / "build" / "release-work"))
        p.add_argument("--out", default=str(REPO / "build" / "release"))
        p.add_argument("--llvm-license", help="the LLVM LICENSE.TXT to bundle")

    p = sub.add_parser("export", help="the commit's source as a tarball")
    common(p)
    p = sub.add_parser("windows", help="the Windows x64 zip")
    common(p)
    p.add_argument("--llvm", default=r"C:\LLVM\23")
    p.add_argument("--sdl3", default=default_sdl3())
    p.add_argument("--openssl", default=default_openssl())
    p.add_argument("--python-zip", help="python-3.x.y-embed-amd64.zip")
    p.add_argument("--python-sha256", help="its SHA-256, when pins.toml does not pin it yet")
    p = sub.add_parser("linux", help="the Linux x86-64 tarball")
    common(p)
    p.add_argument("--llvm-dir", default="/usr/lib/llvm-23/lib/cmake/llvm")
    p.add_argument("--jobs", type=int, default=2)
    p = sub.add_parser("stage", help="checksums, install scripts and the Downloads page")
    common(p)
    p.add_argument("--full-hash", help="the 40-digit commit, on a machine with no clone")
    p.add_argument("--no-windows", action="store_true",
                   help="a Linux-only release: no Windows zip or install.ps1; the page says the Windows compiler is coming")
    p = sub.add_parser("fetch-python", help="download the pinned embeddable Python")
    p.add_argument("--dest", default=str(REPO / "build" / "release-inputs"))

    args = parser.parse_args()
    try:
        if args.command == "export":
            work = Path(args.work).resolve()
            work.mkdir(parents=True, exist_ok=True)
            source = export_source(args.hash, work)
            dest = Path(args.out).resolve()
            dest.mkdir(parents=True, exist_ok=True)
            tarball = dest / f"cone-src-{source.short}.tar.gz"
            with tarfile.open(tarball, "w:gz") as tar:
                for child in sorted(source.folder.iterdir()):
                    tar.add(child, arcname=child.name)
            say(f"{tarball} {tarball.stat().st_size} bytes")
        elif args.command == "windows":
            build_windows(args)
        elif args.command == "linux":
            build_linux(args)
        elif args.command == "stage":
            stage(args)
        else:
            fetch_python(args)
    except ReleaseError as exc:
        print(f"release: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
