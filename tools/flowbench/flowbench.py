"""Measure what flow analysis costs, before and after a change.

    python tools/flowbench/flowbench.py --base <conec of master> [--conec <conec>]

Compiles three sets of inputs with `conec -V 1` and sums its timers, taking the
minimum of --runs runs for each file and each timer on its own, the runs of the
two compilers interleaved so that drift in the machine falls on both alike:

  packages  packages/<name>/src/<name>.cone, each compiled alone (with its imports)
  suite     every test/cases/**/*.cone, each compiled alone
  stress    the files genstress.py generates (many_500, plain_500, big_250,
            big_1000, nest_3, flat_3)

It prints a Markdown table of both compilers' figures, then the budget the
borrow-freezing design set, measured against --base: every PR that changes
flow's cost pastes both into its description. `--conec` defaults to this
tree's build/x64-release, and is refused when older than a compiler source
(a stale binary measures the wrong compiler); `--base` is taken as given.

A compiler without the Flow timer (anything before it landed) reports flow
inside Analysis: its flow column is blank, and "front end" (Analysis + Flow)
and the total are what compare. With -V 2 a compiler that has the gate also
prints how many functions it gated; one extra -V 2 pass collects that.
"""

import argparse
import os
import pathlib
import re
import subprocess
import sys

import genstress

REPO = pathlib.Path(__file__).resolve().parents[2]
IS_WINDOWS = os.name == "nt"
TIMERS = ("LLVM setup", "Load", "Lexer", "Parse", "Analysis", "Gen", "Verify",
          "Optimize", "Codegen", "Flow")
STRESS = (("many", 500), ("plain", 500), ("big", 250), ("big", 1000),
          ("nest", 3), ("flat", 3))
GATE_LINE = re.compile(r"Flow gate: (\d+) of (\d+) functions")


def default_conec() -> pathlib.Path:
    return REPO / "build" / "x64-release" / ("conec.exe" if IS_WINDOWS else "conec")


def check_not_stale(conec: pathlib.Path) -> None:
    """Refuse a compiler older than any source it is built from."""
    if not conec.exists():
        sys.exit(f"flowbench: no compiler at {conec}; build it first")
    newest, newest_path = 0.0, None
    for path in (REPO / "compiler" / "c").rglob("*"):
        if path.suffix in (".c", ".h") and path.is_file() and path.stat().st_mtime > newest:
            newest, newest_path = path.stat().st_mtime, path
    if conec.stat().st_mtime < newest:
        sys.exit(f"flowbench: {conec} is older than {newest_path}; rebuild it first")


def compile_once(conec: pathlib.Path, src: pathlib.Path, out: pathlib.Path,
                 verbosity: int = 1) -> tuple[dict[str, float], str]:
    result = subprocess.run([str(conec), "-V", str(verbosity), "-o", str(out), str(src)],
                            capture_output=True, text=True, cwd=str(out))
    times = {}
    for name in TIMERS:
        found = re.search(re.escape(name) + r":?\s+([0-9.eE+-]+)", result.stdout)
        if found:
            times[name] = float(found.group(1))
    return times, result.stdout


def measure(conecs: list[pathlib.Path], srcs: list[pathlib.Path], runs: int,
            out: pathlib.Path) -> list[dict[str, float]]:
    """Per compiler: each timer summed over the files, each file's minimum over runs."""
    sums = [dict.fromkeys(("front", "flow", "total"), 0.0) for _ in conecs]
    hasflow = [False for _ in conecs]
    for src in srcs:
        best = [None for _ in conecs]
        for _ in range(runs):
            for k, conec in enumerate(conecs):
                times, _ = compile_once(conec, src, out)
                row = {
                    "front": times.get("Analysis", 0.0) + times.get("Flow", 0.0),
                    "flow": times.get("Flow", 0.0),
                    "total": sum(times.values()),
                }
                hasflow[k] |= "Flow" in times
                best[k] = row if best[k] is None else {m: min(best[k][m], row[m]) for m in row}
        for k in range(len(conecs)):
            for m in sums[k]:
                sums[k][m] += best[k][m]
    for k in range(len(conecs)):
        if not hasflow[k]:
            sums[k]["flow"] = None
    return sums


def gate_counts(conec: pathlib.Path, srcs: list[pathlib.Path], out: pathlib.Path):
    """(gated, total) functions over the files, or None when conec has no gate."""
    gated = total = 0
    seen = False
    for src in srcs:
        _, stdout = compile_once(conec, src, out, verbosity=2)
        found = GATE_LINE.search(stdout)
        if found:
            seen = True
            gated += int(found.group(1))
            total += int(found.group(2))
    return (gated, total) if seen else None


def input_sets(stressdir: pathlib.Path) -> list[tuple[str, list[pathlib.Path]]]:
    sets = []
    packages = sorted(p / "src" / f"{p.name}.cone" for p in (REPO / "packages").iterdir()
                      if (p / "src" / f"{p.name}.cone").exists())
    sets.append(("packages", packages))
    sets.append(("suite", sorted((REPO / "test" / "cases").rglob("*.cone"))))
    for mode, n in STRESS:
        sets.append((f"{mode}_{n}", [genstress.write(mode, n, stressdir)]))
    return sets


def lines_in(srcs: list[pathlib.Path]) -> int:
    return sum(len(s.read_text(encoding="utf-8", errors="replace").splitlines()) for s in srcs)


def ms(value) -> str:
    return "-" if value is None else f"{value * 1000:.2f}"


def change(new, base) -> str:
    if new is None or base is None or base == 0:
        return "-"
    return f"{(new / base - 1) * 100:+.1f}%"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--conec", type=pathlib.Path, default=None,
                        help="the compiler under test (default: this tree's build)")
    parser.add_argument("--base", type=pathlib.Path, default=None,
                        help="the compiler to compare against, normally master's")
    parser.add_argument("--runs", type=int, default=5, help="runs per file (default 5)")
    parser.add_argument("--only", default=None,
                        help="comma-separated input sets, e.g. packages,plain_500")
    args = parser.parse_args()

    conec = args.conec or default_conec()
    if args.conec is None:
        check_not_stale(conec)
    conecs = [conec] + ([args.base] if args.base else [])
    for c in conecs:
        if not c.exists():
            sys.exit(f"flowbench: no compiler at {c}")

    work = REPO / "build" / "flowbench"
    out = work / "out"
    out.mkdir(parents=True, exist_ok=True)
    sets = input_sets(work)
    if args.only:
        wanted = set(args.only.split(","))
        sets = [s for s in sets if s[0] in wanted]

    results = {}
    for name, srcs in sets:
        print(f"measuring {name} ({len(srcs)} files, {args.runs} runs)...", file=sys.stderr, flush=True)
        sums = measure(conecs, srcs, args.runs, out)
        results[name] = (len(srcs), lines_in(srcs), sums, gate_counts(conec, srcs, out))

    print(f"flowbench: min of {args.runs} runs per file; times in ms")
    print(f"  conec: {conec}")
    if args.base:
        print(f"  base:  {args.base}")
    print()
    if args.base:
        print("| Input | Files | Lines | Gated fns | Flow (base) | Flow | Flow change"
              " | Front end (base) | Front end | Total (base) | Total | Total change |")
        print("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    else:
        print("| Input | Files | Lines | Gated fns | Flow | Front end | Total | Flow share |")
        print("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for name, (files, lines, sums, gates) in results.items():
        new = sums[0]
        gated = f"{gates[0]} / {gates[1]}" if gates else "-"
        if args.base:
            base = sums[1]
            print(f"| {name} | {files} | {lines:,} | {gated} | {ms(base['flow'])} | {ms(new['flow'])}"
                  f" | {change(new['flow'], base['flow'])} | {ms(base['front'])} | {ms(new['front'])}"
                  f" | {ms(base['total'])} | {ms(new['total'])} | {change(new['total'], base['total'])} |")
        else:
            share = "-" if new["flow"] is None else f"{new['flow'] / new['total'] * 100:.1f}%"
            print(f"| {name} | {files} | {lines:,} | {gated} | {ms(new['flow'])} | {ms(new['front'])}"
                  f" | {ms(new['total'])} | {share} |")

    print()
    budget(results, bool(args.base))


def budget(results, hasbase: bool) -> None:
    """The borrow-freezing design's budget, where the inputs it names were measured."""
    def flow(name, k=0):
        return results[name][2][k]["flow"] if name in results and len(results[name][2]) > k else None

    def total(name, k=0):
        return results[name][2][k]["total"] if name in results and len(results[name][2]) > k else None

    rows = []
    if hasbase:
        for name in ("suite", "packages", "plain_500"):
            new, base = flow(name), flow(name, 1)
            if new is not None and base is not None:
                rows.append((f"B1 flow, {name}", f"{change(new, base)}", "<= +10%", new <= base * 1.10))
        new, base = flow("many_500"), flow("many_500", 1)
        if new is not None and base is not None:
            rows.append(("B2 flow, many_500", f"{new / base:.2f}x", "<= 2.5x", new <= base * 2.5))
        for name in ("suite", "packages"):
            new, base = total(name), total(name, 1)
            if new is not None and base is not None:
                rows.append((f"B5 total, {name}", f"{change(new, base)}", "<= +1%", new <= base * 1.01))
    big, small = flow("big_1000"), flow("big_250")
    if big is not None and small:
        rows.append(("B3 flow, big_1000 / big_250", f"{big / small:.2f}", "<= 5", big / small <= 5))
    nest, flat = flow("nest_3"), flow("flat_3")
    if nest is not None and flat:
        rows.append(("B4 flow, nest_3 / flat_3", f"{nest / flat:.2f}", "<= 8", nest / flat <= 8))
    if not rows:
        return
    print("| Budget | Measured | Limit | |")
    print("| --- | ---: | ---: | --- |")
    for label, measured, limit, ok in rows:
        print(f"| {label} | {measured} | {limit} | {'within' if ok else '**over**'} |")


if __name__ == "__main__":
    main()
