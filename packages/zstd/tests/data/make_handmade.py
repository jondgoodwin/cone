# MAKES: handmade.bin (zstd frames written field by field, each with what it must decompress to) and malformed.bin
# (frames each with one fault, and the kind of error it must be refused with).
# RUN (in this folder): python make_handmade.py   (writes both files here)
# Needed ONLY to regenerate, never to run the tests. Seeded; the same bytes on every run.
#
# The frames here are written by this script's own small encoder (an FSE encoder that walks the decoding table
# backward, a canonical Huffman encoder, the header writers), so that each one carries exactly the feature, or the
# one fault, it is named for: raw and RLE blocks, raw and RLE literals with each header size, Huffman literals with
# direct weights (the example of RFC 8878 section 4.2.1.3), one and four streams in every size format, treeless
# literals, predefined, RLE, FSE-coded and repeated sequence tables, every repeat-offset case, a 3-byte sequence
# count, skippable frames, many frames, an 8-byte content size, a window with a mantissa and a match exactly at its
# edge, and empty input.
#
# The reference (libzstd 1.5.7, zstdref.py) checks this script: every frame in handmade.bin must decompress, with the
# reference, to the content written beside it, and every frame in malformed.bin must be refused by it too, except
# the cases marked below where the reference is more lenient than RFC 8878 (and this package follows the RFC).
#
# handmade.bin: as corpus.bin (make_corpus.py says), with no dictionary at the end: the count (32 bits), then each:
#   name length (8), name, flags (8, 0), frame length (32), frame, content length (32), content XXH64 (64, the
#   reference's), stored length (32; the content when 4096 bytes or fewer, else 0), the stored bytes.
# malformed.bin: the count (32 bits), then each: name length (8), name, expected ErrorKind (8: 1 BadHeader,
#   2 WindowTooLarge, 3 NeedsDictionary, 4 BadBlock, 5 BadTable, 6 BadSequence, 7 BadOffset, 8 Truncated,
#   9 BadChecksum, 10 TooLarge), frame length (32), frame.
import random, struct, os
from zstdref import Zstd

z = Zstd()

LL_BASE = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256,
           512, 1024, 2048, 4096, 8192, 16384, 32768, 65536]
LL_BITS = [0] * 16 + [1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]
ML_BASE = list(range(3, 35)) + [35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195,
                                 16387, 32771, 65539]
ML_BITS = [0] * 32 + [1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]
LL_DEFAULT = ([4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1,
               -1, -1, -1, -1], 6)
ML_DEFAULT = ([1, 4, 3, 2, 2, 2, 2, 2, 2] + [1] * 37 + [-1] * 7, 6)
OF_DEFAULT = ([1, 1, 1, 1, 1, 1, 2, 2, 2] + [1] * 15 + [-1] * 5, 5)


# -------- bits --------

class Fwd:
    """Bits written least significant first; a backward stream is these bits read from the end"""
    def __init__(self):
        self.acc, self.n, self.out = 0, 0, bytearray()

    def put(self, v, k):
        assert 0 <= v < (1 << k) or k == 0 and v == 0
        self.acc |= v << self.n
        self.n += k
        while self.n >= 8:
            self.out.append(self.acc & 255)
            self.acc >>= 8
            self.n -= 8

    def bytes(self):
        if self.n:
            self.out.append(self.acc & 255)
            self.acc, self.n = 0, 0
        return bytes(self.out)


def backward(fields):
    """The bytes of a backward bitstream whose reader meets 'fields' (value, bits) in this order"""
    w = Fwd()
    for v, k in reversed(fields):
        w.put(v, k)
    w.put(1, 1)            # the closing 1 bit
    return w.bytes()


# -------- FSE --------

def dtable(norm, log):
    """RFC 8878 4.1.1: each state's (symbol, bits, base)"""
    size = 1 << log
    sym = [0] * size
    high = size - 1
    nxt = []
    for s, c in enumerate(norm):
        if c == -1:
            sym[high] = s
            high -= 1
            nxt.append(1)
        else:
            nxt.append(max(c, 0))
    pos, step = 0, (size >> 1) + (size >> 3) + 3
    for s, c in enumerate(norm):
        for _ in range(max(c, 0)):
            sym[pos] = s
            pos = (pos + step) & (size - 1)
            while pos > high:
                pos = (pos + step) & (size - 1)
    assert pos == 0
    cells = []
    for u in range(size):
        s = sym[u]
        x = nxt[s]
        nxt[s] += 1
        nb = log - (x.bit_length() - 1)
        cells.append((s, nb, (x << nb) - size))
    return cells


def walk(cells, codes):
    """States for 'codes' chosen from the last back: the first state, and each update's (value, bits)"""
    states = [None] * len(codes)
    states[-1] = next(u for u, c in enumerate(cells) if c[0] == codes[-1])
    for i in range(len(codes) - 2, -1, -1):
        after = states[i + 1]
        states[i] = next(u for u, (s, nb, base) in enumerate(cells)
                         if s == codes[i] and base <= after < base + (1 << nb))
    ups = []
    for i in range(len(codes) - 1):
        s, nb, base = cells[states[i]]
        ups.append((states[i + 1] - base, nb))
    return states[0], ups


def ncount(norm, log):
    """An FSE table description (RFC 8878 4.1.1), as the decoder reads it"""
    w = Fwd()
    w.put(log - 5, 4)
    remaining, threshold, nbits = (1 << log) + 1, 1 << log, log + 1
    s, prev0 = 0, False
    while remaining > 1:
        if prev0:
            start = s
            while norm[s] == 0:
                s += 1
            n0 = s - start
            while n0 >= 3:
                w.put(3, 2)
                n0 -= 3
            w.put(n0, 2)
        c = norm[s]
        v = c + 1
        mx = (2 * threshold - 1) - remaining
        if v < mx:
            w.put(v, nbits - 1)
        elif v < threshold:
            w.put(v, nbits)
        else:
            w.put(v + mx, nbits)
        remaining -= abs(c)
        prev0 = c == 0
        s += 1
        while remaining < threshold:
            nbits -= 1
            threshold >>= 1
    return w.bytes()


# -------- Huffman --------

def huff_codes(weights):
    """From the explicit weights (all symbols but the last): each symbol's (code, bits), and the max bits"""
    total = sum(1 << (w - 1) for w in weights if w)
    maxbits = total.bit_length()
    rest = (1 << maxbits) - total
    assert rest & (rest - 1) == 0
    ws = list(weights) + [rest.bit_length()]
    codes = {}
    at = 0
    for wt in range(1, maxbits + 1):
        for s, x in enumerate(ws):
            if x == wt:
                nb = maxbits + 1 - wt
                codes[s] = (at >> (maxbits - nb), nb)
                at += 1 << (wt - 1)
    return codes, maxbits


def huff_stream(codes, data, tail=()):
    return backward([codes[b] for b in data] + list(tail))


def direct_weights(weights):
    n = len(weights)
    out = bytearray([127 + n])
    for i in range(0, n, 2):
        hi = weights[i]
        lo = weights[i + 1] if i + 1 < n else 0
        out.append(hi << 4 | lo)
    return bytes(out)


# -------- sections --------

def lits_raw(data, hs=None):
    n = len(data)
    if hs is None:
        hs = 1 if n < 32 else 2 if n < 4096 else 3
    if hs == 1:
        h = bytes([n << 3])
    elif hs == 2:
        h = struct.pack("<H", 1 << 2 | n << 4)
    else:
        h = (3 << 2 | n << 4).to_bytes(3, "little")
    return h + data


def lits_rle(b, n, hs=None):
    if hs is None:
        hs = 1 if n < 32 else 2 if n < 4096 else 3
    if hs == 1:
        h = bytes([1 | n << 3])
    elif hs == 2:
        h = struct.pack("<H", 1 | 1 << 2 | n << 4)
    else:
        h = (1 | 3 << 2 | n << 4).to_bytes(3, "little")
    return h + bytes([b])


def lits_huff(codes, data, streams, sf, tree=b"", treeless=False, tail=()):
    """Huffman literals; 'tree' is the description (empty when treeless)"""
    if streams == 1:
        body = huff_stream(codes, data, tail)
    else:
        seg = (len(data) + 3) // 4
        parts = [huff_stream(codes, data[i * seg:(i + 1) * seg if i < 3 else len(data)]) for i in range(4)]
        body = struct.pack("<HHH", len(parts[0]), len(parts[1]), len(parts[2])) + b"".join(parts)
    csize = len(tree) + len(body)
    hs, bits = [(3, 10), (3, 10), (4, 14), (5, 18)][sf]
    assert len(data) < (1 << bits) and csize < (1 << bits)
    h = (3 if treeless else 2) | sf << 2 | len(data) << 4 | csize << (4 + bits)
    return h.to_bytes(hs, "little") + tree + body


def ll_code(ll):
    return ll if ll < 16 else max(c for c in range(36) if LL_BASE[c] <= ll)


def ml_code(ml):
    return ml - 3 if ml < 35 else max(c for c in range(53) if ML_BASE[c] <= ml)


def seq_count(n):
    if n < 128:
        return bytes([n])
    if n < 0x7F00:
        return bytes([(n >> 8) + 128, n & 255])
    return bytes([255]) + struct.pack("<H", n - 0x7F00)


def sequences(seqs, ll_t="predefined", of_t="predefined", ml_t="predefined", tail=(), drop=0):
    """A sequences section for (literal length, match length, Offset_Value) triples. A table is 'predefined',
    ('rle', code), ('fse', norm, log), or ('repeat', norm, log) where norm and log are the earlier block's.
    'tail' adds fields after the last one the decoder reads; 'drop' leaves out that many of the last fields."""
    if not seqs:
        return b"\x00"
    llc = [ll_code(s[0]) for s in seqs]
    mlc = [ml_code(s[1]) for s in seqs]
    ofc = [s[2].bit_length() - 1 for s in seqs]
    mode = 0
    desc = b""
    tabs = []
    for shift, t, default in ((6, ll_t, LL_DEFAULT), (4, of_t, OF_DEFAULT), (2, ml_t, ML_DEFAULT)):
        if t == "predefined":
            tabs.append(dtable(*default))
        elif t[0] == "rle":
            mode |= 1 << shift
            desc += bytes([t[1]])
            tabs.append([(t[1], 0, 0)])
        elif t[0] == "fse":
            mode |= 2 << shift
            desc += ncount(t[1], t[2])
            tabs.append(dtable(t[1], t[2]))
        else:
            mode |= 3 << shift
            tabs.append(dtable(t[1], t[2]) if t[1] is not None else [(t[2], 0, 0)])
    llT, ofT, mlT = tabs
    ll0, llU = walk(llT, llc)
    of0, ofU = walk(ofT, ofc)
    ml0, mlU = walk(mlT, mlc)
    log = lambda t: (len(t) - 1).bit_length()
    fields = [(ll0, log(llT)), (of0, log(ofT)), (ml0, log(mlT))]
    for i, (ll, ml, ov) in enumerate(seqs):
        fields.append((ov - (1 << ofc[i]), ofc[i]))
        fields.append((ml - ML_BASE[mlc[i]], ML_BITS[mlc[i]]))
        fields.append((ll - LL_BASE[llc[i]], LL_BITS[llc[i]]))
        if i + 1 < len(seqs):
            fields += [llU[i], mlU[i], ofU[i]]
    fields = fields[:len(fields) - drop] + list(tail)
    return seq_count(len(seqs)) + bytes([mode]) + desc + backward(fields)


# -------- frames --------

def header(content_size=None, single=False, window_log=17, mantissa=0, checksum=False, did=0, did_bytes=None,
           fcs_bytes=None, reserved=False):
    fhd = 0
    if content_size is None:
        flag, fsize = 0, 0
    elif fcs_bytes is not None:
        fsize = fcs_bytes
        flag = {1: 0, 2: 1, 4: 2, 8: 3}[fsize]
    elif single and content_size < 256:
        flag, fsize = 0, 1
    elif 256 <= content_size < 65536 + 256:
        flag, fsize = 1, 2
    elif content_size < 1 << 32:
        flag, fsize = 2, 4
    else:
        flag, fsize = 3, 8
    dsize = did_bytes if did_bytes is not None else (0 if did == 0 else 1 if did < 256 else 2 if did < 65536 else 4)
    fhd = flag << 6 | (0x20 if single else 0) | (8 if reserved else 0) | (4 if checksum else 0) | \
        {0: 0, 1: 1, 2: 2, 4: 3}[dsize]
    out = struct.pack("<I", 0xFD2FB528) + bytes([fhd])
    if not single:
        out += bytes([(window_log - 10) << 3 | mantissa])
    if dsize:
        out += did.to_bytes(dsize, "little")
    if fsize:
        v = content_size - 256 if fsize == 2 else content_size
        out += v.to_bytes(fsize, "little")
    return out


def block(kind, body, last, size=None):
    n = size if size is not None else len(body)
    return (last | kind << 1 | n << 3).to_bytes(3, "little") + body


def raw(data, last=0):
    return block(0, data, last)


def rle(b, n, last=0):
    return block(1, bytes([b]), last, n)


def comp(body, last=0):
    return block(2, body, last)


def frame(blocks, content, checksum=True, **kw):
    out = header(len(content) if "content_size" not in kw else kw.pop("content_size"), checksum=checksum, **kw)
    out += b"".join(blocks)
    if checksum:
        out += struct.pack("<I", z.xxh64(content) & 0xFFFFFFFF)
    return out


def skippable(n, data):
    return struct.pack("<II", 0x184D2A50 + n, len(data)) + data


# -------- the cases --------

rnd = random.Random(1878)
valid = []
bad = []
LENIENT = []


def ok(name, f, content):
    try:
        back = z.decompress(f) if f else b""
    except RuntimeError as e:
        raise SystemExit("the reference refuses valid case %s: %s" % (name, e))
    assert back == content, name
    valid.append((name, f, content))


def ko(name, kind, f, lenient=False):
    try:
        z.decompress(f, window_log_max=27)
        if not lenient:
            raise SystemExit("the reference accepts malformed case " + name)
        LENIENT.append(name)
    except RuntimeError:
        if lenient:
            raise SystemExit("case %s is marked lenient but the reference refuses it" % name)
    bad.append((name, kind, f))


def text(n):
    w = [b"one ", b"two ", b"three ", b"four ", b"five "]
    out = bytearray()
    while len(out) < n:
        out += rnd.choice(w)
    return bytes(out[:n])


def run_seqs(lits, seqs, reps=(1, 4, 8), prefix=b""):
    """What the sequences make after 'prefix' (the frame's output so far): the output, and the repeat offsets.
    Offset_Value > 3 is the offset + 3; 1 to 3 name a repeat offset (with a literal length of 0 they shift by one,
    and 3 is then the first repeat offset less 1)."""
    out = bytearray(prefix)
    lp = 0
    reps = list(reps)
    for ll, ml, ov in seqs:
        out += lits[lp:lp + ll]
        lp += ll
        if ov > 3:
            off = ov - 3
            reps = [off, reps[0], reps[1]]
        else:
            idx = ov - 1 + (1 if ll == 0 else 0)
            if idx == 0:
                off = reps[0]
            elif idx == 3:
                off = reps[0] - 1
                reps = [off, reps[0], reps[1]]
            else:
                off = reps[idx]
                reps = [off] + [r for k, r in enumerate(reps) if k != idx]
        assert 0 < off <= len(out)
        for _ in range(ml):
            out.append(out[-off])
    return bytes(out + lits[lp:]), reps


# Raw and RLE blocks, an empty block among them
a, b2, c = text(100), text(1000), text(7)
ok("raw-blocks", frame([raw(b""), raw(a), raw(b2), raw(c, 1)], a + b2 + c), a + b2 + c)
rle_content = b"A" + b"B" * 1000 + b"x" + bytes(131072)
ok("rle-blocks", frame([rle(0x41, 1), rle(0x42, 1000), raw(b"x"), rle(0, 131072, 1)], rle_content), rle_content)
ok("raw-block-nochecksum", frame([raw(a, 1)], a, checksum=False), a)

# Raw and RLE literals, each header size, with no sequences
for hs, n in ((1, 31), (2, 32), (2, 300), (3, 31), (3, 400), (3, 4100)):
    d = text(n)
    ok("raw-lits-%dbyte-%d" % (hs, n), frame([comp(lits_raw(d, hs) + sequences([]), 1)], d), d)
    ok("rle-lits-%dbyte-%d" % (hs, n), frame([comp(lits_rle(0x7A, n, hs) + sequences([]), 1)], b"z" * n), b"z" * n)

# Predefined sequences: literals then matches, every repeat-offset case
lit = b"abcdefghijklmnopqrstuvwxyz0123456789"
seqs = [(10, 4, 10 + 3),      # "abcdefghij" then 4 from offset 10           reps 10 4 8
        (5, 5, 1),            # "klmno", then rep1 = 10                      reps 10 4 8
        (3, 3, 2),            # "pqr", rep2 = 4                               reps 4 10 8
        (2, 6, 3),            # "st", rep3 = 8                                reps 8 4 10
        (0, 4, 1),            # ll 0: rep2 = 4                                reps 4 8 10
        (0, 3, 2),            # ll 0: rep3 = 10                               reps 10 4 8
        (0, 5, 3),            # ll 0: rep1 - 1 = 9                            reps 9 10 4
        (1, 7, 30 + 3),       # "u" then offset 30                           reps 30 9 10
        (0, 40, 1)]           # ll 0: rep2 = 9, a length with an extra bit    reps 9 30 10
predef_content, predef_reps = run_seqs(lit, seqs)
assert predef_reps == [9, 30, 10]
predef_block = lits_raw(lit) + sequences(seqs)
ok("predefined-seqs", frame([comp(predef_block, 1)], predef_content), predef_content)
rle_seq_content, _ = run_seqs(b"-" * 40, [(40, 100, 1 + 3), (0, 50, 1)])
ok("predefined-seqs-rle-lits", frame([comp(lits_rle(0x2D, 40) + sequences([(40, 100, 1 + 3), (0, 50, 1)]), 1)],
                                     rle_seq_content), rle_seq_content)
# The same block twice: the repeat offsets go on into the second block, which matches into the first
two, _ = run_seqs(lit, seqs, predef_reps, predef_content)
ok("predefined-two-blocks", frame([comp(predef_block), comp(predef_block, 1)], two), two)
# The second block repeats the predefined tables
ok("repeat-predefined", frame([comp(predef_block), comp(lits_raw(lit) + sequences(
    seqs, ("repeat",) + LL_DEFAULT, ("repeat",) + OF_DEFAULT, ("repeat",) + ML_DEFAULT), 1)], two), two)

# Huffman literals, RFC 8878 section 4.2.1.3's example: weights 4 3 2 0 1 (and 1 for the last symbol, 5)
#   literal 0 1 2 3 4 5 / bits 1 2 3 0 4 4 / codes 1 01 001 - 0000 0001
rfc_w = [4, 3, 2, 0, 1]
rfc_codes, rfc_max = huff_codes(rfc_w)
assert rfc_max == 4 and rfc_codes == {0: (1, 1), 1: (1, 2), 2: (1, 3), 4: (0, 4), 5: (1, 4)}, rfc_codes
rfc_tree = direct_weights(rfc_w)
syms = bytes(rnd.choice([0, 0, 0, 0, 1, 1, 2, 4, 5]) for _ in range(900))
ok("huffman-rfc-example-1stream", frame([comp(lits_huff(rfc_codes, syms, 1, 0, rfc_tree) + sequences([]), 1)],
                                        syms), syms)
ok("huffman-rfc-example-4streams", frame([comp(lits_huff(rfc_codes, syms, 4, 1, rfc_tree) + sequences([]), 1)],
                                         syms), syms)
# The same table again, treeless, four and one streams, after a block that set it
syms2 = bytes(rnd.choice([0, 1, 2, 4, 5]) for _ in range(3000))
tl = syms + syms2 + syms[:100]
ok("huffman-treeless", frame([comp(lits_huff(rfc_codes, syms, 1, 0, rfc_tree) + sequences([])),
                              comp(lits_huff(rfc_codes, syms2, 4, 2, treeless=True) + sequences([])),
                              comp(lits_huff(rfc_codes, syms[:100], 1, 0, treeless=True) + sequences([]), 1)], tl),
   tl)
# Two symbols, a and b, one bit each: 20000 literals, the largest size format
ab_w = [0] * 97 + [1]
ab_codes, _ = huff_codes(ab_w)
ab = bytes(rnd.choice(b"ab") for _ in range(20000))
ok("huffman-sf3", frame([comp(lits_huff(ab_codes, ab, 4, 3, direct_weights(ab_w)) + sequences([]), 1)], ab,
                        single=True), ab)
# Huffman literals with sequences
hs_lit = bytes(rnd.choice([0, 1, 2, 4, 5]) for _ in range(36))
hs_seq = [(10, 4, 13), (5, 5, 1), (3, 3, 2), (18, 9, 3 + 7)]
hs_out, _ = run_seqs(hs_lit, hs_seq)
ok("huffman-with-seqs", frame([comp(lits_huff(rfc_codes, hs_lit, 1, 0, rfc_tree) + sequences(hs_seq), 1)], hs_out),
   hs_out)

# A 3-byte sequence count (33000 sequences), all three tables RLE: each sequence is no literals and 3 bytes from
# the second repeat offset (4, then 1, then 4 ...), so the bitstream holds no bits at all
many_seqs = [(0, 3, 1)] * 33000
mc, _ = run_seqs(b"", many_seqs, prefix=b"abcd")
ok("seq-count-3byte-rle-tables", frame([raw(b"abcd"), comp(lits_raw(b"") + sequences(
    many_seqs, ("rle", 0), ("rle", 0), ("rle", 0)), 1)], mc), mc)

# FSE-coded tables of this script's own, with runs of zero counts (2-bit repeat flags, 3 meaning "and more"),
# then the same tables repeated in the next block. A raw block first gives the offsets something to reach.
ll_norm = [10, -1, 0, 0, 0, 0, 0, 7, 0, 0, 0, 0, 0, 0, 16, 30]          # log 6
ml_norm = [20, 6, -1, -1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 2]          # log 5
of_norm = [0, -1, 4, 4, 8, 15]                                          # log 5
assert sum(map(abs, ll_norm)) == 64 and sum(map(abs, ml_norm)) == 32 and sum(map(abs, of_norm)) == 32


def random_seqs(n):
    out = []
    for _ in range(n):
        lc = rnd.choice([i for i, x in enumerate(ll_norm) if x])
        mc_ = rnd.choice([i for i, x in enumerate(ml_norm) if x])
        oc = rnd.choice([i for i, x in enumerate(of_norm) if x])
        out.append((LL_BASE[lc] + rnd.randrange(1 << LL_BITS[lc]), ML_BASE[mc_] + rnd.randrange(1 << ML_BITS[mc_]),
                    (1 << oc) + rnd.randrange(1 << oc)))
    return out


prefix = text(100)
f1 = random_seqs(80)
f1_lits = text(sum(s[0] for s in f1) + 9)
f1_out, f1_reps = run_seqs(f1_lits, f1, prefix=prefix)
f2 = random_seqs(50)
f2_lits = text(sum(s[0] for s in f2) + 3)
f2_out, _ = run_seqs(f2_lits, f2, f1_reps, f1_out)
fse_block = lits_raw(f1_lits) + sequences(f1, ("fse", ll_norm, 6), ("fse", of_norm, 5), ("fse", ml_norm, 5))
rep_block = lits_raw(f2_lits) + sequences(f2, ("repeat", ll_norm, 6), ("repeat", of_norm, 5), ("repeat", ml_norm, 5))
ok("fse-tables", frame([raw(prefix), comp(fse_block, 1)], f1_out), f1_out)
ok("fse-then-repeat", frame([raw(prefix), comp(fse_block), comp(rep_block, 1)], f2_out), f2_out)

# Frame headers: 8- and 4-byte content sizes for a small size; no content size; single segment; empty
d = text(300)
ok("fcs-8bytes", frame([raw(d, 1)], d, fcs_bytes=8), d)
ok("fcs-4bytes-small", frame([raw(d, 1)], d, fcs_bytes=4), d)
ok("no-content-size", frame([raw(d, 1)], d, content_size=None), d)
ok("single-segment", frame([raw(d, 1)], d, single=True), d)
ok("single-segment-empty", frame([raw(b"", 1)], b"", single=True), b"")
ok("windowed-empty", frame([raw(b"", 1)], b""), b"")
# A window of 1 KiB + 7/8 (1920 bytes) and a match exactly 1920 back
edge_a = text(1900)
edge_lit = text(20)
edge_out, _ = run_seqs(edge_lit, [(20, 50, 1920 + 3)], prefix=edge_a)
ok("window-edge", frame([raw(edge_a), comp(lits_raw(edge_lit) + sequences([(20, 50, 1920 + 3)]), 1)], edge_out,
                        window_log=10, mantissa=7), edge_out)

# Skippable frames (empty and not, several magics), many frames, empty input
sk = skippable(0, b"") + skippable(15, b"skip me") + frame([raw(a, 1)], a) + skippable(7, bytes(300))
ok("skippable-frames", sk, a)
many = bytearray()
mcontent = bytearray()
for i in range(200):
    d = text(i % 17)
    if i % 4 == 0:
        many += frame([raw(d, 1)], d, single=True, checksum=(i % 8 == 0))
    elif i % 4 == 1:
        d = bytes([0x30 + i % 10]) * i
        many += frame([rle(0x30 + i % 10, i, 1)], d, checksum=False)
    elif i % 4 == 2:
        many += frame([comp(lits_raw(d) + sequences([]), 1)], d)
    else:
        many += skippable(i % 16, d)
        d = b""
    mcontent += d
ok("many-frames", bytes(many), bytes(mcontent))
valid.append(("empty-input", b"", b""))

# -------- malformed --------

BadHeader, WindowTooLarge, NeedsDictionary, BadBlock, BadTable, BadSequence, BadOffset, Truncated, BadChecksum, \
    TooLarge = range(1, 11)


def nf(blocks):
    """A frame with no content size and no checksum: only its blocks can be wrong"""
    return frame(blocks, b"", content_size=None, checksum=False)


good = frame([raw(a, 1)], a)
ko("bad-magic", BadHeader, b"\x28\xb5\x2f\xfe" + good[4:])
ko("trailing-garbage", BadHeader, good + b"\x01\x02\x03\x04\x05")
ko("reserved-bit", BadHeader, header(100, reserved=True) + raw(a, 1))
ko("window-256MiB", WindowTooLarge, header(None, window_log=28) + raw(a, 1))
ko("dictionary-id", NeedsDictionary, header(100, did=12345) + raw(a, 1))
ko("dictionary-id-1byte", NeedsDictionary, header(100, did=7) + raw(a, 1))
ko("reserved-block-type", BadBlock, header(100) + block(3, a, 1))
ko("block-over-window", BadBlock, header(None, window_log=10) + raw(text(1500), 1))
# (the header is the fault: the block's bytes need not follow)
ko("block-over-128k", BadBlock, header(None, window_log=20) + raw(bytes(131073), 1)[:40])
ko("rle-over-128k", BadBlock, header(None, window_log=20) + rle(0, 131073, 1))
ko("lits-header-cut", BadBlock, nf([comp(b"\x0e\x01", 1)]))
ko("lits-raw-past-block", BadBlock, nf([comp(lits_raw(b"xyz")[:1] + b"xy", 1)]))
ko("lits-rle-over-128k", BadBlock, nf([comp(lits_rle(0x41, 200000, 3) + sequences([]), 1)]))
bad_jump = bytearray(lits_huff(rfc_codes, syms, 4, 1, rfc_tree))
bad_jump[3 + len(rfc_tree)] = 0xFF
bad_jump[4 + len(rfc_tree)] = 0x7F
ko("lits-jump-table", BadBlock, nf([comp(bytes(bad_jump) + sequences([]), 1)]))
ko("huffman-weights-sum", BadTable, nf([comp(lits_huff({0: (0, 1), 1: (1, 1)}, b"\x00\x01", 1, 0,
                                                       direct_weights([3, 1])) + sequences([]), 1)]))
ko("huffman-weight-over-11", BadTable, nf([comp(lits_huff({0: (0, 1), 1: (1, 1)}, b"\x00\x01", 1, 0,
                                                          direct_weights([12])) + sequences([]), 1)]))
ko("huffman-one-longest-code", BadTable, nf([comp(lits_huff({0: (0, 1), 1: (1, 1)}, b"\x00\x01", 1, 0,
                                                            direct_weights([2])) + sequences([]), 1)]))
ko("huffman-tree-empty", BadTable, nf([comp(b"\x02\x01\x00" + b"\xff\x11", 1)]))
ko("treeless-first", BadTable, nf([comp(lits_huff(rfc_codes, syms, 1, 0, treeless=True) + sequences([]), 1)]))
ko("huffman-stream-left-over", BadSequence, nf([comp(lits_huff(rfc_codes, syms[:50], 1, 0, rfc_tree, tail=[(0, 3)]) +
                                                     sequences([]), 1)]))
ko("seq-reserved-modes", BadBlock, nf([comp(lits_raw(lit) + b"\x01\x01\x80", 1)]))
ko("seq-none-then-bytes", BadBlock, nf([comp(lits_raw(lit) + b"\x00\x00", 1)]))
ko("seq-count-cut", BadBlock, nf([comp(lits_raw(lit) + b"\xff\x01", 1)]))
ko("fse-log-too-big", BadTable, nf([comp(lits_raw(lit) + b"\x01\x80" + ncount([1024], 10) + b"\x01", 1)]))
ko("fse-too-many-symbols", BadTable, nf([comp(lits_raw(lit) + b"\x01\x20" + ncount([2] * 31 + [1, 1], 6) + b"\x01",
                                              1)]))
ko("fse-cut", BadTable, nf([comp(lits_raw(lit) + b"\x01\x80" + ncount(ll_norm, 6)[:2], 1)]))
ko("rle-symbol-out-of-range", BadTable, nf([comp(lits_raw(lit) + b"\x01\x04\x35\x01", 1)]))
ko("repeat-first", BadTable, nf([comp(lits_raw(lit) + b"\x01\xfc\x01", 1)]))
ko("seq-stream-last-byte-0", BadSequence, nf([comp(predef_block[:-1] + b"\x00", 1)]))
ko("seq-stream-left-over", BadSequence, nf([comp(lits_raw(lit) + sequences(seqs, tail=[(0, 5)]), 1)]))
ko("seq-stream-too-short", BadSequence, nf([comp(lits_raw(lit) + sequences(seqs, drop=2), 1)]))
ko("lit-length-past-literals", BadSequence, nf([comp(lits_raw(b"abc") + sequences([(4, 3, 4)]), 1)]))
ko("offset-before-start", BadOffset, nf([comp(lits_raw(b"ab") + sequences([(2, 3, 3 + 3)]), 1)]))
ko("offset-zero-repeat", BadOffset, nf([comp(lits_raw(b"abcd") + sequences([(4, 3, 1 + 3), (0, 3, 3)]), 1)]))
# One byte past the window's edge: 1921 back with a window of 1920 (and 1921 bytes of output)
ko("offset-past-window", BadOffset, frame([raw(edge_a + b"x"), comp(lits_raw(edge_lit) + sequences(
    [(20, 50, 1921 + 3)]), 1)], b"", content_size=None, checksum=False, window_log=10, mantissa=7),
   lenient=True)
ko("block-decodes-over-window", BadBlock, frame([comp(lits_raw(text(1000)) + sequences([(1000, 1000, 1)]), 1)],
                                                b"", content_size=None, checksum=False, window_log=10))
ko("checksum-wrong", BadChecksum, good[:-1] + bytes([good[-1] ^ 0x40]))
ko("content-size-short", BadChecksum, frame([raw(a, 1)], a, content_size=len(a) + 1, checksum=False))
ko("content-size-long", BadChecksum, frame([raw(a, 1)], a, content_size=len(a) - 1, checksum=False))
ko("cut-in-magic", Truncated, good[:3])
ko("cut-in-header", Truncated, good[:5])
ko("cut-in-block", Truncated, good[:20])
ko("cut-before-checksum", Truncated, good[:-4])
ko("cut-in-checksum", Truncated, good[:-1])
ko("cut-skippable", Truncated, skippable(3, b"abcdef")[:10])
ko("cut-second-frame", Truncated, good + good[:7])

# -------- write --------

here = os.path.dirname(os.path.abspath(__file__))
h = bytearray(struct.pack("<I", len(valid)))
for name, f, content in valid:
    stored = content if len(content) <= 4096 else b""
    h += bytes([len(name)]) + name.encode() + b"\x00" + struct.pack("<I", len(f)) + f
    h += struct.pack("<IQI", len(content), z.xxh64(content), len(stored)) + stored
with open(os.path.join(here, "handmade.bin"), "wb") as fh:
    fh.write(h)
m = bytearray(struct.pack("<I", len(bad)))
for name, kind, f in bad:
    m += bytes([len(name)]) + name.encode() + bytes([kind]) + struct.pack("<I", len(f)) + f
with open(os.path.join(here, "malformed.bin"), "wb") as fh:
    fh.write(m)
print("handmade.bin: %d frames, %d bytes; malformed.bin: %d cases, %d bytes" % (len(valid), len(h), len(bad), len(m)))
print("the reference accepts (more lenient than RFC 8878):", ", ".join(LENIENT) or "none")
