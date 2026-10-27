"""The third oracle for core's FloatText: Python's repr of a float.

floatproof.cone, given a file, writes one line per checked f64 it samples:
its bits in hex and the text FloatText made of it. Python's repr is the
shortest text that reads back as the same float (David Gay's), written with
the same rule for fixed and exponent form, so each text must equal it byte
for byte.

    python floatproof_repr.py lines.txt
"""

import struct
import sys


def main(path):
    count = 0
    bad = 0
    with open(path, "rb") as f:
        for raw in f:
            hexbits, text = raw.decode("ascii").rstrip("\n").split(" ", 1)
            x = struct.unpack("<d", struct.pack("<Q", int(hexbits, 16)))[0]
            want = repr(x)
            count += 1
            if text != want:
                bad += 1
                if bad <= 20:
                    print(f"DIFFERS bits {hexbits} FloatText {text} repr {want}")
    print(f"lines {count}, differing from repr {bad}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
