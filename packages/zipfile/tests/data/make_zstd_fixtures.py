"""Makes the zip files with method 93 (zstd) that tests/zstd.cone reads (run it from this folder).

Python's zipfile writes method 93 only from 3.14 (ZIP_ZSTANDARD); the Python this was made with, 3.13, does
not, so the archives are built here by hand, as make_fixtures.py builds its hand-made ones: each entry's data is
compressed by the REFERENCE zstd (libzstd 1.5.7 as Git for Windows installs it, called through ctypes by the
zstd package's tests/data/zstdref.py, which pins the library by SHA-256), then wrapped in zip headers by
make_fixtures.raw_zip. Nothing here is run by a test: the output is checked in. Every compressed body is checked
against the reference (and against the entry's CRC-32) before it is written, and every damaged one is checked to
be refused by the reference, so the files say what the comments say.

    python make_zstd_fixtures.py

The content is a fixed function of its name, which tests/zstd.cone repeats:
  hello.txt        "Hello, zstd!\\n"
  lorem.txt        300 lines "line N: the quick brown fox jumps over the lazy dog\\n", N from 0
  pattern.bin      2000 bytes, byte i = (i * 37 + i // 7) % 256
  noise.bin        3000 bytes of xorshift64 noise, seed 7 (see noise())
  rows.txt         6000 of the lorem lines (over 300 KB: several 128 KiB blocks)
"""
import os
import struct
import sys
import zlib

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
sys.path.insert(0, os.path.join(here, "..", "..", "..", "zstd", "tests", "data"))
import make_fixtures as mf          # noqa: E402  (raw_zip, lorem, pattern)
import zstdref                      # noqa: E402

Z = zstdref.Zstd()
MASK = (1 << 64) - 1


def lines(n):
    return "".join("line %d: the quick brown fox jumps over the lazy dog\n" % i for i in range(n)).encode()


def noise(n, seed):
    out = bytearray()
    x = (seed * 2862933555777941757 + 3037000493) & MASK
    for _ in range(n):
        x ^= (x << 13) & MASK
        x ^= x >> 7
        x ^= (x << 17) & MASK
        out.append(x & 255)
    return bytes(out)


HELLO = b"Hello, zstd!\n"
LOREM = mf.lorem()
PATTERN = mf.pattern()
NOISE = noise(3000, 7)
ROWS = lines(6000)


def packed(data, **kw):
    """The reference's frame of data, checked to decode back to it"""
    body = Z.compress(data, **kw)
    assert Z.decompress(body) == data
    return body


def skippable(payload):
    return struct.pack("<II", 0x184D2A50, len(payload)) + payload


def entry(name, data, method=93, **kw):
    """A zip entry: method 93 gets the frame given as body= (or the reference's level-3 frame of data)"""
    e = dict(name=name.encode(), data=data, method=method)
    if method == 93:
        e["body"] = kw.pop("body") if "body" in kw else packed(data)
    e.update(kw)
    return e


def raw_frame(window_byte, content):
    """A frame by hand: no content size, no checksum, a window descriptor of the caller's choosing, and one last
    raw block holding content"""
    assert len(content) < 1 << 21
    return struct.pack("<IBB", 0xFD2FB528, 0x00, window_byte) + struct.pack("<I", (len(content) << 3) | 1)[:3] + content


def build(out_dir):
    p = lambda n: os.path.join(out_dir, n)

    # ---- zstd-basic.zip: method 93 entries of several sizes, made several ways
    two_frames = packed(HELLO) + skippable(b"skip me") + packed(LOREM, level=19)
    with open(p("zstd-basic.zip"), "wb") as f:
        f.write(mf.raw_zip([
            entry("hello.txt", HELLO),
            entry("dir/sub/lorem.txt", LOREM, body=packed(LOREM, level=19)),
            entry("dir/empty.txt", b"", body=packed(b"")),
            entry("dir/nobytes.txt", b"", body=b""),                       # no frame at all, for no data
            entry("bin/pattern.bin", PATTERN),
            entry("bin/noise.bin", NOISE),                                 # incompressible: the frame holds raw blocks
            entry("big/rows.txt", ROWS, body=packed(ROWS, level=1)),
            entry("flags/nosum.txt", LOREM, body=packed(LOREM, checksum=False, content_size=False)),
            entry("flags/flushed.txt", LOREM, body=Z.compress_stream(LOREM, flush_every=1000)),
            entry("flags/frames.txt", HELLO + LOREM, body=two_frames),     # two frames and a skippable one between
        ], comment=b"zstd fixture archive"))

    # ---- zstd-mixed.zip: stored, deflate and zstd entries in one archive, in no particular order
    with open(p("zstd-mixed.zip"), "wb") as f:
        f.write(mf.raw_zip([
            entry("a-stored.txt", HELLO, method=0),
            entry("b-deflate.txt", LOREM, method=8),
            entry("c-zstd.txt", LOREM, body=packed(LOREM, level=9)),
            entry("d-zstd.bin", PATTERN),
            entry("e-deflate.bin", PATTERN, method=8),
            entry("sub/", b"", method=0),
            entry("sub/f-stored.bin", NOISE, method=0),
            entry("sub/g-zstd.txt", HELLO),
            entry("h-zstd-empty.txt", b"", body=packed(b"")),
            entry("i-deflate-empty.txt", b"", method=8),
        ]))

    # ---- zstd-bad.zip: damaged zstd entries, each a different way, with a good one first and last
    good = packed(LOREM)
    flipped = bytearray(good)
    flipped[len(good) // 2] ^= 0x40
    rejected_by_reference(bytes(flipped))
    cut = good[:-20]
    rejected_by_reference(cut)
    badsum = bytearray(good)
    badsum[-1] ^= 0xFF                                                     # the content checksum's last byte
    rejected_by_reference(bytes(badsum))
    badmagic = bytearray(good)
    badmagic[0] ^= 0xFF
    rejected_by_reference(bytes(badmagic))
    trailing = good + b"\x01\x02\x03 not a frame"
    rejected_by_reference(trailing)
    # a frame that names a dictionary (id 7): single segment, 5 bytes of content, one raw block
    dictframe = struct.pack("<IBBB", 0xFD2FB528, 0x21, 7, 5) + struct.pack("<I", (5 << 3) | 1)[:3] + b"hello"
    rejected_by_reference(dictframe)
    with open(p("zstd-bad.zip"), "wb") as f:
        f.write(mf.raw_zip([
            entry("first-ok.txt", LOREM, body=good),
            entry("flipped.txt", LOREM, body=bytes(flipped)),
            entry("cut.txt", LOREM, body=cut),
            entry("badsum.txt", LOREM, body=bytes(badsum)),
            entry("badmagic.txt", LOREM, body=bytes(badmagic)),
            entry("trailing.txt", LOREM, body=trailing),
            entry("dictionary.txt", b"hello", body=dictframe),
            entry("crc.txt", LOREM, body=good, crc=(zlib.crc32(LOREM) ^ 1) & 0xFFFFFFFF),      # the CRC-32 is wrong
            entry("size-over.txt", LOREM, body=good, usize=len(LOREM) + 10),                  # claims more than it holds
            entry("size-under.txt", LOREM, body=good, usize=len(LOREM) - 10),                 # claims less than it holds
            entry("last-ok.txt", HELLO),
        ]))

    # ---- zstd-window.zip: frames that claim a window, with 5 bytes of content each
    w28 = raw_frame(18 << 3, b"hello")        # window 2^28 = 256 MiB: past zstd's default of 128 MiB, a real frame
    w37 = raw_frame(27 << 3, b"hello")        # window 2^37 = 128 GiB: more than any decoder holds
    w27 = raw_frame(17 << 3, b"hello")        # window 2^27 = 128 MiB exactly: allowed
    assert Z.decompress(w27) == b"hello"
    assert Z.decompress(w28, window_log_max=31) == b"hello"
    try:
        Z.decompress(w28, window_log_max=27)
        raise SystemExit("the reference accepted a 256 MiB window at its default limit")
    except RuntimeError:
        pass
    rejected_by_reference(w37)
    with open(p("zstd-window.zip"), "wb") as f:
        f.write(mf.raw_zip([
            entry("ok.txt", HELLO),
            entry("w27.txt", b"hello", body=w27),
            entry("w28.txt", b"hello", body=w28),
            entry("w37.txt", b"hello", body=w37),
        ]))


def rejected_by_reference(body):
    try:
        Z.decompress(body)
    except RuntimeError:
        return
    raise SystemExit("the reference decoded a frame that was to be damaged")


if __name__ == "__main__":
    build(here)
