// Softmax over each of 'rows' rows of 'n' floats, the stable way: subtract the
// row's maximum, exponentiate, divide by the sum. One workgroup of 256
// invocations for each row; the maximum and the sum are each found by
// per-invocation partials (invocation t takes t, t + 256 ...) and a tree in
// workgroup memory. Bindings: params (by value), x, y.
//
// slangc: -fp-mode precise

struct SoftmaxParams {
    uint n;
    uint rows;          // at most 65535: one workgroup each
    uint pad0;
    uint pad1;
};

[[vk::binding(0, 0)]] StructuredBuffer<SoftmaxParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<float> x;
[[vk::binding(2, 0)]] RWStructuredBuffer<float> y;

groupshared float partial[256];

[shader("compute")]
[numthreads(256, 1, 1)]
void softmaxMain(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex) {
    SoftmaxParams p = params[0];
    uint row = gid.x;
    if (row >= p.rows) { return; }
    uint base = row * p.n;
    float m = -3.4e38;
    for (uint i = tid; i < p.n; i += 256u) { m = max(m, x[base + i]); }
    partial[tid] = m;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 128u; s > 0u; s >>= 1u) {
        if (tid < s) { partial[tid] = max(partial[tid], partial[tid + s]); }
        GroupMemoryBarrierWithGroupSync();
    }
    float rowMax = partial[0];
    GroupMemoryBarrierWithGroupSync();
    float sum = 0.0;
    for (uint i = tid; i < p.n; i += 256u) { sum += exp(x[base + i] - rowMax); }
    partial[tid] = sum;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 128u; s > 0u; s >>= 1u) {
        if (tid < s) { partial[tid] += partial[tid + s]; }
        GroupMemoryBarrierWithGroupSync();
    }
    float total = partial[0];
    for (uint i = tid; i < p.n; i += 256u) { y[base + i] = exp(x[base + i] - rowMax) / total; }
}
