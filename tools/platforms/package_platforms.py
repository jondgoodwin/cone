#!/usr/bin/env python3
"""Package platforms: what each package's [package] os declares, and the checks that hold the
packages to it (lane 773, row 565; the survey is Ruminations/package-platform-map-2026-11-19.md).

Usage (run it from anywhere; <packages> is the packages folder, default ../../packages beside this file):
  package_platforms.py list [--os linux] [--lacks FILE]   one line per package: <name> <status> <why>
        status is run (the package claims the system and the box can build what it needs),
        skipped (it declares other systems only; why is its reason), or
        unverifiable (it claims the system but needs a library the box lacks, per --lacks).
  package_platforms.py run  [--os linux] [--lacks FILE]   only the names with status run, one line
  package_platforms.py static                             the static check: a package that claims Linux
        has a src file with @c(system) externs and no '#os' guard anywhere in it, or imports a package
        that does not claim Linux. Prints each offence; exit 1 if there is one.
  package_platforms.py table                              <name> <systems> lines, for the catalogue

The reason of a skipped package is the comment lines directly above its `os` line in congo.toml,
or "declared <systems> only" where there are none.

The --lacks file lists, one per line, the C libraries the box cannot link (for the Linux test box:
SDL3); '#' starts a comment. A package is unverifiable where it, or a package its src, tests or
examples import (transitively through src), links a library named there in [link] or
[link.<system>]: congo test builds its examples, so a package whose examples cannot link fails whole.
"""
import re
import sys
import tomllib
from pathlib import Path

DEFAULT_OS = ["windows", "linux"]
IMPORT_RE = re.compile(r"^\s*(?:pub\s+)?import\s+([A-Za-z_]\w*)", re.M)
SYSTEM_RE = re.compile(r"@c\(system\)")
GUARD_RE = re.compile(r"#os\b")


class Package:
    def __init__(self, folder: Path):
        self.folder = folder
        self.name = folder.name
        text = (folder / "congo.toml").read_text(encoding="utf-8")
        data = tomllib.loads(text)
        self.os = list(data.get("package", {}).get("os", DEFAULT_OS))
        self.libs = set(data.get("link", {}).get("libraries", []))
        for system in ("windows", "linux", "macos"):
            if system in data.get("link", {}):
                self.libs |= {(system, lib) for lib in data["link"][system].get("libraries", [])}
        self.reason = self._reason(text)
        self.imports: set[str] = set()      # imported by src/
        self.all_imports: set[str] = set()  # imported by src/, tests/ or examples/

    def _reason(self, text: str) -> str:
        lines = text.splitlines()
        for i, line in enumerate(lines):
            if re.match(r"\s*os\s*=", line):
                above = []
                j = i - 1
                while j >= 0 and lines[j].lstrip().startswith("#"):
                    above.insert(0, lines[j].lstrip().lstrip("#").strip())
                    j -= 1
                if above:
                    return " ".join(above)
        return "declared " + " and ".join(self.os) + " only"

    def source_files(self):
        for f in sorted((self.folder / "src").rglob("*.cone")):
            yield f


def load(packages: Path) -> dict[str, Package]:
    found = {m.parent.name: Package(m.parent) for m in sorted(packages.glob("*/congo.toml"))}
    for pkg in found.values():
        for f in sorted(pkg.folder.rglob("*.cone")):
            rel = f.relative_to(pkg.folder).parts
            if "build" in rel:
                continue
            for m in IMPORT_RE.finditer(f.read_text(encoding="utf-8", errors="replace")):
                if m.group(1) in found and m.group(1) != pkg.name:
                    pkg.all_imports.add(m.group(1))
                    if rel[0] == "src":
                        pkg.imports.add(m.group(1))
    return found


def closure(found: dict[str, Package], name: str) -> set[str]:
    seen, todo = set(), [name]
    while todo:
        n = todo.pop()
        if n in seen:
            continue
        seen.add(n)
        todo.extend(found[n].all_imports if n == name else found[n].imports)
    return seen


def lacked(found: dict[str, Package], name: str, system: str, lacks: set[str]) -> list[str]:
    out = set()
    for n in closure(found, name):
        for lib in found[n].libs:
            if isinstance(lib, tuple):
                if lib[0] != system:
                    continue
                lib = lib[1]
            if lib.lower() in lacks:
                out.add(lib)
    return sorted(out)


def statuses(found: dict[str, Package], system: str, lacks: set[str]):
    for name, pkg in found.items():
        if system not in pkg.os:
            yield name, "skipped", pkg.reason
            continue
        missing = lacked(found, name, system, lacks)
        if missing:
            yield name, "unverifiable", "needs " + ", ".join(missing) + ", which the box lacks"
        else:
            yield name, "run", ""


def static_check(found: dict[str, Package]) -> list[str]:
    """Offences, for packages that claim Linux."""
    bad = []
    for name, pkg in found.items():
        if "linux" not in pkg.os:
            continue
        for f in pkg.source_files():
            text = f.read_text(encoding="utf-8", errors="replace")
            if SYSTEM_RE.search(text) and not GUARD_RE.search(text):
                bad.append(f"{name}: claims linux, but {f.relative_to(pkg.folder).as_posix()} has"
                           " @c(system) externs and no #os guard")
        for dep in sorted(pkg.imports):
            if "linux" not in found[dep].os:
                bad.append(f"{name}: claims linux, but imports {dep}, which does not")
    return bad


def main(argv: list[str]) -> int:
    args = argv[1:]
    if not args:
        print(__doc__)
        return 2
    cmd = args.pop(0)
    opts = {"--os": "linux", "--lacks": None, "--packages": str(Path(__file__).resolve().parents[2] / "packages")}
    while args:
        key = args.pop(0)
        if key not in opts or not args:
            print(f"unknown or incomplete option {key}", file=sys.stderr)
            return 2
        opts[key] = args.pop(0)
    found = load(Path(opts["--packages"]))
    lacks = set()
    if opts["--lacks"]:
        for line in Path(opts["--lacks"]).read_text(encoding="utf-8").splitlines():
            line = line.split("#")[0].strip().lower()
            if line:
                lacks.add(line)
    if cmd == "list":
        for name, status, why in statuses(found, opts["--os"], lacks):
            print(f"{name} {status}" + (f" {why}" if why else ""))
    elif cmd == "run":
        print(" ".join(n for n, s, _ in statuses(found, opts["--os"], lacks) if s == "run"))
    elif cmd == "table":
        for name, pkg in found.items():
            print(f"{name} {','.join(pkg.os)}")
    elif cmd == "static":
        bad = static_check(found)
        for line in bad:
            print(line)
        if bad:
            return 1
        print(f"static: ok, {len(found)} packages")
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
