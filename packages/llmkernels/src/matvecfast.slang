// y = W x for a Q6_K, Q8_0 or F16 weight matrix at the speed of the GPU's memory (matvecq4k.slang is the Q4_K one, and
// explains the plan; this is the same). One subgroup (32 invocations) takes one row, a workgroup of four subgroups
// four rows. The invocations read the row's bytes in order, 16 bytes each, in 32-bit words (these blocks are not
// 4-byte aligned: a Q8_0 block is 34 bytes and a Q6_K super-block 210, so a block starts 2 bytes into a word every other
// time, and `load16` joins two words' halves), multiply them with x read as float4s, and the subgroup adds its
// partial sums with WaveActiveSum. One entry point for each type, sharing the bindings: params (by value), the
// weights' words, x (as float4), y.
//
//  - Q6_K (210-byte super-block: 128 bytes of low nibbles, 64 of high bit pairs, 16 int8 scales, half d): eight
//    invocations to a super-block, four super-blocks at a time. Invocation c = (n, lh, kk) takes the 128-element
//    half n, the 16 elements lh of each of its 32-element groups k = kk and k = kk + 2, which are the low and the
//    high nibbles of the same 16 bytes: 16 bytes of nibbles, 16 of bit pairs, two scales, 32 elements.
//  - Q8_0 (34-byte block: half d, 32 int8): two invocations to a block (16 elements each), 16 blocks at a time.
//  - F16: eight halves (16 bytes) an invocation, 256 elements at a time.
//
// The weights must start on an even byte (wByteOffset) and x on a multiple of 4 floats (xOffset); an F16 row is a
// multiple of 8 elements. Kernels checks these, and that the subgroup is 32 wide. The sums are added in a different
// order from the CPU twin's, so a result differs from it in the last bits of an f32, within llmreference's
// sumTolerance.
//
// slangc: -fp-mode precise

import llmcommon;

struct MatvecParams {
    uint rows;          // output elements
    uint cols;          // input elements: a whole number of blocks (256 for Q6_K, 32 for Q8_0, 8 for F16)
    uint rowBytes;      // bytes from one row to the next
    uint groupsX;       // the dispatch's width in groups (rows beyond it fold into y)
    uint wByteOffset;   // byte offset of row 0 in the weights buffer: even
    uint xOffset;       // x[xOffset ..] is the input: a multiple of 4
    uint yOffset;       // y[yOffset ..] is the output
    uint qtype;         // unused here
};

[[vk::binding(0, 0)]] StructuredBuffer<MatvecParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<uint> weights;
[[vk::binding(2, 0)]] StructuredBuffer<float4> x;
[[vk::binding(3, 0)]] RWStructuredBuffer<float> y;

// The 16 bytes at byte 'addr' (even) as four words. The fifth word is read only when the address is 2 bytes into a
// word, when its low half is among the bytes, so no read goes past the 16 bytes' own words.
uint4 load16(uint addr) {
    uint w = addr >> 2u;
    uint a = weights[w];
    uint b = weights[w + 1u];
    uint c = weights[w + 2u];
    uint d = weights[w + 3u];
    if ((addr & 2u) != 0u) {
        uint e = weights[w + 4u];
        return uint4((a >> 16u) | (b << 16u), (b >> 16u) | (c << 16u), (c >> 16u) | (d << 16u), (d >> 16u) | (e << 16u));
    }
    return uint4(a, b, c, d);
}

// The byte at 'addr'
uint byteAt(uint addr) {
    return (weights[addr >> 2u] >> ((addr & 3u) * 8u)) & 255u;
}

// The 16-bit value at the even byte 'addr'
uint halfAt(uint addr) {
    return (weights[addr >> 2u] >> ((addr & 2u) * 8u)) & 0xFFFFu;
}

// ---- Q6_K ----

// Four elements: the low 4 bits of byte b are the word's bits (8 b + sh) to (8 b + sh + 3), the high 2 bits the word
// wh's bits (8 b + hs) and (8 b + hs + 1); the value is the six bits less 32. Answers the sum of six-bit x and, in
// sx, the sum of the four x (so the caller can take 32 of it off at the end).
float sixes(uint wl, uint wh, uint sh, uint hs, float4 v, out float sx) {
    uint q0 = ((wl >> sh) & 15u) | (((wh >> hs) & 3u) << 4u);
    uint q1 = ((wl >> (8u + sh)) & 15u) | (((wh >> (8u + hs)) & 3u) << 4u);
    uint q2 = ((wl >> (16u + sh)) & 15u) | (((wh >> (16u + hs)) & 3u) << 4u);
    uint q3 = ((wl >> (24u + sh)) & 15u) | (((wh >> (24u + hs)) & 3u) << 4u);
    sx = (v.x + v.y) + (v.z + v.w);
    return float(q0) * v.x + float(q1) * v.y + float(q2) * v.z + float(q3) * v.w;
}

[shader("compute")]
[numthreads(128, 1, 1)]
void matvecQ6K(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex) {
    MatvecParams p = params[0];
    uint lane = tid & 31u;
    uint row = foldedGroup(gid, p.groupsX) * 4u + (tid >> 5u);
    // the whole subgroup has the same row, so all leave or none
    if (row >= p.rows) { return; }
    uint nsb = p.cols >> 8u;
    uint sbl = lane >> 3u;
    uint c = lane & 7u;
    uint n = c >> 2u;               // the 128-element half
    uint lh = (c >> 1u) & 1u;       // the first or second 16 of a 32-element group
    uint kk = c & 1u;               // groups kk (low nibbles) and kk + 2 (high nibbles)
    uint rowByte = p.wByteOffset + row * p.rowBytes;
    float acc = 0.0;
    // every invocation runs the same number of steps (a super-block past the row's end is read as the first, and its
    // sum left out), so the subgroup is whole at the sum below
    for (uint step = 0u; step < ((nsb + 3u) >> 2u); step++) {
        uint sbRaw = step * 4u + sbl;
        bool inRow = sbRaw < nsb;
        uint sb = inRow ? sbRaw : 0u;
        uint xa = (p.xOffset + sb * 256u + 128u * n + 32u * kk + 16u * lh) >> 2u;
        uint xb = xa + 16u;
        float4 xa0 = x[xa], xa1 = x[xa + 1u], xa2 = x[xa + 2u], xa3 = x[xa + 3u];
        float4 xb0 = x[xb], xb1 = x[xb + 1u], xb2 = x[xb + 2u], xb3 = x[xb + 3u];
        uint base = rowByte + sb * 210u;
        uint4 ql = load16(base + 64u * n + 32u * kk + 16u * lh);
        uint4 qh = load16(base + 128u + 32u * n + 16u * lh);
        float d = halfBitsToFloat(halfAt(base + 208u));
        float scA = float(int(byteAt(base + 192u + 8u * n + 2u * kk + lh) ^ 0x80u) - 128);
        float scB = float(int(byteAt(base + 192u + 8u * n + 2u * (kk + 2u) + lh) ^ 0x80u) - 128);
        float sa0, sa1, sa2, sa3, sb0, sb1, sb2, sb3;
        float qa = (sixes(ql.x, qh.x, 0u, 2u * kk, xa0, sa0) + sixes(ql.y, qh.y, 0u, 2u * kk, xa1, sa1))
                 + (sixes(ql.z, qh.z, 0u, 2u * kk, xa2, sa2) + sixes(ql.w, qh.w, 0u, 2u * kk, xa3, sa3));
        float qb = (sixes(ql.x, qh.x, 4u, 2u * kk + 4u, xb0, sb0) + sixes(ql.y, qh.y, 4u, 2u * kk + 4u, xb1, sb1))
                 + (sixes(ql.z, qh.z, 4u, 2u * kk + 4u, xb2, sb2) + sixes(ql.w, qh.w, 4u, 2u * kk + 4u, xb3, sb3));
        float sxa = (sa0 + sa1) + (sa2 + sa3);
        float sxb = (sb0 + sb1) + (sb2 + sb3);
        float part = d * (scA * (qa - 32.0 * sxa) + scB * (qb - 32.0 * sxb));
        acc += inRow ? part : 0.0;
    }
    float total = WaveActiveSum(acc);
    if (lane == 0u) { y[p.yOffset + row] = total; }
}

// ---- Q8_0 ----

// Four signed bytes of a word times four x values, added
float sbytes(uint w, float4 v) {
    return float(int(w << 24u) >> 24) * v.x + float(int(w << 16u) >> 24) * v.y + float(int(w << 8u) >> 24) * v.z
         + float(int(w) >> 24) * v.w;
}

[shader("compute")]
[numthreads(128, 1, 1)]
void matvecQ80(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex) {
    MatvecParams p = params[0];
    uint lane = tid & 31u;
    uint row = foldedGroup(gid, p.groupsX) * 4u + (tid >> 5u);
    if (row >= p.rows) { return; }
    uint nb = p.cols >> 5u;
    uint bl = lane >> 1u;           // which of 16 blocks
    uint hf = lane & 1u;            // the first or second 16 elements of it
    uint rowByte = p.wByteOffset + row * p.rowBytes;
    float acc = 0.0;
    for (uint step = 0u; step < ((nb + 15u) >> 4u); step++) {
        uint bRaw = step * 16u + bl;
        bool inRow = bRaw < nb;
        uint b = inRow ? bRaw : 0u;
        uint xi = (p.xOffset + b * 32u + 16u * hf) >> 2u;
        float4 x0 = x[xi], x1 = x[xi + 1u], x2 = x[xi + 2u], x3 = x[xi + 3u];
        uint base = rowByte + b * 34u;
        uint4 q = load16(base + 2u + 16u * hf);
        float d = halfBitsToFloat(halfAt(base));
        float part = d * ((sbytes(q.x, x0) + sbytes(q.y, x1)) + (sbytes(q.z, x2) + sbytes(q.w, x3)));
        acc += inRow ? part : 0.0;
    }
    float total = WaveActiveSum(acc);
    if (lane == 0u) { y[p.yOffset + row] = total; }
}

// ---- F16 ----

[shader("compute")]
[numthreads(128, 1, 1)]
void matvecF16(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex) {
    MatvecParams p = params[0];
    uint lane = tid & 31u;
    uint row = foldedGroup(gid, p.groupsX) * 4u + (tid >> 5u);
    if (row >= p.rows) { return; }
    uint rowByte = p.wByteOffset + row * p.rowBytes;
    float acc = 0.0;
    for (uint step = 0u; step < ((p.cols + 255u) >> 8u); step++) {
        uint eRaw = step * 256u + lane * 8u;
        bool inRow = eRaw < p.cols;
        uint e = inRow ? eRaw : 0u;
        uint xi = (p.xOffset + e) >> 2u;
        float4 x0 = x[xi];
        float4 x1 = x[xi + 1u];
        uint4 q = load16(rowByte + e * 2u);
        float part = (halfBitsToFloat(q.x & 0xFFFFu) * x0.x + halfBitsToFloat(q.x >> 16u) * x0.y)
                   + (halfBitsToFloat(q.y & 0xFFFFu) * x0.z + halfBitsToFloat(q.y >> 16u) * x0.w)
                   + (halfBitsToFloat(q.z & 0xFFFFu) * x1.x + halfBitsToFloat(q.z >> 16u) * x1.y)
                   + (halfBitsToFloat(q.w & 0xFFFFu) * x1.z + halfBitsToFloat(q.w >> 16u) * x1.w);
        acc += inRow ? part : 0.0;
    }
    float total = WaveActiveSum(acc);
    if (lane == 0u) { y[p.yOffset + row] = total; }
}
