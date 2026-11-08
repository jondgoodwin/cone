#!/usr/bin/env python3
"""Smoke-test a Cone archive ON THE ARCHIVE ITSELF, in a clean environment.

    python tools/release/smoke.py windows cone-<hash8>-windows-x64.zip [--install-script install.ps1]
    python3 tools/release/smoke.py linux  cone-<hash8>-linux-x86_64.tar.gz [--install-script install.sh]

The archive is unpacked into a new, empty folder (with --install-script, by
running that generated script on the archive, as a user would), and everything
after that runs with only what the archive holds:

  * a minimal environment: on Windows the system folders and nothing else on PATH
    (no repository, no Python, no Git), a new user profile, and no CONEC,
    CONE_PACKAGES, CONE_DEPS, LIB, INCLUDE or CONGO_HOME; on Linux /usr/bin and /bin
    and a new HOME;
  * `congo new hello` then `congo run` must print the greeting;
  * an actor program of the archive's own packages (actors/tests/defaults.cone)
    must print what its .out holds;
  * on Windows, the sdl and tls tests, which link the SDL3 and OpenSSL files that
    travel inside those packages.

It prints a transcript and exits 1 at the first failure. Programs run through
congo, which turns a missing-DLL dialog into an exit status; nothing opens a
window.

Python 3.11 or later, standard library only (this script; the archive brings its
own for Congo on Windows).
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path

IS_WINDOWS = os.name == "nt"


class Failure(Exception):
    pass


def say(text: str) -> None:
    print(f"smoke: {text}", flush=True)


def clean_env(profile: Path, path_entries: list[str]) -> dict[str, str]:
    """An environment with nothing of the developer's machine in it."""
    if IS_WINDOWS:
        sysroot = os.environ["SystemRoot"]
        keep = ("SystemRoot", "windir", "ComSpec", "PATHEXT", "OS", "PROCESSOR_ARCHITECTURE",
                "PROCESSOR_IDENTIFIER", "PROCESSOR_LEVEL", "PROCESSOR_REVISION",
                "NUMBER_OF_PROCESSORS", "ProgramFiles", "ProgramFiles(x86)", "ProgramW6432",
                "CommonProgramFiles", "CommonProgramFiles(x86)", "CommonProgramW6432",
                "ProgramData", "SystemDrive", "COMPUTERNAME", "USERNAME", "ALLUSERSPROFILE",
                "PUBLIC", "DriverData")
        env = {k: os.environ[k] for k in keep if k in os.environ}
        local = profile / "AppData" / "Local"
        roaming = profile / "AppData" / "Roaming"
        temp = local / "Temp"
        for folder in (temp, roaming):
            folder.mkdir(parents=True, exist_ok=True)
        env.update(USERPROFILE=str(profile), HOMEDRIVE=profile.drive,
                   HOMEPATH=str(profile)[len(profile.drive):], APPDATA=str(roaming),
                   LOCALAPPDATA=str(local), TEMP=str(temp), TMP=str(temp))
        system32 = Path(sysroot) / "System32"
        env["PATH"] = os.pathsep.join(
            [str(system32), sysroot, str(system32 / "Wbem"),
             str(system32 / "WindowsPowerShell" / "v1.0"), *path_entries])
        return env
    profile.mkdir(parents=True, exist_ok=True)
    return {"HOME": str(profile), "TMPDIR": str(profile),
            "PATH": os.pathsep.join(["/usr/bin", "/bin", *path_entries]),
            "LANG": "C.UTF-8"}


def sh(command: str | list[str], env: dict, cwd: Path, expect: str | None = None,
       timeout: int = 900) -> str:
    """Run a command (a string goes to cmd /c or sh -c), echo it and its output, fail
    on a non-zero status or when `expect` is not in the output."""
    shown = command if isinstance(command, str) else " ".join(command)
    say(f"$ {shown}")
    if isinstance(command, str):
        command = ["cmd", "/c", command] if IS_WINDOWS else ["sh", "-c", command]
    result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, timeout=timeout)
    out = (result.stdout + result.stderr).decode("utf-8", "replace").replace("\r\n", "\n")
    for line in out.rstrip().splitlines()[-25:]:
        print(f"      {line}")
    if result.returncode != 0:
        raise Failure(f"exit status {result.returncode}: {shown}")
    if expect is not None and expect not in out:
        raise Failure(f"{shown}: expected {expect!r} in the output")
    return out


def unpack(archive: Path, dest: Path) -> Path:
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as zf:
            zf.extractall(dest)
    else:
        with tarfile.open(archive) as tar:
            tar.extractall(dest, filter="tar")
    return dest / "cone"


def run_smoke(kind: str, archive: Path, install_script: Path | None) -> None:
    # A space in the folder's name, as in many users' profile folders
    work = Path(tempfile.mkdtemp(prefix="cone smoke "))
    say(f"clean folder {work}")
    profile = work / "profile"
    if install_script is None:
        root = unpack(archive, work / "unpacked")
    else:
        root = work / "installed"
        if IS_WINDOWS:
            env = clean_env(profile, [])
            env.update(CONE_HOME=str(root), CONE_NO_PATH="1", CONE_ARCHIVE=str(archive))
            # Windows PowerShell 5.1, which every Windows has
            sh(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
                f"Get-Content -Raw '{install_script}' | Invoke-Expression"],
               env, work, expect="SHA-256 verified")
        else:
            env = clean_env(profile, [])
            env.update(CONE_HOME=str(root), CONE_BIN=str(work / "linkbin"), CONE_ARCHIVE=str(archive))
            sh(["sh", str(install_script)], env, work, expect="SHA-256 verified")
    bin_dir = root / "bin"
    if not (bin_dir / ("conec.exe" if IS_WINDOWS else "conec")).is_file():
        raise Failure(f"no conec in {bin_dir}")

    link_dir = work / "linkbin"
    path = [str(bin_dir)] if IS_WINDOWS or install_script is None else [str(link_dir)]
    env = clean_env(profile, path)
    prog = work / "prog"
    prog.mkdir()

    if IS_WINDOWS:
        found = subprocess.run("where python", shell=True, cwd=prog, env=env, capture_output=True)
        if found.returncode == 0:
            raise Failure("a python is on the clean PATH: " + found.stdout.decode(errors="replace"))
        say("no python on PATH (confirmed); LIB, INCLUDE, CONEC, CONE_PACKAGES, CONE_DEPS, CONGO_HOME unset")
    else:
        found = subprocess.run("command -v python3 >/dev/null", shell=True, env=env)
        if found.returncode != 0:
            raise Failure("python3 is not installed in this environment: sudo apt install python3")

    sh("congo new hello", env, prog)
    sh("congo run", env, prog / "hello", expect="Hello, world!")

    defaults = root / "packages" / "actors" / "tests"
    shutil.copy2(defaults / "defaults.cone", prog / "defaults.cone")
    expected = (defaults / "defaults.out").read_text(encoding="utf-8").replace("\r\n", "\n")
    out = sh("congo run defaults.cone", env, prog)
    got = out[out.index(expected.splitlines()[0]):] if expected.splitlines()[0] in out else out
    if expected.strip() not in got:
        raise Failure("the actor program's output is not actors/tests/defaults.out")
    say("actor sample printed what its .out holds")

    if IS_WINDOWS:
        sh("congo test events", env, root / "packages" / "sdl", timeout=1800)
        sh("congo test loopback", env, root / "packages" / "tls", timeout=1800)
    else:
        sh("congo test loopback", env, root / "packages" / "tls", timeout=1800)
    say("ALL PASSED")
    shutil.rmtree(work, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("kind", choices=("windows", "linux"))
    parser.add_argument("archive")
    parser.add_argument("--install-script", help="install through this generated script")
    args = parser.parse_args()
    if (args.kind == "windows") != IS_WINDOWS:
        print(f"smoke: a {args.kind} archive is tested on {args.kind}", file=sys.stderr)
        return 2
    try:
        run_smoke(args.kind, Path(args.archive).resolve(),
                  Path(args.install_script).resolve() if args.install_script else None)
    except Failure as exc:
        print(f"smoke: FAILED: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
