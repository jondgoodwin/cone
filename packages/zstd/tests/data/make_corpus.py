# MAKES: corpus.bin, zstd frames made by the reference zstd (libzstd 1.5.7, see zstdref.py), with what each must
# decompress to. RUN (in this folder): python make_corpus.py   (writes corpus.bin here; prints the modes covered)
# Needed ONLY to regenerate, never to run the tests. Seeded: the inputs are the same on every run; the frames are
# the same for the same libzstd.
#
# What is in it: levels 1, 3, 9, 19 and 22 ('--ultra -22') on text, levels 1 and 19 on a mix of text, binary and
# incompressible data and on records, level 22 again on 3 MB; long-distance mode with a match 3 MB back; frames made
# by streaming (no content size, a flush every so many bytes, so many blocks and tables repeated from block to
# block), one of them 600 KB under a 32 KiB window; tiny inputs (0 to 9 bytes, one repeated byte); a run of one byte
# (RLE blocks); incompressible data (raw blocks); literals left raw; Huffman literals in the size formats libzstd
# makes for these sizes, with direct and FSE-coded weights; sequences whose codes repeat (RLE tables); many frames and
# skippable frames concatenated; frames made with a raw-content dictionary. Kept under 200 KB with
# make_handmade.py's two files; the modes this corpus does not reach (the largest literals size format, treeless
# literals in four streams, a 3-byte sequence count, an 8-byte content size) are in handmade.bin.
#
# File layout (numbers little-endian): the count of cases (32 bits), then each:
#   name length (8 bits), name, flags (8 bits: 1 = needs the dictionary below), frame length (32), the frames,
#   content length (32), content XXH64 (64 bits, libzstd's own ZSTD_XXH64), stored length (32; the content
#   itself when it is 4096 bytes or fewer, else 0), the stored bytes.
# Then the dictionary: its length (32) and bytes.
import random, struct, sys, os
from zstdref import Zstd, modes_of

z = Zstd()

VOCAB = [b"alpha", b"beta", b"gamma", b"delta", b"epsilon", b"zeta", b"eta", b"theta", b"iota", b"kappa", b"lambda",
         b"mu", b"the", b"of", b"and", b"compress", b"window", b"frame", b"block", b"sequence"]


def words(n, seed):
    r = random.Random(seed)
    out = bytearray()
    while len(out) < n:
        out += r.choice(VOCAB) + (b"\n" if r.random() < 0.05 else b" ")
    return bytes(out[:n])


def randbytes(n, seed):
    r = random.Random(seed)
    return bytes(r.getrandbits(8) for _ in range(n))


def letters(n, seed, alphabet=6, base=97):
    r = random.Random(seed)
    return bytes(base + int(r.random() ** 2 * alphabet) for _ in range(n))


def records(n, seed):
    # 16-byte records: a 4-byte counter, a random byte, a fixed 11-byte tag: the same sequence shape over and over
    r = random.Random(seed)
    out = bytearray()
    i = 0
    while len(out) < n:
        out += struct.pack("<I", i * 3) + bytes([r.getrandbits(8)]) + b"-tag:record"
        i += 1
    return bytes(out[:n])


def floats(n, seed):
    r = random.Random(seed)
    out = bytearray()
    x = 0.0
    while len(out) < n:
        x += r.random() - 0.5
        out += struct.pack("<f", round(x, 2))
    return bytes(out[:n])


def mixed(seed):
    return words(12000, seed) + randbytes(500, seed + 1) + floats(8000, seed + 2) + records(8000, seed + 3) + \
        letters(6000, seed + 4) + words(4000, seed)


cases = []          # (name, flags, frames, content)


def add(name, frames, content, flags=0, dictionary=None):
    # the reference must give back the content
    if frames:
        back = z.decompress(frames, dictionary)
        assert back == content, name
    cases.append((name, flags, frames, content))


text = words(30000, 1)
mix = mixed(10)
recs = records(20000, 20)
for lv in (1, 3, 9, 19, 22):
    add("text-L%d" % lv, z.compress(text, lv, checksum=(lv != 9)), text)
    # (On inputs this small libzstd fits level 22's parameters to the size, and they come out as level 19's: the
    # frames are the same, so level 22 is kept once here, on text, and on the 3 MB input below)
    if lv in (1, 19):
        add("mixed-L%d" % lv, z.compress(mix, lv), mix)
    if lv in (1, 19):
        add("records-L%d" % lv, z.compress(recs, lv, checksum=(lv != 19)), recs)

# Long-distance mode: 4 KB of noise repeated after 3 MB of other data, which only a long window reaches
seedblock = randbytes(4096, 30)
far = seedblock + words(4096, 31) * 768 + seedblock
add("long-L19", z.compress(far, 19, ldm=True, window_log=23), far)
add("long-L3", z.compress(far, 3, ldm=True, window_log=22), far)
add("ultra-L22-3MB", z.compress(far, 22), far)

# Streaming: no content size; flushes make short blocks, and tables that repeat from block to block
add("stream-L3-flush997", z.compress_stream(mix[:12000], 3, flush_every=997), mix[:12000])
add("stream-L19-flush5000", z.compress_stream(text, 19, checksum=False, flush_every=5000), text)
add("stream-L1-whole", z.compress_stream(recs[:8000], 1), recs[:8000])
# A small window (32 KiB) under 600 KB of output: 900 bytes of noise over and over, each a match 900 back, so a
# streaming decoder must keep the window and let the rest go many times over. (A smaller window costs more bytes
# here: libzstd's blocks are no bigger than the window, and it codes each block's first copy as literals.)
chunk = randbytes(900, 35)
add("window32k-600k", z.compress_stream(chunk * 667, 3, window_log=15), chunk * 667)

# Tiny inputs, at two levels, with and without the checksum
for n in range(0, 10):
    d = letters(n, 40 + n)
    add("tiny%d-L1" % n, z.compress(d, 1), d)
    add("tiny%d-L19" % n, z.compress(d, 19, checksum=False), d)
add("tiny-run20", z.compress(b"a" * 20, 3), b"a" * 20)
add("tiny-nosize", z.compress(b"hello hello hello", 3, content_size=False), b"hello hello hello")

# One byte repeated: RLE blocks
run = b"\x07" * 300000
add("run300k", z.compress(run, 3), run)
# Incompressible: raw blocks
noise = randbytes(1500, 50)
add("noise1500", z.compress(noise, 3), noise)
# Literals left raw (ZSTD_c_literalCompressionMode = uncompressed)
add("rawlits-L3", z.compress(mix[:15000], 3, literal_mode=2), mix[:15000])
# Huffman literals in each size format: under 1 KiB (one stream), under 16 KiB, a whole block
lit1 = letters(800, 60)
lit2 = letters(5000, 61)
add("lits-800", z.compress(lit1, 3), lit1)
add("lits-5000", z.compress(lit2, 3), lit2)
# Few small symbols: the Huffman weights are written directly (4 bits each)
small = letters(3000, 63, alphabet=8, base=0)
add("lits-direct-weights", z.compress(small, 3), small)
# Small blocks (a target compressed block size), so tables repeat and literals go treeless
add("smallblocks-L19", z.compress(text[:15000], 19, target_cblock=1340), text[:15000])

# Many frames, with skippable frames among them
many = bytearray()
content = bytearray()
for i in range(40):
    d = words(50 + i * 37, 70 + i) if i % 3 else records(100 + i * 11, 70 + i)
    many += z.compress(d, (i % 5) * 4 + 1, checksum=(i % 2 == 0))
    content += d
    if i % 13 == 5:
        many += struct.pack("<II", 0x184D2A50 + i % 16, 7 + i) + randbytes(7 + i, i)
add("many-frames", bytes(many), bytes(content))
skip_only = struct.pack("<II", 0x184D2A5F, 3) + b"abc" + struct.pack("<II", 0x184D2A50, 0)
add("skippable-only", skip_only, b"")
add("empty-input", b"", b"")

# A raw-content dictionary: frames that match from it
dictionary = words(3000, 80) + records(1000, 81)
dtext = words(3000, 80)[1000:] + records(2000, 81)
add("dict-L3", z.compress(dtext, 3, dictionary=dictionary), dtext, flags=1, dictionary=dictionary)
add("dict-L19", z.compress(dtext, 19, dictionary=dictionary), dtext, flags=1, dictionary=dictionary)
add("dict-tiny", z.compress(b"alpha beta gamma delta", 1, dictionary=dictionary), b"alpha beta gamma delta", flags=1,
    dictionary=dictionary)

out = bytearray(struct.pack("<I", len(cases)))
seen = set()
for name, flags, frames, data in cases:
    if frames:
        modes_of(frames, seen)
    stored = data if len(data) <= 4096 else b""
    out += bytes([len(name)]) + name.encode() + bytes([flags]) + struct.pack("<I", len(frames)) + frames
    out += struct.pack("<IQI", len(data), z.xxh64(data), len(stored)) + stored
out += struct.pack("<I", len(dictionary)) + dictionary
here = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(here, "corpus.bin"), "wb") as f:
    f.write(out)
print("corpus.bin: %d cases, %d bytes" % (len(cases), len(out)))
for name, flags, frames, data in cases:
    print("  %-22s %8d -> %8d" % (name, len(frames), len(data)))
print("covered:", " ".join(sorted(seen)))
