# MAKES: own/dictwords.br, a Brotli stream that names a static dictionary word of every length (4 to 24) with every
# one of the 121 transforms (2,541 words), and own/dictwords, the bytes it must decompress to. Called by
# makefixtures.py (give it the RFC's text as its third argument); needed ONLY to regenerate.
#
# The stream is written here, by hand, bit by bit: one meta-block for each word length, with a prefix code of one
# symbol for the insert-and-copy commands (no literals; only the length's extra bits are sent) and a flat
# 6-bit code for the 64 distance symbols, so that each command says its word with a distance symbol and its
# extra bits. The expected bytes come from this file's own reading of RFC 7932 section 8 and Appendix B (the
# transforms are parsed from the RFC text by brotlitables.py): they do not come from the Cone decoder, and
# makefixtures.py has the C brotli command decompress the stream and checks that it agrees.
import os, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
import brotlitables as bt

WINDOW = (1 << 16) - 16


class Bits:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, v, bits):
        self.acc |= v << self.n
        self.n += bits
        while self.n >= 8:
            self.out.append(self.acc & 255)
            self.acc >>= 8
            self.n -= 8

    def align(self):
        if self.n:
            self.put(0, 8 - self.n)


def ferment(w, p):
    c = w[p]
    if c < 192:
        if 97 <= c <= 122:
            w[p] ^= 32
        return 1
    if c < 224:
        if p + 1 < len(w):
            w[p + 1] ^= 32
        return 2
    if p + 2 < len(w):
        w[p + 2] ^= 5
    return 3


def transform(word, t):
    prefix, kind, suffix = t
    w = bytearray(word)
    if kind == 1:
        if w:
            ferment(w, 0)
    elif kind == 2:
        i = 0
        while i < len(w):
            i += ferment(w, i)
    elif 3 <= kind <= 11:
        w = w[kind - 2:]
    elif kind >= 12:
        k = kind - 11
        w = w[:len(w) - k] if len(w) >= k else bytearray()
    return prefix + bytes(w) + suffix


def copy_code(n):
    # (code, extra bits, base) of the copy length code holding n
    bits = [0] * 8 + [1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 24]
    base = [2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 18, 22, 30, 38, 54, 70, 102, 134, 198, 326, 582, 1094, 2118]
    for c in range(24):
        if base[c] <= n < base[c] + (1 << bits[c]):
            return c, bits[c], base[c]
    raise ValueError(n)


def dist_symbol(d):
    # the distance symbol and extra bits (NPOSTFIX 0, NDIRECT 0) for distance d, section 4
    m = d + 3
    nbits = m.bit_length() - 2
    top = m >> nbits
    b = top - 2
    return 16 + 2 * (nbits - 1) + b, nbits, m - (top << nbits)


def build(rfc_path):
    lines = open(rfc_path, encoding="ascii", errors="replace").read().split("\n")
    data, ndbits = bt.dictionary(lines)
    trans = bt.transforms(lines)
    off = {}
    o = 0
    for n in range(4, 25):
        off[n] = o
        o += n << ndbits[n]
    w = Bits()
    w.put(0, 1)                                   # WBITS 16
    expected = bytearray()
    for n in range(4, 25):
        nwords = 1 << ndbits[n]
        cmds = []
        pos = len(expected)
        total = 0
        for tid in range(121):
            idx = (tid * 7 + n * 13) % nwords
            word = data[off[n] + idx * n: off[n] + (idx + 1) * n]
            res = transform(word, trans[tid])
            max_dist = min(WINDOW, pos + total)
            dist = max_dist + 1 + (tid << ndbits[n]) + idx
            cmds.append((dist, res))
            total += len(res)
        last = n == 24
        mlen = total
        w.put(1 if last else 0, 1)                # ISLAST
        if last:
            w.put(0, 1)                           # not ISLASTEMPTY
        w.put(0, 2)                               # MNIBBLES 4
        w.put(mlen - 1, 16)
        if not last:
            w.put(0, 1)                           # not ISUNCOMPRESSED
        w.put(0, 3)                               # one block type in each category
        w.put(0, 6)                               # NPOSTFIX 0, NDIRECT 0
        w.put(0, 2)                               # the literal context mode
        w.put(0, 2)                               # one literal tree, one distance tree
        w.put(1, 2); w.put(0, 2); w.put(97, 8)    # the literal code: one symbol (never used)
        cc, ebits, base = copy_code(n)
        sym = 128 + cc if cc < 8 else 192 + (cc - 8)
        w.put(1, 2); w.put(0, 2); w.put(sym, 10)  # the command code: one symbol, no literals
        # the distance code: complex, 64 symbols of 6 bits (the code-length code has one symbol, 6)
        w.put(0, 2)
        for _ in range(7):
            w.put(0, 2)
        w.put(7, 4)
        for _ in range(10):
            w.put(0, 2)
        for dist, res in cmds:
            w.put(n - base, ebits)
            s, nb, extra = dist_symbol(dist)
            assert 16 <= s < 64
            rev = int(format(s, "06b")[::-1], 2)
            w.put(rev, 6)
            w.put(extra, nb)
            expected += res
    w.align()
    return bytes(w.out), bytes(expected)
