"""Writes Test_Quick_Changes.tzif, a synthetic version 2 TZif file for tests/localtime.cone.

Two time types: type 0 'AAA' at UTC+0 and type 1 'BBB' at UTC+1 (daylight saving flag set), two
transitions 12 hours apart, and the footer 'AAA0' (no rule). So the zone's offset is +0 until
2020-01-01T00:00:00Z, +1 until 2020-01-01T12:00:00Z, +0 after: a gap of an hour (wall 00:00 to
00:59:59 on 1 January) and 12 hours later an overlap of an hour (wall 12:00 to 12:59:59).

    python make_synthetic.py
"""
import struct

T1 = 1577836800            # 2020-01-01T00:00:00Z
T2 = T1 + 12 * 3600


def header(isut, isstd, leap, times, types, chars):
    return b"TZif" + b"2" + bytes(15) + struct.pack(">6I", isut, isstd, leap, times, types, chars)


v1 = header(0, 0, 0, 0, 1, 1) + struct.pack(">ibB", 0, 0, 0) + b"\0"
chars = b"AAA\0BBB\0"
v2 = (header(0, 0, 0, 2, 2, len(chars))
      + struct.pack(">2q", T1, T2) + bytes([1, 0])
      + struct.pack(">ibB", 0, 0, 0) + struct.pack(">ibB", 3600, 1, 4)
      + chars + b"\nAAA0\n")
with open("Test_Quick_Changes.tzif", "wb") as f:
    f.write(v1 + v2)
