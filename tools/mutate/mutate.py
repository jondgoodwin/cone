#!/usr/bin/env python3
"""A mutation pass over the language suite's sources, to reach the compiler's "impossible" arms.

The compiler's default arms for states it believes cannot happen (compiler/c/doc/diagnostics/measuring.md,
"How the compiler reacts to reaching a state it believes impossible") abort with "Compiler defect"
(ErrorUnreachable). Reading the code does not tell whether a source reaches one; compiling
adversarial sources does. This takes every single-file scenario under test/cases, deletes, swaps or
duplicates one token in it, runs conec on the result, and flags a compile that
  - says "Compiler defect" or "Compile abandoned: the compiler reached a state it had ruled out",
  - ends in a signal or an exit status that is neither success (0) nor a reported error (1), or
  - times out.
A flagged mutant is kept under the output folder with the compiler's output beside it.

Run it once per release and whenever a node kind is added (Backlog\\compiler.md, ruled 1 Nov 2026).

    python tools/mutate/mutate.py --conec build/x64-release/conec.exe --out build/mutate
        [--per-file 3] [--seed 1] [--limit 400] [--jobs 4] [--timeout 20]

The same --seed gives the same mutants. Every flagged mutant is a compiler defect to triage: a
program that deserves a diagnostic and gets an abort. The run lists them and exits 1 when it found any.
It runs conec only, never a built program.
"""
import argparse
import concurrent.futures
import random
import re
import subprocess
import sys
from pathlib import Path

TOKEN = re.compile(r"[A-Za-z_][A-Za-z_0-9]*|[0-9][0-9A-Za-z_.]*|\"(?:[^\"\\\n]|\\.)*\"|[^\sA-Za-z_0-9]")
MARKERS = ("Compiler defect", "Compile abandoned", "reached a state it had ruled out")
# 0 compiled, 1 a reported error, 2 a source or import file the command line named cannot be read
# (a mutant that breaks an 'import' or 'mod' line names no file; that is no compiler defect)
OK_STATUS = (0, 1, 2)


def tokens(text):
    return [(m.start(), m.end()) for m in TOKEN.finditer(text)]


def mutate(text, rng):
    """Return (mutant text, description) with one token deleted, swapped with its neighbour or duplicated."""
    spans = tokens(text)
    if len(spans) < 4:
        return None
    i = rng.randrange(len(spans) - 1)
    (a0, a1), (b0, b1) = spans[i], spans[i + 1]
    kind = rng.choice(("delete", "swap", "duplicate"))
    if kind == "delete":
        return text[:a0] + text[a1:], f"delete token at {a0}: {text[a0:a1]!r}"
    if kind == "swap":
        return text[:a0] + text[b0:b1] + text[a1:b0] + text[a0:a1] + text[b1:], \
            f"swap tokens at {a0}: {text[a0:a1]!r} {text[b0:b1]!r}"
    return text[:a1] + " " + text[a0:a1] + text[a1:], f"duplicate token at {a0}: {text[a0:a1]!r}"


def run_one(job):
    conec, src, outdir, timeout = job
    work = outdir / "work" / src.stem
    work.mkdir(parents=True, exist_ok=True)
    try:
        proc = subprocess.run([str(conec), "--checktree", "-o", str(work), str(src)],
                              capture_output=True, text=True, errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        return src, "timeout", ""
    output = proc.stdout + proc.stderr
    if any(m in output for m in MARKERS):
        return src, "abort-with-report", output
    if proc.returncode not in OK_STATUS:
        return src, f"exit status {proc.returncode}", output
    return src, None, output


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--conec", required=True)
    ap.add_argument("--cases", default="test/cases")
    ap.add_argument("--out", default="build/mutate")
    ap.add_argument("--per-file", type=int, default=3)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--limit", type=int, default=0, help="stop after this many sources (0: all)")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--timeout", type=int, default=20)
    args = ap.parse_args()

    conec = Path(args.conec).resolve()
    out = Path(args.out)
    (out / "mutants").mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)
    # A file directly under a group folder is a scenario of one file; those in a folder of
    # their own import their siblings and cannot be compiled alone
    sources = sorted(Path(args.cases).glob("*/*.cone"))
    if args.limit:
        rng.shuffle(sources)
        sources = sources[:args.limit]

    jobs, notes = [], {}
    for src in sources:
        try:
            text = src.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue   # deliberately invalid UTF-8 and the like
        for n in range(args.per_file):
            made = mutate(text, rng)
            if made is None:
                break
            mutant, what = made
            path = out / "mutants" / f"{src.stem}-m{n}-{args.seed}.cone"
            path.write_text(mutant, encoding="utf-8", newline="")
            notes[path] = (src, what)
            jobs.append((conec, path, out, args.timeout))

    flagged = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for path, verdict, output in pool.map(run_one, jobs):
            if verdict:
                src, what = notes[path]
                (out / "mutants" / (path.name + ".out.txt")).write_text(output, encoding="utf-8")
                flagged.append((path, verdict, src, what))
                print(f"FLAG {verdict}: {path}  (from {src}: {what})")
    print(f"{len(jobs)} mutants of {len(sources)} sources, {len(flagged)} flagged")
    return 1 if flagged else 0


if __name__ == "__main__":
    sys.exit(main())
