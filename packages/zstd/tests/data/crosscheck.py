# The compressor against the reference decoder. Makes a seeded corpus, has examples/crosscheck.cone compress every
# input at levels 1, 2 and 3 (one call, streamed 4096 in / 1000 out, and for small inputs streamed a byte at a time;
# it checks this package's own decoder on each), then decompresses every frame with the reference library (libzstd
# 1.5.7 through zstdref.py, pinned there) and compares it with its input, byte for byte. It also lists which block,
# literals and sequence modes the frames use. No test runs it (congo test has no Python); it is run by hand when the
# compressor changes, and the package header says what it found.
#
#   python packages/zstd/tests/data/crosscheck.py [work folder]       (default: a new temporary folder)
#
# Exit status 0 when every frame decodes to its input with both decoders.
import os, random, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import zstdref  # noqa: E402

PKG = os.path.dirname(os.path.dirname(HERE))
REPO = os.path.dirname(os.path.dirname(PKG))
SEED = 20261026

WORDS = ("fn let var mut imm while if else return match case struct pub mod import use List Array u8 u32 usize i32 "
         "self out pos len push the of a to in is and for that from block frame bits table state literal match offset "
         "window 0usize 1u32 += == != < > { } ( ) ; , . : // = -> &").split()


def text(r, n):
    """Source-like text: Zipf-weighted words, spaces, line breaks and indentation"""
    weights = [1.0 / (i + 1) for i in range(len(WORDS))]
    out = bytearray()
    while len(out) < n:
        line = "  " * r.randrange(4) + " ".join(r.choices(WORDS, weights, k=r.randrange(1, 12)))
        out += line.encode() + b"\n"
    return bytes(out[:n])


def rand(r, n):
    return bytes(r.getrandbits(8) for _ in range(n))


def alphabet(r, n, letters):
    return bytes(r.choice(letters) for _ in range(n))


def runs(r, n):
    out = bytearray()
    while len(out) < n:
        out += bytes([r.getrandbits(8)]) * r.randrange(1, 300)
    return bytes(out[:n])


def tokens(r, n):
    """Short tokens (3 to 6 bytes) from a small set: many short matches, many sequences a block"""
    toks = [rand(r, r.randrange(3, 7)) for _ in range(48)]
    out = bytearray()
    while len(out) < n:
        out += r.choice(toks)
    return bytes(out[:n])


def numbers(r, n):
    """Binary records: a counter, a slowly moving float and a small enum"""
    out = bytearray()
    i = 0
    x = 0.0
    while len(out) < n:
        x += r.uniform(-0.01, 0.01)
        out += struct.pack("<If", i, x) + bytes([r.randrange(4)])
        i += 1
    return bytes(out[:n])


def even_runs(r, n):
    """Runs of 64 of one byte value each, 2048 to a block: every sequence alike (RLE tables for all three codes)"""
    out = bytearray()
    v = 0
    while len(out) < n:
        v = (v + 1 + r.randrange(255)) & 255
        out += bytes([v]) * 64
    return bytes(out[:n])


def one_literal(r):
    """Two blocks with no 'z' in them (text, then random bytes), then 4 KiB copied from 256 KiB back and pieces of
    'z' and 63 bytes copied from 256 KiB back, the last block all such pieces: its literals are all 'z' (RLE
    literals), as no copy can run on past its piece"""
    d = 2 * 131072
    head = text(r, 8192).replace(b"z", b"y") + bytes(r.choice(range(255)) for _ in range(d - 8192))
    head = head.replace(b"z", b"\xff")
    out = bytearray(head)
    out += out[len(out) - d:len(out) - d + 4096]
    while len(out) < 2 * d:
        out += b"z" + out[len(out) + 1 - d:len(out) + 64 - d]
    return bytes(out)


def two_offsets(r):
    """Three blocks of random bytes, then 64 bytes copied from the frame's start, 64 from its byte 130, and 4-byte
    pieces copied from as far back as those two, in turn: the last block is over 32700 matches with no literals
    (the 3-byte sequence count). A byte at an even place is under 128 when its place is a multiple of 4 and 128 or
    more otherwise, so no copy runs on past its piece."""
    start = 3 * 131072
    out = bytearray(rand(r, start))
    for i in range(0, start, 2):
        out[i] = (out[i] & 127) | (0 if i % 4 == 0 else 128)
    d1 = start
    out += out[0:64]
    d2 = len(out) - 130
    out += out[130:194]
    k = 0
    while len(out) < 4 * 131072:
        d = d1 if k % 2 == 0 else d2
        out += out[len(out) - d:len(out) - d + 4]
        k += 1
    return bytes(out)


def small_numbers(r, n):
    """Bytes 0 to 10, skewed: a Huffman tree of few weights, written 4 bits each"""
    return bytes(min(10, int(r.expovariate(0.4))) for _ in range(n))


LCG_WORDS = [b"the", b"frame", b"block", b"of", b"and", b"literal", b"match", b"offset", b"a", b"sequence", b"table",
             b"to", b"window", b"is", b"in", b"state"]


def lcg_data(kind, n):
    """tests/roundtrip.cone's data() for kinds 3 (six letters) and 5 (words), byte for byte"""
    d = bytearray()
    s = 12345
    while len(d) < n:
        s = (s * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        if kind == 3:
            d.append(97 + (s >> 45) % 6)
        else:
            for ch in LCG_WORDS[(s >> 33) % 16]:
                if len(d) < n:
                    d.append(ch)
            if len(d) < n:
                d.append(10 if (s >> 60) == 0 else 32)
    return bytes(d)


def far_repeat(r, dist):
    """4 KiB, 'dist' - 4 KiB of noise, the same 4 KiB again: a match exactly 'dist' back"""
    head = rand(r, 4096)
    return head + rand(r, dist - 4096) + head


def corpus():
    r = random.Random(SEED)
    cases = []
    for n in (0, 1, 2, 3, 8, 9, 63, 64, 65, 255, 256, 257, 1023, 1024, 4096):
        cases.append(("text", text(r, n)))
    for n in (10000, 100000, 131071, 131072, 131073, 262145, 1000000):
        cases.append(("text", text(r, n)))
    for n in (100, 1000, 70000, 200000):
        cases.append(("random", rand(r, n)))
    cases.append(("zeros", bytes(300000)))
    cases.append(("one byte value", b"\x7f" * 4000))
    cases.append(("runs", runs(r, 200000)))
    cases.append(("six letters", alphabet(r, 100000, b"abcdef")))
    cases.append(("two letters", alphabet(r, 50000, b"xy")))
    cases.append(("all 256 values", bytes(range(256)) * 600))
    cases.append(("tokens", tokens(r, 300000)))
    cases.append(("numbers", numbers(r, 400000)))
    cases.append(("even runs", even_runs(r, 300000)))
    cases.append(("one literal", one_literal(r)))
    cases.append(("two offsets", two_offsets(r)))
    cases.append(("small numbers", small_numbers(r, 50000)))
    for d in (1 << 19, (1 << 19) + 1, 1 << 20, (1 << 20) + 1, 1 << 21, (1 << 21) + 1):
        cases.append(("repeat %d back" % d, far_repeat(r, d)))
    big = bytearray()
    while len(big) < 5 * 1024 * 1024:
        k = r.randrange(5)
        big += text(r, 200000) if k < 2 else rand(r, 50000) if k == 2 else numbers(r, 100000) if k == 3 \
            else bytes(big[-300000:-100000]) if len(big) > 300000 else runs(r, 20000)
    cases.append(("mixed 5 MB", bytes(big)))
    cases.append(("lcg six letters", lcg_data(3, 300000)))
    cases.append(("lcg words", lcg_data(5, 300000)))
    return cases


def main():
    work = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp(prefix="zstd-crosscheck-")
    os.makedirs(work, exist_ok=True)
    ref = zstdref.Zstd()
    cases = corpus()
    blob = bytearray(struct.pack("<I", len(cases)))
    for _, d in cases:
        blob += struct.pack("<I", len(d)) + d
    corpus_path = os.path.join(work, "corpus.bin")
    frames_path = os.path.join(work, "frames.bin")
    with open(corpus_path, "wb") as f:
        f.write(blob)
    congo = os.path.join(REPO, "tools", "congo", "congo.py")
    example = os.path.join(PKG, "examples", "crosscheck.cone")
    run = subprocess.run([sys.executable, congo, "run", "--release", example, "--", corpus_path, frames_path],
                         cwd=work, capture_output=True, text=True)
    lines = [l for l in run.stdout.splitlines() if l.strip() and not l.lstrip().startswith(("Compiling", "Linking",
                                                                                          "Finished", "Running"))]
    print("\n".join(lines))
    if run.returncode != 0:
        print(run.stderr)
        print("crosscheck.cone failed (exit %d)" % run.returncode)
        return 1
    with open(frames_path, "rb") as f:
        frames = f.read()
    nframes = struct.unpack_from("<I", frames, 0)[0]
    p = 4
    seen = set()
    golden = []
    good = bad = 0
    k = 0
    for name, d in cases:
        for level in (1, 2, 3):
            for way in (0, 1, 2):
                if way == 2 and len(d) > 4096:
                    continue
                n = struct.unpack_from("<I", frames, p)[0]
                fr = frames[p + 4:p + 4 + n]
                p += 4 + n
                k += 1
                try:
                    back = ref.decompress(fr)
                    ok = back == d
                except RuntimeError as e:
                    ok = False
                    print("  %s (%d bytes) level %d way %d: the reference refuses it: %s" % (name, len(d), level,
                                                                                             way, e))
                if ok:
                    good += 1
                    zstdref.modes_of(fr, seen)
                    if name.startswith("lcg") and way == 0:
                        golden.append("  %s, level %d: XXH64 of the frame (low 63 bits) %d" % (
                            name, level, ref.xxh64(fr) & 0x7FFFFFFFFFFFFFFF))
                else:
                    bad += 1
                    print("  %s (%d bytes) level %d way %d: WRONG" % (name, len(d), level, way))
    assert k == nframes and p == len(frames), "the frames file does not match the corpus"
    print("libzstd %s: %d inputs, %d frames; %d decoded to their input, %d did not" % (zstdref.LIB_VERSION,
                                                                                       len(cases), k, good, bad))
    print("modes used: " + ", ".join(sorted(seen)))
    print("tests/roundtrip.cone's golden values, read back by the reference:")
    print("\n".join(golden))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
