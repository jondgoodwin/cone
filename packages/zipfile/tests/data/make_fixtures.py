"""Makes the small zip files the zip package's tests read (run it from this folder).

Python's zipfile is the oracle for the ordinary archives; the rest are built here
byte by byte from the PKWARE APPNOTE where the library would not write what the
test needs (zip64 records, flagless non-ASCII names, a symlink entry, an encrypted
flag). Nothing here is run by a test: the output is checked in.

    python make_fixtures.py             the archives Python can make
    python make_fixtures.py --windows   also the two made by Windows' own Compress-Archive
                                        (Windows PowerShell 5.1 and PowerShell 7)

The content is a fixed function of its name, which tests/*.cone repeats:
  hello.txt            "Hello, zip!\n"
  dir/sub/lorem.txt    300 lines "line N: the quick brown fox jumps over the lazy dog\n", N from 0
  dir/empty.txt        nothing
  bin/pattern.bin      2000 bytes, byte i = (i * 37 + i // 7) % 256
  unicode/héllo-ß-日本.txt   "unicode name\n"
  emptydir/            a folder entry
"""
import io
import os
import shutil
import struct
import subprocess
import sys
import zipfile
import zlib

STAMP = (2026, 10, 26, 12, 34, 56)   # fixed, so the files do not change from run to run


def lorem():
    return "".join("line %d: the quick brown fox jumps over the lazy dog\n" % n for n in range(300)).encode()


def pattern():
    return bytes((i * 37 + i // 7) % 256 for i in range(2000))


def files():
    return [
        ("hello.txt", b"Hello, zip!\n", zipfile.ZIP_DEFLATED),
        ("dir/sub/lorem.txt", lorem(), zipfile.ZIP_DEFLATED),
        ("dir/empty.txt", b"", zipfile.ZIP_STORED),
        ("bin/pattern.bin", pattern(), zipfile.ZIP_STORED),
        ("unicode/h\u00e9llo-\u00df-\u65e5\u672c.txt", b"unicode name\n", zipfile.ZIP_DEFLATED),
    ]


def info(name, method, attr=None, system=None):
    zi = zipfile.ZipInfo(name, STAMP)
    zi.compress_type = method
    if attr is not None:
        zi.external_attr = attr
    if system is not None:
        zi.create_system = system
    return zi


def basic(path, stream=False):
    """The ordinary archive; with stream=True written to a stream that cannot seek, so each
    entry gets a data descriptor (flag bit 3)."""
    class Unseekable(io.RawIOBase):
        def __init__(self):
            self.buf = io.BytesIO()

        def writable(self):
            return True

        def write(self, b):
            return self.buf.write(b)

    sink = Unseekable() if stream else open(path, "wb")
    with zipfile.ZipFile(sink, "w") as zf:
        for name, data, method in files():
            zf.writestr(info(name, method, 0o100644 << 16, 3), data)
        zf.writestr(info("emptydir/", zipfile.ZIP_STORED, (0o40755 << 16) | 0x10, 3), b"")
        zf.comment = b"fixture archive"
    if stream:
        with open(path, "wb") as f:
            f.write(sink.buf.getvalue())
    else:
        sink.close()


def zip64_local(path):
    """Python's own zip64 entry: written with force_zip64, so its local header carries the zip64 extra."""
    with zipfile.ZipFile(path, "w") as zf:
        zi = info("big.txt", zipfile.ZIP_DEFLATED)
        with zf.open(zi, "w", force_zip64=True) as w:
            w.write(lorem())
        zf.writestr(info("small.txt", zipfile.ZIP_STORED), b"small\n")


def le(fmt, *a):
    return struct.pack("<" + fmt, *a)


def dos(t):
    y, mo, d, h, mi, s = t
    return ((y - 1980) << 9) | (mo << 5) | d, (h << 11) | (mi << 5) | (s // 2)


def raw_zip(entries, zip64=False, comment=b"", prefix=b""):
    """Builds an archive by hand. Each entry is a dict: name (bytes), data (bytes), method (0 or 8),
    flags (default 0), attr (external attributes), extra_local, extra_central (bytes), z64 (put sizes and
    the offset in a zip64 extra of the central record), made_by (version made by, high byte the system)."""
    out = bytearray(prefix)
    base = len(prefix)
    central = bytearray()
    date, tm = dos(STAMP)
    for e in entries:
        data = e["data"]
        method = e.get("method", 0)
        body = zlib.compress(data)[2:-4] if method == 8 else data
        crc = zlib.crc32(data) & 0xFFFFFFFF
        flags = e.get("flags", 0)
        name = e["name"]
        offset = len(out) - base
        out += le("IHHHHHIIIHH", 0x04034B50, 20, flags, method, tm, date, crc, len(body), len(data), len(name),
                  len(e.get("extra_local", b"")))
        out += name + e.get("extra_local", b"") + body
        z64 = e.get("z64", False)
        extra = e.get("extra_central", b"")
        if z64 == "offset":
            # only the offset is in the zip64 field; the sizes stay in their 32-bit places
            extra = le("HHQ", 1, 8, offset) + extra
        elif z64:
            extra = le("HHQQQ", 1, 24, len(data), len(body), offset) + extra
        wide = z64 is True
        central += le("IHHHHHHIIIHHHHHII", 0x02014B50, e.get("made_by", 20), 20, flags, method, tm, date, crc,
                      0xFFFFFFFF if wide else len(body), 0xFFFFFFFF if wide else len(data), len(name), len(extra), 0,
                      0, 0, e.get("attr", 0), 0xFFFFFFFF if z64 else offset)
        central += name + extra
    cd_off = len(out) - base
    out += central
    n = len(entries)
    if zip64:
        z64_at = len(out) - base
        out += le("IQHHIIQQQQ", 0x06064B50, 44, 45, 45, 0, 0, n, n, len(central), cd_off)
        out += le("IIQI", 0x07064B50, 0, z64_at, 1)
        out += le("IHHHHIIH", 0x06054B50, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF, len(comment))
    else:
        out += le("IHHHHIIH", 0x06054B50, 0, 0, n, n, len(central), cd_off, len(comment))
    out += comment
    return bytes(out)


def build(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    p = lambda n: os.path.join(out_dir, n)

    basic(p("py-basic.zip"))
    basic(p("py-stream.zip"), stream=True)
    zip64_local(p("py-zip64-local.zip"))

    # A zip64 end record and zip64 central extras, for small sizes (the forcing is the point)
    with open(p("hand-zip64.zip"), "wb") as f:
        f.write(raw_zip([
            dict(name=b"a.txt", data=b"first\n", method=0, z64=True),
            dict(name=b"b/c.txt", data=lorem(), method=8, z64=True),
        ], zip64=True, comment=b"zip64 by hand"))

    # A zip64 central extra that holds the offset alone (an entry small but far into a big archive)
    with open(p("hand-zip64-offset.zip"), "wb") as f:
        f.write(raw_zip([
            dict(name=b"a.txt", data=b"first\n"),
            dict(name=b"far.txt", data=b"far into the archive\n", z64="offset"),
        ]))

    # Names that are not UTF-8 and carry no flag: code page 437, the format's default
    with open(p("hand-cp437.zip"), "wb") as f:
        f.write(raw_zip([
            dict(name="caf\u00e9-\u00fc.txt".encode("cp437"), data=b"cp437 name\n"),
            dict(name="plain.txt".encode(), data=b"ascii name\n"),
            dict(name="utf8-\u00e9.txt".encode("utf-8"), data=b"unflagged utf-8 name\n"),
        ]))

    # An archive with bytes in front of it, as a self-extractor leaves (offsets count from the zip's start)
    with open(p("hand-prefix.zip"), "wb") as f:
        f.write(raw_zip([dict(name=b"x.txt", data=b"after a prefix\n")], prefix=b"MZ" + bytes(range(100))))

    # Zip slip, one of each shape in one archive, and a good entry first
    slip = [dict(name=b"ok.txt", data=b"fine\n")]
    for n in (b"../evil1.txt", b"/abs2.txt", b"C:/drive3.txt", b"C:drive4.txt", b"..\\back5.txt",
              b"a/../../up6.txt", b"a/b/../../../up7.txt", b"\\\\server\\share\\unc8.txt"):
        slip.append(dict(name=n, data=b"evil\n"))
    with open(p("hand-slip.zip"), "wb") as f:
        f.write(raw_zip(slip))

    # A symlink entry (Unix mode 0120777 in the high half of the external attributes; the data is the target)
    with open(p("hand-symlink.zip"), "wb") as f:
        f.write(raw_zip([
            dict(name=b"real.txt", data=b"real\n", attr=0o100644 << 16, made_by=(3 << 8) | 20),
            dict(name=b"link", data=b"/etc/passwd", attr=0o120777 << 16, made_by=(3 << 8) | 20),
        ]))

    # Encrypted: flag bit 0 (traditional ZipCrypto), and WinZip AES (method 99)
    with open(p("hand-encrypted.zip"), "wb") as f:
        f.write(raw_zip([
            dict(name=b"clear.txt", data=b"clear\n"),
            dict(name=b"secret.txt", data=b"0123456789ab", flags=1),
        ]))
    with open(p("hand-aes.zip"), "wb") as f:
        f.write(raw_zip([dict(name=b"secret.txt", data=b"0123456789ab", flags=1, method=99)]))

    # Two entries with one name (the format allows it; extractors disagree about what it means)
    with open(p("hand-dup.zip"), "wb") as f:
        f.write(raw_zip([dict(name=b"d.txt", data=b"first\n"), dict(name=b"d.txt", data=b"second\n")]))

    # A method this package does not read (14, LZMA): the data is not LZMA, and is never touched
    with open(p("hand-lzma.zip"), "wb") as f:
        f.write(raw_zip([dict(name=b"x.lzma", data=b"not really lzma", method=14)]))


def windows(out_dir):
    """The two Compress-Archive archives, made from a folder of the same files."""
    work = os.path.join(os.environ.get("TEMP", "."), "zip-fixture-src")
    shutil.rmtree(work, ignore_errors=True)
    for name, data, _ in files():
        if name == "bin/pattern.bin":
            continue
        full = os.path.join(work, name)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.write(data)
    os.makedirs(os.path.join(work, "emptydir"), exist_ok=True)
    for exe, out in (("powershell", "win-ps51.zip"), ("pwsh", "win-ps7.zip")):
        dest = os.path.abspath(os.path.join(out_dir, out))
        if os.path.exists(dest):
            os.remove(dest)
        cmd = "Compress-Archive -Path '%s\\*' -DestinationPath '%s'" % (work, dest)
        subprocess.check_call([exe, "-NoProfile", "-Command", cmd])
    # Windows' tar (bsdtar) writes zip with -a: data descriptors, the extended timestamp, Unix modes
    dest = os.path.abspath(os.path.join(out_dir, "win-tar.zip"))
    if os.path.exists(dest):
        os.remove(dest)
    subprocess.check_call([os.path.join(os.environ.get("SystemRoot", "C:\\Windows"), "System32", "tar.exe"),
                           "-a", "-cf", dest, "-C", work, "hello.txt", "dir", "emptydir"])   # (Windows' tar crashes on the non-ASCII name)
    shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    build(here)
    if "--windows" in sys.argv:
        windows(here)
