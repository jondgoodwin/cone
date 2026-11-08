// Elementwise add and multiply: y[i] = a[i] + b[i] and y[i] = a[i] * b[i] for
// i < n. One invocation for each element, 256 a workgroup. (Llama's residual
// connection is the add; SwiGLU's gate is silu(gate) then the multiply.)
// Bindings (both entry points): params (by value), a, b, y.
//
// slangc: -fp-mode precise

import llmcommon;

struct ElementwiseParams {
    uint n;
    uint groupsX;
    uint pad0;
    uint pad1;
};

[[vk::binding(0, 0)]] StructuredBuffer<ElementwiseParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<float> a;
[[vk::binding(2, 0)]] StructuredBuffer<float> b;
[[vk::binding(3, 0)]] RWStructuredBuffer<float> y;

[shader("compute")]
[numthreads(256, 1, 1)]
void addMain(uint3 gid : SV_GroupID, uint lid : SV_GroupIndex) {
    ElementwiseParams p = params[0];
    uint i = foldedGroup(gid, p.groupsX) * 256u + lid;
    if (i >= p.n) { return; }
    y[i] = a[i] + b[i];
}

[shader("compute")]
[numthreads(256, 1, 1)]
void mulMain(uint3 gid : SV_GroupID, uint lid : SV_GroupIndex) {
    ElementwiseParams p = params[0];
    uint i = foldedGroup(gid, p.groupsX) * 256u + lid;
    if (i >= p.n) { return; }
    y[i] = a[i] * b[i];
}
