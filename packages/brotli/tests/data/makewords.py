"""Makes own/words.w10.br and own/words.w16.br: 400,000 bytes of pseudo-random words (the same generator as
tests/streaming.cone's 'words'), compressed by the C 'brotli' command (1.2.0) at quality 5 with windows of 10 and 16
bits. A window this small against output this long makes the streaming decoder trim its history several times.

    python makewords.py
"""
import os
import subprocess

N = 400000


def words(n):
    w = ["alpha ", "beta ", "gamma ", "delta ", "epsilon "]
    d = bytearray()
    s = 3
    while len(d) < n:
        s = (s * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        d += w[(s >> 40) % 5].encode()
    return bytes(d[:n])


here = os.path.dirname(os.path.abspath(__file__))
raw = words(N)
for bits in (10, 16):
    out = subprocess.run(["brotli", "-q", "5", "-w", str(bits), "-c"], input=raw, stdout=subprocess.PIPE, check=True).stdout
    with open(os.path.join(here, "own", "words.w%d.br" % bits), "wb") as f:
        f.write(out)
    print(bits, len(out))
