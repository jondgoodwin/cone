# MAKES: own/postfix2.br, a hand-written Brotli stream with NPOSTFIX 2 and NDIRECT 2 (so 8 direct distance codes), and
# own/postfix2, the bytes it must decompress to. Called by makefixtures.py; needed ONLY to regenerate. The C
# 'brotli' command decompresses the stream, and makefixtures.py checks that it agrees with the bytes made here.
#
# One last meta-block, WBITS 16, with simple prefix codes of four symbols each (all of length 2): four literals,
# four insert-and-copy symbols and four distance symbols: one direct code (distance 2) and three that use the
# postfix (low codes 1, 2 and 3, at three different numbers of extra bits). The commands are drawn at random
# from a fixed seed. The expected bytes are made here by RFC 7932 section 4's distance formula and section 5's
# copy rule, not by the Cone decoder.
import random

NPOSTFIX, NDIRECT_FIELD = 2, 2
NDIRECT = NDIRECT_FIELD << NPOSTFIX             # 8 direct distance codes
ALPHA_D = 16 + NDIRECT + (48 << NPOSTFIX)       # 216 distance symbols: 8 bits for a symbol in a simple code
LITS = [97, 98, 99, 100]
# insert-and-copy symbols 128 + (insert code << 3) + copy code, with insert lengths 0 to 5 and copy lengths 2 to 9
CMDS = [(3, 1), (5, 6), (0, 3), (2, 0)]         # (insert length, copy code)


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


def dist_of(dcode, extra):
    # section 4: the distance a symbol and its extra bits name
    if dcode < 16 + NDIRECT:
        return dcode - 15
    k = dcode - NDIRECT - 16
    nb = 1 + (k >> (NPOSTFIX + 1))
    hcode = k >> NPOSTFIX
    lcode = k & ((1 << NPOSTFIX) - 1)
    offset = ((2 + (hcode & 1)) << nb) - 4
    return ((offset + extra) << NPOSTFIX) + lcode + NDIRECT + 1


def nbits(dcode):
    return 0 if dcode < 16 + NDIRECT else 1 + ((dcode - NDIRECT - 16) >> (NPOSTFIX + 1))


def simple_code(w, syms, alpha_bits):
    # a simple prefix code of four symbols of length 2; returns {symbol: (reversed code, 2)}
    w.put(1, 2)                                 # HSKIP 1: a simple code
    w.put(3, 2)                                 # NSYM - 1
    for s in sorted(syms):
        w.put(s, alpha_bits)
    w.put(0, 1)                                 # tree-select 0: four lengths of 2
    return {s: (int(format(i, "02b")[::-1], 2), 2) for i, s in enumerate(sorted(syms))}


def build():
    rnd = random.Random(7)
    dsyms = [16 + 1,                                        # direct: distance 2
             16 + NDIRECT + (3 << NPOSTFIX) + 1,            # 2 extra bits, low code 1
             16 + NDIRECT + (6 << NPOSTFIX) + 2,            # 4 extra bits, low code 2
             16 + NDIRECT + (9 << NPOSTFIX) + 3]            # 5 extra bits, low code 3
    assert len(set(dsyms)) == 4 and max(dsyms) < ALPHA_D
    cmd_syms = [128 + (ic << 3) + cc for ic, cc in CMDS]
    out = bytearray()
    cmds = []
    for i in range(700):
        k = 0 if i == 0 else rnd.randrange(4)
        ins, cc = CMDS[k]
        lits = [rnd.choice(LITS) for _ in range(ins)]
        out += bytes(lits)
        ln = cc + 2
        # a distance symbol whose range holds a distance within the output so far
        opts = []
        for s in dsyms:
            for extra in range(1 << nbits(s)):
                if dist_of(s, extra) <= len(out):
                    opts.append((s, extra))
        s, extra = rnd.choice(opts)
        d = dist_of(s, extra)
        for _ in range(ln):
            out.append(out[-d])
        cmds.append((cmd_syms[k], lits, s, extra))
    w = Bits()
    w.put(0, 1)                                 # WBITS 16
    w.put(1, 1)                                 # ISLAST
    w.put(0, 1)                                 # not ISLASTEMPTY
    w.put(0, 2)                                 # MNIBBLES 4
    w.put(len(out) - 1, 16)
    w.put(0, 3)                                 # one block type in each category
    w.put(NPOSTFIX, 2)
    w.put(NDIRECT_FIELD, 4)
    w.put(0, 2)                                 # the literal context mode
    w.put(0, 2)                                 # one literal tree, one distance tree
    lc = simple_code(w, LITS, 8)
    cc_ = simple_code(w, cmd_syms, 10)
    dc = simple_code(w, dsyms, 8)
    for sym, lits, s, extra in cmds:
        w.put(*cc_[sym])
        for l in lits:
            w.put(*lc[l])
        w.put(*dc[s])
        w.put(extra, nbits(s))
    w.align()
    return bytes(w.out), bytes(out)


if __name__ == "__main__":
    s, e = build()
    print(len(s), len(e))
