"""Makes the archives tests/stream.cone reads whose entries use data descriptors but are all deflated
(py-stream.zip, which make_fixtures.py makes, has a stored entry with data, which a stream cannot frame).

Python's zipfile, writing to a stream that cannot seek, writes each entry's local header with no sizes and
flag bit 3, then the data, then a data descriptor (signature, CRC-32, sizes).

    python make_stream_fixtures.py        (writes stream-desc.zip, stream-desc64.zip and stream-desc-small.zip beside it)
"""
import os
import zipfile

here = os.path.dirname(os.path.abspath(__file__))


class Unseekable:
    """A file that can only be written to in order"""
    def __init__(self, path):
        self.f = open(path, "wb")
        self.n = 0

    def write(self, b):
        self.n += len(b)
        return self.f.write(b)

    def tell(self):
        return self.n

    def flush(self):
        self.f.flush()

    def close(self):
        self.f.close()


def lines(n):
    return "".join("line %d: the quick brown fox jumps over the lazy dog\n" % i for i in range(n)).encode()


def make(name, force64):
    u = Unseekable(os.path.join(here, name))
    with zipfile.ZipFile(u, "w", zipfile.ZIP_DEFLATED) as z:
        for entry, data in (("one.txt", lines(50)), ("two/lines.txt", lines(900)),
                            ("two/short.txt", b"short\n"), ("three.txt", lines(3000))):
            with z.open(entry, "w", force_zip64=force64) as f:
                f.write(data)
        # an empty entry, stored, as Python writes a folder or an empty file
        with z.open("empty.txt", "w", force_zip64=force64) as f:
            pass
    u.close()


def make_small(name):
    u = Unseekable(os.path.join(here, name))
    with zipfile.ZipFile(u, "w", zipfile.ZIP_DEFLATED) as z:
        for entry, data in (("one.txt", lines(3)), ("two/short.txt", b"short\n")):
            with z.open(entry, "w") as f:
                f.write(data)
        with z.open("empty.txt", "w") as f:
            pass
    u.close()


make("stream-desc.zip", False)
make("stream-desc64.zip", True)
make_small("stream-desc-small.zip")
