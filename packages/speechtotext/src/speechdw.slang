// The Conformer convolution module's depthwise 1-D convolution (the batch norm after it folded into its weights
// and bias by the converter), then SiLU: for each time step t and channel ch,
//   y[t][ch] = silu( bias[ch] + sum over k < K of w[ch][k] * x[t - pad + k][ch] )
// with x taken as 0 outside 0 .. T-1. x and y are T vectors of C floats (time-major). The weights are C rows of K
// floats at float offset wOff in 'small', the biases C floats at bOff. One invocation for each output, channels
// fastest, 256 a workgroup (folds past 65535 workgroups). Bindings: params (by value), small, x, y.
//
// slangc: -fp-mode precise

import llmcommon;

struct DwParams {
    uint t;             // time steps
    uint c;             // channels
    uint k;             // kernel width
    uint pad;           // zeros either side
    uint wOff;
    uint bOff;
    uint total;         // t * c
    uint groupsX;
};

[[vk::binding(0, 0)]] StructuredBuffer<DwParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<float> small;
[[vk::binding(2, 0)]] StructuredBuffer<float> x;
[[vk::binding(3, 0)]] RWStructuredBuffer<float> y;

[shader("compute")]
[numthreads(256, 1, 1)]
void dwconv1dMain(uint3 gid : SV_GroupID, uint lid : SV_GroupIndex) {
    DwParams p = params[0];
    uint i = foldedGroup(gid, p.groupsX) * 256u + lid;
    if (i >= p.total) { return; }
    uint tt = i / p.c;
    uint ch = i - tt * p.c;
    float acc = small[p.bOff + ch];
    for (uint kk = 0u; kk < p.k; kk++) {
        int src = int(tt) - int(p.pad) + int(kk);
        if (src >= 0 && src < int(p.t)) {
            acc += small[p.wOff + ch * p.k + kk] * x[uint(src) * p.c + ch];
        }
    }
    y[i] = acc / (1.0 + exp(-acc));
}
