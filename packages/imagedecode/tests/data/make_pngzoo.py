# An independent PNG writer in Python (zlib for the compression), for the decoder's
# cross-check: every colour type and bit depth, interlaced or not, scanline filters
# chosen at random per row, a tRNS where the type allows, with the pixels each file
# must decode to (worked out here from the specification's rules). Test data only.
import sys, zlib, struct, random

rnd = random.Random(20261012)

def chunk(tag, data):
    c = struct.pack(">I", len(data)) + tag + data
    return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc: return a
    if pb <= pc: return b
    return c

def filter_row(ft, cur, prev, bpp):
    out = bytearray([ft])
    for i, x in enumerate(cur):
        a = cur[i - bpp] if i >= bpp else 0
        b = prev[i] if prev else 0
        c = prev[i - bpp] if (prev and i >= bpp) else 0
        if ft == 0: p = 0
        elif ft == 1: p = a
        elif ft == 2: p = b
        elif ft == 3: p = (a + b) // 2
        else: p = paeth(a, b, c)
        out.append((x - p) & 255)
    return out

def pack_row(vals, depth):
    if depth == 16:
        return b"".join(struct.pack(">H", v) for v in vals)
    if depth == 8:
        return bytes(vals)
    bits = []
    out = bytearray()
    cur, n = 0, 0
    for v in vals:
        cur = (cur << depth) | v
        n += depth
        if n == 8:
            out.append(cur); cur, n = 0, 0
    if n: out.append(cur << (8 - n))
    return bytes(out)

ADAM = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]

def make(ct, depth, il, w, h):
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
    maxv = (1 << depth) - 1
    npal = (maxv + 1) if ct == 3 else 0
    pal = [(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(npal)]
    trns_pal = [rnd.randrange(256) for _ in range(min(npal, 5))] if ct == 3 else []
    samples = []
    for y in range(h):
        row = []
        for x in range(w):
            if rnd.random() < 0.5 and x > 0:
                row.append(row[-1])  # runs, so filters have something to predict
            else:
                row.append([rnd.randrange(maxv + 1) for _ in range(ch)])
        samples.append(row)
    key = None
    if ct in (0, 2) and (depth + il) % 2 == 0:
        key = tuple(samples[h // 2][w // 2])
    # the pixels each must decode to, as 8-bit or 16-bit RGBA
    out16 = depth == 16
    full = 65535 if out16 else 255
    exp = []
    for y in range(h):
        for x in range(w):
            s = samples[y][x]
            if ct == 3:
                i = s[0]
                a = trns_pal[i] if i < len(trns_pal) else 255
                px = (*pal[i], a)
            elif ct == 0:
                g = s[0] * full // maxv
                px = (g, g, g, 0 if key is not None and tuple(s) == key else full)
            elif ct == 4:
                g = s[0] * full // maxv
                px = (g, g, g, s[1] * full // maxv)
            elif ct == 2:
                px = tuple(v * full // maxv for v in s) + (0 if key is not None and tuple(s) == key else full,)
            else:
                px = tuple(v * full // maxv for v in s)
            exp.append(px)
    bpp = max(1, ch * depth // 8)
    raw = bytearray()
    def emit(rows):
        prev = None
        for vals in rows:
            cur = pack_row([v for px in vals for v in px], depth)
            ft = rnd.randrange(5)
            raw.extend(filter_row(ft, cur, prev, bpp))
            prev = cur
    if not il:
        emit(samples)
    else:
        for x0, y0, dx, dy in ADAM:
            rows = [[samples[y][x] for x in range(x0, w, dx)] for y in range(y0, h, dy)]
            rows = [r for r in rows if r]
            if rows: emit(rows)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ct, 0, 0, 1 if il else 0))
    if ct == 3:
        png += chunk(b"PLTE", bytes(c for p in pal for c in p))
        if trns_pal: png += chunk(b"tRNS", bytes(trns_pal))
    if key is not None:
        png += chunk(b"tRNS", b"".join(struct.pack(">H", k) for k in key))
    # split the data over several IDAT chunks, to test their joining
    z = zlib.compress(bytes(raw), 9)
    cut = max(1, len(z) // 3)
    for i in range(0, len(z), cut):
        png += chunk(b"IDAT", z[i:i + cut])
    png += chunk(b"IEND", b"")
    return png, exp, out16

recs = []
for ct in (0, 2, 3, 4, 6):
    for depth in (1, 2, 4, 8, 16):
        if ct == 3 and depth > 8: continue
        if ct in (2, 4, 6) and depth < 8: continue
        for il in (0, 1):
            w, h = (13, 9) if il else (11, 7)
            recs.append(make(ct, depth, il, w, h) + (w, h))

out = bytearray(struct.pack("<I", len(recs)))
for png, exp, out16, w, h in recs:
    out += struct.pack("<HHBBI", w, h, 16 if out16 else 8, 0, len(png)) + png
    for px in exp:
        for v in px:
            out += struct.pack(">H", v) if out16 else bytes([v])
sys.stdout.buffer.write(out)
