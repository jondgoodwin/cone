# MAKES: zoo.bin (DEFLATE, zlib and gzip streams made by Python's zlib module, with what each must inflate to).
# RUN (in this folder):  python make_zoo.py > zoo.bin
# Python's standard library only (zlib 1.3.1 when this was made); needed ONLY to regenerate, never to run the tests.
# The streams are the reference: made by zlib itself, so an inflater is checked against the original implementation,
# not against its own deflater. Every kind of stream the format has is in here: stored, fixed-Huffman and
# dynamic-Huffman blocks, matches at the window's far edge, empty blocks from sync flushes, zlib's other strategies,
# and gzip members with every header field (the extra field, a name, a comment, the header's CRC-16, a time).
#
# File layout (all numbers 32 bits little-endian unless said): the count of streams, then each:
#   name length (8 bits), name, format (8 bits: 0 raw, 1 zlib, 2 gzip), stream length, stream bytes,
#   what it inflates to: length, CRC-32 (zlib.crc32 here), then the bytes themselves if that is 4096 bytes
#   or fewer (their count is the length), else none.
# A name beginning "multi" is several gzip members in a row (the data is the members' data joined).
import sys, zlib, struct, random, gzip as pygzip


def words(n, seed):
    r = random.Random(seed)
    w = [b"alpha", b"beta", b"gamma", b"delta", b"epsilon", b"zeta", b"eta", b"theta", b"iota", b"kappa", b"lambda", b"mu", b"the", b"of", b"and"]
    out = bytearray()
    while len(out) < n:
        out += r.choice(w) + (b"\n" if r.random() < 0.05 else b" ")
    return bytes(out[:n])


def randbytes(n, seed):
    r = random.Random(seed)
    return bytes(r.getrandbits(8) for _ in range(n))


def letters(n, seed):
    r = random.Random(seed)
    return bytes(97 + int(r.random() ** 2 * 6) for _ in range(n))


def structured(n, seed):
    r = random.Random(seed)
    return bytes(((i * 7) % 251 + (1 if r.random() < 0.06 else 0)) & 255 for i in range(n))


block = randbytes(32768, 5)
data = {
    "empty": b"",
    "one": b"a",
    "hello": b"hello hello hello",
    "words3000": words(3000, 1),
    "words40000": words(40000, 2),
    "random65600": randbytes(65600, 3),                  # more than one stored block
    "zeros70000": bytes(70000),                          # matches of length 258 at distance 1
    "exact32768": block + block,                         # a match at distance exactly 32768
    "letters40000": letters(40000, 4),                   # six symbols: dynamic Huffman pays
    "structured60000": structured(60000, 5),
    "far": block + randbytes(10, 6) + block[:30000],     # a repeat from the window's far edge
}
streams = []


def add(name, ds, fmt, blob, expect=None):
    streams.append((name, fmt, blob, data[ds] if expect is None else expect))


def zl(d, level, wbits, strategy=zlib.Z_DEFAULT_STRATEGY):
    c = zlib.compressobj(level, zlib.DEFLATED, wbits, 9, strategy)
    return c.compress(d) + c.flush()


for ds in ["empty", "one", "hello", "words3000", "words40000", "zeros70000", "letters40000"]:
    for level in (1, 6, 9):
        add("z%d-%s" % (level, ds), ds, 1, zl(data[ds], level, 15))
    add("raw6-" + ds, ds, 0, zl(data[ds], 6, -15))
    add("gz6-" + ds, ds, 2, pygzip.compress(data[ds], 6, mtime=0))
for ds in ["exact32768", "far", "structured60000"]:
    add("z6-" + ds, ds, 1, zl(data[ds], 6, 15))
add("z1-structured60000", "structured60000", 1, zl(data["structured60000"], 1, 15))
for ds in ["empty", "one", "hello", "words3000"]:
    add("raw0-" + ds, ds, 0, zl(data[ds], 0, -15))
    add("z0-" + ds, ds, 1, zl(data[ds], 0, 15))
add("z0-random65600", "random65600", 1, zl(data["random65600"], 0, 15))
# zlib's other strategies: fixed codes only, literals only, distance-1 matches only, filtered
for sname, strat in (("fixed", zlib.Z_FIXED), ("huffonly", zlib.Z_HUFFMAN_ONLY), ("rle", zlib.Z_RLE), ("filtered", zlib.Z_FILTERED)):
    for ds in ("words3000", "zeros70000", "letters40000"):
        add("z6%s-%s" % (sname, ds), ds, 1, zl(data[ds], 6, 15, strat))
# sync and full flushes: empty stored blocks between the parts
d = data["words40000"]
c = zlib.compressobj(6, zlib.DEFLATED, 15)
blob = c.compress(d[:10000]) + c.flush(zlib.Z_SYNC_FLUSH) + c.compress(d[10000:25000]) + c.flush(zlib.Z_FULL_FLUSH) + c.compress(d[25000:]) + c.flush()
add("z6flushes-words40000", "words40000", 1, blob)
# a zlib header that says the window is 512 bytes
add("z6win9-words40000", "words40000", 1, zl(data["words40000"], 6, 9))


def gzip_member(payload, mtime=0, xfl=0, os_=255, name=None, comment=None, extra=None, hcrc=False, text=False):
    flg = 0
    if text: flg |= 1
    if hcrc: flg |= 2
    if extra is not None: flg |= 4
    if name is not None: flg |= 8
    if comment is not None: flg |= 16
    h = bytes([0x1F, 0x8B, 8, flg]) + struct.pack("<I", mtime) + bytes([xfl, os_])
    if extra is not None: h += struct.pack("<H", len(extra)) + extra
    if name is not None: h += name + b"\0"
    if comment is not None: h += comment + b"\0"
    if hcrc: h += struct.pack("<H", zlib.crc32(h) & 0xFFFF)
    return h + zl(payload, 6, -15) + struct.pack("<II", zlib.crc32(payload) & 0xFFFFFFFF, len(payload) & 0xFFFFFFFF)


# Header fields (the test knows these values): time 1700000000, XFL 2, system 3, name "test.txt",
# comment "a comment", extra "AB" + 3 bytes (subfield id "AB", length 3, "xyz"), the header's CRC-16, the text flag
add("gzhdr-all", "words3000", 2, gzip_member(data["words3000"], 1700000000, 2, 3, b"test.txt", b"a comment", b"AB" + struct.pack("<H", 3) + b"xyz", True, True))
add("gzhdr-name", "hello", 2, gzip_member(data["hello"], 1700000000, 0, 3, b"hello.txt"))
add("gzhdr-comment", "hello", 2, gzip_member(data["hello"], 0, 0, 255, None, b"only a comment"))
add("gzhdr-extra", "hello", 2, gzip_member(data["hello"], 0, 0, 255, None, None, b"AB" + struct.pack("<H", 3) + b"xyz"))
add("gzhdr-hcrc", "hello", 2, gzip_member(data["hello"], 5, 4, 0, None, None, None, True))
# Several members in a row: hello, words3000 (with a name), an empty member, "tail"
multi = data["hello"] + data["words3000"] + b"" + b"tail"
streams.append(("multi", 2, gzip_member(data["hello"]) + gzip_member(data["words3000"], 9, 0, 3, b"two") + gzip_member(b"") + gzip_member(b"tail"), multi))

out = bytearray()
out += struct.pack("<I", len(streams))
for name, fmt, blob, exp in streams:
    # the reference itself must agree with the claim
    if fmt == 0: assert zlib.decompress(blob, -15) == exp, name
    elif fmt == 1: assert zlib.decompress(blob) == exp, name
    else: assert pygzip.decompress(blob) == exp, name
    out += bytes([len(name)]) + name.encode() + bytes([fmt]) + struct.pack("<I", len(blob)) + blob
    out += struct.pack("<II", len(exp), zlib.crc32(exp) & 0xFFFFFFFF)
    if len(exp) <= 4096: out += exp
sys.stdout.buffer.write(bytes(out))
