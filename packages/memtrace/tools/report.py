#!/usr/bin/env python3
"""Turns a memtrace run (the sink's JSON lines, packages/memtrace/README.md) into the demand-side report tables.

    python report.py run.json [more.json ...] [--phase NAME] [--no-actors]

Several files are merged by time ("t", the trace package's ticks): the sink's stream today and, once row 423's
link shim writes one, its stream too (a line with "s":"supply" is kept and listed, and its totals are compared with
the demand side's block-making counts; the full supply comparison is row 431's).

Tables, each cut by phase (the marks memtrace.phase wrote; one phase "run" when there are none):
  1. per key: kind, elem, events, sum n, max n (all actors added)
  2. per region and element size: instances made, growths, drops, live at the end, live count at the samples (mean,
     peak, spread), slack, bytes asked for and bytes copied by growth, turnover
  3. pools, per instance: takes, returns, live, high-water mark, growths, capacity, free-list depth
  4. iobufs, by size: takes, returns, most out at once, and each actor's takes against its returns
  5. roll-ups by each attribute of row 421's kind table: shape, travel, growth, path (homogeneous against
     heterogeneous is the shape roll-up)
"""
import json
import math
import sys
from collections import defaultdict

# Row 421's kind table (Ruminations/cone-alloc-inventory-2026-11-16.md, Part 1), per region name in the schema:
# (kind number, shape, travel, growth, path)
ATTRS = {
    "list":      ("K01", "homogeneous", "travels", "moving", "region"),
    "deque":     ("K03", "homogeneous", "travels", "moving", "region"),
    "dict":      ("K04", "homogeneous", "travels", "moving", "region"),
    "genpool":   ("K07", "homogeneous", "travels", "moving", "region"),
    "densepool": ("K08", "homogeneous", "travels", "moving", "region"),
    "pool":      ("K06", "homogeneous", "stays", "moving", "region"),
    "iobuf":     ("K10", "classes", "travels", "chunks", "os"),
    "arena":     ("K18", "heterogeneous", "stays", "chunks", "raw"),
    "gc":        ("K21", "heterogeneous", "stays", "none", "malloc"),
    "msg":       ("K20", "heterogeneous", "travels", "none", "malloc"),
    "arc":       ("K15", "heterogeneous", "travels", "none", "raw"),
}
OPS_MAKE = ("make", "take")       # an instance or object comes to be
OPS_GROW = ("grow",)
OPS_DROP = ("drop", "return", "free")


def split(kind):
    region, _, op = kind.partition(".")
    return region, op


class Run:
    def __init__(self):
        self.hz = 1
        self.cum = {}                      # key -> numbers, cumulative
        self.phases = []                   # (name, t, snapshot)
        self.series = []                   # (t, {(region, elem): live count}) at each sample
        self.cache_series = []             # (t, bytes the IoBuf pool's per-thread caches hold) at each sample
        self.total = None
        self.supply = []
        self.t0 = None
        self.t1 = None


def key_of(k):
    return (k["k"], k["e"], k["a"], k["i"])


def live_counts(cum):
    live = defaultdict(int)
    for (kind, elem, who, inst), v in cum.items():
        region, op = split(kind)
        if op in ("make",):
            live[(region, elem)] += v["c"]
        elif op in ("drop",):
            live[(region, elem)] -= v["c"]
        elif op == "take":
            live[(region, elem)] += v["c"]
        elif op in ("return", "free"):
            live[(region, elem)] -= v["c"]
    return live


def load(paths):
    lines = []
    for p in paths:
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        # a keys block spans lines; a record is one "{"s":..." start to the next
        buf = ""
        for ln in text.splitlines():
            if ln.startswith('{"s":') and buf:
                lines.append(buf)
                buf = ""
            buf += ln.strip()
        if buf:
            lines.append(buf)
    recs = []
    for ln in lines:
        try:
            recs.append(json.loads(ln))
        except json.JSONDecodeError:
            continue
    recs.sort(key=lambda r: r.get("t", 0))
    r = Run()
    for rec in recs:
        s = rec.get("s")
        if s == "start":
            r.hz = rec.get("hz", 1)
            r.t0 = rec["t"] if r.t0 is None else r.t0
        elif s in ("sample", "total"):
            for k in rec["keys"]:
                r.cum[key_of(k)] = {f: k[f] for f in ("c", "n", "nx", "x", "xx", "xl")}
            r.t1 = rec["t"]
            if r.t0 is None:
                r.t0 = rec["t"]
            if s == "sample":
                r.series.append((rec["t"], live_counts(r.cum)))
                r.cache_series.append((rec["t"], sum(v["xl"] for kk, v in r.cum.items() if kk[0] == "bufpool.cache")))
            else:
                r.total = dict(r.cum)
        elif s == "phase":
            r.phases.append((rec["name"], rec["t"], {k: dict(v) for k, v in r.cum.items()}))
        elif s == "supply":
            r.supply.append(rec)
    if r.total is None:
        r.total = dict(r.cum)
    return r


def diff(a, b):
    """b - a, per key (cumulative counters; the maxes and last values are b's)."""
    out = {}
    for k, v in b.items():
        o = a.get(k)
        if o is None:
            out[k] = dict(v)
        else:
            out[k] = {"c": v["c"] - o["c"], "n": v["n"] - o["n"], "nx": v["nx"], "x": v["x"] - o["x"],
                      "xx": v["xx"], "xl": v["xl"]}
    return {k: v for k, v in out.items() if v["c"] != 0}


def cuts(r):
    """[(label, delta dict, seconds)]"""
    out = []
    prev = {}
    prev_t = r.t0
    for name, t, snap in r.phases:
        out.append((f"before {name}" if not out else out[-1][0], None, 0))   # placeholder, replaced below
        out.pop()
    names = []
    snaps = []
    ts = []
    if r.phases:
        names.append(f"until {r.phases[0][0]}")
        snaps.append(r.phases[0][2])
        ts.append(r.phases[0][1])
        for i, (name, t, snap) in enumerate(r.phases):
            nxt = r.phases[i + 1] if i + 1 < len(r.phases) else None
            names.append(f"{name}")
            snaps.append(nxt[2] if nxt else r.total)
            ts.append(nxt[1] if nxt else r.t1)
    else:
        names.append("run")
        snaps.append(r.total)
        ts.append(r.t1)
    prev = {}
    prev_t = r.t0
    res = []
    for n, s, t in zip(names, snaps, ts):
        secs = max((t - prev_t) / r.hz, 1e-9) if prev_t is not None and t is not None else 0
        res.append((n, diff(prev, s), secs))
        prev = s
        prev_t = t
    return res


def fmt(n):
    if isinstance(n, float):
        return f"{n:,.1f}"
    return f"{n:,}"


def table(title, head, rows):
    print(f"\n== {title}")
    if not rows:
        print("   (none)")
        return
    w = [len(h) for h in head]
    srows = [[str(c) for c in row] for row in rows]
    for row in srows:
        for i, c in enumerate(row):
            w[i] = max(w[i], len(c))
    print("  " + "  ".join(h.ljust(w[i]) if i == 0 else h.rjust(w[i]) for i, h in enumerate(head)))
    for row in srows:
        print("  " + "  ".join(c.ljust(w[i]) if i == 0 else c.rjust(w[i]) for i, c in enumerate(row)))


def by_region(delta):
    """(region, elem) -> {op: {c, n, nx, x, xx, xl}} with actors and instances added."""
    out = defaultdict(lambda: defaultdict(lambda: {"c": 0, "n": 0, "nx": 0, "x": 0, "xx": 0}))
    for (kind, elem, who, inst), v in delta.items():
        region, op = split(kind)
        d = out[(region, elem)][op]
        d["c"] += v["c"]
        d["n"] += v["n"]
        d["x"] += v["x"]
        d["nx"] = max(d["nx"], v["nx"])
        d["xx"] = max(d["xx"], v["xx"])
    return out


def region_row(region, elem, ops, secs, series_stats):
    mk = sum(ops[o]["c"] for o in OPS_MAKE if o in ops)
    gr = ops["grow"]["c"] if "grow" in ops else 0
    dr = sum(ops[o]["c"] for o in OPS_DROP if o in ops)
    # bytes the region asked its supply for: block capacity made and grown, in elements, times the element size
    # (per-object kinds: one element each)
    if region in ("list", "deque", "dict", "genpool", "densepool", "pool"):
        made = sum(ops[o]["n"] for o in ("make", "grow") if o in ops)
        asked = made * elem
        copied = (ops["grow"]["x"] * elem) if "grow" in ops else 0
    else:
        asked = mk * elem
        copied = 0
    slack = 0
    if "drop" in ops:
        slack = (ops["drop"]["x"] - ops["drop"]["n"]) * elem
    mean, peak, spread = series_stats
    live_now = mk - dr
    turnover = ""
    if mean and secs:
        turnover = f"{asked / secs / (mean * elem):.2f}" if elem else ""
    return [f"{region}", elem, fmt(mk), fmt(gr), fmt(dr), fmt(live_now),
            f"{mean:.1f}", fmt(peak), f"{spread:.1f}", fmt(asked), fmt(copied), fmt(slack), turnover]


def series_for(r, t_lo, t_hi):
    """per (region, elem): mean, peak and standard deviation of the live count over the samples in [t_lo, t_hi]."""
    vals = defaultdict(list)
    keys = set()
    for t, live in r.series:
        keys.update(live.keys())
    for t, live in r.series:
        if t_lo <= t <= t_hi:
            for k in keys:
                vals[k].append(live.get(k, 0))
    out = {}
    for k, v in vals.items():
        if not v:
            continue
        m = sum(v) / len(v)
        sd = math.sqrt(sum((x - m) ** 2 for x in v) / len(v))
        out[k] = (m, max(v), sd)
    return out


def report(r, only=None, show_actors=True):
    print(f"memtrace report: {len(r.total)} keys, {len(r.series)} samples, "
          f"{((r.t1 or 0) - (r.t0 or 0)) / r.hz:.2f} s, {len(r.phases)} phase marks"
          + (f", {len(r.supply)} supply lines (compared in row 431)" if r.supply else ""))
    bounds = [r.t0] + [p[1] for p in r.phases] + [r.t1]
    for i, (label, delta, secs) in enumerate(cuts(r)):
        if only and only not in label:
            continue
        lo = bounds[i] if r.phases else r.t0
        hi = bounds[i + 1] if r.phases else r.t1
        # with phases the first cut is the run's start up to the first mark
        if r.phases:
            lo, hi = (r.t0, r.phases[0][1]) if i == 0 else (r.phases[i - 1][1], (r.phases[i][1] if i < len(r.phases) else r.t1))
        print(f"\n#### phase: {label}   ({secs:.2f} s)")
        if not delta:
            print("   (no events)")
            continue
        st = series_for(r, lo, hi)
        # 1. per key
        rows = []
        agg = defaultdict(lambda: [0, 0, 0])
        for (kind, elem, who, inst), v in delta.items():
            a = agg[(kind, elem)]
            a[0] += v["c"]
            a[1] += v["n"]
            a[2] = max(a[2], v["nx"])
        for (kind, elem), (c, n, nx) in sorted(agg.items()):
            rows.append([kind, elem, fmt(c), fmt(c / secs), fmt(n), fmt(nx)])
        table("per key (all actors added)", ["kind", "elem", "events", "per second", "sum n", "max n"], rows)
        # 2. per region and element size
        reg = by_region(delta)
        rows = []
        for (region, elem), ops in sorted(reg.items()):
            if region in ("memory", "bufpool"):
                continue
            rows.append(region_row(region, elem, ops, secs, st.get((region, elem), (0, 0, 0))))
        table("per region and element size",
              ["region", "elem", "made", "grown", "dropped", "live end", "live mean", "live peak", "spread",
               "bytes asked", "bytes copied", "slack bytes", "turnover/s"], rows)
        # 3. pools, per instance
        rows = []
        inst_ops = defaultdict(lambda: defaultdict(lambda: {"c": 0, "n": 0, "nx": 0, "xl": 0, "xx": 0}))
        for (kind, elem, who, inst), v in delta.items():
            region, op = split(kind)
            if region in ("pool", "genpool", "densepool"):
                d = inst_ops[(region, elem, inst)][op]
                d["c"] += v["c"]
                d["nx"] = max(d["nx"], v["nx"])
                d["xx"] = max(d["xx"], v["xx"])
                d["xl"] = v["xl"]
                d["n"] += v["n"]
        for (region, elem, inst), ops in sorted(inst_ops.items()):
            tk = ops["take"]["c"] if "take" in ops else 0
            rt = ops["return"]["c"] if "return" in ops else 0
            gr = ops["grow"]["c"] if "grow" in ops else 0
            cap = max([ops[o]["nx"] for o in ("make", "grow") if o in ops] + [0])
            hwm = ops["take"]["nx"] if "take" in ops else 0
            depth = ops["return"]["xl"] if "return" in ops else (ops["take"]["xl"] if "take" in ops else 0)
            share = f"{100.0 * (1 - gr / tk):.1f}%" if tk else ""
            rows.append([f"{region}@{inst:x}", elem, fmt(tk), fmt(rt), fmt(tk - rt), fmt(hwm), fmt(gr), fmt(cap),
                         fmt(depth), share])
        table("pools, per instance (live is this phase's takes less returns)",
              ["pool", "cell", "takes", "returns", "net", "high-water", "growths", "capacity", "free depth",
               "met w/o supply"], rows)
        # 4. iobufs, by size and by actor
        rows = []
        by_size = defaultdict(lambda: {"take": 0, "return": 0, "out": 0})
        by_actor = defaultdict(lambda: {"take": 0, "return": 0})
        for (kind, elem, who, inst), v in delta.items():
            region, op = split(kind)
            if region == "iobuf":
                by_size[elem][op] += v["c"]
                by_size[elem]["out"] = max(by_size[elem]["out"], v["nx"] if v["nx"] < 1 << 62 else 0)
                by_actor[who][op] += v["c"]
        for elem, d in sorted(by_size.items()):
            rows.append([elem, fmt(d["take"]), fmt(d["return"]), fmt(d["take"] - d["return"]), fmt(d["out"])])
        table("iobufs, by block size", ["block", "taken", "returned", "net", "most out (any)"], rows)
        if show_actors:
            rows = [[("actor %d" % who) if who != 0xFFFFFFFFFFFFFFFF else "other", fmt(d["take"]), fmt(d["return"]),
                     fmt(d["take"] - d["return"])] for who, d in sorted(by_actor.items())]
            table("iobufs, by actor (taken there against returned there)", ["actor", "taken", "returned", "taken-returned"], rows)
            amsg = defaultdict(lambda: {"make": 0, "drop": 0})
            for (kind, elem, who, inst), v in delta.items():
                region, op = split(kind)
                if region == "msg":
                    amsg[who][op] += v["c"]
            rows = [[("actor %d" % who), fmt(d["make"]), fmt(d["drop"]), fmt(d["make"] - d["drop"])]
                    for who, d in sorted(amsg.items())]
            table("actor messages, by actor (sent from there against run there)",
                  ["actor", "sent", "run", "sent-run"], rows)
        # 4b. the IoBuf pool's own supply from the OS (no malloc shim sees it), and its caches
        ops = defaultdict(lambda: [0, 0])
        for (kind, elem, who, inst), v in delta.items():
            if kind.startswith("bufpool.") and kind != "bufpool.cache":
                ops[kind][0] += v["c"]
                ops[kind][1] += v["n"]
        rows = [[k, fmt(c), fmt(n), fmt(n / secs)] for k, (c, n) in sorted(ops.items())]
        res = ops["bufpool.reserve"][1]
        com = ops["bufpool.commit"][1]
        rel = ops["bufpool.release"][1]
        rows.append(["net committed less released (bytes)", "", fmt(com - rel), ""])
        cs = [b for t, b in r.cache_series if lo <= t <= hi]
        if cs:
            m = sum(cs) / len(cs)
            sd = math.sqrt(sum((x - m) ** 2 for x in cs) / len(cs))
            rows.append(["thread caches hold (bytes at samples: mean / peak / spread)", "",
                         f"{m:,.0f} / {max(cs):,} / {sd:,.0f}", ""])
        table("IoBuf pool (bufpool): OS supply and thread caches", ["what", "events", "bytes", "bytes/s"], rows)
        # 5. roll-ups by attribute
        for ai, aname in enumerate(("shape", "travel", "growth", "path"), start=1):
            groups = defaultdict(lambda: [0, 0, 0, 0])
            for (region, elem), ops in reg.items():
                if region not in ATTRS:
                    continue
                attr = ATTRS[region][ai]
                row = region_row(region, elem, ops, secs, (0, 0, 0))
                mk = sum(ops[o]["c"] for o in OPS_MAKE if o in ops)
                gr = ops["grow"]["c"] if "grow" in ops else 0
                dr = sum(ops[o]["c"] for o in OPS_DROP if o in ops)
                if region in ("list", "deque", "dict", "genpool", "densepool", "pool"):
                    asked = sum(ops[o]["n"] for o in ("make", "grow") if o in ops) * elem
                else:
                    asked = mk * elem
                g = groups[attr]
                g[0] += mk
                g[1] += gr
                g[2] += dr
                g[3] += asked
            rows = [[a, fmt(g[0]), fmt(g[1]), fmt(g[2]), fmt(g[0] - g[2]), fmt(g[3]),
                     f"{g[0] / secs:,.0f}"] for a, g in sorted(groups.items())]
            table(f"roll-up by {aname} (row 421)", [aname, "made", "grown", "dropped", "live end", "bytes asked",
                                                      "made per second"], rows)


def main(argv):
    paths = [a for a in argv if not a.startswith("--")]
    only = None
    if "--phase" in argv:
        only = argv[argv.index("--phase") + 1]
        paths = [p for p in paths if p != only]
    if not paths:
        print(__doc__)
        return 2
    report(load(paths), only, "--no-actors" not in argv)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
