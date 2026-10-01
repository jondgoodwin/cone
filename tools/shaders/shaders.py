#!/usr/bin/env python3
"""Shaders: compile the packages' Slang shaders ahead of time to SPIR-V, and
embed the SPIR-V in the Cone source that draws with it.

    python tools/shaders/shaders.py            compile every .slang, then embed
    python tools/shaders/shaders.py --check    check that everything is current
    python tools/shaders/shaders.py <folder>   the same, below <folder> only

The default folder is the repository's packages/.

The packages' shaders are written in Slang and compiled ahead of time. A
shader's source is a .slang file in the package
that owns it, beside the Cone file that uses it (an example's or a test's in
examples/ or tests/, a package's own in src/). This tool compiles each
<name>.slang to <name>.spv beside it, with the Vulkan SDK's slangc:

    slangc <name>.slang -target spirv -profile spirv_1_5
           -matrix-layout-column-major -fvk-use-entrypoint-name -o <name>.spv

- SPIR-V 1.5 is what Vulkan 1.2 and later accept; the gpu package asks for
  Vulkan 1.3.
- Column-major matrix layout is the layout geomath's Mat4 has in memory, so a
  Mat4 is copied into a uniform buffer as it is, and 'mul(m, v)' in a shader
  is 'm * v' in Cone.
- Every entry point in the file ('[shader("vertex")] vertexMain', say) is
  kept, under its own name, in one module: a pipeline names the entry point
  of each stage. A compute entry point ('[shader("compute")]') is one more,
  its workgroup given by '[numthreads(x, y, z)]'; slangc finds each entry
  point's stage from its '[shader(...)]'.

Both the .slang and the .spv are committed, so that a machine without the SDK
builds and tests everything; only changing a shader needs slangc (found
through VULKAN_SDK, else on PATH).

CONE LOADS THE BYTES BY EMBEDDING THEM. A Cone file that draws with a shader
holds its SPIR-V as a global array of 32-bit words, between two marker lines
this tool writes between (the first names the .spv, relative to the Cone
file, and the global):

    // spirv-begin triangle.spv TRIANGLE_SPIRV
    ... written by this tool: do not edit ...
    // spirv-end

and passes it to 'device.createShaderModule(&TRIANGLE_SPIRV)'. Embedding
needs no file found at run time (a test runs in its own build folder, an
example wherever it is started), and is what 'glslc -mfmt=c' does for C. A
package is one Cone file, so a package's shaders are embedded in its
src/<name>.cone.

WHAT --check CHECKS, with no SDK needed: that every .slang has its .spv; that
every .spv is embedded somewhere, with the SHA-256 of its .slang (its line
ends made LF, as git may check it out either way) recorded in the block equal
to that of the .slang as it is now, and the words in the block equal to the
.spv's bytes. So a .slang edited and not recompiled, or a .spv recompiled and
not embedded, fails. Where slangc is found, it also compiles each .slang
again and checks the result is byte for byte the committed .spv.

MODULES. A .slang with no entry point (no '[shader(...)]') is a module:
shaders import it by name ('import noise;') and it is not compiled alone,
so it has no .spv. Its name is found beside the importing file, then in
each package's src/ folder (every one is passed to slangc with -I). A
shader's hash covers the modules it imports, and theirs, as well as itself,
so a module edited and its importers not recompiled fails --check.

EXTRA ARGUMENTS. A line '// slangc: <arguments>' in a shader adds those
arguments to its compile (e.g. '-fp-mode precise', which marks every float
operation NoContraction; the noise package's shaders need it).

WORKGROUPS. Every '[numthreads(x, y, z)]' in a shader must be within
WebGPU's default limits, so that the shader runs in a browser too: x and y
at most 256, z at most 64, x y z at most 256. Both compiling and --check
refuse one that is not (the gpu package refuses it again at run time, from
the SPIR-V).

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

REPO = Path(__file__).resolve().parent.parent.parent
DEFAULT_ROOT = REPO / "packages"
SLANGC_ARGS = ["-target", "spirv", "-profile", "spirv_1_5",
               "-matrix-layout-column-major", "-fvk-use-entrypoint-name"]
SKIP_DIRS = {"build", ".git"}

BEGIN = re.compile(r"^// spirv-begin (\S+) (\w+)[ \t]*\r?$", re.M)
END = re.compile(r"^// spirv-end[ \t]*\r?$", re.M)
HASH = re.compile(r"sha256 ([0-9a-f]{64})")
WORD = re.compile(r"0x([0-9A-Fa-f]{8})u32")
IMPORT = re.compile(r"^[ \t]*import[ \t]+(\w+)[ \t]*;", re.M)
ENTRY = re.compile(r"\[shader\(")
EXTRA = re.compile(r"^// slangc:[ \t]*(.+?)[ \t]*\r?$", re.M)
NUMTHREADS = re.compile(r"\[numthreads\(\s*(\d+)\s*(?:,\s*(\d+)\s*)?(?:,\s*(\d+)\s*)?\)\]")
# WebGPU's default compute limits: maxComputeWorkgroupSizeX, Y and Z, and
# maxComputeInvocationsPerWorkgroup
WORKGROUP_LIMITS = (256, 256, 64)
MAX_INVOCATIONS = 256


class ShaderError(Exception):
    pass


def find_slangc() -> Path | None:
    sdk = os.environ.get("VULKAN_SDK")
    if sdk:
        for name in ("slangc.exe", "slangc"):
            p = Path(sdk) / "Bin" / name
            if p.is_file():
                return p
    found = shutil.which("slangc")
    return Path(found) if found else None


def walk(root: Path, suffix: str) -> list[Path]:
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        out += [Path(dirpath) / f for f in sorted(filenames) if f.endswith(suffix)]
    return out


def lf_bytes(path: Path) -> bytes:
    return path.read_bytes().replace(b"\r\n", b"\n")


def module_dirs() -> list[Path]:
    """The packages' src/ folders, where an imported module is looked for."""
    return sorted(p for p in DEFAULT_ROOT.glob("*/src") if p.is_dir())


def is_module(slang: Path) -> bool:
    """Whether a .slang is a module (no entry point), compiled only by importers."""
    return not ENTRY.search(lf_bytes(slang).decode("utf-8"))


def imports_of(slang: Path) -> list[Path]:
    """Every module 'slang' imports, directly or through another, in the
    order first reached; a name found nowhere is left to slangc to report."""
    out: list[Path] = []
    todo = [slang]
    while todo:
        f = todo.pop(0)
        for name in IMPORT.findall(lf_bytes(f).decode("utf-8")):
            for d in [f.parent, *module_dirs()]:
                m = (d / f"{name}.slang").resolve()
                if m.is_file():
                    if m not in out and m != slang.resolve():
                        out.append(m)
                        todo.append(m)
                    break
    return out


def source_hash(slang: Path) -> str:
    """The SHA-256 of a .slang, its line ends made LF, followed by those of
    the modules it imports (a shader importing nothing hashes itself alone)."""
    data = lf_bytes(slang)
    for m in imports_of(slang):
        data += f"\n// import {m.name}\n".encode("utf-8") + lf_bytes(m)
    return hashlib.sha256(data).hexdigest()


def workgroup_problems(slang: Path) -> list[str]:
    """Each '[numthreads]' of 'slang' beyond WebGPU's default limits."""
    out = []
    for m in NUMTHREADS.finditer(lf_bytes(slang).decode("utf-8")):
        size = [int(g) if g else 1 for g in m.groups()]
        if any(s > lim for s, lim in zip(size, WORKGROUP_LIMITS)) or size[0] * size[1] * size[2] > MAX_INVOCATIONS:
            out.append(f"{shown(slang)}: {m.group(0)} is beyond WebGPU's limits"
                       f" ({' x '.join(map(str, WORKGROUP_LIMITS))}, {MAX_INVOCATIONS} invocations)")
    return out


def compile_slang(slangc: Path, slang: Path, spv: Path) -> None:
    extra = [a for line in EXTRA.findall(lf_bytes(slang).decode("utf-8")) for a in line.split()]
    includes = [a for d in module_dirs() for a in ("-I", str(d))]
    result = subprocess.run([str(slangc), str(slang), *SLANGC_ARGS, *extra, *includes, "-o", str(spv)],
                            capture_output=True, text=True)
    if result.returncode != 0 or not spv.is_file():
        raise ShaderError(f"slangc failed on {shown(slang)}:\n{result.stdout}{result.stderr}")


def words_of(spv: Path) -> list[int]:
    data = spv.read_bytes()
    if len(data) % 4 != 0 or len(data) < 20:
        raise ShaderError(f"{shown(spv)} is not SPIR-V: {len(data)} bytes")
    words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
    if words[0] != 0x07230203:
        raise ShaderError(f"{shown(spv)} is not SPIR-V: its magic number is {words[0]:#010x}")
    return words


def block_text(spv: Path, name: str, nl: str) -> str:
    """The lines between the markers: the embedded words of 'spv'."""
    slang = spv.with_suffix(".slang")
    words = words_of(spv)
    lines = [f"// Written by tools/shaders/shaders.py from {slang.name}"
             f" (sha256 {source_hash(slang)}): do not edit.",
             f"imm {name} Array[u32, {len(words)}] = ["]
    for i in range(0, len(words), 6):
        lines.append("  " + ", ".join(f"0x{w:08x}u32" for w in words[i:i + 6])
                     + ("," if i + 6 < len(words) else ""))
    lines.append("];")
    return nl.join(lines) + nl


def blocks(text: str):
    """Each embedded block: (begin match, end match)."""
    pos = 0
    while True:
        b = BEGIN.search(text, pos)
        if not b:
            return
        e = END.search(text, b.end())
        if not e:
            raise ShaderError(f"a '// spirv-begin {b.group(1)}' has no '// spirv-end' after it")
        yield b, e
        pos = e.end()


def embed(cone: Path) -> list[Path]:
    """Rewrite the blocks of 'cone' from their .spv files; answer the .spv
    files it embeds."""
    raw = cone.read_bytes().decode("utf-8")
    nl = "\r\n" if "\r\n" in raw else "\n"
    out, pos, used = [], 0, []
    for b, e in blocks(raw):
        spv = (cone.parent / b.group(1)).resolve()
        if not spv.is_file():
            raise ShaderError(f"{shown(cone)} embeds {b.group(1)}, which does not exist")
        start = raw.index("\n", b.start()) + 1
        out += [raw[pos:start], block_text(spv, b.group(2), nl)]
        pos = e.start()
        used.append(spv)
    out.append(raw[pos:])
    new = "".join(out)
    if new != raw:
        cone.write_bytes(new.encode("utf-8"))
        print(f"embedded {', '.join(p.name for p in used)} in {shown(cone)}")
    return used


def check_embedded(cone: Path, problems: list[str]) -> list[Path]:
    raw = cone.read_bytes().decode("utf-8")
    used = []
    for b, e in blocks(raw):
        spv = (cone.parent / b.group(1)).resolve()
        used.append(spv)
        where = f"{shown(cone)}, {b.group(2)}"
        if not spv.is_file():
            problems.append(f"{where}: {b.group(1)} does not exist")
            continue
        body = raw[b.end():e.start()]
        h = HASH.search(body)
        slang = spv.with_suffix(".slang")
        if not slang.is_file():
            problems.append(f"{where}: {shown(slang)}, its source, does not exist")
        elif not h or h.group(1) != source_hash(slang):
            imported = "" if not imports_of(slang) else " (or a module it imports)"
            problems.append(f"{where}: {slang.name}{imported} has changed since it was compiled and embedded;"
                            " run tools/shaders/shaders.py")
        if [int(w, 16) for w in WORD.findall(body)] != words_of(spv):
            problems.append(f"{where}: the words embedded are not {shown(spv)}'s;"
                            " run tools/shaders/shaders.py")
    return used


def shown(p: Path) -> str:
    try:
        return str(p.resolve().relative_to(REPO))
    except ValueError:
        return str(p)


def main() -> int:
    ap = argparse.ArgumentParser(description="Compile Slang shaders to SPIR-V and embed them in Cone source.")
    ap.add_argument("folder", nargs="?", default=str(DEFAULT_ROOT))
    ap.add_argument("--check", action="store_true", help="check that every .spv and embedding is current")
    args = ap.parse_args()
    root = Path(args.folder).resolve()
    slangs = [s for s in walk(root, ".slang") if not is_module(s)]
    cones = walk(root, ".cone")
    slangc = find_slangc()
    try:
        if args.check:
            problems: list[str] = []
            embedded: set[Path] = set()
            for cone in cones:
                embedded.update(check_embedded(cone, problems))
            with tempfile.TemporaryDirectory() as tmp:
                for slang in slangs:
                    problems += workgroup_problems(slang)
                    spv = slang.with_suffix(".spv")
                    if not spv.is_file():
                        problems.append(f"{shown(slang)} has no {spv.name}; run tools/shaders/shaders.py")
                        continue
                    if spv.resolve() not in embedded:
                        problems.append(f"{shown(spv)} is embedded in no Cone file")
                    if slangc:
                        again = Path(tmp) / spv.name
                        compile_slang(slangc, slang, again)
                        if again.read_bytes() != spv.read_bytes():
                            problems.append(f"{shown(spv)} is not what slangc makes of {slang.name} now")
            for p in problems:
                print(f"shaders: {p}")
            how = "compiled again by slangc and compared" if slangc else "not compiled again: no slangc"
            print(f"shaders: {len(slangs)} shaders checked ({how}); "
                  + ("all current" if not problems else f"{len(problems)} problems"))
            return 1 if problems else 0

        if slangs and not slangc:
            raise ShaderError("no slangc: install the Vulkan SDK (and set VULKAN_SDK) or put slangc on PATH")
        beyond = [p for slang in slangs for p in workgroup_problems(slang)]
        if beyond:
            raise ShaderError("\n".join(beyond))
        for slang in slangs:
            spv = slang.with_suffix(".spv")
            old = spv.read_bytes() if spv.is_file() else None
            compile_slang(slangc, slang, spv)
            if spv.read_bytes() != old:
                print(f"compiled {shown(slang)} -> {spv.name} ({spv.stat().st_size} bytes)")
        for cone in cones:
            embed(cone)
        print(f"shaders: {len(slangs)} shaders compiled with {slangc.name if slangc else 'nothing'}")
        return 0
    except ShaderError as err:
        print(f"shaders: error: {err}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
