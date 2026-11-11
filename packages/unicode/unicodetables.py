"""Writes src/unicodetables.cone: the Unicode properties and simple case mappings
the unicode package looks up.

    python unicodetables.py <folder of UCD files>      (in packages/unicode)

The folder holds UnicodeData.txt, PropList.txt and DerivedCoreProperties.txt,
downloaded from https://www.unicode.org/Public/UCD/latest/ucd/ (or the same
file names under a numbered version folder); the version is read from the
header of PropList.txt and written into the output.

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
    flags = [0] * N
    for c in range(N):
        flags[c] = (dp["Alphabetic"][c] * 1 | dp["Uppercase"][c] * 2 | dp["Lowercase"][c] * 4
                    | pl["White_Space"][c] * 8 | numeric[c] * 16)
    out = "// unicodetables: Unicode %s properties and simple case mappings, as run tables.\n" % version
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
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "src", "unicodetables.cone"), "w", encoding="utf8", newline="\n") as f:
        f.write(out)
    # The totals tests/everychar.cone checks, over every code point but the surrogates
    nonsur = [c for c in range(N) if not 0xD800 <= c <= 0xDFFF]
    print("totals: alphabetic %d upper %d lower %d white %d numeric %d toUpper %d toLower %d" % (
        sum(flags[c] & 1 != 0 for c in nonsur), sum(flags[c] & 2 != 0 for c in nonsur),
        sum(flags[c] & 4 != 0 for c in nonsur), sum(flags[c] & 8 != 0 for c in nonsur),
        sum(flags[c] & 16 != 0 for c in nonsur), sum(c + upper[c] for c in nonsur),
        sum(c + lower[c] for c in nonsur)))
    print("Unicode", version, "flag runs", len(fs), "upper runs", len(us), "lower runs", len(ls))


main()
