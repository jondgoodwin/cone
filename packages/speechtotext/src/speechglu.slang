// The gated linear unit of the Conformer's convolution module: x is 'rows' vectors of 2c floats, y 'rows' vectors
// of c: y[r][i] = x[r][i] * sigmoid(x[r][c + i]). One invocation for each output, 256 a workgroup (folds past 65535
// workgroups). Bindings: params (by value), x, y.
//
// slangc: -fp-mode precise

import llmcommon;

struct GluParams {
    uint c;             // output width
    uint total;         // rows * c
    uint groupsX;
    uint pad;
};

[[vk::binding(0, 0)]] StructuredBuffer<GluParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<float> x;
[[vk::binding(2, 0)]] RWStructuredBuffer<float> y;

[shader("compute")]
[numthreads(256, 1, 1)]
void gluMain(uint3 gid : SV_GroupID, uint lid : SV_GroupIndex) {
    GluParams p = params[0];
    uint i = foldedGroup(gid, p.groupsX) * 256u + lid;
    if (i >= p.total) { return; }
    uint r = i / p.c;
    uint k = i - r * p.c;
    float g = x[r * 2u * p.c + p.c + k];
    y[i] = x[r * 2u * p.c + k] * (1.0 / (1.0 + exp(-g)));
}
