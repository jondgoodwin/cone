// RMSNorm, as Llama uses it: for each of 'rows' vectors of 'n' floats,
// y[i] = x[i] * (1 / sqrt(mean(x^2) + eps)) * w[i]. One workgroup of 256
// invocations for each row: invocation t adds x[t]^2, x[t + 256]^2 ... and the
// 256 partial sums are added by a tree in workgroup memory. Bindings: params
// (by value), x, the weight w (n floats, shared by every row), y.
//
// slangc: -fp-mode precise

struct RmsnormParams {
    uint n;             // elements in a row
    uint rows;          // rows (at most 65535: one workgroup each)
    float eps;
    uint pad;
};

[[vk::binding(0, 0)]] StructuredBuffer<RmsnormParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<float> x;
[[vk::binding(2, 0)]] StructuredBuffer<float> w;
[[vk::binding(3, 0)]] RWStructuredBuffer<float> y;

groupshared float partial[256];

[shader("compute")]
[numthreads(256, 1, 1)]
void rmsnormMain(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex) {
    RmsnormParams p = params[0];
    uint row = gid.x;
    if (row >= p.rows) { return; }
    uint base = row * p.n;
    float ss = 0.0;
    for (uint i = tid; i < p.n; i += 256u) {
        float v = x[base + i];
        ss += v * v;
    }
    partial[tid] = ss;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 128u; s > 0u; s >>= 1u) {
        if (tid < s) { partial[tid] += partial[tid + s]; }
        GroupMemoryBarrierWithGroupSync();
    }
    float scale = 1.0 / sqrt(partial[0] / float(p.n) + p.eps);
    for (uint i = tid; i < p.n; i += 256u) {
        y[base + i] = (x[base + i] * scale) * w[i];
    }
}
