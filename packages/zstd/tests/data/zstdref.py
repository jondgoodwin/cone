# The reference zstd, for MAKING and CHECKING test data only: libzstd 1.5.7 as Git for Windows installs it, called
# through ctypes. Nothing here is shipped or linked into the package, and no test runs it: the scripts beside this
# file use it to write the .bin files the tests read, to check that what they wrote is what the reference makes of
# it, and (crosscheck.py) to decode what this package's compressor writes.
#
# Pinned: the library must be exactly this file (version and SHA-256 below), or the scripts refuse to run.
import ctypes, hashlib

LIB_PATH = r"C:\Program Files\Git\mingw64\bin\libzstd.dll"
LIB_VERSION = "1.5.7"
LIB_SHA256 = "b95c223a9548a9ecf51377c962e0bc8f0c51eb0c6f67a296dbc885996f0dd40d"

# ZSTD_cParameter values (zstd.h, 1.5.7)
C_LEVEL = 100
C_WINDOWLOG = 101
C_STRATEGY = 107
C_LDM = 160
C_CONTENTSIZE = 200
C_CHECKSUM = 201
C_DICTID = 202
C_LITERAL_MODE = 1002      # ZSTD_c_literalCompressionMode (experimental): 1 Huffman, 2 raw
C_TARGET_CBLOCK = 130      # ZSTD_c_targetCBlockSize

E_CONTINUE, E_FLUSH, E_END = 0, 1, 2


class _InBuf(ctypes.Structure):
    _fields_ = [("src", ctypes.c_void_p), ("size", ctypes.c_size_t), ("pos", ctypes.c_size_t)]


class _OutBuf(ctypes.Structure):
    _fields_ = [("dst", ctypes.c_void_p), ("size", ctypes.c_size_t), ("pos", ctypes.c_size_t)]


class Zstd:
    def __init__(self):
        with open(LIB_PATH, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        if digest != LIB_SHA256:
            raise SystemExit("libzstd is not the pinned file: sha256 " + digest)
        d = ctypes.CDLL(LIB_PATH)
        d.ZSTD_versionString.restype = ctypes.c_char_p
        if d.ZSTD_versionString().decode() != LIB_VERSION:
            raise SystemExit("libzstd is not version " + LIB_VERSION)
        d.ZSTD_isError.restype = ctypes.c_uint
        d.ZSTD_isError.argtypes = [ctypes.c_size_t]
        d.ZSTD_getErrorName.restype = ctypes.c_char_p
        d.ZSTD_getErrorName.argtypes = [ctypes.c_size_t]
        d.ZSTD_compressBound.restype = ctypes.c_size_t
        d.ZSTD_compressBound.argtypes = [ctypes.c_size_t]
        d.ZSTD_createCCtx.restype = ctypes.c_void_p
        d.ZSTD_freeCCtx.argtypes = [ctypes.c_void_p]
        d.ZSTD_CCtx_setParameter.restype = ctypes.c_size_t
        d.ZSTD_CCtx_setParameter.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        d.ZSTD_CCtx_loadDictionary.restype = ctypes.c_size_t
        d.ZSTD_CCtx_loadDictionary.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
        d.ZSTD_compress2.restype = ctypes.c_size_t
        d.ZSTD_compress2.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        d.ZSTD_compressStream2.restype = ctypes.c_size_t
        d.ZSTD_compressStream2.argtypes = [ctypes.c_void_p, ctypes.POINTER(_OutBuf), ctypes.POINTER(_InBuf), ctypes.c_int]
        d.ZSTD_createDCtx.restype = ctypes.c_void_p
        d.ZSTD_freeDCtx.argtypes = [ctypes.c_void_p]
        d.ZSTD_DCtx_setParameter.restype = ctypes.c_size_t
        d.ZSTD_DCtx_setParameter.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        d.ZSTD_DCtx_loadDictionary.restype = ctypes.c_size_t
        d.ZSTD_DCtx_loadDictionary.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
        d.ZSTD_decompressStream.restype = ctypes.c_size_t
        d.ZSTD_decompressStream.argtypes = [ctypes.c_void_p, ctypes.POINTER(_OutBuf), ctypes.POINTER(_InBuf)]
        d.ZSTD_XXH64.restype = ctypes.c_uint64
        d.ZSTD_XXH64.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint64]
        self.d = d

    def _check(self, r, what):
        if self.d.ZSTD_isError(r):
            raise RuntimeError(what + ": " + self.d.ZSTD_getErrorName(r).decode())
        return r

    def compress(self, data, level=3, checksum=True, content_size=True, window_log=0, ldm=False, literal_mode=0,
                 dictionary=None, dict_id=True, target_cblock=0):
        """One call: ZSTD_compress2 with these parameters"""
        d = self.d
        c = d.ZSTD_createCCtx()
        try:
            self._params(c, level, checksum, content_size, window_log, ldm, literal_mode, dictionary, dict_id,
                         target_cblock)
            cap = d.ZSTD_compressBound(len(data))
            out = ctypes.create_string_buffer(cap)
            n = self._check(d.ZSTD_compress2(c, out, cap, data, len(data)), "compress2")
            return out.raw[:n]
        finally:
            d.ZSTD_freeCCtx(c)

    def compress_stream(self, data, level=3, checksum=True, flush_every=0, window_log=0, ldm=False):
        """Streaming: the content size is not known, and a flush every 'flush_every' bytes ends a block there"""
        d = self.d
        c = d.ZSTD_createCCtx()
        try:
            self._params(c, level, checksum, True, window_log, ldm, 0, None, True, 0)
            src = ctypes.create_string_buffer(data, len(data)) if data else ctypes.create_string_buffer(1)
            base = ctypes.addressof(src)
            out = bytearray()
            cap = 1 << 17
            ob = ctypes.create_string_buffer(cap)
            pos = 0
            step = flush_every if flush_every > 0 else max(len(data), 1)
            while True:
                end = min(len(data), pos + step)
                last = end >= len(data)
                ib = _InBuf(base + pos, end - pos, 0)
                mode = E_END if last else E_FLUSH
                while True:
                    o = _OutBuf(ctypes.addressof(ob), cap, 0)
                    r = self._check(d.ZSTD_compressStream2(c, ctypes.byref(o), ctypes.byref(ib), mode), "stream")
                    out += ob.raw[:o.pos]
                    if r == 0 and ib.pos == ib.size:
                        break
                pos = end
                if last:
                    return bytes(out)
        finally:
            d.ZSTD_freeCCtx(c)

    def _params(self, c, level, checksum, content_size, window_log, ldm, literal_mode, dictionary, dict_id,
                target_cblock):
        s = self.d.ZSTD_CCtx_setParameter
        self._check(s(c, C_LEVEL, level), "level")
        self._check(s(c, C_CHECKSUM, 1 if checksum else 0), "checksum")
        self._check(s(c, C_CONTENTSIZE, 1 if content_size else 0), "contentsize")
        self._check(s(c, C_DICTID, 1 if dict_id else 0), "dictid")
        if window_log:
            self._check(s(c, C_WINDOWLOG, window_log), "windowlog")
        if ldm:
            self._check(s(c, C_LDM, 1), "ldm")
        if literal_mode:
            self._check(s(c, C_LITERAL_MODE, literal_mode), "literal mode")
        if target_cblock:
            self._check(s(c, C_TARGET_CBLOCK, target_cblock), "target cblock")
        if dictionary is not None:
            self._check(self.d.ZSTD_CCtx_loadDictionary(c, dictionary, len(dictionary)), "dictionary")

    def decompress(self, data, dictionary=None, window_log_max=31):
        """The reference's verdict: the bytes, or an exception naming its error"""
        d = self.d
        x = d.ZSTD_createDCtx()
        try:
            self._check(d.ZSTD_DCtx_setParameter(x, 100, window_log_max), "windowLogMax")
            if dictionary is not None:
                self._check(d.ZSTD_DCtx_loadDictionary(x, dictionary, len(dictionary)), "dictionary")
            src = ctypes.create_string_buffer(data, len(data)) if data else ctypes.create_string_buffer(1)
            ib = _InBuf(ctypes.addressof(src), len(data), 0)
            cap = 1 << 17
            ob = ctypes.create_string_buffer(cap)
            out = bytearray()
            r = 1
            while True:
                o = _OutBuf(ctypes.addressof(ob), cap, 0)
                r = self._check(d.ZSTD_decompressStream(x, ctypes.byref(o), ctypes.byref(ib)), "decompress")
                out += ob.raw[:o.pos]
                # Done when the input is used and the frame is complete (r == 0, even when this call filled the
                # buffer exactly, as a 128 KiB output does) or nothing more will come out
                if ib.pos == ib.size and (r == 0 or o.pos < cap):
                    break
            if r != 0:
                raise RuntimeError("decompress: the input ends inside a frame")
            return bytes(out)
        finally:
            d.ZSTD_freeDCtx(x)

    def xxh64(self, data, seed=0):
        return self.d.ZSTD_XXH64(data, len(data), seed)


# -------- a block-structure reader: which modes a frame uses (for coverage, not decoding) --------

LIT = ["raw", "rle", "huffman", "treeless"]
SEQ = ["predefined", "rle", "fse", "repeat"]


def le(b, p, n):
    return int.from_bytes(b[p:p + n], "little")


def modes_of(buf, seen):
    """Adds to 'seen' (a set) a word for every feature the frames in buf use"""
    p = 0
    while p < len(buf):
        magic = le(buf, p, 4)
        if magic & 0xFFFFFFF0 == 0x184D2A50:
            seen.add("frame:skippable")
            p += 8 + le(buf, p + 4, 4)
            continue
        assert magic == 0xFD2FB528, "not a frame"
        fhd = buf[p + 4]
        single = fhd & 0x20
        did = [0, 1, 2, 4][fhd & 3]
        fcs = [1 if single else 0, 2, 4, 8][fhd >> 6]
        seen.add("frame:" + ("single-segment" if single else "windowed"))
        seen.add("frame:fcs%d" % fcs)
        if fhd & 4:
            seen.add("frame:checksum")
        p += 5 + (0 if single else 1) + did + fcs
        while True:
            bh = le(buf, p, 3)
            last, kind, size = bh & 1, (bh >> 1) & 3, bh >> 3
            p += 3
            if kind == 0:
                seen.add("block:raw")
                p += size
            elif kind == 1:
                seen.add("block:rle")
                p += 1
            else:
                seen.add("block:compressed")
                block_modes(buf[p:p + size], seen)
                p += size
            if last:
                break
        if fhd & 4:
            p += 4


def block_modes(b, seen):
    lt, sf = b[0] & 3, (b[0] >> 2) & 3
    if lt < 2:
        hs = [1, 2, 1, 3][sf]
        regen = (b[0] >> 3) if sf in (0, 2) else (le(b, 0, hs) >> 4)
        seen.add("lit:%s:%dbyte-header" % (LIT[lt], hs))
        q = hs + (regen if lt == 0 else 1)
    else:
        hs, bits = [(3, 10), (3, 10), (4, 14), (5, 18)][sf]
        h = le(b, 0, hs)
        csize = (h >> (4 + bits)) & ((1 << bits) - 1)
        seen.add("lit:%s:%s:sf%d" % (LIT[lt], "1stream" if sf == 0 else "4streams", sf))
        if lt == 2:
            seen.add("huffman-weights:" + ("direct" if b[hs] >= 128 else "fse"))
        q = hs + csize
    n0 = b[q]
    if n0 == 0:
        seen.add("seq:none")
        return
    seen.add("seq:count-%dbyte" % (1 if n0 < 128 else 2 if n0 < 255 else 3))
    q += 1 if n0 < 128 else 2 if n0 < 255 else 3
    m = b[q]
    for name, v in (("ll", m >> 6), ("of", (m >> 4) & 3), ("ml", (m >> 2) & 3)):
        seen.add("seq:%s:%s" % (name, SEQ[v]))
