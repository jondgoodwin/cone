// y = W x for a Q4_K weight matrix, at the speed of the GPU's memory: the wide matvec for the type most of a model
// is made of. One subgroup (32 invocations) takes one row; a workgroup is four subgroups, so four rows. Decoding
// streams every weight once, so the work is reading the row's bytes in order and fast:
//
//  - Eight invocations to a super-block (144 bytes: half d, half dmin, 12 bytes of scales, 128 bytes of nibbles),
//    four super-blocks at a time. Invocation c of its eight reads the super-block's 16-byte header and its own 16
//    bytes of the nibbles, each a single 16-byte load (a StructuredBuffer<uint4>), so the 32 invocations' loads of one
//    step cover 576 consecutive bytes. Its 16 bytes are 16 low-nibble elements and 16 high-nibble elements of the
//    super-block (columns 64 g + 16 h + [0, 16) and 64 g + 32 + 16 h + [0, 16), with g = c / 2 and h = c % 2), which
//    it multiplies with the x values at those columns, read as float4s straight from memory (x is 16 to 48 KB and
//    stays in the GPU's caches; staging it in workgroup memory costs a barrier and was measured 2x slower).
//  - Dequantisation in registers: a weight is d sc nibble - dmin mn, so a half super-block's contribution is
//    d sc (sum of nibble x) - dmin mn (sum of x), two sums a multiply-add each. No dequantised weight is stored.
//  - The 32 invocations' partial sums are added by the subgroup's own operation (WaveActiveSum): no workgroup
//    memory, no barrier. The subgroup must be 32 invocations wide (the workgroup's 4 rows are 4 subgroups); the host
//    (Kernels) runs this kernel only on a device whose subgroups are 32 and do arithmetic, and the older
//    matvec.slang elsewhere.
//
// The weights must start on a multiple of 16 bytes (wByteOffset; a row, 144 bytes a super-block, then does), x on
// a multiple of 4 floats (xOffset); Kernels checks both. The sums are added in a different order from the CPU
// twin's (and the dequantisation is regrouped as above), so a result differs from it in the last bits of an f32,
// within llmreference's sumTolerance. Bindings: params (by value), the weights (as uint4), x (as float4), y.
//
// slangc: -fp-mode precise

import llmcommon;

struct MatvecParams {
    uint rows;          // output elements
    uint cols;          // input elements: a whole number of super-blocks (256)
    uint rowBytes;      // bytes from one row to the next
    uint groupsX;       // the dispatch's width in groups (rows beyond it fold into y)
    uint wByteOffset;   // byte offset of row 0 in the weights buffer: a multiple of 16
    uint xOffset;       // x[xOffset ..] is the input: a multiple of 4
    uint yOffset;       // y[yOffset ..] is the output
    uint qtype;         // unused here
};

[[vk::binding(0, 0)]] StructuredBuffer<MatvecParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<uint4> weights;
[[vk::binding(2, 0)]] StructuredBuffer<float4> x;
[[vk::binding(3, 0)]] RWStructuredBuffer<float> y;

// Byte k (0 to 11) of the 12 bytes of a super-block's scales and minimums, held as three words
uint scaleByte(uint3 s, uint k) {
    uint w = (k < 4u) ? s.x : ((k < 8u) ? s.y : s.z);
    return (w >> ((k & 3u) * 8u)) & 255u;
}

// The six-bit scale and minimum number j (0 to 7), as ggml's get_scale_min_k4
void scaleMin(uint3 s, uint j, out float sc, out float mn) {
    if (j < 4u) {
        sc = float(scaleByte(s, j) & 63u);
        mn = float(scaleByte(s, j + 4u) & 63u);
    } else {
        sc = float((scaleByte(s, j + 4u) & 15u) | ((scaleByte(s, j - 4u) >> 6u) << 4u));
        mn = float((scaleByte(s, j + 4u) >> 4u) | ((scaleByte(s, j) >> 6u) << 4u));
    }
}

// The four low nibbles of a word times four x values, added
float lows(uint w, float4 v) {
    return float(w & 15u) * v.x + float((w >> 8u) & 15u) * v.y + float((w >> 16u) & 15u) * v.z + float((w >> 24u) & 15u) * v.w;
}

float highs(uint w, float4 v) {
    return float((w >> 4u) & 15u) * v.x + float((w >> 12u) & 15u) * v.y + float((w >> 20u) & 15u) * v.z + float((w >> 28u) & 15u) * v.w;
}

float sum4(float4 v) { return (v.x + v.y) + (v.z + v.w); }

[shader("compute")]
[numthreads(128, 1, 1)]
void matvecQ4K(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex) {
    MatvecParams p = params[0];
    uint lane = tid & 31u;
    uint row = foldedGroup(gid, p.groupsX) * 4u + (tid >> 5u);
    // the whole subgroup has the same row, so all leave or none
    if (row >= p.rows) { return; }
    uint nsb = p.cols >> 8u;
    uint sbl = lane >> 3u;          // which of four super-blocks
    uint c = lane & 7u;             // which 16 bytes of its nibbles
    uint g = c >> 1u;
    uint h = c & 1u;
    uint rowWord = (p.wByteOffset + row * p.rowBytes) >> 4u;
    float acc = 0.0;
    // every invocation runs the same number of steps (a super-block past the row's end is read as the first, and
    // its sum left out), so the subgroup is whole at the sum below
    for (uint step = 0u; step < ((nsb + 3u) >> 2u); step++) {
        uint sbRaw = step * 4u + sbl;
        bool inRow = sbRaw < nsb;
        uint sb = inRow ? sbRaw : 0u;
        uint xi = (p.xOffset + sb * 256u + 64u * g + 16u * h) >> 2u;
        float4 xl0 = x[xi], xl1 = x[xi + 1u], xl2 = x[xi + 2u], xl3 = x[xi + 3u];
        float4 xh0 = x[xi + 8u], xh1 = x[xi + 9u], xh2 = x[xi + 10u], xh3 = x[xi + 11u];
        uint4 head = weights[rowWord + sb * 9u];
        uint4 q = weights[rowWord + sb * 9u + 1u + c];
        float d = halfBitsToFloat(head.x & 0xFFFFu);
        float dmin = halfBitsToFloat(head.x >> 16u);
        uint3 s = uint3(head.y, head.z, head.w);
        float scL, mnL, scH, mnH;
        scaleMin(s, 2u * g, scL, mnL);
        scaleMin(s, 2u * g + 1u, scH, mnH);
        float sumL = (lows(q.x, xl0) + lows(q.y, xl1)) + (lows(q.z, xl2) + lows(q.w, xl3));
        float sumH = (highs(q.x, xh0) + highs(q.y, xh1)) + (highs(q.z, xh2) + highs(q.w, xh3));
        float xsumL = (sum4(xl0) + sum4(xl1)) + (sum4(xl2) + sum4(xl3));
        float xsumH = (sum4(xh0) + sum4(xh1)) + (sum4(xh2) + sum4(xh3));
        float part = ((d * scL) * sumL - (dmin * mnL) * xsumL) + ((d * scH) * sumH - (dmin * mnH) * xsumH);
        acc += inRow ? part : 0.0;
    }
    float total = WaveActiveSum(acc);
    if (lane == 0u) { y[p.yOffset + row] = total; }
}
