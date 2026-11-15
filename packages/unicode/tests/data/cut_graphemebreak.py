"""Cuts tests/data/graphemebreak.txt from GraphemeBreakTest.txt.

    python cut_graphemebreak.py <folder of UCD files>      (in packages/unicode/tests/data)

Each test line keeps its code points and its breaks, in ASCII, comment dropped:
"/" is a break (U+00F7) and "x" is no break (U+00D7), e.g. "/ 000D x 000A /".
"""

import os
import sys

d = sys.argv[1]
out = []
for line in open(os.path.join(d, "GraphemeBreakTest.txt"), encoding="utf8"):
    line = line.split("#")[0].strip()
    if line:
        out.append(line.replace("÷", "/").replace("×", "x"))
here = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(here, "graphemebreak.txt"), "w", encoding="ascii", newline="\n") as f:
    f.write("\n".join(out) + "\n")
print(len(out), "lines")
