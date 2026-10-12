# MAKES: b440 b411 bmix b441 b4h3 (.jpg and .png). Run AFTER make_jpeg.py, same folder (it reuses that script's picture).
# RUN (in this folder):  python make_jpeg2.py .
# Synthetic pictures only. Pillow (and numpy) are needed ONLY to regenerate, never to run the tests.
# A minimal baseline JPEG encoder for the sampling layouts Pillow cannot write
# (4:4:0, 4:1:1, mixed ones) and for restart markers on them. Tables are taken
# from a Pillow-saved file (the standard Annex K tables). The oracle for what
# these files decode to is Pillow (libjpeg-turbo) again. Test data only.
import sys, os, io, math, struct
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
out = sys.argv[1]

def segments(data):
    i = 2
    while i < len(data):
        assert data[i] == 0xFF
        m = data[i + 1]
        if m == 0xDA:
            return
        n = struct.unpack(">H", data[i + 2:i + 4])[0]
        yield m, data[i + 4:i + 2 + n]
        i += 2 + n

buf = io.BytesIO()
Image.new("RGB", (16, 16), (200, 100, 50)).save(buf, "JPEG", quality=90)
dqt = []
dht = []
for m, body in segments(buf.getvalue()):
    if m == 0xDB:
        j = 0
        while j < len(body):
            dqt.append((body[j] & 15, list(body[j + 1:j + 65])))
            j += 65
    if m == 0xC4:
        j = 0
        while j < len(body):
            tc_th = body[j]
            counts = list(body[j + 1:j + 17])
            n = sum(counts)
            vals = list(body[j + 17:j + 17 + n])
            dht.append((tc_th, counts, vals))
            j += 17 + n

ZZ = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
      35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63]

# Huffman encode tables: symbol -> (code, length)
def build(counts, vals):
    codes = {}
    code = 0
    k = 0
    for l in range(1, 17):
        for _ in range(counts[l - 1]):
            codes[vals[k]] = (code, l)
            code += 1
            k += 1
        code <<= 1
    return codes

tabs = {}
for tc_th, counts, vals in dht:
    tabs[tc_th] = build(counts, vals)
qtabs = {i: t for i, t in dqt}  # zigzag order

C = np.zeros((8, 8))
for u in range(8):
    for x in range(8):
        C[u, x] = (math.sqrt(1 / 8) if u == 0 else math.sqrt(2 / 8)) * math.cos((2 * x + 1) * u * math.pi / 16)

class Bits:
    def __init__(self): self.b = bytearray(); self.acc = 0; self.n = 0
    def put(self, code, l):
        self.acc = (self.acc << l) | code; self.n += l
        while self.n >= 8:
            byte = (self.acc >> (self.n - 8)) & 255
            self.b.append(byte)
            if byte == 0xFF: self.b.append(0)
            self.n -= 8
        self.acc &= (1 << self.n) - 1
    def flush(self):
        if self.n: self.put((1 << (8 - self.n)) - 1, 8 - self.n)

def size_bits(v):
    a = abs(v); s = 0
    while a: s += 1; a >>= 1
    return s, (v if v >= 0 else v + (1 << s) - 1)

def encode(im, name, sampling, restart=0):
    # sampling: [(h, v)] for Y, Cb, Cr
    w, h = im.size
    ycc = np.asarray(im.convert("YCbCr"), dtype=np.float64)
    hmax = max(s[0] for s in sampling); vmax = max(s[1] for s in sampling)
    mcux = -(-w // (8 * hmax)); mcuy = -(-h // (8 * vmax))
    planes = []
    for c, (hh, vv) in enumerate(sampling):
        pw = mcux * hh * 8; ph = mcuy * vv * 8
        full = np.pad(ycc[:, :, c], ((0, mcuy * vmax * 8 - h), (0, mcux * hmax * 8 - w)), mode="edge")
        fx = hmax // hh; fy = vmax // vv
        p = full.reshape(ph, fy, pw, fx).mean(axis=(1, 3)) if (fx > 1 or fy > 1) else full
        planes.append(p)
    q = [qtabs[0], qtabs[1], qtabs[1]]
    o = bytearray(b"\xff\xd8")
    for t in (0, 1):
        o += b"\xff\xdb" + struct.pack(">HB", 67, t) + bytes(qtabs[t])
    o += b"\xff\xc0" + struct.pack(">HBHHB", 17, 8, h, w, 3)
    for c in range(3):
        o += bytes([c + 1, (sampling[c][0] << 4) | sampling[c][1], 0 if c == 0 else 1])
    for tc_th, counts, vals in dht:
        o += b"\xff\xc4" + struct.pack(">H", 19 + len(vals)) + bytes([tc_th]) + bytes(counts) + bytes(vals)
    if restart:
        o += b"\xff\xdd" + struct.pack(">HH", 4, restart)
    o += b"\xff\xda" + struct.pack(">HB", 12, 3)
    for c in range(3):
        o += bytes([c + 1, 0x00 if c == 0 else 0x11])
    o += bytes([0, 63, 0])
    bits = Bits()
    pred = [0, 0, 0]
    count = 0
    rst = 0
    for my in range(mcuy):
        for mx in range(mcux):
            if restart and count and count % restart == 0:
                bits.flush(); o += bits.b; bits = Bits()
                o += bytes([0xFF, 0xD0 + rst]); rst = (rst + 1) & 7
                pred = [0, 0, 0]
            for c in range(3):
                hh, vv = sampling[c]
                dct, act = (tabs[0x00], tabs[0x10]) if c == 0 else (tabs[0x01], tabs[0x11])
                qz = qtabs[0] if c == 0 else qtabs[1]
                for by in range(vv):
                    for bx in range(hh):
                        y0 = (my * vv + by) * 8; x0 = (mx * hh + bx) * 8
                        blk = planes[c][y0:y0 + 8, x0:x0 + 8] - 128
                        co = C @ blk @ C.T
                        zz = [int(round(co.flat[ZZ[k]] / qz[k])) for k in range(64)]
                        diff = zz[0] - pred[c]; pred[c] = zz[0]
                        s, v = size_bits(diff)
                        bits.put(*dct[s]);
                        if s: bits.put(v, s)
                        run = 0
                        for k in range(1, 64):
                            if zz[k] == 0: run += 1; continue
                            while run > 15:
                                bits.put(*act[0xF0]); run -= 16
                            s, v = size_bits(zz[k])
                            bits.put(*act[(run << 4) | s]); bits.put(v, s); run = 0
                        if run: bits.put(*act[0x00])
            count += 1
    bits.flush(); o += bits.b
    o += b"\xff\xd9"
    path = os.path.join(out, name + ".jpg")
    open(path, "wb").write(o)
    ref = Image.open(path); ref.load()
    ref.convert("RGB").save(os.path.join(out, name + ".png"))
    print(name, len(o), ref.size)

# the same picture as mkjpeg.py's (imported without running it)
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_jpeg.py")).read().split("def save")[0]
exec(src.replace("out = sys.argv[1]", "").replace("os.makedirs(out, exist_ok=True)", ""))
im = picture(61, 43, 1)
encode(im, "b440", [(1, 2), (1, 1), (1, 1)])
encode(im, "b411", [(4, 1), (1, 1), (1, 1)])
encode(im, "bmix", [(2, 2), (2, 1), (1, 2)])
encode(im, "b441", [(2, 1), (1, 1), (1, 1)], restart=2)
encode(im, "b4h3", [(3, 1), (1, 1), (1, 1)])
