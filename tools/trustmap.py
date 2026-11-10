#!/usr/bin/env python3
"""The trust map: `conec --report-trust` over every package and what instantiates it, as a table.

Two passes, both through Congo's build descriptions (so a package with a gpu/
folder, submodules or several files compiles as it does under `congo build`),
and neither generates code:

  1. every package under packages/, in dependency order, as a library: conec
     type checks it and prints one line per site, and still writes the package's
     include file, which the packages and programs compiled after it import;
  2. every program in each package's tests/ and examples/, as a lone file
     importing the package, against those include files. A generic is checked
     only as instances, so this is where the generics that real code uses get
     their sites counted.

Sites are merged by source position (the include file keeps the source's file
and line), so nothing counts twice. A site that only a test or example reached
is marked. A site in a tests/ or examples/ file itself is counted apart: it is
test code's, not the package's. `generic` lines give every generic template and
how many instances were made; one that no run instantiated is a blind spot.

    python tools/trustmap.py [--conec PATH] [--jobs N] [--only a,b] [--out DIR]
                             [--no-tests] [--aggregate-only]

Writes under DIR (default build/trustmap): raw.jsonl (every conec run's output),
sites.txt, by-function.md, generics.txt and summary.md, and prints the summary.
`--aggregate-only` rebuilds the files from raw.jsonl without running anything.
The option itself is documented in compiler/c/ir/trustreport.c.
Site kinds: deref, ref-from-ptr, intrinsic, extern, reinterpret, reinterpret-perm,
sendable.
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PACKAGES = REPO / "packages"
sys.path.insert(0, str(REPO / "tools" / "congo"))
import congo  # noqa: E402

KINDS = ["deref", "ref-from-ptr", "intrinsic", "extern", "reinterpret", "reinterpret-perm", "sendable"]
SITE = re.compile(r"^(deref|ref-from-ptr|intrinsic|extern|reinterpret-perm|reinterpret|sendable) (.+?):(\d+) (\S+)(?: \[(.*)\])?$")
GENERIC = re.compile(r"^generic (.+?):(\d+) (\S+) \[(\d+)\]$")
PTROPS = re.compile(r"^pointer-operations (.+) (\d+)$")


def find_conec(arg):
    if arg:
        return Path(arg)
    for exe in ("conec.exe", "conec"):
        path = REPO / "build" / "x64-release" / exe
        if path.exists():
            return path
    sys.exit("no conec: build it, or pass --conec")


class Collector:
    """Every conec run's output, kept as it comes, so a long run can be re-aggregated."""

    def __init__(self, path):
        self.lock = threading.Lock()
        self.libs = threading.RLock()
        self.file = open(path, "w", encoding="utf-8")
        self.done = set()
        self.failed = {}

    def add(self, phase, label, ok, text):
        with self.lock:
            self.file.write(json.dumps({"phase": phase, "label": label, "ok": ok, "text": text}) + "\n")
            self.file.flush()


def install(collector, conec_path):
    """Replace Congo's compile of a unit with a report-only one (nothing is generated, a library's include
    file still is), and its GPU kernels with nothing."""

    def compile_unit(conec, unit, out, mode, top, env, packages=(), includes=None, announce=True, prelude=(),
                     homes=None):
        pkg = unit.pkg
        if pkg.lone:
            return one_compile(conec, unit, out, mode, env, packages, includes)
        with collector.libs:        # a library is compiled once, whichever program wanted it first
            return one_compile(conec, unit, out, mode, env, packages, includes)

    def one_compile(conec, unit, out, mode, env, packages, includes):
        pkg = unit.pkg
        obj = out / f"{pkg.name}{congo.OBJ_EXT}"
        key = pkg.key
        if not pkg.lone:
            if key in collector.failed:
                raise congo.CongoError(f"{key} did not compile: {collector.failed[key]}")
            if key in collector.done:
                return obj
        output = "executable" if pkg.lone else "library"
        desc = out / f"{pkg.name}.conebuild"
        desc.write_text(congo.description(unit, mode, output, includes or out, packages), encoding="utf-8")
        include = congo.include_for(pkg, out)
        include.unlink(missing_ok=True)
        run = subprocess.run([str(conec), "--report-trust", "-o", str(out), str(desc)], env=env,
                             capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=900)
        phase = "test" if pkg.lone else "src"
        label = f"{pkg.name} ({pkg.src})" if pkg.lone else pkg.name
        ok = run.returncode == 0 and (output != "library" or include.is_file())
        if not ok:
            first = [ln for ln in (run.stdout + run.stderr).splitlines() if ln.startswith("Error")][:2]
            why = "; ".join(first) or f"exit {run.returncode}"
            if not pkg.lone:
                with collector.lock:
                    collector.failed[key] = why
            collector.add(phase, label, False, why)
            raise congo.CongoError(f"could not compile {label}: {why}")
        collector.add(phase, label, True, run.stdout)
        if not pkg.lone:
            with collector.lock:
                collector.done.add(key)
        return obj

    congo.compile_unit = compile_unit
    congo.compile_gpu = lambda *a, **k: Path("none.spv")
    congo.find_conec = lambda: conec_path


def run_everything(args, out):
    conec = find_conec(args.conec).resolve()
    collector = Collector(out / "raw.jsonl")
    install(collector, conec)
    registry = congo.open_registry()
    mine = {n: p for n, p in registry.packages.items() if p.root.parent.resolve() == PACKAGES.resolve()}
    names = sorted(mine)
    if args.only:
        names = [n for n in names if n in args.only.split(",")]
    session = congo.Session(registry, out / "inc", "debug")
    broken = {}
    for name in names:
        pkg = mine[name]
        try:
            order = congo.build_order(pkg, registry)
            session.compile(order, pkg, announce=False)
        except congo.CongoError as exc:
            broken[name] = str(exc)
        except Exception as exc:  # a package Congo cannot read at all
            broken[name] = f"{type(exc).__name__}: {exc}"
        print(f"src {name} {'FAILED' if name in broken else 'ok'}", file=sys.stderr, flush=True)
    (out / "broken.json").write_text(json.dumps(broken, indent=1), encoding="utf-8")
    if args.no_tests:
        return
    # Phase 2: the tests and examples, lone programs against the include files phase 1 left
    lock = threading.Lock()
    jobs = []
    for name in names:
        for sub in (congo.TESTS, congo.EXAMPLES):
            for file in congo.programs(mine[name].root / sub, None):
                jobs.append((name, sub, file))

    def one(job):
        name, sub, file = job
        try:
            sess = congo.Session(registry, out / "inc", "debug")
            sess.objs = dict(session.objs)
            sess.gpu_checked = set(session.gpu_checked)
            with lock:
                pkg = congo.lone_package(file)
                order = congo.build_order(pkg, registry)
            into = out / "programs" / name / sub / file.stem
            into.mkdir(parents=True, exist_ok=True)
            sess.compile(order, pkg, top_out=into, announce=False)
            state = "ok"
        except Exception as exc:
            state = "FAILED"
            collector.add("test", f"{name}/{sub}/{file.stem}", False, f"{type(exc).__name__}: {exc}")
        print(f"{sub} {name}/{file.stem} {state}", file=sys.stderr, flush=True)

    with ThreadPoolExecutor(args.jobs) as pool:
        list(pool.map(one, jobs))


def package_of(rel):
    parts = rel.split("/")
    return parts[1] if len(parts) > 2 and parts[0] == "packages" else None


def is_test_file(rel):
    parts = rel.split("/")
    return len(parts) > 3 and parts[2] in ("tests", "examples")


def normal(file):
    """A repo-relative path with forward slashes, or None outside the repo. A file read from an include file
    is spelled 'name#include path': the name is relative to the package's src/ (a line mark), and the include
    file is named for the package."""
    if "#" in file:
        name, inc = file.split("#", 1)
        stem = Path(inc).stem
        if Path(name).is_absolute():
            return None             # the include file's own text, not the source's
        file = str(PACKAGES / stem / "src" / name)
    path = Path(file)
    try:
        return path.resolve().relative_to(REPO).as_posix()
    except (ValueError, OSError):
        return None


class Normals(dict):
    """normal() of each file spelling, worked out once."""

    def __call__(self, file):
        if file not in self:
            self[file] = normal(file)
        return self[file]


FIRST_TABLE = {"deref": 1415, "ref-from-ptr": 1440, "intrinsic": 748, "extern": 1920, "reinterpret": 207, "sendable": 46}


def aggregate(out):
    seen = {"src": set(), "test": set()}          # phase -> site keys
    generics = collections.defaultdict(lambda: [0, 0])   # (file, line, name) -> [src instances, test instances]
    ptrops = {}                                   # file -> pointer expressions that are not sites
    failed_test = []
    cache = Normals()
    runs = 0
    for line in (out / "raw.jsonl").read_text(encoding="utf-8").splitlines():
        rec = json.loads(line)
        phase = rec["phase"]
        if not rec["ok"]:
            if phase == "test":
                failed_test.append((rec["label"], rec["text"]))
            continue
        runs += 1
        for text in rec["text"].splitlines():
            m = SITE.match(text)
            if m:
                kind, file, ln, fn, what = m.groups()
                rel = cache(file)
                if rel and package_of(rel):
                    seen[phase].add((kind, rel, int(ln), fn, what or ""))
                continue
            m = GENERIC.match(text)
            if m:
                file, ln, name, n = m.groups()
                rel = cache(file)
                if rel and package_of(rel):
                    generics[(rel, int(ln), name)][0 if phase == "src" else 1] += int(n)
                continue
            m = PTROPS.match(text)
            if m:
                rel = cache(m.group(1))
                if rel and package_of(rel):
                    ptrops[rel] = max(ptrops.get(rel, 0), int(m.group(2)))
    return seen, generics, ptrops, failed_test, runs


def write_all(out):
    seen, generics, ptrops, failed_test, runs = aggregate(out)
    allsites = seen["src"] | seen["test"]
    only_test = seen["test"] - seen["src"]
    own = {k for k in allsites if is_test_file(k[1])}        # in a tests/ or examples/ file
    pkgsites = allsites - own
    by_pkg = collections.defaultdict(list)
    for k in pkgsites:
        by_pkg[package_of(k[1])].append(k)
    total = collections.Counter(k[0] for k in pkgsites)
    total_src = collections.Counter(k[0] for k in pkgsites if k in seen["src"])
    total_only = collections.Counter(k[0] for k in pkgsites if k in only_test)
    ptr_by_pkg = collections.Counter()
    for rel, n in ptrops.items():
        if not is_test_file(rel):
            ptr_by_pkg[package_of(rel)] += n

    gens = {k: v for k, v in generics.items() if not is_test_file(k[0])}
    blind = sorted(k for k, v in gens.items() if v[0] + v[1] == 0)
    only_by_tests = sorted(k for k, v in gens.items() if v[0] == 0 and v[1] > 0)
    gen_by_pkg = collections.defaultdict(lambda: [0, 0, 0])      # total, never, only tests
    for k, v in gens.items():
        row = gen_by_pkg[package_of(k[0])]
        row[0] += 1
        row[1] += v[0] + v[1] == 0
        row[2] += v[0] == 0 and v[1] > 0
    (out / "generics.txt").write_text(
        "# Generic templates that no source, test or example instantiated (file:line name)\n"
        + "".join(f"{f}:{l} {n}\n" for f, l, n in blind)
        + "\n# Instantiated only by a test or example\n"
        + "".join(f"{f}:{l} {n}\n" for f, l, n in only_by_tests), encoding="utf-8")

    (out / "sites.txt").write_text(
        "".join(f"{k} {f}:{l} {fn}" + (f" [{w}]" if w else "") + (" *test-only" if (k, f, l, fn, w) in only_test else "")
                + (" *in-test-file" if (k, f, l, fn, w) in own else "") + "\n"
                for k, f, l, fn, w in sorted(allsites)), encoding="utf-8")

    all_names = sorted(p for p in os.listdir(PACKAGES) if (PACKAGES / p / "src" / f"{p}.cone").is_file())
    broken = json.loads((out / "broken.json").read_text(encoding="utf-8")) if (out / "broken.json").exists() else {}
    compiled = [n for n in all_names if n not in broken]
    none = [n for n in compiled if n not in by_pkg]
    lines = ["# Trust map (second)", "",
             f"Packages compiled for the report: {len(compiled)} of {len(all_names)}; conec runs kept: {runs}.", ""]
    lines += ["## Total by kind", "",
              "| kind | first table | now: package source | now: plus tests and examples | reached only by a test or example |",
              "| --- | ---: | ---: | ---: | ---: |"]
    for k in KINDS:
        lines.append(f"| {k} | {FIRST_TABLE.get(k, '')} | {total_src[k]} | {total[k]} | {total_only[k]} |")
    lines.append(f"| all | {sum(FIRST_TABLE.values())} | {sum(total_src.values())} | {sum(total.values())} | {sum(total_only.values())} |")
    lines += ["", f"Sites in a tests/ or examples/ file itself (test code's own, not in the table): {len(own)}.", "",
              f"Pointer expressions that are not sites (not reported): {sum(ptr_by_pkg.values())}.", ""]
    if broken:
        lines += [f"## Packages that did not compile for the report ({len(broken)})", ""]
        for name, why in sorted(broken.items()):
            lines.append(f"- {name}: {why.splitlines()[0][:200] if why else '?'}")
        lines.append("")
    if failed_test:
        lines += [f"## Tests and examples that did not compile for the report ({len(failed_test)})", ""]
        for label, why in failed_test[:40]:
            lines.append(f"- {label}: {why.splitlines()[0][:160] if why else '?'}")
        if len(failed_test) > 40:
            lines.append(f"- ... and {len(failed_test) - 40} more (raw.jsonl)")
        lines.append("")
    lines += [f"## Packages with no sites ({len(none)})", "", ", ".join(none) or "none", ""]
    lines += [f"## Generics ({len(gens)} templates)", "",
              f"No run instantiated {len(blind)} of them (generics.txt lists them by name); a further {len(only_by_tests)} were "
              "instantiated only by a test or example.", "",
              "| package | generics | never instantiated | instantiated only by tests or examples |",
              "| --- | ---: | ---: | ---: |"]
    for pkg in sorted(gen_by_pkg, key=lambda p: (-gen_by_pkg[p][1], p)):
        if gen_by_pkg[pkg][1] or gen_by_pkg[pkg][2]:
            lines.append(f"| {pkg} | {gen_by_pkg[pkg][0]} | {gen_by_pkg[pkg][1]} | {gen_by_pkg[pkg][2]} |")
    lines.append("")
    lines += ["## By package", "",
              "| package | sites | files | functions | " + " | ".join(KINDS) + " | only test | pointer ops not reported |",
              "| --- | ---: | ---: | ---: | " + " | ".join("---:" for _ in KINDS) + " | ---: | ---: |"]
    for pkg in sorted(by_pkg, key=lambda p: (-len(by_pkg[p]), p)):
        keys = by_pkg[pkg]
        kinds = collections.Counter(k[0] for k in keys)
        files = {k[1] for k in keys}
        funcs = {(k[1], k[3]) for k in keys}
        lines.append(f"| {pkg} | {len(keys)} | {len(files)} | {len(funcs)} | " + " | ".join(str(kinds[k]) for k in KINDS)
                     + f" | {sum(1 for k in keys if k in only_test)} | {ptr_by_pkg[pkg]} |")
    lines.append("")
    summary = "\n".join(lines)
    (out / "summary.md").write_text(summary, encoding="utf-8")

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
    print(f"(sites: {out / 'sites.txt'}; per function: {out / 'by-function.md'}; generics: {out / 'generics.txt'})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--conec")
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--only", help="comma-separated package names")
    ap.add_argument("--out", default=str(REPO / "build" / "trustmap"))
    ap.add_argument("--no-tests", action="store_true", help="the packages' own source only")
    ap.add_argument("--aggregate-only", action="store_true")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    if not args.aggregate_only:
        run_everything(args, out)
    write_all(out)


if __name__ == "__main__":
    main()
