// LayerNorm, as the Conformer uses it: for each of 'rows' vectors of 'n' floats,
// y[i] = (x[i] - mean) / sqrt(variance + eps) * w[i] + b[i], the variance the mean of the squared differences (the
// biased one, as torch.nn.LayerNorm). One workgroup of 256 invocations for each row: the mean by a tree, then the
// variance of the differences by a tree (two passes, so a row of large values loses nothing to cancellation).
// Bindings: params (by value), small (the model's small F32 tensors; w at float offset wOff, b at bOff), x, y.
//
// slangc: -fp-mode precise

struct NormParams {
    uint n;             // elements in a row
    uint rows;          // rows (at most 65535: one workgroup each)
    float eps;
    uint wOff;          // float offset of the scale in 'small'
    uint bOff;          // float offset of the shift in 'small'
    uint pad0;
    uint pad1;
    uint pad2;
};

[[vk::binding(0, 0)]] StructuredBuffer<NormParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<float> small;
[[vk::binding(2, 0)]] StructuredBuffer<float> x;
[[vk::binding(3, 0)]] RWStructuredBuffer<float> y;

groupshared float partial[256];

[shader("compute")]
[numthreads(256, 1, 1)]
void layernormMain(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex) {
    NormParams p = params[0];
    uint row = gid.x;
    if (row >= p.rows) { return; }
    uint base = row * p.n;
    float s = 0.0;
    for (uint i = tid; i < p.n; i += 256u) { s += x[base + i]; }
    partial[tid] = s;
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 128u; k > 0u; k >>= 1u) {
        if (tid < k) { partial[tid] += partial[tid + k]; }
        GroupMemoryBarrierWithGroupSync();
    }
    float mean = partial[0] / float(p.n);
    GroupMemoryBarrierWithGroupSync();
    float ss = 0.0;
    for (uint i = tid; i < p.n; i += 256u) {
        float d = x[base + i] - mean;
        ss += d * d;
    }
    partial[tid] = ss;
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 128u; k > 0u; k >>= 1u) {
        if (tid < k) { partial[tid] += partial[tid + k]; }
        GroupMemoryBarrierWithGroupSync();
    }
    float scale = 1.0 / sqrt(partial[0] / float(p.n) + p.eps);
    for (uint i = tid; i < p.n; i += 256u) {
        y[base + i] = ((x[base + i] - mean) * scale) * small[p.wOff + i] + small[p.bOff + i];
    }
}
