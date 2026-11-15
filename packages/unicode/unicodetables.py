"""Writes src/unicodetables.cone: the Unicode properties and simple case mappings
the unicode package looks up.

    python unicodetables.py <folder of UCD files>      (in packages/unicode)

The folder holds UnicodeData.txt, PropList.txt, DerivedCoreProperties.txt and EastAsianWidth.txt,
downloaded from https://www.unicode.org/Public/UCD/latest/ucd/ (or the same
file names under a numbered version folder); the version is read from the
header of PropList.txt and written into the output. The folder also holds
CaseFolding.txt and DerivedNormalizationProps.txt of the same version (18.0.0, from
https://www.unicode.org/Public/UCD/latest/ucd/ on 14 Nov 2026, headers checked the same
way), and NormalizationTest.txt, which tests/data is cut from. The folder also holds
SpecialCasing.txt of the same version (the copy used was SpecialCasing-18.0.0.txt,
from https://www.unicode.org/Public/UCD/latest/ucd/ on 12 Nov 2026); its version
header must match PropList.txt's or the run stops. EastAsianWidth.txt (EastAsianWidth-18.0.0.txt,
same source and day) is checked the same way.

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
  foldStarts / foldDelta     CaseFolding.txt's C entries (common, one code point to one):
                             the run's change to a code point's number to fold it. 0
                             where there is none.
  foldFullKeys / foldFullVals   CaseFolding.txt's F entries (full, one to several): the
                             sorted code points, and three u32 for each, padded with 0.
                             The S (simple, where an F entry exists) and T (Turkic)
                             entries are not read.
  cccStarts / cccVals        canonical combining class (UnicodeData field 3), a u8 for each run.
  decompKeys / decompOffs / decompPool   the full (recursive) canonical decomposition of
                             every code point that has one, Hangul syllables apart (the
                             algorithm does them): the sorted code points, an offset
                             into the pool for each (one more at the end), and the pool
                             of u32 code points.
  compFirst / compSecond / compResult   the primary composites: each direct canonical
                             decomposition of two code points that is not in
                             Full_Composition_Exclusion (DerivedNormalizationProps.txt),
                             sorted by (first, second).
  nfcQcStarts / nfcQcVals    NFC_QC (DerivedNormalizationProps.txt), a u8 for each run:
                             0 Yes, 1 Maybe, 2 No.
  widthStarts / widthVals    the display width of a code point in terminal columns,
                             one u8 for each run: 2 for East Asian Width W and F, else
                             0 for general category Mn, Me, Cf and the controls Cc
                             (C0 and C1), else 1 (Ambiguous counts as 1, as terminals
                             do). Per code point: no grapheme clusters.
  graphemeStarts / graphemeVals   the grapheme-cluster properties of UAX #29, a u8 for each
                             run: bits 0-3 Grapheme_Cluster_Break (0 Other, 1 CR, 2 LF,
                             3 Control, 4 Extend, 5 ZWJ, 6 Regional_Indicator, 7 Prepend,
                             8 SpacingMark, 9 L, 10 V, 11 T, 12 LV, 13 LVT), 16 is
                             Extended_Pictographic, bits 5-6 Indic_Conjunct_Break (0 None,
                             1 Consonant, 2 Linker, 3 Extend). From GraphemeBreakProperty.txt
                             (ucd/auxiliary/), emoji-data.txt (ucd/emoji/) and the InCB lines of
                             DerivedCoreProperties.txt, same version (headers checked, 18.0.0,
                             from https://www.unicode.org/Public/UCD/latest/ucd/ on 14 Nov 2026).
                             GraphemeBreakTest.txt (ucd/auxiliary/) is what tests/data/graphemebreak.txt
                             is cut from (tests/data/cut_graphemebreak.py), and is version-checked too.
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


def read_width(unidata, eaw):
    """Display width of every code point: 2 (EAW W, F), 0 (Mn, Me, Cf, Cc), else 1."""
    zero = [False] * N
    first = None
    for line in open(unidata, encoding="utf8"):
        f = line.rstrip("\n").split(";")
        c = int(f[0], 16)
        if f[1].endswith(", First>"):
            first = c
            continue
        cps = range(first, c + 1) if f[1].endswith(", Last>") else [c]
        if f[2] in ("Mn", "Me", "Cf", "Cc"):
            for x in cps:
                zero[x] = True
    wide = [False] * N
    for line in open(eaw, encoding="utf8"):
        line = line.split("#")[0].strip()
        if not line:
            continue
        a, p = [x.strip() for x in line.split(";")[:2]]
        if p in ("W", "F"):
            lo, _, hi = a.partition("..")
            for c in range(int(lo, 16), int(hi or lo, 16) + 1):
                wide[c] = True
    return [2 if wide[c] else 0 if zero[c] else 1 for c in range(N)]


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


def read_fold(path):
    """CaseFolding.txt: ({cp: delta} for C, {cp: [cps]} for F)."""
    common, full = {}, {}
    for line in open(path, encoding="utf8"):
        line = line.split("#")[0].strip()
        if not line:
            continue
        f = [x.strip() for x in line.split(";")]
        c = int(f[0], 16)
        m = [int(x, 16) for x in f[2].split()]
        if f[1] == "C":
            common[c] = m[0] - c
        elif f[1] == "F":
            if len(m) > 3:
                sys.exit("fold of %04X does not fit" % c)
            full[c] = m
    return common, full


def read_norm(unidata):
    """(ccc per code point, direct canonical decomposition {cp: [cps]}). The First/Last ranges
    (CJK, Hangul syllables, private use) have no ccc and no decomposition table entry."""
    ccc = [0] * N
    direct = {}
    for line in open(unidata, encoding="utf8"):
        f = line.rstrip("\n").split(";")
        c = int(f[0], 16)
        if f[1].endswith(", First>") or f[1].endswith(", Last>"):
            continue
        ccc[c] = int(f[3])
        if f[5] and not f[5].startswith("<"):
            direct[c] = [int(x, 16) for x in f[5].split()]
    return ccc, direct


def read_normprops(path):
    """(set of Full_Composition_Exclusion, NFC_QC value per code point: 0 Y, 1 M, 2 N)."""
    excl = set()
    qc = [0] * N
    for line in open(path, encoding="utf8"):
        line = line.split("#")[0].strip()
        if not line:
            continue
        f = [x.strip() for x in line.split(";")]
        lo, _, hi = f[0].partition("..")
        r = range(int(lo, 16), int(hi or lo, 16) + 1)
        if f[1] == "Full_Composition_Exclusion":
            excl.update(r)
        elif f[1] == "NFC_QC":
            for c in r:
                qc[c] = 1 if f[2] == "M" else 2
    return excl, qc


GCB = {"Other": 0, "CR": 1, "LF": 2, "Control": 3, "Extend": 4, "ZWJ": 5, "Regional_Indicator": 6,
       "Prepend": 7, "SpacingMark": 8, "L": 9, "V": 10, "T": 11, "LV": 12, "LVT": 13}
INCB = {"Consonant": 1, "Linker": 2, "Extend": 3}


def read_grapheme(d, version):
    """Per code point a u8: GCB in bits 0-3, 16 Extended_Pictographic, InCB in bits 5-6 (32, 64)."""
    for fn in ("GraphemeBreakProperty", "GraphemeBreakTest"):
        fv = re.search(fn + r"-([0-9.]+)\.txt", open(os.path.join(d, fn + ".txt"), encoding="utf8").readline()).group(1)
        if fv != version:
            sys.exit("%s is %s but PropList is %s" % (fn, fv, version))
    ev = re.search(r"# Version: ([0-9.]+)", open(os.path.join(d, "emoji-data.txt"), encoding="utf8").read()).group(1)
    if ev != version:
        sys.exit("emoji-data is %s but PropList is %s" % (ev, version))
    out = [0] * N

    def fields(name):
        for line in open(os.path.join(d, name), encoding="utf8"):
            line = line.split("#")[0].strip()
            if line:
                f = [x.strip() for x in line.split(";")]
                lo, _, hi = f[0].partition("..")
                yield range(int(lo, 16), int(hi or lo, 16) + 1), f
    for r, f in fields("GraphemeBreakProperty.txt"):
        for c in r:
            out[c] |= GCB[f[1]]
    for r, f in fields("emoji-data.txt"):
        if f[1] == "Extended_Pictographic":
            for c in r:
                out[c] |= 16
    for r, f in fields("DerivedCoreProperties.txt"):
        if f[1] == "InCB":
            for c in r:
                out[c] |= INCB[f[2]] << 5
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
    ev = re.search(r"EastAsianWidth-([0-9.]+)\.txt", open(os.path.join(d, "EastAsianWidth.txt"), encoding="utf8").readline()).group(1)
    if ev != version:
        sys.exit("EastAsianWidth is %s but PropList is %s" % (ev, version))
    width = read_width(os.path.join(d, "UnicodeData.txt"), os.path.join(d, "EastAsianWidth.txt"))
    for fn in ("CaseFolding", "DerivedNormalizationProps"):
        fv = re.search(fn + r"-([0-9.]+)\.txt", open(os.path.join(d, fn + ".txt"), encoding="utf8").readline()).group(1)
        if fv != version:
            sys.exit("%s is %s but PropList is %s" % (fn, fv, version))
    fold_c, fold_f = read_fold(os.path.join(d, "CaseFolding.txt"))
    fold = [fold_c.get(c, 0) for c in range(N)]
    ccc, direct = read_norm(os.path.join(d, "UnicodeData.txt"))
    excl, qc = read_normprops(os.path.join(d, "DerivedNormalizationProps.txt"))
    full_decomp = {}

    def decomp(c):
        if c not in direct:
            return [c]
        if c not in full_decomp:
            full_decomp[c] = [y for x in direct[c] for y in decomp(x)]
        return full_decomp[c]
    dkeys = sorted(direct)
    dpool, doffs = [], []
    for c in dkeys:
        doffs.append(len(dpool))
        dpool.extend(decomp(c))
    doffs.append(len(dpool))
    pairs = sorted((m[0], m[1], c) for c, m in direct.items() if len(m) == 2 and c not in excl)
    flags = [0] * N
    for c in range(N):
        flags[c] = (dp["Alphabetic"][c] * 1 | dp["Uppercase"][c] * 2 | dp["Lowercase"][c] * 4
                    | pl["White_Space"][c] * 8 | numeric[c] * 16)
    out = "// unicodetables: Unicode %s properties and simple case mappings and display widths, as run\n// tables, the unconditional SpecialCasing expansions, case folding and the normalization data.\n" % version
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
    ws, wv = runs(width)
    out += emit("widthStarts", "u32", ws, "u32", 8, True)
    out += emit("widthVals", "u8", wv, "u8", 16, False)
    for name, tbl in (("specialUpper", sp_up), ("specialLower", sp_lo)):
        out += emit(name + "Keys", "u32", sorted(tbl), "u32", 8, True)
        vals = [(tbl[c] + [0, 0, 0])[i] for c in sorted(tbl) for i in range(3)]
        out += emit(name + "Vals", "u32", vals, "u32", 8, True)
    fs2, fv2 = runs(fold)
    out += emit("foldStarts", "u32", fs2, "u32", 8, True)
    out += emit("foldDelta", "i32", fv2, "i32", 12, False)
    out += emit("foldFullKeys", "u32", sorted(fold_f), "u32", 8, True)
    out += emit("foldFullVals", "u32", [(fold_f[c] + [0, 0, 0])[i] for c in sorted(fold_f) for i in range(3)], "u32", 8, True)
    cs, cv = runs(ccc)
    out += emit("cccStarts", "u32", cs, "u32", 8, True)
    out += emit("cccVals", "u8", cv, "u8", 16, False)
    out += emit("decompKeys", "u32", dkeys, "u32", 8, True)
    out += emit("decompOffs", "u32", doffs, "u32", 12, False)
    out += emit("decompPool", "u32", dpool, "u32", 8, True)
    out += emit("compFirst", "u32", [p[0] for p in pairs], "u32", 8, True)
    out += emit("compSecond", "u32", [p[1] for p in pairs], "u32", 8, True)
    out += emit("compResult", "u32", [p[2] for p in pairs], "u32", 8, True)
    qs, qv = runs(qc)
    out += emit("nfcQcStarts", "u32", qs, "u32", 8, True)
    out += emit("nfcQcVals", "u8", qv, "u8", 16, False)
    gs, gv = runs(read_grapheme(d, version))
    out += emit("graphemeStarts", "u32", gs, "u32", 8, True)
    out += emit("graphemeVals", "u8", gv, "u8", 16, False)
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
    print("width totals (tests/width.cone): zero %d wide %d" % (
        sum(width[c] == 0 for c in nonsur), sum(width[c] == 2 for c in nonsur)))
    print("fold: C runs %d F entries %d; ccc runs %d; decomposable %d pool %d; composition pairs %d; nfc_qc runs %d" % (
        len(fs2), len(fold_f), len(cs), len(dkeys), len(dpool), len(pairs), len(qs)))
    print("grapheme runs %d" % len(gs))
    print("special: upper %d lower %d" % (len(sp_up), len(sp_lo)))
    print("Unicode", version, "width runs", len(ws), "flag runs", len(fs), "upper runs", len(us), "lower runs", len(ls))


main()
