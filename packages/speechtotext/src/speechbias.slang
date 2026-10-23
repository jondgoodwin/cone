// A bias added in place: y[r * n + c] += bias[bOff + c] for every row r and column c, 'total' = rows * n elements.
// One invocation for each element, 256 a workgroup. Bindings: params (by value), small (the model's small F32
// tensors), y (read and written).
//
// slangc: -fp-mode precise

import llmcommon;

struct BiasParams {
    uint n;             // columns
    uint total;         // rows * n
    uint groupsX;
    uint bOff;          // float offset of the bias in 'small'
};

[[vk::binding(0, 0)]] StructuredBuffer<BiasParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<float> small;
[[vk::binding(2, 0)]] RWStructuredBuffer<float> y;

[shader("compute")]
[numthreads(256, 1, 1)]
void biasAddMain(uint3 gid : SV_GroupID, uint lid : SV_GroupIndex) {
    BiasParams p = params[0];
    uint i = foldedGroup(gid, p.groupsX) * 256u + lid;
    if (i >= p.total) { return; }
    y[i] = y[i] + small[p.bOff + (i % p.n)];
}
