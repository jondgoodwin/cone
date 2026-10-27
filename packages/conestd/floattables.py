"""Writes floattables.cone: the powers of five that floatfmt.cone's Ryu needs.

    python floattables.py          (in packages/conestd; rewrites floattables.cone)

Ryu (Ulf Adams, "Ryu: fast float-to-string conversion", PLDI 2018) scales an
f64's mantissa by a power of ten with one 64 x 128-bit multiply, taking the
power of five (or its inverse) as a 125-bit number:

  pow5[i]     5^i made 125 bits long: 5^i * 2^(125 - bits(5^i)), cut to
              an integer (the low bits dropped once 5^i is longer), i < 326
  pow5inv[q]  2^(bits(5^q) - 1 + 125) / 5^q, cut to an integer, plus one,
              q < 342

bits(n) is the length of n in bits. Each entry is two u64 words, low then
high, one after the other in a flat array. Nothing but Python's integers.
"""

import os

POW5_BITCOUNT = 125
POW5_INV_BITCOUNT = 125
POW5_TABLE_SIZE = 326
POW5_INV_TABLE_SIZE = 342
MASK64 = (1 << 64) - 1


def pow5(i):
    p = 5 ** i
    shift = p.bit_length() - POW5_BITCOUNT
    return p >> shift if shift >= 0 else p << -shift


def pow5inv(q):
    p = 5 ** q
    j = p.bit_length() - 1 + POW5_INV_BITCOUNT
    return (1 << j) // p + 1


def table(name, values, what):
    lines = [f"// {what}", f"let {name}: Array[u64, {2 * len(values)}] = ["]
    for k, v in enumerate(values):
        assert v.bit_length() <= 128
        sep = "," if k + 1 < len(values) else ""
        lines.append(f"  0x{v & MASK64:016X}u64, 0x{v >> 64:016X}u64{sep}")
    lines.append("];")
    return "\n".join(lines)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "floattables.cone")
    text = "\n".join([
        "// floattables: the powers of five floatfmt.cone's Ryu multiplies by, two u64",
        "// words to an entry, low then high. Written by floattables.py, which says how",
        "// each is made; change that and run it, never this file.",
        "",
        table("pow5Split", [pow5(i) for i in range(POW5_TABLE_SIZE)],
              "5^i made 125 bits long, i from 0 to 325"),
        "",
        table("pow5InvSplit", [pow5inv(q) for q in range(POW5_INV_TABLE_SIZE)],
              "2^(bits(5^q) - 1 + 125) / 5^q, plus one, q from 0 to 341"),
        "",
    ])
    with open(out, "w", encoding="ascii", newline="\n") as f:
        f.write(text)


if __name__ == "__main__":
    main()
