// Gather columns: y[r, c] = x[xOff + r * xRowStride + idx[c] * xIdxStride] for r < rows and c < cols, idx a list of
// unsigned integers. With it a sequence's rows or columns are repeated by their durations (Kokoro's alignment matrix
// as a product, without the matrix). Bindings: args, x, idx, y.
//
// Args words: 0 rows, 1 cols, 2 x offset, 3 x row stride, 4 x idx stride, 5 y offset, 6 y row stride, 7 idx offset,
// 8 groupsX.
//
// slangc: -fp-mode precise

import speechcommon;

[[vk::binding(0, 0)]] StructuredBuffer<Args> args;
[[vk::binding(1, 0)]] StructuredBuffer<float> x;
[[vk::binding(2, 0)]] StructuredBuffer<uint> idx;
[[vk::binding(3, 0)]] RWStructuredBuffer<float> y;

[shader("compute")]
[numthreads(256, 1, 1)]
void gatherMain(uint3 gid : SV_GroupID, uint lid : SV_GroupIndex) {
    Args p = args[0];
    uint i = foldedGroup(gid, p.v[8]) * 256u + lid;
    uint rows = p.v[0];
    uint cols = p.v[1];
    if (i >= rows * cols) { return; }
    uint r = i / cols;
    uint c = i % cols;
    y[p.v[5] + r * p.v[6] + c] = x[p.v[2] + r * p.v[3] + idx[p.v[7] + c] * p.v[4]];
}
