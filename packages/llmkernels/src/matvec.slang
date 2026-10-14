// y = W x for a weight matrix W of any of the five types (F32, F16, Q8_0,
// Q4_K, Q6_K), dequantised as it is read. One workgroup of 64 invocations
// for each output row; the row is cut into units of 32 elements, invocation t
// takes units t, t + 64, t + 128 ..., adding each unit's 32 products in
// column order, and the 64 partial sums are added by a tree in workgroup
// memory. The type is a parameter (a uniform branch), so one kernel serves
// all five. Bindings: params (by value), the weights' words, x, y.
//
// slangc: -fp-mode precise

import llmcommon;

struct MatvecParams {
    uint rows;          // output elements
    uint cols;          // input elements: a whole number of blocks of the type
    uint rowBytes;      // bytes from one row to the next
    uint groupsX;       // the dispatch's width in groups (rows beyond it fold into y)
    uint wByteOffset;   // byte offset of row 0 in the weights buffer
    uint xOffset;       // x[xOffset ..] is the input
    uint yOffset;       // y[yOffset ..] is the output
    uint qtype;         // QTYPE_*
};

[[vk::binding(0, 0)]] StructuredBuffer<MatvecParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<uint> weights;
[[vk::binding(2, 0)]] StructuredBuffer<float> x;
[[vk::binding(3, 0)]] RWStructuredBuffer<float> y;

groupshared float partial[64];

[shader("compute")]
[numthreads(64, 1, 1)]
void matvecMain(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex) {
    MatvecParams p = params[0];
    uint row = foldedGroup(gid, p.groupsX);
    // the whole workgroup has the same row, so all leave or none
    if (row >= p.rows) { return; }
    uint rowByte = p.wByteOffset + row * p.rowBytes;
    uint units = (p.cols + 31u) / 32u;
    float acc = 0.0;
    for (uint u = tid; u < units; u += 64u) {
        acc += unitDot(weights, p.qtype, rowByte, u, p.cols, x, p.xOffset);
    }
    partial[tid] = acc;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 32u; s > 0u; s >>= 1u) {
        if (tid < s) { partial[tid] += partial[tid + s]; }
        GroupMemoryBarrierWithGroupSync();
    }
    if (tid == 0u) { y[p.yOffset + row] = partial[0]; }
}
