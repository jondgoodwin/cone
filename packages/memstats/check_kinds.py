"""Checks that the kind numbers core's markers write as literals match kinds.toml.

    python packages/memstats/check_kinds.py

The markers are `memstatsAsking(<n>u32, ...)` in core.cone (RawBlocks, Uni, Rc, MemRegion)
and sync.cone (Arc); the set of numbers they use must be the numbers kinds.toml gives those
names, and every kind kinds.toml marks `who = "core"` must have a marker. Exits 1 on a mismatch.
"""
import re
import sys
import tomllib
from pathlib import Path

root = Path(__file__).resolve().parent
kinds = tomllib.loads((root / "kinds.toml").read_text(encoding="utf-8"))["kinds"]
number = {v["name"]: int(k) for k, v in kinds.items()}

failed = 0
src = (root.parent / "core/src/core.cone").read_text(encoding="utf-8")
sync = (root.parent / "sync/src/sync.cone").read_text(encoding="utf-8")

def literals(text):
    return [int(m) for m in re.findall(r"memstatsAsking\((\d+)u32, \w+\);", text) if int(m) != 0]

found = {"core": literals(src), "sync": literals(sync)}
want_core = sorted([number["RawBlocks"], number["Uni"], number["Rc"], number["MemRegion"]])
want_sync = [number["Arc"]]
if sorted(set(found["core"])) != want_core:    # a kind may be written twice (alloc and resize)
    print("core.cone markers use", sorted(set(found["core"])), "but kinds.toml says", want_core)
    failed = 1
if found["sync"] != want_sync:
    print("sync.cone markers use", found["sync"], "but kinds.toml says", want_sync)
    failed = 1
for k, v in kinds.items():
    if v.get("who") == "core" and not any(n == int(k) for n in found["core"] + found["sync"]):
        print("kinds.toml names", v["name"], "as marked in core, but no marker writes", k)
        failed = 1
# the markers of packages that write a literal of their own kind: aio (HostJob) and memtrace (Instrument)
for pkg, name in (("aio/src/aio.cone", "HostJob"), ("memtrace/src/memtrace.cone", "Instrument")):
    text = (root.parent / pkg).read_text(encoding="utf-8")
    used = {int(m) for m in re.findall(r"msAsk\((\d+)u32, ", text) if int(m) != 0}
    if used != {number[name]}:
        print(pkg, "markers use", sorted(used), "but kinds.toml gives", name, number[name])
        failed = 1

print("kinds.toml and the markers agree" if not failed else "MISMATCH")
sys.exit(failed)
