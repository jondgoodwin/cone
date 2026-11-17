"""A run's totals, per kind: python packages/memstats/summary.py memstats.jsonl

Reads the totals line (the last) and the samples of a MEMSTATS_OUT file and prints, per
kind, the allocations and bytes, frees, the share freed by another actor and by another
worker, reallocs (grown, moved), the median lifetime, and the check that the numbers add
up: the last sample's tagged-plus-stray live bytes against the process's private bytes.
This is only a first look; the workload report (row 431) is the real report.
"""
import json
import sys
from pathlib import Path

try:
    import tomllib
    kinds = tomllib.loads((Path(__file__).resolve().parent / "kinds.toml").read_text(encoding="utf-8"))["kinds"]
except Exception:
    kinds = {}

lines = [json.loads(l) for l in Path(sys.argv[1]).read_text(encoding="utf-8").splitlines() if l.strip()]
totals = lines[-1]
samples = [l for l in lines if "live" in l]
freq = totals["freq"]
print(f"{len(samples)} samples, {(totals['end'] - totals['start']) / freq:.2f} s, {totals['threads']} threads, "
      f"{totals['foreign_frees']} frees of blocks the C runtime made itself")
print(f"{'kind':<14}{'allocs':>10}{'bytes':>13}{'frees':>10}{'other actor':>12}{'other wkr':>10}{'reallocs':>9}{'moved':>8}{'life p50 us':>12}")
tot_a = tot_b = 0
for k in totals["kinds"]:
    na = sum(c[1] + c[5] * 0 for c in k["cells"])
    ba = sum(c[2] for c in k["cells"])
    nf = sum(c[3] for c in k["cells"])
    life = k["life"]
    n = sum(c for _, c in life)
    acc = 0
    p50 = 0.0
    for b, c in sorted(life):
        acc += c
        if n and acc * 2 >= n:
            p50 = (2 ** (b - 1) if b else 0) * 1e6 / freq
            break
    name = kinds.get(str(k["k"]), {}).get("name", "?")
    tot_a += na
    tot_b += ba
    print(f"{k['k']:>3} {name:<10}{na:>10}{ba:>13}{nf:>10}{k['freed_other_actor'][0]:>12}{k['freed_other_worker'][0]:>10}"
          f"{k['reallocs']:>9}{k['moved']:>8}{p50:>12.1f}")
print(f"{'all':<14}{tot_a:>10}{tot_b:>13}")
la = totals.get("large_aligned", [0, 0])
print(f"large aligned (untagged, alignment over ~512 KiB): {la[0]} blocks, {la[1]} bytes; "
      f"foreign frees {totals['foreign_frees']}, foreign reallocs {totals.get('foreign_reallocs', 0)}")
strays = sorted(totals["strays"], key=lambda s: -s[1])
print(f"stray blocks (kind 0): {len(strays)} sites by return address, load base {totals['base']:#x}; the largest ten "
      f"(count, bytes, then the call into malloc and three callers above it, as offsets from the base; "
      f"resolve through the program's PDB):")
for n, b, *ras in strays[:10]:
    print(f"  {n:>8} {b:>11}  " + "  ".join(f"{a - totals['base']:#x}" if a else "-" for a in ras))
if samples:
    s = samples[-1]
    print(f"last sample: tagged+stray live {s['live_b']} bytes ({s['live_n']} blocks), process private {s['private']}, "
          f"unaccounted {s['unaccounted']}")
