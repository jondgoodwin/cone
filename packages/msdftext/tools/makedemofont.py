#!/usr/bin/env python3
"""Writes the demo fonts of the msdftext package as Cone source.

    python tools/makedemofont.py            rewrite src/demofont.cone

The fonts are made here, from nothing, so that they carry no licence but this
script's: geometric glyphs of the author's own drawing, dedicated to the
public domain (CC0). Python standard library only.

Six glyphs, 1000 units to the em, advances 500 300 400 600 700 700:

    0 .notdef   empty
    1 space     empty                          U+0020
    2 I         a bar 100..300 x 0..700        U+0049
    3 A         a triangle (0,0) (250,700) (500,0)   U+0041
    4 O         a rounded square 50..650 x 0..700 of quadratics, with a
                counter 200..500 x 150..550    U+004F
    5 i         a composite: the bar of glyph 2 scaled by 1/2 and moved by
                (200, 0), so 250..350 x 0..350 U+0069

The demo font (in src) takes cmap format 4 with a delta segment and a segment
by glyph-id array, and GPOS kerning: lookup type 2 format 1 (A then I: -60) and
a type 9 extension holding format 2 (O then I: -30, O then A: -20). The
test font (in tests) takes cmap format 12 and the legacy kern table
(A I -60, O I -30, I A +10).
"""
import struct, sys, os

def u16(v): return struct.pack(">H", v & 0xFFFF)
def i16(v): return struct.pack(">h", v)
def u32(v): return struct.pack(">I", v & 0xFFFFFFFF)

ADV = [500, 300, 400, 600, 700, 700]
LSB = [0, 0, 100, 0, 50, 250]
NUM_H_METRICS = 5

def simple(contours):
    """contours: list of lists of (x, y, on)."""
    xs = [p[0] for c in contours for p in c]
    ys = [p[1] for c in contours for p in c]
    out = i16(len(contours)) + i16(min(xs)) + i16(min(ys)) + i16(max(xs)) + i16(max(ys))
    n = 0
    for c in contours:
        n += len(c)
        out += u16(n - 1)
    out += u16(0)  # no instructions
    pts = [p for c in contours for p in c]
    out += bytes([1 if p[2] else 0 for p in pts])
    px = py = 0
    for p in pts:
        out += i16(p[0] - px); px = p[0]
    for p in pts:
        out += i16(p[1] - py); py = p[1]
    return out

bar = [(100, 0, 1), (100, 700, 1), (300, 700, 1), (300, 0, 1)]
tri = [(0, 0, 1), (250, 700, 1), (500, 0, 1)]
outer = [(50, 350, 1), (50, 700, 0), (350, 700, 1), (650, 700, 0), (650, 350, 1), (650, 0, 0), (350, 0, 1), (50, 0, 0)]
inner = [(200, 350, 1), (200, 150, 0), (350, 150, 1), (500, 150, 0), (500, 350, 1), (500, 550, 0), (350, 550, 1), (200, 550, 0)]

def composite():
    # ARG_1_AND_2_ARE_WORDS | ARGS_ARE_XY_VALUES | WE_HAVE_A_SCALE, the last component
    out = i16(-1) + i16(250) + i16(0) + i16(350) + i16(350)
    out += u16(1 | 2 | 8) + u16(2) + i16(200) + i16(0) + i16(8192)  # 0.5 in F2Dot14
    return out

def pad4(b): return b + b"\0" * ((-len(b)) % 4)

GLYPHS = [b"", b"", simple([bar]), simple([tri]), simple([outer, inner]), composite()]

def head(bbox):
    return (u32(0x00010000) + u32(0x00010000) + u32(0) + u32(0x5F0F3CF5) + u16(0) + u16(1000) + b"\0" * 16
            + i16(bbox[0]) + i16(bbox[1]) + i16(bbox[2]) + i16(bbox[3]) + u16(0) + u16(8) + i16(2) + i16(1) + i16(0))

def maxp(): return u32(0x00010000) + u16(6) + b"\0" * 26
def hhea():
    return (u32(0x00010000) + i16(800) + i16(-200) + i16(100) + u16(700) + i16(0) + i16(0) + i16(700)
            + i16(1) + i16(0) + i16(0) + b"\0" * 8 + i16(0) + u16(NUM_H_METRICS))

def hmtx():
    out = b"".join(u16(ADV[g]) + i16(LSB[g]) for g in range(NUM_H_METRICS))
    out += b"".join(i16(LSB[g]) for g in range(NUM_H_METRICS, 6))
    return out

def cmap4():
    segs = [(0x20, 0x20, 1 - 0x20, None), (0x41, 0x41, 3 - 0x41, None), (0x49, 0x4F, 0, [2, 0, 0, 0, 0, 0, 4]),
            (0x69, 0x69, 5 - 0x69, None), (0xFFFF, 0xFFFF, 1, None)]
    n = len(segs)
    arr = []
    ro = []
    for i, (s, e, d, g) in enumerate(segs):
        if g is None: ro.append(0)
        else: ro.append((n - i) * 2 + len(arr) * 2); arr += g
    body = u16(4) + u16(0) + u16(0) + u16(n * 2) + u16(0) + u16(0) + u16(0)
    body += b"".join(u16(s[1]) for s in segs) + u16(0) + b"".join(u16(s[0]) for s in segs)
    body += b"".join(u16(s[2]) for s in segs) + b"".join(u16(r) for r in ro) + b"".join(u16(a) for a in arr)
    body = body[:2] + u16(len(body)) + body[4:]
    return u16(0) + u16(1) + u16(3) + u16(1) + u32(12) + body

def cmap12():
    groups = [(0x20, 0x20, 1), (0x41, 0x41, 3), (0x49, 0x49, 2), (0x4F, 0x4F, 4), (0x69, 0x69, 5)]
    body = u16(12) + u16(0) + u32(16 + 12 * len(groups)) + u32(0) + u32(len(groups))
    body += b"".join(u32(a) + u32(b) + u32(g) for a, b, g in groups)
    return u16(0) + u16(1) + u16(3) + u16(10) + u32(12) + body

def name():
    recs = [(1, "MsdfDemo"), (2, "Regular"),
            (13, "Made for the msdftext package's tests. Public domain (CC0 1.0): no rights reserved."),
            (14, "https://creativecommons.org/publicdomain/zero/1.0/")]
    strs = b""
    out = u16(0) + u16(len(recs)) + u16(6 + 12 * len(recs))
    for nid, s in recs:
        enc = s.encode("utf-16-be")
        out += u16(3) + u16(1) + u16(0x409) + u16(nid) + u16(len(enc)) + u16(len(strs))
        strs += enc
    return out + strs

def coverage(glyphs): return u16(1) + u16(len(glyphs)) + b"".join(u16(g) for g in glyphs)

def gpos():
    # Subtable 1: PairPos format 1, A (3) then I (2): XAdvance -60
    # layout: format, coverageOffset, vf1, vf2, pairSetCount, pairSetOffsets[1]
    cov1 = coverage([3])
    pairset = u16(1) + u16(2) + i16(-60)
    sub1 = u16(1) + u16(12) + u16(4) + u16(0) + u16(1) + u16(12 + len(cov1)) + cov1 + pairset
    # Subtable 2: PairPos format 2, O (4) then class: I -> 1: -30, A -> 2: -20
    cov2 = coverage([4])
    cd1 = u16(2) + u16(1) + u16(4) + u16(4) + u16(1)               # O = class 1
    cd2 = u16(1) + u16(2) + u16(2) + u16(1) + u16(2)               # glyphs 2, 3 = classes 1, 2
    recs = b"".join(i16(v) for v in [0, 0, 0, 0, -30, -20])        # class1 0: three; class1 1: three
    head2 = 16
    # Offsets: coverage, classDef1, classDef2 (from the subtable), after the 16-byte header and the records
    sub2 = (u16(2) + u16(head2 + len(recs)) + u16(4) + u16(0)
            + u16(head2 + len(recs) + len(cov2)) + u16(head2 + len(recs) + len(cov2) + len(cd1))
            + u16(2) + u16(3) + recs + cov2 + cd1 + cd2)
    # Lookup 0: type 2, one subtable
    lk0 = u16(2) + u16(0) + u16(1) + u16(8) + sub1
    # Lookup 1: type 9 extension -> the type 2 format 2 subtable
    ext = u16(1) + u16(2) + u32(8) + sub2
    lk1 = u16(9) + u16(0) + u16(1) + u16(8) + ext
    lookups = u16(2) + u16(6) + u16(6 + len(lk0)) + lk0 + lk1
    feature = u16(0) + u16(2) + u16(0) + u16(1)
    features = u16(1) + b"kern" + u16(8) + feature
    langsys = u16(0) + u16(0xFFFF) + u16(1) + u16(0)
    script = u16(4) + u16(0) + langsys
    scripts = u16(1) + b"DFLT" + u16(8) + script
    off_scripts = 10
    off_features = off_scripts + len(scripts)
    off_lookups = off_features + len(features)
    return u32(0x00010000) + u16(off_scripts) + u16(off_features) + u16(off_lookups) + scripts + features + lookups

def kern():
    pairs = sorted([((3 << 16) | 2, -60), ((4 << 16) | 2, -30), ((2 << 16) | 3, 10)])
    n = len(pairs)
    sub = u16(0) + u16(14 + 6 * n) + u16(1) + u16(n) + u16(0) + u16(0) + u16(0)
    sub += b"".join(u32(k) + i16(v) for k, v in pairs)
    return u16(0) + u16(1) + sub

def font(cmap, with_gpos, with_kern):
    glyf = b""
    loca = b""
    for g in GLYPHS:
        loca += u32(len(glyf))
        glyf += pad4(g)
    loca += u32(len(glyf))
    tables = {b"cmap": cmap, b"glyf": glyf, b"head": head((0, 0, 650, 700)), b"hhea": hhea(), b"hmtx": hmtx(),
              b"loca": loca, b"maxp": maxp(), b"name": name()}
    if with_gpos: tables[b"GPOS"] = gpos()
    if with_kern: tables[b"kern"] = kern()
    tags = sorted(tables)
    n = len(tags)
    out = u32(0x00010000) + u16(n) + u16(0) + u16(0) + u16(0)
    off = 12 + 16 * n
    body = b""
    for t in tags:
        data = pad4(tables[t])
        s = sum(struct.unpack(">%dI" % (len(data) // 4), data)) & 0xFFFFFFFF
        out += t + u32(s) + u32(off + len(body)) + u32(len(tables[t]))
        body += data
    return out + body

def cone_array(name, data, doc, public):
    lines = []
    row = []
    for b in data:
        row.append("%du8" % b)
        if len(row) == 24:
            lines.append("  " + ", ".join(row) + ","); row = []
    if row: lines.append("  " + ", ".join(row) + ",")
    lines[-1] = lines[-1].rstrip(",")
    return "%s%simm %s Array[u8, %d] = [\n%s\n];\n" % (doc, "pub " if public else "", name, len(data), "\n".join(lines))

if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    demo = font(cmap4(), True, False)
    test = font(cmap12(), False, True)
    doc = ("// The demo font: six geometric glyphs made by tools/makedemofont.py, public domain (CC0 1.0).\n"
           "// I (a bar), A (a triangle), O (a rounded square with a counter, of quadratics), i (a composite)\n"
           "// and a space, 1000 units to the em; GPOS kerning. Written by the tool: do not edit.\n")
    doc2 = "// The same glyphs with cmap format 12 and the legacy kern table, for tests of those readers.\n"
    with open(os.path.join(here, "..", "src", "demofont.cone"), "w", newline="\n") as f:
        f.write(cone_array("DEMO_FONT", demo, doc, True) + "\n" + cone_array("DEMO_FONT_KERN", test, doc2, True))
    print(len(demo), len(test))
