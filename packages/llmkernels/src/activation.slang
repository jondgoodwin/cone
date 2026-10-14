// SiLU and GELU, elementwise: y[i] = f(x[i]) for i < n. SiLU is x / (1 + e^-x)
// (the swish Llama's feed-forward gates with). GELU is the tanh form ggml
// and llama.cpp use, 0.5 x (1 + tanh(sqrt(2 / pi) (x + 0.044715 x^3))), not
// the erf form. One invocation for each element, 256 a workgroup. Bindings
// (both entry points): params (by value), x, y.
//
// slangc: -fp-mode precise

import llmcommon;

struct ActivationParams {
    uint n;
    uint groupsX;
    uint pad0;
    uint pad1;
};

[[vk::binding(0, 0)]] StructuredBuffer<ActivationParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<float> x;
[[vk::binding(2, 0)]] RWStructuredBuffer<float> y;

[shader("compute")]
[numthreads(256, 1, 1)]
void siluMain(uint3 gid : SV_GroupID, uint lid : SV_GroupIndex) {
    ActivationParams p = params[0];
    uint i = foldedGroup(gid, p.groupsX) * 256u + lid;
    if (i >= p.n) { return; }
    float v = x[i];
    y[i] = v / (1.0 + exp(-v));
}

[shader("compute")]
[numthreads(256, 1, 1)]
void geluMain(uint3 gid : SV_GroupID, uint lid : SV_GroupIndex) {
    ActivationParams p = params[0];
    uint i = foldedGroup(gid, p.groupsX) * 256u + lid;
    if (i >= p.n) { return; }
    float v = x[i];
    y[i] = 0.5 * v * (1.0 + tanh(0.79788456080286535588 * (v + 0.044715 * v * v * v)));
}
