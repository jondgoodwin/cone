// The llmkernels package's shared shader code: reading the bytes of a
// quantised weight row out of a buffer of 32-bit words, decoding a weight
// of each of the five types, and the index fold every kernel uses. It is a
// module, with no entry points: tools/shaders/shaders.py compiles the
// shaders that import it ('import llmcommon;'), not it alone.
//
// The block layouts are GGUF's (ggml's), and are named in llmkernels.cone's
// header: the same arithmetic in the same order as blocks.cone's CPU
// decoders, which the tests compare against.
//
//   F32   4 bytes an element
//   F16   2 bytes an element, an IEEE half
//   Q8_0  34-byte block of 32 elements: half d; int8 qs[32]
//   Q4_K  144-byte super-block of 256: half d; half dmin; uint8 scales[12]; uint8 qs[128]
//   Q6_K  210-byte super-block of 256: uint8 ql[128]; uint8 qh[64]; int8 scales[16]; half d
//
// A weight matrix is rows of 'cols' elements, each row a whole number of
// blocks, rows one after another. Its bytes live in a StructuredBuffer<uint>
// (little-endian words: byte b of the matrix is bits 8 (b & 3) of word
// b >> 2), so no 8-bit or 16-bit storage feature is needed; a half is
// unpacked with f16tof32 (a conversion, not 16-bit arithmetic). Every access
// is by byte offset, because a 34- or 210-byte block starts on any even byte.
//
// The decoders take 'rowByte', the byte offset of the row's first block in
// the buffer, and a column.

static const uint QTYPE_F32 = 0u;
static const uint QTYPE_F16 = 1u;
static const uint QTYPE_Q8_0 = 8u;
static const uint QTYPE_Q4_K = 12u;
static const uint QTYPE_Q6_K = 14u;

uint loadU8(StructuredBuffer<uint> b, uint byteOffset) {
    return (b[byteOffset >> 2u] >> ((byteOffset & 3u) * 8u)) & 0xFFu;
}

int loadI8(StructuredBuffer<uint> b, uint byteOffset) {
    return int(loadU8(b, byteOffset) ^ 0x80u) - 128;
}

// A 16-bit value at an even byte offset
uint loadU16(StructuredBuffer<uint> b, uint byteOffset) {
    return (b[byteOffset >> 2u] >> ((byteOffset & 2u) * 8u)) & 0xFFFFu;
}

float loadF16(StructuredBuffer<uint> b, uint byteOffset) {
    return f16tof32(loadU16(b, byteOffset));
}

// Q4_K's six-bit scale and minimum number j (0 to 7) from its 12 bytes at 'sbase'
void scaleMinK4(StructuredBuffer<uint> b, uint sbase, uint j, out uint sc, out uint mn) {
    if (j < 4u) {
        sc = loadU8(b, sbase + j) & 63u;
        mn = loadU8(b, sbase + j + 4u) & 63u;
    } else {
        sc = (loadU8(b, sbase + j + 4u) & 0xFu) | ((loadU8(b, sbase + j - 4u) >> 6u) << 4u);
        mn = (loadU8(b, sbase + j + 4u) >> 4u) | ((loadU8(b, sbase + j) >> 6u) << 4u);
    }
}

// The element at column 'col' of the row at byte 'rowByte', of any type: one
// element at a time, as the tiled matmul reads its weights
float weightAt(StructuredBuffer<uint> w, uint qtype, uint rowByte, uint col) {
    if (qtype == QTYPE_F32) {
        return asfloat(w[(rowByte >> 2u) + col]);
    }
    if (qtype == QTYPE_F16) {
        return loadF16(w, rowByte + col * 2u);
    }
    if (qtype == QTYPE_Q8_0) {
        uint base = rowByte + (col >> 5u) * 34u;
        float d = loadF16(w, base);
        return d * float(loadI8(w, base + 2u + (col & 31u)));
    }
    if (qtype == QTYPE_Q4_K) {
        uint base = rowByte + (col >> 8u) * 144u;
        uint e = col & 255u;
        uint g = e >> 6u;            // the 64-element group
        uint h = (e >> 5u) & 1u;     // low nibbles (0) or high nibbles (1)
        uint l = e & 31u;
        float d = loadF16(w, base);
        float dmin = loadF16(w, base + 2u);
        uint sc;
        uint mn;
        scaleMinK4(w, base + 4u, 2u * g + h, sc, mn);
        float d1 = d * float(sc);
        float m1 = dmin * float(mn);
        uint q = loadU8(w, base + 16u + 32u * g + l);
        uint nib = (h == 0u) ? (q & 0xFu) : (q >> 4u);
        return d1 * float(nib) - m1;
    }
    // QTYPE_Q6_K
    uint base = rowByte + (col >> 8u) * 210u;
    uint e = col & 255u;
    uint n = e >> 7u;                // the 128-element half
    uint k = (e >> 5u) & 3u;         // which 32 of it
    uint l = e & 31u;
    float d = loadF16(w, base + 208u);
    uint qlb = loadU8(w, base + 64u * n + 32u * (k & 1u) + l);
    uint low = (k < 2u) ? (qlb & 0xFu) : (qlb >> 4u);
    uint qhb = loadU8(w, base + 128u + 32u * n + l);
    uint hi = (qhb >> (2u * k)) & 3u;
    int q = int(low | (hi << 4u)) - 32;
    int sc = loadI8(w, base + 192u + 8u * n + 2u * k + (l >> 4u));
    return (d * float(sc)) * float(q);
}

// The partial dot product of the 32 elements of unit 'u' of a row (columns
// 32 u to 32 u + 31, fewer at the end of an F32 or F16 row) with x[xo + column].
// A unit is a Q8_0 block, an eighth of a Q4_K or Q6_K super-block, or 32
// elements of an F32 or F16 row. The elements are added in column order.
float unitDot(StructuredBuffer<uint> w, uint qtype, uint rowByte, uint u, uint cols,
              StructuredBuffer<float> x, uint xo) {
    float acc = 0.0;
    if (qtype == QTYPE_F32) {
        uint c0 = u * 32u;
        uint n = min(32u, cols - c0);
        for (uint l = 0u; l < n; l++) {
            acc += asfloat(w[(rowByte >> 2u) + c0 + l]) * x[xo + c0 + l];
        }
        return acc;
    }
    if (qtype == QTYPE_F16) {
        uint c0 = u * 32u;
        uint n = min(32u, cols - c0);
        for (uint l = 0u; l < n; l++) {
            acc += loadF16(w, rowByte + (c0 + l) * 2u) * x[xo + c0 + l];
        }
        return acc;
    }
    if (qtype == QTYPE_Q8_0) {
        uint base = rowByte + u * 34u;
        float d = loadF16(w, base);
        for (uint l = 0u; l < 32u; l++) {
            acc += (d * float(loadI8(w, base + 2u + l))) * x[xo + u * 32u + l];
        }
        return acc;
    }
    uint sb = u >> 3u;               // the super-block
    uint j = u & 7u;                 // which eighth of it
    if (qtype == QTYPE_Q4_K) {
        uint base = rowByte + sb * 144u;
        uint g = j >> 1u;
        uint h = j & 1u;
        float d = loadF16(w, base);
        float dmin = loadF16(w, base + 2u);
        uint sc;
        uint mn;
        scaleMinK4(w, base + 4u, j, sc, mn);
        float d1 = d * float(sc);
        float m1 = dmin * float(mn);
        for (uint l = 0u; l < 32u; l++) {
            uint q = loadU8(w, base + 16u + 32u * g + l);
            uint nib = (h == 0u) ? (q & 0xFu) : (q >> 4u);
            acc += (d1 * float(nib) - m1) * x[xo + sb * 256u + 64u * g + 32u * h + l];
        }
        return acc;
    }
    // QTYPE_Q6_K
    uint base = rowByte + sb * 210u;
    uint n = j >> 2u;
    uint k = j & 3u;
    float d = loadF16(w, base + 208u);
    for (uint l = 0u; l < 32u; l++) {
        uint qlb = loadU8(w, base + 64u * n + 32u * (k & 1u) + l);
        uint low = (k < 2u) ? (qlb & 0xFu) : (qlb >> 4u);
        uint qhb = loadU8(w, base + 128u + 32u * n + l);
        uint hi = (qhb >> (2u * k)) & 3u;
        int q = int(low | (hi << 4u)) - 32;
        int sc = loadI8(w, base + 192u + 8u * n + 2u * k + (l >> 4u));
        acc += ((d * float(sc)) * float(q)) * x[xo + sb * 256u + 128u * n + 32u * k + l];
    }
    return acc;
}

// A kernel launched as a grid of groups too wide for one dimension (a
// dispatch is at most 65535 groups a way) is folded: the CPU sends groupsX
// groups across and as many rows of them as it takes; the group's number is
// y * groupsX + x
uint foldedGroup(uint3 group, uint groupsX) {
    return group.y * groupsX + group.x;
}
