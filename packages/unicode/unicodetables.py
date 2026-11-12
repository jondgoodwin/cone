"""Writes src/unicodetables.cone: the Unicode properties and simple case mappings
the unicode package looks up.

    python unicodetables.py <folder of UCD files>      (in packages/unicode)

The folder holds UnicodeData.txt, PropList.txt and DerivedCoreProperties.txt,
downloaded from https://www.unicode.org/Public/UCD/latest/ucd/ (or the same
file names under a numbered version folder); the version is read from the
header of PropList.txt and written into the output. The folder also holds
SpecialCasing.txt of the same version (the copy used was SpecialCasing-18.0.0.txt,
from https://www.unicode.org/Public/UCD/latest/ucd/ on 12 Nov 2026); its version
header must match PropList.txt's or the run stops.

Every property is a run table: the code points 0 to 0x10FFFF are cut into runs
of equal value, and a table holds each run's first code point (sorted), with
the value of the run beside it. A lookup is a binary search for the last start
that is not above the code point.

  flagStarts / flagBits   one u8 of bits for each run, all five properties:
                          1 Alphabetic (DerivedCoreProperties), 2 Uppercase,
                          4 Lowercase (both DerivedCoreProperties), 8 White_Space
                          (PropList), 16 Numeric (general category Nd, Nl or No,
                          UnicodeData field 2, as Rust's char::is_numeric)
  upperStarts / upperDelta   the run's change to a code point's number to
  lowerStarts / lowerDelta   make its upper or lower case: UnicodeData's simple
                             mapping (fields 12 and 13, one code point to one).
                             0 where there is none. The multi-character mappings
                             of SpecialCasing.txt (U+00DF to "SS") are not here.
  specialUpperKeys / specialUpperVals    SpecialCasing.txt's unconditional entries
  specialLowerKeys / specialLowerVals    (those with no condition field), for the
                             text-level toUpper and toLower: the sorted code points
                             whose full mapping differs from the simple one, and
                             three u32 for each, the full mapping padded with 0 (no
                             mapping contains U+0000). The conditional entries
                             (final sigma, the lt, tr and az rules) are left out.
                             The title-case column is not used.
"""

import os
import re
import sys

N = 0x110000


def read_props(path, wanted):
    out = {w: [False] * N for w in wanted}
    for line in open(path, encoding="utf8"):
        line = line.split("#")[0].strip()
        if not line:
            continue
        a, p = [x.strip() for x in line.split(";")[:2]]
        if p in out:
            lo, _, hi = a.partition("..")
            for c in range(int(lo, 16), int(hi or lo, 16) + 1):
                out[p][c] = True
    return out


def read_unicodedata(path):
    numeric = [False] * N
    upper = [0] * N
    lower = [0] * N
    first = None
    for line in open(path, encoding="utf8"):
        f = line.rstrip("\n").split(";")
        c = int(f[0], 16)
        if f[1].endswith(", First>"):
            first = c
            continue
        cps = range(first, c + 1) if f[1].endswith(", Last>") else [c]
        for x in cps:
            if f[2] in ("Nd", "Nl", "No"):
                numeric[x] = True
        if f[12]:
            upper[c] = int(f[12], 16) - c
        if f[13]:
            lower[c] = int(f[13], 16) - c
    return numeric, upper, lower


def read_special(path):
    """The unconditional entries: {code point: (lower, upper)}, each a list of code points."""
    out = {}
    for line in open(path, encoding="utf8"):
        line = line.split("#")[0].strip()
        if not line:
            continue
        f = [x.strip() for x in line.split(";")]
        # code; lower; title; upper; [condition;]  (the trailing ; leaves an empty last field)
        if len(f) != 5 or f[4] != "":
            continue
        out[int(f[0], 16)] = ([int(x, 16) for x in f[1].split()], [int(x, 16) for x in f[3].split()])
    return out


def runs(values):
    starts, vals = [], []
    for c, v in enumerate(values):
        if not vals or vals[-1] != v:
            starts.append(c)
            vals.append(v)
    return starts, vals


def emit(name, ty, items, suffix, per_line, hexa):
    if hexa:
        texts = ["0x%06X%s" % (v, suffix) for v in items]
    else:
        texts = ["%d%s" % (v, suffix) for v in items]
    lines = [", ".join(texts[i:i + per_line]) for i in range(0, len(texts), per_line)]
    # No comma after the last entry: the grammar does not take one
    return "let %s: Array[%s, %d] = [\n  %s\n];\n\n" % (name, ty, len(items), ",\n  ".join(lines))


def main():
    d = sys.argv[1]
    version = re.search(r"PropList-([0-9.]+)\.txt", open(os.path.join(d, "PropList.txt"), encoding="utf8").readline()).group(1)
    dp = read_props(os.path.join(d, "DerivedCoreProperties.txt"), ["Alphabetic", "Uppercase", "Lowercase"])
    pl = read_props(os.path.join(d, "PropList.txt"), ["White_Space"])
    numeric, upper, lower = read_unicodedata(os.path.join(d, "UnicodeData.txt"))
    special = read_special(os.path.join(d, "SpecialCasing.txt"))
    sv = re.search(r"SpecialCasing-([0-9.]+)\.txt", open(os.path.join(d, "SpecialCasing.txt"), encoding="utf8").readline()).group(1)
    if sv != version:
        sys.exit("SpecialCasing is %s but PropList is %s" % (sv, version))
    sp_up, sp_lo = {}, {}
    for c, (lo, up) in sorted(special.items()):
        for tbl, full, simple in ((sp_up, up, c + upper[c]), (sp_lo, lo, c + lower[c])):
            if full == [simple]:
                continue
            if len(full) > 3 or 0 in full:
                sys.exit("special mapping of %04X does not fit" % c)
            tbl[c] = full
    flags = [0] * N
    for c in range(N):
        flags[c] = (dp["Alphabetic"][c] * 1 | dp["Uppercase"][c] * 2 | dp["Lowercase"][c] * 4
                    | pl["White_Space"][c] * 8 | numeric[c] * 16)
    out = "// unicodetables: Unicode %s properties and simple case mappings, as run tables,\n// and the unconditional SpecialCasing expansions.\n" % version
    out += "// Written by unicodetables.py, which says how each is made; change that and\n"
    out += "// run it, never this file.\n\n"
    fs, fv = runs(flags)
    out += emit("flagStarts", "u32", fs, "u32", 8, True)
    out += emit("flagBits", "u8", fv, "u8", 16, False)
    us, uv = runs(upper)
    out += emit("upperStarts", "u32", us, "u32", 8, True)
    out += emit("upperDelta", "i32", uv, "i32", 12, False)
    ls, lv = runs(lower)
    out += emit("lowerStarts", "u32", ls, "u32", 8, True)
    out += emit("lowerDelta", "i32", lv, "i32", 12, False)
    for name, tbl in (("specialUpper", sp_up), ("specialLower", sp_lo)):
        out += emit(name + "Keys", "u32", sorted(tbl), "u32", 8, True)
        vals = [(tbl[c] + [0, 0, 0])[i] for c in sorted(tbl) for i in range(3)]
        out += emit(name + "Vals", "u32", vals, "u32", 8, True)
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "src", "unicodetables.cone"), "w", encoding="utf8", newline="\n") as f:
        f.write(out)
    # The totals tests/everychar.cone checks, over every code point but the surrogates
    nonsur = [c for c in range(N) if not 0xD800 <= c <= 0xDFFF]
    print("totals: alphabetic %d upper %d lower %d white %d numeric %d toUpperSimple %d toLowerSimple %d" % (
        sum(flags[c] & 1 != 0 for c in nonsur), sum(flags[c] & 2 != 0 for c in nonsur),
        sum(flags[c] & 4 != 0 for c in nonsur), sum(flags[c] & 8 != 0 for c in nonsur),
        sum(flags[c] & 16 != 0 for c in nonsur), sum(c + upper[c] for c in nonsur),
        sum(c + lower[c] for c in nonsur)))
    print("special: upper %d lower %d" % (len(sp_up), len(sp_lo)))
    print("Unicode", version, "flag runs", len(fs), "upper runs", len(us), "lower runs", len(ls))


main()
