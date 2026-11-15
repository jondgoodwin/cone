# MAKES: the compressed test streams of the brotli package and their index (index.txt), and copies the originals
# beside them. Needed ONLY to regenerate; the tests read the committed files and never call anything.
# RUN (in this folder), with the reference compressor and a checkout of github.com/google/brotli (MIT licence):
#   python makefixtures.py <brotli-repo> <conestd.lib> <rfc7932.txt>
# (the last two are optional; without the RFC's text the dictionary-words stream of makedict.py is left out)
#
# Two sources, both listed in index.txt as "stream original" pairs (paths relative to this folder):
#   google/   files copied from the Brotli repository's tests/testdata (its LICENSE is in google/LICENSE): the
#             small ones, and for each the stream its authors made. Not copied: the Canterbury text files
#             (alice29, asyoulik, lcet10, plrabn12), whose terms were not checked, and bb.binast (12 MB).
#   own/      three inputs of ours (the syntax diagrams page of the manual, a Cone source, the start of a binary
#             from our build) with their originals, compressed by the C 'brotli' command, version 1.2.0
#             (/mingw64/bin/brotli as Git for Windows installs it), at qualities 0, 5 and 11 and windows of 10,
#             16 and 24 bits: own/<name>.q<quality>w<window>.br. The command is checked to be that version.
import os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = "brotli"

GOOGLE = ["10x10y", "64x", "backward65536", "cp1251-utf16le", "cp852-utf8", "empty", "monkey", "quickfox",
          "ukkonooa", "x", "xyzzy", "zeros", "random_org_10k.bin",
          "mapsdatazrh"]
# Other streams of the same data: name.compressed.NN decodes to the original named here
GOOGLE_VARIANTS = {"empty": ["%02d" % i for i in range(17)], "x": ["00", "01", "02", "03"]}


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    repo = sys.argv[1]
    ver = subprocess.run([CLI, "--version"], capture_output=True, text=True).stdout.strip()
    if ver != "brotli 1.2.0":
        sys.exit("the reference compressor must be brotli 1.2.0, not " + repr(ver))
    td = os.path.join(repo, "tests", "testdata")
    index = []
    os.makedirs(os.path.join(HERE, "google"), exist_ok=True)
    shutil.copy(os.path.join(repo, "LICENSE"), os.path.join(HERE, "google", "LICENSE"))
    for n in GOOGLE:
        shutil.copy(os.path.join(td, n), os.path.join(HERE, "google", n))
        shutil.copy(os.path.join(td, n + ".compressed"), os.path.join(HERE, "google", n + ".compressed"))
        index.append("google/%s.compressed google/%s" % (n, n))
        for v in GOOGLE_VARIANTS.get(n, []):
            shutil.copy(os.path.join(td, n + ".compressed." + v), os.path.join(HERE, "google", n + ".compressed." + v))
            index.append("google/%s.compressed.%s google/%s" % (n, v, n))
    os.makedirs(os.path.join(HERE, "own"), exist_ok=True)
    lib = sys.argv[2] if len(sys.argv) > 2 else None
    inputs = {
        "manual.html": open(os.path.join(HERE, "..", "..", "..", "..", "doc", "manual", "syntax-diagrams.xhtml"), "rb").read()[:100000],
        "inflate.cone": open(os.path.join(HERE, "..", "..", "..", "deflate", "src", "inflate.cone"), "rb").read()[:60000],
    }
    if lib:
        inputs["conestd.bin"] = open(lib, "rb").read()[:65536]
    for name, data in inputs.items():
        p = os.path.join(HERE, "own", name)
        with open(p, "wb") as f:
            f.write(data)
        for q in (0, 5, 11):
            for w in (10, 16, 24):
                out = os.path.join(HERE, "own", "%s.q%dw%d.br" % (name, q, w))
                subprocess.run([CLI, "-q", str(q), "-w", str(w), "-f", "-c", p], stdout=open(out, "wb"), check=True)
                index.append("own/%s.q%dw%d.br own/%s" % (name, q, w, name))
    if len(sys.argv) > 3:
        import makedict
        stream, want = makedict.build(sys.argv[3])
        with open(os.path.join(HERE, "own", "dictwords.br"), "wb") as f:
            f.write(stream)
        with open(os.path.join(HERE, "own", "dictwords"), "wb") as f:
            f.write(want)
        got = subprocess.run([CLI, "-d", "-c", os.path.join(HERE, "own", "dictwords.br")], capture_output=True).stdout
        if got != want:
            sys.exit("the C brotli command does not read dictwords.br as makedict.py expects")
        index.append("own/dictwords.br own/dictwords")
    with open(os.path.join(HERE, "index.txt"), "w", newline="\n") as f:
        f.write("\n".join(index) + "\n")
    print(len(index), "streams")


main()
