#!/usr/bin/env python3
"""The trust map: `conec --report-trust` over every package, as a table.

For each package under packages/ that has src/<name>.cone, compiles a one-line
module that imports it (the compiler reads the package from source, type checks
it, prints one line per unconstrained operation and generates no code), then
joins what the runs printed. A site belongs to the package whose folder holds
its file; core and the other packages a run pulls in are the same lines in every
run, so each site is counted once.

    python tools/trustmap.py [--conec PATH] [--jobs N] [--only a,b] [--out DIR] [--full]

Writes DIR/sites.txt (every site: kind, file, line, function, what), DIR/by-function.md
(the per-function table) and prints the by-package summary. Without --out, DIR is
build/trustmap. The option itself is documented in compiler/c/ir/trustreport.c.
Site kinds: deref, ref-from-ptr, intrinsic, extern, reinterpret, sendable.
"""
import argparse
import collections
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PACKAGES = REPO / "packages"
KINDS = ["deref", "ref-from-ptr", "intrinsic", "extern", "reinterpret", "sendable"]
SITE = re.compile(r"^(deref|ref-from-ptr|intrinsic|extern|reinterpret|sendable) (.+?):(\d+) (\S+)(?: \[(.*)\])?$")


def find_conec(arg):
    if arg:
        return Path(arg)
    for exe in ("conec.exe", "conec"):
        path = REPO / "build" / "x64-release" / exe
        if path.exists():
            return path
    sys.exit("no conec: build it, or pass --conec")


def compile_package(conec, name, work):
    folder = work / name
    folder.mkdir(parents=True, exist_ok=True)
    source = folder / f"{name}_trust.cone"
    # core is imported into every module, so its own report is a module that imports nothing
    source.write_text(f"mod {name}_trust;\n" + ("" if name == "core" else f"\nimport {name};\n"), encoding="utf-8")
    run = subprocess.run([str(conec), "--report-trust", "--path=" + str(PACKAGES), "-o", str(folder), str(source)],
                         capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=600)
    return name, run


def package_of(file):
    """The package folder a source path lies in, or None."""
    path = Path(file)
    try:
        relative = path.resolve().relative_to(PACKAGES)
    except (ValueError, OSError):
        return None
    return relative.parts[0] if relative.parts else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--conec")
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--only", help="comma-separated package names")
    ap.add_argument("--out", default=str(REPO / "build" / "trustmap"))
    args = ap.parse_args()
    conec = find_conec(args.conec)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    names = sorted(p.name for p in PACKAGES.iterdir() if (p / "src" / f"{p.name}.cone").is_file())
    if args.only:
        names = [n for n in names if n in args.only.split(",")]

    sites = {}                       # (kind, file, line, function, what) -> package
    ptrops = collections.Counter()   # package -> pointer expressions that are not sites
    seen_files = {}
    failed = {}
    with ThreadPoolExecutor(args.jobs) as pool:
        for name, run in pool.map(lambda n: compile_package(conec, n, out), names):
            if run.returncode != 0:
                failed[name] = (run.stdout + run.stderr).strip().splitlines()[:3]
                continue
            for line in run.stdout.splitlines():
                m = SITE.match(line)
                if m:
                    kind, file, ln, fn, what = m.groups()
                    pkg = package_of(file)
                    if pkg:
                        sites[(kind, Path(file).resolve().relative_to(REPO).as_posix(), int(ln), fn, what or "")] = pkg
                    continue
                m = re.match(r"^pointer-operations (.+) (\d+)$", line)
                if m:
                    pkg = package_of(m.group(1))
                    if pkg and m.group(1) not in seen_files:
                        seen_files[m.group(1)] = 1
                        ptrops[pkg] += int(m.group(2))

    # The raw sites
    ordered = sorted(sites)
    (out / "sites.txt").write_text(
        "".join(f"{k} {f}:{l} {fn}" + (f" [{w}]" if w else "") + "\n" for k, f, l, fn, w in ordered),
        encoding="utf-8")

    by_pkg = collections.defaultdict(list)
    for key, pkg in sites.items():
        by_pkg[pkg].append(key)
    total = collections.Counter(k[0] for k in sites)

    lines = ["# Trust map", "",
             f"Packages compiled: {len(names) - len(failed)} of {len(names)}.", ""]
    lines += ["## Total by kind", "", "| kind | sites |", "| --- | ---: |"]
    lines += [f"| {k} | {total[k]} |" for k in KINDS]
    lines += [f"| all | {sum(total.values())} |", ""]
    lines += ["Pointer expressions that are not sites (not reported): "
              f"{sum(ptrops.values())}.", ""]
    if failed:
        lines += ["## Packages that did not compile for the report", ""]
        for name, why in sorted(failed.items()):
            lines.append(f"- {name}: {why[0] if why else '?'}")
        lines.append("")
    none = [n for n in names if n not in failed and n not in by_pkg]
    lines += [f"## Packages with no sites ({len(none)})", "", ", ".join(none) or "none", ""]
    lines += ["## By package", "",
              "| package | sites | files | functions | " + " | ".join(KINDS) + " | pointer ops not reported |",
              "| --- | ---: | ---: | ---: | " + " | ".join("---:" for _ in KINDS) + " | ---: |"]
    for pkg in sorted(by_pkg, key=lambda p: (-len(by_pkg[p]), p)):
        keys = by_pkg[pkg]
        kinds = collections.Counter(k[0] for k in keys)
        files = {k[1] for k in keys}
        funcs = {(k[1], k[3]) for k in keys}
        lines.append(f"| {pkg} | {len(keys)} | {len(files)} | {len(funcs)} | "
                     + " | ".join(str(kinds[k]) for k in KINDS) + f" | {ptrops[pkg]} |")
    lines.append("")
    summary = "\n".join(lines)
    (out / "summary.md").write_text(summary, encoding="utf-8")

    # By function
    rows = ["# Sites by package, file and function", ""]
    for pkg in sorted(by_pkg):
        rows += [f"## {pkg}", ""]
        per = collections.defaultdict(collections.Counter)
        for kind, f, l, fn, w in by_pkg[pkg]:
            per[(f, fn)][kind] += 1
        for (f, fn), kinds in sorted(per.items()):
            rows.append(f"- {f} {fn}: " + ", ".join(f"{k} {n}" for k, n in sorted(kinds.items())))
        rows.append("")
    (out / "by-function.md").write_text("\n".join(rows), encoding="utf-8")
    print(summary)
    print(f"(sites: {out / 'sites.txt'}; per function: {out / 'by-function.md'})")


if __name__ == "__main__":
    main()
