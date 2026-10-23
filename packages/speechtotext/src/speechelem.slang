// Two elementwise kernels with the same bindings (params by value, a, b, y):
//
//   scaleAddMain   y[i] = a[i] + s * b[i]            the Conformer's half-weight residual (s = 0.5) and the plain one (s = 1)
//   addReluMain    y[i] = max(a[aOff + i] + b[i], 0) the joint network: one frame of the projected encoder output
//                                                    (a, from element aOff) plus the projected prediction, then ReLU
//
// One invocation for each of the n elements, 256 a workgroup; a dispatch past 65535 workgroups folds.
//
// slangc: -fp-mode precise

import llmcommon;

struct ElemParams {
    uint n;
    uint groupsX;
    uint aOff;
    float s;
};

[[vk::binding(0, 0)]] StructuredBuffer<ElemParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<float> a;
[[vk::binding(2, 0)]] StructuredBuffer<float> b;
[[vk::binding(3, 0)]] RWStructuredBuffer<float> y;

[shader("compute")]
[numthreads(256, 1, 1)]
void scaleAddMain(uint3 gid : SV_GroupID, uint lid : SV_GroupIndex) {
    ElemParams p = params[0];
    uint i = foldedGroup(gid, p.groupsX) * 256u + lid;
    if (i >= p.n) { return; }
    y[i] = a[i] + p.s * b[i];
}

[shader("compute")]
[numthreads(256, 1, 1)]
void addReluMain(uint3 gid : SV_GroupID, uint lid : SV_GroupIndex) {
    ElemParams p = params[0];
    uint i = foldedGroup(gid, p.groupsX) * 256u + lid;
    if (i >= p.n) { return; }
    y[i] = max(a[p.aOff + i] + b[i], 0.0);
}
