#!/usr/bin/env python3
"""Fetch the prebuilt C libraries Cone's packages bind, pinned and verified.

    python tools/deps/fetch.py            fetch every dependency below
    python tools/deps/fetch.py openssl    only that one

Each goes in its own folder of the fetched-dependencies folder, deps/ at the
repository's root (CONE_DEPS names another), which git ignores: the binaries
are never committed. Congo looks there for a runtime DLL a package's
[link] runtime names (tools/congo/README.md, "Runtime libraries").

OPENSSL 3.5.9, from CPython's own pinned binaries: the cpython-bin-deps
repository (github.com/python/cpython-bin-deps), tag openssl-bin-3.5.9, folder
amd64/, MSVC-built, the DLLs signed by the Python Software Foundation,
Apache-2.0. The DLLs, their import libraries and the licence are fetched,
about 9 MB; the PDBs and the C headers are not (Cone binds C by hand, and
compiles no C). They land in deps/openssl/:

    libssl-3.dll  libcrypto-3.dll      the runtime, beside every program
    libssl.lib    libcrypto.lib        the import libraries, for the link
    LICENSE.txt

Each file is downloaded from the tag, checked against its size and SHA-256
pinned below, and only then put in place; a file already there whose hash is
right is not downloaded again. On Windows each DLL's Authenticode signature
must also be valid and its signer the Python Software Foundation (checked
through PowerShell's Get-AuthenticodeSignature). A file that fails a check is
not kept, and the script exits 1.

To move to another tag: change the tag, fetch the files by hand, and pin their
new sizes and hashes, checking each against the git blob SHA-1 the tag's tree
lists (gh api repos/python/cpython-bin-deps/git/trees/<tag>?recursive=1).

Python 3.11 or later, standard library only.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
IS_WINDOWS = os.name == "nt"


@dataclass(frozen=True)
class Pinned:
    name: str           # the file, in the source folder and in the dependency's folder
    size: int
    sha256: str
    signed: bool = False    # an Authenticode-signed DLL


@dataclass(frozen=True)
class Dependency:
    folder: str         # its folder in deps/
    what: str
    url: str            # the source folder; each file's name is appended
    signer: str         # the Authenticode signer every signed file must have
    files: tuple[Pinned, ...]


DEPENDENCIES = (
    Dependency(
        "openssl",
        "OpenSSL 3.5.9 (cpython-bin-deps, tag openssl-bin-3.5.9, amd64)",
        # The tag pointed at commit ddb46a415b4995440723183e644f8d2c346567a2
        # when these were pinned, on 3 Oct 2026
        "https://raw.githubusercontent.com/python/cpython-bin-deps/openssl-bin-3.5.9/amd64/",
        "Python Software Foundation",
        (
            Pinned("libssl-3.dll", 1308920,
                   "8957182a9c385bfd9f3f790921de2ddc614cf91f99edbed4e9e5446a46504e32", True),
            Pinned("libcrypto-3.dll", 6021880,
                   "75495fe0e079ab00d49be740d3c710a0b94e3f74f9648cb7cc9f768fc7f52319", True),
            Pinned("libssl.lib", 144852,
                   "01752aede9145e6acb6e24d8d173d72d960f5e3d0d89b3618b24a8b64bf9ec7c"),
            Pinned("libcrypto.lib", 1353604,
                   "4db850f374711397dab048c4c0cf04d4efb1f96d1a56c24e42ef8aec814e87be"),
            Pinned("LICENSE.txt", 10352,
                   "ed72ce2b51ee58f117e5a021e2e04af158857f40269fbc03491f0b2a99dbcc96"),
        ),
    ),
)


class FetchError(Exception):
    """A failure, reported as one message and exit status 1."""


def deps_folder() -> Path:
    """The fetched-dependencies folder: CONE_DEPS, else deps/ in the repository.
    Congo looks in the same one."""
    named = os.environ.get("CONE_DEPS")
    return Path(named).resolve() if named else REPO / "deps"


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def matches(path: Path, pin: Pinned) -> bool:
    return path.is_file() and path.stat().st_size == pin.size and sha256_of(path) == pin.sha256


def download(url: str, into: Path) -> None:
    try:
        with urllib.request.urlopen(url, timeout=60) as response, into.open("wb") as f:
            shutil.copyfileobj(response, f)
    except OSError as exc:
        into.unlink(missing_ok=True)
        raise FetchError(f"cannot download {url}: {exc}") from None


def signer_of(path: Path) -> tuple[str, str] | None:
    """(status, signer subject) of a file's Authenticode signature, through
    PowerShell; None where PowerShell cannot be run."""
    script = ("$s = Get-AuthenticodeSignature -LiteralPath $env:FETCH_FILE; "
              "$s.Status.ToString(); "
              "if ($s.SignerCertificate) { $s.SignerCertificate.Subject } else { '' }")
    # Windows PowerShell finds its own modules only by its own PSModulePath:
    # one inherited from PowerShell 7 (run from pwsh) names pwsh's modules
    # first, and Get-AuthenticodeSignature's module then fails to load
    env = {k: v for k, v in os.environ.items() if k.upper() != "PSMODULEPATH"}
    env["FETCH_FILE"] = str(path)
    try:
        ran = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
                             env=env, capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.SubprocessError):
        return None
    lines = ran.stdout.splitlines()
    if ran.returncode != 0 or not lines:
        return None
    return lines[0].strip(), (lines[1].strip() if len(lines) > 1 else "")


def check_signature(path: Path, signer: str) -> str:
    """'' when the signature is valid and the signer's organisation is signer;
    otherwise why not. Where it cannot be checked, says so and passes: the
    pinned hash is the check that decides."""
    found = signer_of(path)
    if found is None:
        print(f"warning: {path.name}'s signature not checked: PowerShell's"
              f" Get-AuthenticodeSignature could not be run", file=sys.stderr)
        return ""
    status, subject = found
    if status != "Valid":
        return f"{path.name}: its Authenticode signature is {status}, not Valid"
    if f"O={signer}," not in subject + ",":
        return f"{path.name}: signed by {subject or 'nobody'}, not {signer}"
    return ""


def fetch(dep: Dependency, root: Path) -> None:
    folder = root / dep.folder
    folder.mkdir(parents=True, exist_ok=True)
    print(f"{dep.what}")
    for pin in dep.files:
        target = folder / pin.name
        if matches(target, pin):
            print(f"    {pin.name}: already there, verified")
            continue
        part = folder / (pin.name + ".part")
        download(dep.url + pin.name, part)
        size, digest = part.stat().st_size, sha256_of(part)
        if size != pin.size or digest != pin.sha256:
            part.unlink()
            raise FetchError(f"{dep.url + pin.name}: downloaded {size} bytes with SHA-256"
                             f" {digest}, but {pin.size} bytes with SHA-256 {pin.sha256} are"
                             f" pinned; nothing was kept")
        os.replace(part, target)
        print(f"    {pin.name}: downloaded, {size} bytes, SHA-256 verified")
    if IS_WINDOWS:
        for pin in dep.files:
            if pin.signed:
                wrong = check_signature(folder / pin.name, dep.signer)
                if wrong:
                    (folder / pin.name).unlink()
                    raise FetchError(f"{wrong}; the file was removed")
        print(f"    signatures: valid, signed by {dep.signer}")
    print(f"    in {folder}")


def main(argv: list[str]) -> int:
    if sys.version_info < (3, 11):
        print("fetch: needs Python 3.11 or later", file=sys.stderr)
        return 1
    names = {dep.folder: dep for dep in DEPENDENCIES}
    wanted = argv or list(names)
    for name in wanted:
        if name not in names:
            print(f"fetch: error: no dependency '{name}'; the dependencies are"
                  f" {', '.join(names)}", file=sys.stderr)
            return 1
    root = deps_folder()
    try:
        for name in wanted:
            fetch(names[name], root)
    except FetchError as exc:
        print(f"fetch: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
