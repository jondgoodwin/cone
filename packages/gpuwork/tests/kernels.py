#!/usr/bin/env python3
"""Compile the Cone compute kernels of gpuwork's tests/kernels.cone to SPIR-V
and embed the SPIR-V in that same file.

    python tests/kernels.py            compile the kernels, then embed them
    python tests/kernels.py --check    check that the embedded SPIR-V is current

kernels.cone is a test program that runs Cone kernels on the GPU through
gpuwork and checks them against the same functions compiled for the CPU, its
twins. One source makes both: the kernels are written once, in kernels.cone,
between two marker lines,

    // cone-kernels-begin
    ... structs, '@compute' functions, and functions they call ...
    // cone-kernels-end

and the test program compiles them for the CPU as part of itself. This tool
compiles the same lines for Vulkan, with the repository's conec:

    conec --triple=spirv1.6-unknown-vulkan1.3 -o <tmp> <tmp>/kernels/kernels.cone

where <tmp>/kernels/kernels.cone is 'mod kernels;', blank lines, and the
block, each of its lines on the line it has in the test, so that a kernel's
failure on the GPU names the test's own file and line ('cone.file 1
kernels.cone' in the module, and the line the error buffer records). The
block may use only what core gives every module: it is compiled alone. The
module is checked with spirv-val (--target-env vulkan1.3) where the Vulkan SDK
has it, and its words written between

    // cone-spirv-begin KERNELS_SPIRV
    ... written by this tool: do not edit ...
    // cone-spirv-end

as tools/shaders/shaders.py embeds a Slang shader's (its markers are its own,
so that shaders.py, which wants a .slang beside each .spv, passes them by).
The block's first line records the SHA-256 of the kernels' lines (their line
ends made LF) and of where they start.

WHAT --check CHECKS: that the hash recorded is that of the kernels as they
are now (kernels edited and not recompiled fail), and, where conec is found,
that compiling them again makes exactly the words embedded.

The compiler is CONEC if set, else the repository's build/x64-release/conec.exe;
it is warned about if older than the compiler's sources (a direct conec run
does not check, as congo and test/run.py do).

This is a test-local step until congo builds a package's GPU kernels itself.
Python 3.11 or later, standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TEST = HERE / "kernels.cone"
REPO = HERE.parent.parent.parent
MODULE = "kernels"
NAME = "KERNELS_SPIRV"
TRIPLE = "spirv1.6-unknown-vulkan1.3"

KBEGIN = re.compile(r"^// cone-kernels-begin[ \t]*\r?$", re.M)
KEND = re.compile(r"^// cone-kernels-end[ \t]*\r?$", re.M)
SBEGIN = re.compile(r"^// cone-spirv-begin (\w+)[ \t]*\r?$", re.M)
SEND = re.compile(r"^// cone-spirv-end[ \t]*\r?$", re.M)
HASH = re.compile(r"sha256 ([0-9a-f]{64})")
WORD = re.compile(r"0x([0-9A-Fa-f]{8})u32")


class KernelError(Exception):
    pass


def find_conec() -> Path:
    named = os.environ.get("CONEC")
    if named:
        return Path(named)
    built = REPO / "build" / "x64-release" / ("conec.exe" if os.name == "nt" else "conec")
    if built.is_file():
        return built
    found = shutil.which("conec")
    if found:
        return Path(found)
    raise KernelError(f"no conec: set CONEC, or build it at {built}")


def warn_if_stale(conec: Path) -> None:
    stamp = conec.stat().st_mtime
    for path in (REPO / "compiler" / "c").rglob("*"):
        if path.suffix in (".c", ".h") and path.stat().st_mtime > stamp:
            print(f"kernels: warning: {conec} is older than {path.relative_to(REPO)}; rebuild it",
                  file=sys.stderr)
            return


def find_spirv_val() -> Path | None:
    sdk = os.environ.get("VULKAN_SDK")
    if sdk:
        for name in ("spirv-val.exe", "spirv-val"):
            p = Path(sdk) / "Bin" / name
            if p.is_file():
                return p
    found = shutil.which("spirv-val")
    return Path(found) if found else None


def kernels_of(text: str) -> tuple[int, list[str]]:
    """The kernels' block: the line number of its first line, and its lines."""
    b = KBEGIN.search(text)
    e = KEND.search(text, b.end()) if b else None
    if not b or not e:
        raise KernelError(f"{TEST.name} has no '// cone-kernels-begin' ... '// cone-kernels-end' block")
    first = text.count("\n", 0, b.end()) + 2
    body = text[b.end():e.start()].replace("\r\n", "\n")
    lines = body.split("\n")[1:-1]
    return first, lines


def kernels_hash(first: int, lines: list[str]) -> str:
    data = f"first line {first}\n".encode("utf-8") + "\n".join(lines).encode("utf-8")
    return hashlib.sha256(data).hexdigest()


def compile_kernels(first: int, lines: list[str]) -> list[int]:
    conec = find_conec()
    warn_if_stale(conec)
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp) / MODULE
        folder.mkdir()
        source = folder / f"{MODULE}.cone"
        # 'mod kernels;' on line 1, the block's lines on their own
        text = [f"mod {MODULE};"] + [""] * (first - 2) + lines
        source.write_text("\n".join(text) + "\n", encoding="utf-8", newline="\n")
        result = subprocess.run([str(conec), f"--triple={TRIPLE}", "-o", tmp, str(source)],
                                capture_output=True, text=True)
        spv = Path(tmp) / f"{MODULE}.spv"
        if result.returncode != 0 or not spv.is_file():
            raise KernelError(f"conec failed ({result.returncode:#x}) on the kernels:\n"
                              f"{result.stdout}{result.stderr}")
        val = find_spirv_val()
        if val:
            check = subprocess.run([str(val), "--target-env", "vulkan1.3", str(spv)],
                                   capture_output=True, text=True)
            if check.returncode != 0:
                raise KernelError(f"spirv-val refuses the kernels' module:\n{check.stdout}{check.stderr}")
        data = spv.read_bytes()
    if len(data) % 4 != 0 or len(data) < 20:
        raise KernelError(f"conec's module is not SPIR-V: {len(data)} bytes")
    words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
    if words[0] != 0x07230203:
        raise KernelError(f"conec's module is not SPIR-V: its magic number is {words[0]:#010x}")
    return words


def spirv_block(text: str):
    b = SBEGIN.search(text)
    e = SEND.search(text, b.end()) if b else None
    if not b or not e or b.group(1) != NAME:
        raise KernelError(f"{TEST.name} has no '// cone-spirv-begin {NAME}' ... '// cone-spirv-end' block")
    return b, e


def block_text(words: list[int], digest: str, nl: str) -> str:
    lines = [f"// Written by tests/kernels.py from the kernels above (sha256 {digest}): do not edit.",
             f"imm {NAME} Array[u32, {len(words)}] = ["]
    for i in range(0, len(words), 6):
        lines.append("  " + ", ".join(f"0x{w:08x}u32" for w in words[i:i + 6])
                     + ("," if i + 6 < len(words) else ""))
    lines.append("];")
    return nl.join(lines) + nl


def main() -> int:
    ap = argparse.ArgumentParser(description="Compile tests/kernels.cone's Cone kernels and embed them.")
    ap.add_argument("--check", action="store_true", help="check that the embedded SPIR-V is current")
    args = ap.parse_args()
    try:
        raw = TEST.read_bytes().decode("utf-8")
        nl = "\r\n" if "\r\n" in raw else "\n"
        first, lines = kernels_of(raw)
        digest = kernels_hash(first, lines)
        b, e = spirv_block(raw)
        if args.check:
            body = raw[b.end():e.start()]
            problems = []
            h = HASH.search(body)
            if not h or h.group(1) != digest:
                problems.append("the kernels have changed since they were compiled; run tests/kernels.py")
            try:
                again = compile_kernels(first, lines)
                if [int(w, 16) for w in WORD.findall(body)] != again:
                    problems.append("the words embedded are not what conec makes of the kernels now")
                how = "compiled again and compared"
            except KernelError as err:
                how = f"not compiled again: {err}"
            for p in problems:
                print(f"kernels: {p}")
            print(f"kernels: {TEST.name} checked ({how}); "
                  + ("current" if not problems else f"{len(problems)} problems"))
            return 1 if problems else 0
        words = compile_kernels(first, lines)
        start = raw.index("\n", b.start()) + 1
        new = raw[:start] + block_text(words, digest, nl) + raw[e.start():]
        if new != raw:
            TEST.write_bytes(new.encode("utf-8"))
            print(f"kernels: embedded {len(words)} words in {TEST.name}")
        else:
            print(f"kernels: {TEST.name} is current")
        return 0
    except KernelError as err:
        print(f"kernels: error: {err}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
