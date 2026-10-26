#!/usr/bin/env python3
"""Fetch ngtcp2's release source, pinned and verified, and build it: the QUIC
library package quic binds, with its OpenSSL 3.5 crypto helper.

    python tools/deps/build_ngtcp2.py                      Windows: MSVC, tls's OpenSSL
    python tools/deps/build_ngtcp2.py --work C:\\src\\zz     where the source and build go
    python3 tools/deps/build_ngtcp2.py --openssl ~/ossl35  Linux: gcc, an OpenSSL 3.5 install

NGTCP2 1.22.1, from its official GitHub release (github.com/ngtcp2/ngtcp2,
tag v1.22.1, 16 Apr 2026): ngtcp2-1.22.1.tar.gz, checked against the size and
SHA-256 pinned below, and its detached signature (.asc) checked by gpg where
gpg can be found (Git for Windows has one): the release is signed by Tatsuhiro
Tsujikawa's signing subkey, whose fingerprint is pinned below, under the
primary key also pinned. The public key is fetched from his GitHub account
(github.com/tatsuhiro-t.gpg) into a keyring of the build's own, never the
user's; a good signature by any other key is refused. Where gpg cannot be
run, the script says so and goes on: the pinned hash is the check that decides.
MIT licence (COPYING, copied into the output).

Why 1.22.1 and not the latest: from 1.23.0 ngtcp2 picks its min, max and
format helpers by C11 _Generic over char, signed char and unsigned char, which
MSVC before 19.44 (Visual Studio 2022 17.14) wrongly refuses as one type
(error C7700); this machine's MSVC is 19.42. 1.22.1 is the last release
without them, and has the OpenSSL 3.5 crypto helper (crypto/ossl). Moving to
the latest needs MSVC 19.44 or later: change VERSION, SIZE, SHA256 and run it.

THE BUILD. CMake and Ninja, Release, shared libraries only, the library and
its crypto helper only (no examples, no tests, no nghttp3):

    -DCMAKE_BUILD_TYPE=Release -DENABLE_LIB_ONLY=ON -DENABLE_SHARED_LIB=ON
    -DENABLE_STATIC_LIB=OFF -DBUILD_TESTING=OFF -DENABLE_OPENSSL=ON
    -DOPENSSL_ROOT_DIR=... -DOPENSSL_INCLUDE_DIR=... -DOPENSSL_SSL_LIBRARY=...
    -DOPENSSL_CRYPTO_LIBRARY=...

ENABLE_OPENSSL with an OpenSSL that has SSL_set_quic_tls_cbs (3.5's own QUIC
TLS interface) builds the 'ossl' helper, libngtcp2_crypto_ossl; the quictls
helper is left out because that OpenSSL lacks quictls's interface.

On Windows: MSVC (Visual Studio 2022's x64 tools, found by vswhere, or the
environment's when it already is an x64 one), against tls's OpenSSL 3.5.9 in
deps/openssl/ and its headers in deps/openssl/include/ (fetch.py's 'openssl'
and 'openssl-headers', fetched first if missing). It puts into deps/ngtcp2/:

    ngtcp2.dll  ngtcp2_crypto_ossl.dll      the runtime, beside every program
    ngtcp2.lib  ngtcp2_crypto_ossl.lib      the import libraries, for the link
    COPYING  BUILD.txt                      ngtcp2's licence; how it was built

On Linux: the system's cc, against the OpenSSL 3.5 installed under --openssl
(Ubuntu 24.04's is 3.0, which has no QUIC TLS interface). Into deps/ngtcp2/
go libngtcp2.so*, libngtcp2_crypto_ossl.so* and that OpenSSL's libssl.so*
and libcrypto.so*, so the one folder is both the link's and the loader's
(LD_LIBRARY_PATH).

The source and the build tree go in --work (default build/ngtcp2/ in the
repository, which git ignores). Python 3.11 or later, standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fetch  # noqa: E402  (tls's OpenSSL, beside this script)

VERSION = "1.22.1"
ARCHIVE = f"ngtcp2-{VERSION}.tar.gz"
URL = f"https://github.com/ngtcp2/ngtcp2/releases/download/v{VERSION}/"
SIZE = 1217378
SHA256 = "063d80531acac0ddbbc1b9d12829a824edc2abe8dba2e632fd1ce15cfd5632f9"

# The release's signer: Tatsuhiro Tsujikawa's signing subkey, under his primary
SIGNER_KEY_URL = "https://github.com/tatsuhiro-t.gpg"
SIGNING_SUBKEY = "516B622918D15C478AB1EA3A5339A2BE82E07DEC"
PRIMARY_KEY = "F4F3B91474D1EB29889BD0EF7E8403D5D673C366"

CMAKE_FLAGS = [
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DENABLE_LIB_ONLY=ON",
    "-DENABLE_SHARED_LIB=ON",
    "-DENABLE_STATIC_LIB=OFF",
    "-DBUILD_TESTING=OFF",
    "-DENABLE_OPENSSL=ON",
]

IS_WINDOWS = os.name == "nt"


class BuildError(Exception):
    """A failure, reported as one message and exit status 1."""


def download(url: str, into: Path) -> None:
    try:
        with urllib.request.urlopen(url, timeout=120) as response, into.open("wb") as f:
            shutil.copyfileobj(response, f)
    except OSError as exc:
        into.unlink(missing_ok=True)
        raise BuildError(f"cannot download {url}: {exc}") from None


def fetch_source(work: Path) -> Path:
    """The release archive in work, its size and SHA-256 checked."""
    archive = work / ARCHIVE
    if archive.is_file() and archive.stat().st_size == SIZE and fetch.sha256_of(archive) == SHA256:
        print(f"    {ARCHIVE}: already there, verified")
        return archive
    part = work / (ARCHIVE + ".part")
    download(URL + ARCHIVE, part)
    size, digest = part.stat().st_size, fetch.sha256_of(part)
    if size != SIZE or digest != SHA256:
        part.unlink()
        raise BuildError(f"{URL + ARCHIVE}: downloaded {size} bytes with SHA-256 {digest},"
                         f" but {SIZE} bytes with SHA-256 {SHA256} are pinned; nothing was kept")
    os.replace(part, archive)
    print(f"    {ARCHIVE}: downloaded, {size} bytes, SHA-256 verified")
    return archive


def find_gpg() -> str | None:
    found = shutil.which("gpg")
    if found:
        return found
    if IS_WINDOWS:
        for root in (os.environ.get("ProgramFiles", r"C:\Program Files"),
                     os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")):
            candidate = Path(root) / "Git" / "usr" / "bin" / "gpg.exe"
            if candidate.is_file():
                return str(candidate)
    return None


def check_signature(work: Path, archive: Path) -> None:
    """The archive's .asc a good signature by the pinned subkey, or a BuildError;
    where gpg cannot be run, a warning."""
    gpg = find_gpg()
    if gpg is None:
        print(f"warning: {ARCHIVE}'s signature not checked: no gpg found", file=sys.stderr)
        return
    asc = work / (ARCHIVE + ".asc")
    if not asc.is_file():
        download(URL + ARCHIVE + ".asc", asc)
    key = work / "signer.gpg"
    if not key.is_file():
        download(SIGNER_KEY_URL, key)
    home = work / "gnupg"
    home.mkdir(mode=0o700, exist_ok=True)
    # The keyring the build's own, and every file, named relative to work,
    # gpg's folder when it runs: Git for Windows' gpg (an MSYS program) reads
    # neither a GNUPGHOME nor a --homedir holding a drive letter
    homedir = ["--homedir", home.name]
    try:
        subprocess.run([gpg, *homedir, "--batch", "--quiet", "--import", key.name],
                       cwd=work, capture_output=True, text=True, timeout=120)
        ran = subprocess.run([gpg, *homedir, "--batch", "--status-fd", "1", "--verify", asc.name, archive.name],
                             cwd=work, capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.SubprocessError) as exc:
        print(f"warning: {ARCHIVE}'s signature not checked: gpg could not be run ({exc})", file=sys.stderr)
        return
    finally:
        # Its agents (keyboxd, gpg-agent) belong to this keyring alone
        gpgconf = Path(gpg).with_name("gpgconf.exe" if IS_WINDOWS else "gpgconf")
        if gpgconf.is_file():
            subprocess.run([str(gpgconf), *homedir, "--kill", "all"], cwd=work, capture_output=True, timeout=60)
    for line in ran.stdout.splitlines():
        parts = line.split()
        # [GNUPG:] VALIDSIG <signing key fpr> <date> ... <primary key fpr>
        if len(parts) >= 3 and parts[1] == "VALIDSIG":
            if parts[2] == SIGNING_SUBKEY and parts[-1] == PRIMARY_KEY:
                print(f"    {ARCHIVE}.asc: good signature, subkey {SIGNING_SUBKEY}")
                return
            raise BuildError(f"{ARCHIVE}.asc is signed by {parts[2]} (primary {parts[-1]}),"
                             f" not the pinned {SIGNING_SUBKEY} (primary {PRIMARY_KEY})")
    raise BuildError(f"{ARCHIVE}.asc: no good signature: {(ran.stdout + ran.stderr).strip()}")


def extract(archive: Path, work: Path) -> Path:
    source = work / f"ngtcp2-{VERSION}"
    if not (source / "CMakeLists.txt").is_file():
        with tarfile.open(archive, "r:gz") as tar:
            tar.extractall(work, filter="data")
    return source


def vs_environment() -> dict[str, str]:
    """The environment of Visual Studio's x64 tools: this one when it already
    is, else vcvars64.bat's, found by vswhere."""
    if os.environ.get("VSCMD_ARG_TGT_ARCH") == "x64" and shutil.which("cl"):
        return dict(os.environ)
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
        "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.is_file():
        raise BuildError(f"no vswhere.exe at {vswhere}: is Visual Studio 2022 installed?")
    found = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires",
                            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                            "-property", "installationPath"], capture_output=True, text=True)
    vcvars = Path(found.stdout.strip()) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    if not vcvars.is_file():
        raise BuildError(f"no vcvars64.bat under {found.stdout.strip() or 'any Visual Studio'}")
    ran = subprocess.run(f'"{vcvars}" >nul && set', shell=True, capture_output=True, text=True)
    env = {}
    for line in ran.stdout.splitlines():
        name, eq, value = line.partition("=")
        if eq:
            env[name] = value
    if "VCToolsVersion" not in env:
        raise BuildError(f"{vcvars} did not set up the x64 tools")
    return env


def tool(name: str, env: dict[str, str]) -> str:
    """A program's path, found on env's PATH (Windows looks for a program on
    the caller's own PATH, not the one handed to the child)."""
    path = next((v for k, v in env.items() if k.upper() == "PATH"), None)
    return shutil.which(name, path=path) or name


def run(cmd: list[str], env: dict[str, str]) -> None:
    print("    " + " ".join(cmd))
    if subprocess.run([tool(cmd[0], env), *cmd[1:]], env=env).returncode != 0:
        raise BuildError(f"{cmd[0]} failed")


def build(source: Path, work: Path, openssl: dict[str, Path], env: dict[str, str]) -> Path:
    tree = work / "build"
    run(["cmake", "-S", str(source), "-B", str(tree), *CMAKE_FLAGS,
         f"-DOPENSSL_ROOT_DIR={openssl['root']}",
         f"-DOPENSSL_INCLUDE_DIR={openssl['include']}",
         f"-DOPENSSL_SSL_LIBRARY={openssl['ssl']}",
         f"-DOPENSSL_CRYPTO_LIBRARY={openssl['crypto']}"], env)
    run(["cmake", "--build", str(tree)], env)
    return tree


def compiler_line(env: dict[str, str]) -> str:
    if IS_WINDOWS:
        ran = subprocess.run([tool("cl", env)], env=env, capture_output=True, text=True)
        return (ran.stderr.splitlines() or ["cl"])[0].strip()
    ran = subprocess.run([tool("cc", env), "--version"], env=env, capture_output=True, text=True)
    return (ran.stdout.splitlines() or ["cc"])[0].strip()


def main(argv: list[str]) -> int:
    if sys.version_info < (3, 11):
        print("build_ngtcp2: needs Python 3.11 or later", file=sys.stderr)
        return 1
    p = argparse.ArgumentParser(description="Fetch and build ngtcp2 for package quic")
    p.add_argument("--work", default=str(fetch.REPO / "build" / "ngtcp2"),
                   help="where the source and the build tree go")
    p.add_argument("--openssl", help="Linux: the prefix an OpenSSL 3.5 is installed under")
    args = p.parse_args(argv)
    work = Path(args.work).expanduser().resolve()
    work.mkdir(parents=True, exist_ok=True)
    deps = fetch.deps_folder()
    out = deps / "ngtcp2"
    try:
        print(f"ngtcp2 {VERSION} (github.com/ngtcp2/ngtcp2, release v{VERSION})")
        archive = fetch_source(work)
        check_signature(work, archive)
        source = extract(archive, work)
        if IS_WINDOWS:
            names = {dep.name: dep for dep in fetch.DEPENDENCIES}
            fetch.fetch(names["openssl"], deps)
            fetch.fetch(names["openssl-headers"], deps)
            ossl = deps / "openssl"
            openssl = {"root": ossl, "include": ossl / "include", "ssl": ossl / "libssl.lib",
                       "crypto": ossl / "libcrypto.lib"}
            env = vs_environment()
        else:
            if not args.openssl:
                raise BuildError("on Linux, --openssl names the prefix an OpenSSL 3.5 is installed under")
            prefix = Path(args.openssl).expanduser().resolve()
            libdir = next((prefix / d for d in ("lib64", "lib") if (prefix / d / "libssl.so").exists()), None)
            if libdir is None:
                raise BuildError(f"no libssl.so under {prefix}/lib64 or {prefix}/lib")
            openssl = {"root": prefix, "include": prefix / "include", "ssl": libdir / "libssl.so",
                       "crypto": libdir / "libcrypto.so"}
            env = dict(os.environ)
        tree = build(source, work, openssl, env)
        out.mkdir(parents=True, exist_ok=True)
        if IS_WINDOWS:
            made = [tree / "lib" / "ngtcp2.dll", tree / "lib" / "ngtcp2.lib",
                    tree / "crypto" / "ossl" / "ngtcp2_crypto_ossl.dll",
                    tree / "crypto" / "ossl" / "ngtcp2_crypto_ossl.lib"]
        else:
            made = sorted((tree / "lib").glob("libngtcp2.so*")) + \
                sorted((tree / "crypto" / "ossl").glob("libngtcp2_crypto_ossl.so*")) + \
                sorted(Path(openssl["ssl"]).parent.glob("libssl.so*")) + \
                sorted(Path(openssl["ssl"]).parent.glob("libcrypto.so*"))
        for f in made + [source / "COPYING"]:
            if not f.exists():
                raise BuildError(f"the build made no {f}")
            shutil.copy2(f, out / f.name, follow_symlinks=False)
        (out / "BUILD.txt").write_text(
            f"ngtcp2 {VERSION}, {ARCHIVE} SHA-256 {SHA256}\n"
            f"built by tools/deps/build_ngtcp2.py with {compiler_line(env)}\n"
            f"cmake {' '.join(CMAKE_FLAGS)}\n"
            f"against OpenSSL: {openssl['include']}, {openssl['ssl']}, {openssl['crypto']}\n",
            encoding="utf-8")
        print(f"    in {out}")
    except BuildError as exc:
        print(f"build_ngtcp2: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
