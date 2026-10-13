#!/usr/bin/env python3
"""The exit-bar test for fetching packages by name, scripted so it can be rerun.

    python tools/registry/exit_bar.py --archive cone-<hash8>-windows-x64.zip
    python tools/registry/exit_bar.py --standin          (rehearsal; see below)

The bar (WI library-registry, "The exit-bar test"): on a machine with only the
compiler and Congo installed, one command builds and runs a program whose
dependencies, including core, stdio and a package wrapping a C library, are
fetched by name, built and linked. No package is built into the compiler and no
interface file is hand-written.

The archive is unpacked into a fresh folder and run in a CLEAN ENVIRONMENT: PATH is
its own bin folder, git's, and Windows' own; no Python (the archive carries one),
no repository, no LIB or INCLUDE, no CONEC, CONESTD or CONE_DEPS, and a Congo home
that is a new empty folder. Two things from the machine are used because nothing
here can do without them: Microsoft's Build Tools (the linker, which Congo finds
itself) and git with the credentials it has for the private registry.

Parts, each reported PASS, FAIL or SKIPPED, with why:

  1. HELLO     the archive as shipped: `congo new hello`, `congo run`.
  2. FETCH     a copy of the archive with every package removed from packages/ (only
               conestd, the runtime, stays), the registry list set in the Congo
               home, and ONE command, `congo run fetch_demo.cone`, for a program
               importing stdio and a package wrapping a C library. core, libc and
               stdio are fetched from the real private registry by name; the C
               package comes from a first registry made here, a git repository
               holding a package with a prebuilt DLL in it, which proves the
               ordered list of registries and a wrapper that carries its library.
  3. NEEDS-REGISTRY
               with the real registry's URL altered to one that does not exist,
               the same command fails and says which registry it could not fetch
               (the registry really is what supplies core).

--standin assembles the folder from this checkout (the committed packages/ and
congo.py, the conec and conestd.lib built in build/x64-release) and runs it with
the Python running this script, since there is no archive to unpack. It rehearses
the script and is NOT the archive test: the archive's own layout, its embedded
Python and its file set are not exercised.

Windows only for now (the C library fixture is a DLL built with cl). The registry
URL is --registry, default the private index https://github.com/jondgoodwin/cone-registry.git.
Python 3.11 or later, standard library only.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import textwrap
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
REGISTRY = "https://github.com/jondgoodwin/cone-registry.git"
IS_WINDOWS = os.name == "nt"

results: list[tuple[str, str, str]] = []


def report(part: str, outcome: str, why: str = "") -> None:
    results.append((part, outcome, why))
    print(f"exit_bar: {part}: {outcome}" + (f" ({why})" if why else ""), flush=True)


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(text).lstrip("\n"), encoding="utf-8", newline="\n")


def unpack(archive: Path, into: Path) -> Path:
    """The archive's root folder (the one holding bin/), unpacked into a new folder."""
    into.mkdir(parents=True)
    if archive.is_dir():
        shutil.copytree(archive, into / archive.name)
    elif archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as z:
            z.extractall(into)
    else:
        with tarfile.open(archive) as t:
            t.extractall(into, filter="data")
    for candidate in [into, *sorted(p for p in into.iterdir() if p.is_dir())]:
        if (candidate / "bin").is_dir():
            return candidate
    raise SystemExit(f"exit_bar: no bin/ folder in {archive}")


def standin(into: Path) -> Path:
    """Assemble an archive-shaped folder from this checkout."""
    root = into / "cone"
    (root / "bin").mkdir(parents=True)
    build = REPO / "build" / "x64-release"
    for name in ("conec.exe", "conestd.lib"):
        if not (build / name).is_file():
            raise SystemExit(f"exit_bar: --standin needs {build / name} (build conec first)")
        shutil.copy2(build / name, root / "bin" / name)
    tarball = into / "standin.tar"
    subprocess.run(["git", "archive", "--format=tar", "-o", str(tarball), "HEAD", "packages",
                    "tools/congo/congo.py"], cwd=REPO, check=True)
    with tarfile.open(tarball) as t:
        t.extractall(root, filter="data")
    return root


def congo_command(root: Path, is_standin: bool) -> list[str]:
    if is_standin:
        return [sys.executable, str(root / "tools" / "congo" / "congo.py")]
    for name in ("congo.cmd", "congo.bat", "congo"):
        if (root / "bin" / name).is_file():
            return [str(root / "bin" / name)]
    raise SystemExit(f"exit_bar: no congo in {root / 'bin'}")


def clean_env(root: Path, home: Path, is_standin: bool) -> dict[str, str]:
    system = os.environ.get("SystemRoot", r"C:\Windows")
    git = shutil.which("git")
    folders = [str(root / "bin"), str(Path(system) / "System32"), system]
    if git:
        folders.append(str(Path(git).parent))
    if is_standin:
        folders.append(str(Path(sys.executable).parent))
    env = {k: v for k, v in os.environ.items()
           if k.upper() not in {"PATH", "LIB", "INCLUDE", "LIBPATH", "CONEC", "CONESTD", "CONE_DEPS",
                                "CONE_PACKAGES", "PYTHONPATH", "PYTHONHOME", "CONGO_HOME"}}
    env["PATH"] = os.pathsep.join(folders)
    env["CONGO_HOME"] = str(home)
    env["GIT_TERMINAL_PROMPT"] = "0"
    return env


def run(command: list[str], cwd: Path, env: dict[str, str], timeout: int = 900) -> subprocess.CompletedProcess:
    return subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=timeout)


def git(*args: str, cwd: Path) -> str:
    env = dict(os.environ, GIT_AUTHOR_NAME="exit bar", GIT_AUTHOR_EMAIL="exit-bar@example.com",
               GIT_COMMITTER_NAME="exit bar", GIT_COMMITTER_EMAIL="exit-bar@example.com")
    result = subprocess.run(["git", *args], cwd=cwd, env=env, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"exit_bar: git {' '.join(args)}: {result.stderr.strip()}")
    return result.stdout.strip()


def c_wrapper_registry(root: Path, work: Path) -> str:
    """A registry (a git repository) with one entry, tri, a package wrapping a C library
    whose import library and DLL are in the package. Built with the Build Tools' cl, the
    one tool every Windows build here needs. The URL of the registry."""
    sys.path.insert(0, str(root / "tools" / "congo"))
    import congo  # noqa: E402  (the archive's own copy: it knows where the Build Tools are)
    env = congo.Linker.vs_environment()
    src = work / "tri-src"
    write(src / "congo.toml", """
        [package]
        name = "tri"
        version = "0.1.0"
        output = "library"

        [link]
        libraries = ["tri3"]
        paths = ["clib"]
        runtime = ["tri3"]
        """)
    write(src / "src" / "tri.cone", "mod @c tri;\n\npub extern fn triple(n i32) i32;\n")
    write(src / "csrc" / "triple.c", "__declspec(dllexport) int triple(int n) { return 3 * n; }\n")
    (src / "clib").mkdir()
    found = shutil.which("cl", path=env["PATH"])
    subprocess.run([found, "/nologo", "/LD", str(src / "csrc" / "triple.c"), "/Fe:tri3.dll"],
                   cwd=src / "clib", env=env, check=True, capture_output=True)
    for scratch in ("tri3.exp", "triple.obj"):
        (src / "clib" / scratch).unlink(missing_ok=True)
    git("init", "--quiet", "-b", "main", cwd=src)
    git("add", "-A", cwd=src)
    git("add", "-f", "clib", cwd=src)
    git("commit", "--quiet", "-m", "tri, a package wrapping a C library", cwd=src)
    commit = git("rev-parse", "HEAD", cwd=src)
    bare = work / "tri.git"
    git("clone", "--quiet", "--bare", str(src), str(bare), cwd=work)
    index = work / "wrappers"
    write(index / "tri.toml", congo.index_entry_text("tri", bare.as_uri(), commit, ""))
    git("init", "--quiet", "-b", "main", cwd=index)
    git("add", "-A", cwd=index)
    git("commit", "--quiet", "-m", "tri", cwd=index)
    index_bare = work / "wrappers.git"
    git("clone", "--quiet", "--bare", str(index), str(index_bare), cwd=work)
    return index_bare.as_uri()


def main() -> int:
    ap = argparse.ArgumentParser(description="The exit-bar test for fetching packages by name.")
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--archive", type=Path, help="the downloaded compiler: a .zip, .tar.gz or unpacked folder")
    group.add_argument("--standin", action="store_true", help="rehearse with a folder assembled from this checkout")
    ap.add_argument("--registry", default=REGISTRY, help=f"the index to fetch the Cone packages from (default {REGISTRY})")
    ap.add_argument("--keep", action="store_true", help="keep the work folder")
    args = ap.parse_args()
    if not IS_WINDOWS:
        report("ALL", "SKIPPED", "Windows only so far: the C library fixture is a DLL built with cl")
        return 0

    work = Path(tempfile.mkdtemp(prefix="exitbar-"))
    print(f"exit_bar: work folder {work}", flush=True)
    try:
        root = standin(work / "src") if args.standin else unpack(args.archive.resolve(), work / "src")
        is_standin = args.standin
        congo = congo_command(root, is_standin)

        # 1. HELLO: the archive as shipped
        home = work / "home1"
        env = clean_env(root, home, is_standin)
        job = work / "job1"
        job.mkdir()
        run(congo + ["new", "hello"], job, env)
        out = run(congo + ["run"], job / "hello", env)
        if out.returncode == 0 and out.stdout.rstrip().endswith("Hello, world!"):
            report("HELLO", "PASS")
        else:
            report("HELLO", "FAIL", (out.stderr or out.stdout).strip()[-600:])

        # 2. FETCH: no packages shipped, everything by name
        for folder in sorted((root / "packages").iterdir()):
            if (folder / "congo.toml").is_file():
                shutil.rmtree(folder, onerror=lambda f, p, e: (os.chmod(p, 0o700), f(p)))
        wrappers = c_wrapper_registry(root, work)
        home = work / "home2"
        write(home / "config.toml", f'[registry]\nindexes = ["{wrappers}", "{args.registry}"]\n')
        env = clean_env(root, home, is_standin)
        job = work / "job2"
        write(job / "fetch_demo.cone", """
            mod fetch_demo;

            import stdio;
            import tri;

            fn main() i32 {
              stdio.print <- "triple of 14 is ";
              stdio.print <- tri.triple(14i32);
              stdio.print <- "\\n";
              0i32;
            }
            """)
        out = run(congo + ["run", "fetch_demo.cone"], job, env)
        fetched = str(home / "src")
        compiled = {line.split()[1]: line for line in out.stdout.splitlines()
                    if line.strip().startswith("Compiling")}
        from_registry = [n for n in ("libc", "core", "stdio", "tri")
                         if n in compiled and fetched.lower() in compiled[n].lower()]
        if (out.returncode == 0 and out.stdout.rstrip().endswith("triple of 14 is 42")
                and from_registry == ["libc", "core", "stdio", "tri"]):
            report("FETCH", "PASS", "libc, core, stdio and tri fetched by name, built and linked")
        else:
            report("FETCH", "FAIL", f"fetched from the registries: {from_registry}; "
                   + (out.stderr or out.stdout).strip()[-800:])

        # 3. NEEDS-REGISTRY: core really comes from the registry
        home = work / "home3"
        write(home / "config.toml", f'[registry]\nindexes = ["{wrappers}", "{work.as_uri()}/no-such-registry.git"]\n')
        env = clean_env(root, home, is_standin)
        out = run(congo + ["run", "fetch_demo.cone"], job, env)
        if out.returncode != 0 and "no-such-registry" in out.stderr:
            report("NEEDS-REGISTRY", "PASS", "without the real registry the build fails, naming it")
        else:
            report("NEEDS-REGISTRY", "FAIL", (out.stderr or out.stdout).strip()[-600:])
    finally:
        if args.keep:
            print(f"exit_bar: kept {work}")
        else:
            shutil.rmtree(work, onerror=lambda f, p, e: (os.chmod(p, 0o700), f(p)), ignore_errors=True)

    print("exit_bar: " + "; ".join(f"{p} {o}" for p, o, _ in results))
    if args.standin:
        print("exit_bar: this was the stand-in rehearsal, not the archive test")
    return 0 if all(o in ("PASS", "SKIPPED") for _, o, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())
