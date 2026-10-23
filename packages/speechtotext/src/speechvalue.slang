// The attention-weighted sum of the values: out[i][h * dk + d] = sum over j < T of w[h][i][j] * v[j][h * dk + d],
// w the softmax of the scores ([h][i][j]), v T rows of nHeads * dk floats. One invocation for each output
// (T * nHeads * dk), channels fastest, 256 a workgroup (folds past 65535 workgroups).
// Bindings: params (by value), w, v, out.
//
// slangc: -fp-mode precise

import llmcommon;

struct ValueParams {
    uint t;
    uint nHeads;
    uint dk;
    uint total;         // t * nHeads * dk
    uint groupsX;
    uint pad0;
    uint pad1;
    uint pad2;
};

[[vk::binding(0, 0)]] StructuredBuffer<ValueParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<float> w;
[[vk::binding(2, 0)]] StructuredBuffer<float> v;
[[vk::binding(3, 0)]] RWStructuredBuffer<float> outv;

[shader("compute")]
[numthreads(256, 1, 1)]
void attnValueMain(uint3 gid : SV_GroupID, uint lid : SV_GroupIndex) {
    ValueParams p = params[0];
    uint idx = foldedGroup(gid, p.groupsX) * 256u + lid;
    if (idx >= p.total) { return; }
    uint width = p.nHeads * p.dk;
    uint i = idx / width;
    uint hd = idx - i * width;
    uint h = hd / p.dk;
    uint wBase = (h * p.t + i) * p.t;
    float acc = 0.0;
    for (uint j = 0u; j < p.t; j++) {
        acc += w[wBase + j] * v[j * width + hd];
    }
    outv[idx] = acc;
}
