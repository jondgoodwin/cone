"""Run the regex benchmark rows alternating Cone, Twin B and Twin A, and print the medians and ratios.

    python tools/regexbench/run_alternated.py --cone "<command that runs one row, {row} in place>" [--runs 5] [rows...]

For example (from a Visual Studio prompt, in the repository root; `congo run --release` builds and runs the
package's example):

    --cone "python tools/congo/congo.py run --release packages/regex/examples/regexbench.cone -- {row}"

The Rust twin is `tools/regexbench/rust` built with `cargo build --release`; `--rust` names its exe (default is the
shared target folder the earlier twins use). Each run is one process, so the order of the sides changes with every
run (a b c, c b a, ...). A row counts only if the three sides print the same count and checksum.
"""
import argparse
import statistics
import subprocess
import sys

ROWS = ["p1-is", "p1-find", "p2-is", "p2-find", "p3-find", "p3-caps", "p4-find", "p4-caps", "p5-is", "p5-find",
        "p6-find", "compile", "adv64k", "adv1m"]


def run(cmd, shell):
    out = subprocess.run(cmd, shell=shell, capture_output=True, text=True)
    for line in reversed(out.stdout.splitlines()):
        if " us=" in line:
            f = dict(x.split("=") for x in line.split()[1:])
            return int(f["us"]), f["count"], f["sum"]
    sys.exit("no result line from %r:\n%s\n%s" % (cmd, out.stdout, out.stderr))


def load():
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-c", "(Get-CimInstance Win32_Processor).LoadPercentage"],
                             capture_output=True, text=True).stdout.strip()
        return out + "%"
    except Exception:
        return "?"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cone", required=True)
    ap.add_argument("--rust", default=r"C:\src\zz-rusttwin-target\release\regexbench.exe")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("rows", nargs="*")
    a = ap.parse_args()
    rows = a.rows or ROWS
    print("%-8s %9s %9s %9s %8s %8s  %s" % ("row", "cone ms", "twinB ms", "twinA ms", "cone/B", "cone/A", "load"))
    for row in rows:
        times = {"cone": [], "b": [], "a": []}
        answers = set()
        l = load()
        for i in range(a.runs):
            order = ["cone", "b", "a"] if i % 2 == 0 else ["a", "b", "cone"]
            for side in order:
                if side == "cone":
                    r = run(a.cone.format(row=row), True)
                else:
                    r = run([a.rust, "bench", side, row], False)
                times[side].append(r[0])
                answers.add((side, r[1], r[2]))
        counts = {(c, s) for _, c, s in answers}
        same = len(counts) == 1
        m = {k: statistics.median(v) / 1000.0 for k, v in times.items()}
        print("%-8s %9.1f %9.1f %9.1f %8.2f %8.2f  %s%s" % (row, m["cone"], m["b"], m["a"], m["cone"] / m["b"],
                                                          m["cone"] / m["a"], l, "" if same else "  CHECKSUMS DIFFER %s" % sorted(answers)))
        sys.stdout.flush()


main()
