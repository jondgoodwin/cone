// Layer normalisation over the n elements of each of 'rows' rows: y = (x - mean) / sqrt(var + eps) (the variance is
// the biased one), then by mode: 0 plain; 1 times gamma[j] plus beta[j]; 2 adaptive: times (1 + gb[j]) plus gb[n + j]
// (a style's scale and shift; gamma holds gb). Then the leaky ReLU of slope 'slope' (1.0: none).
//
// Element j of row r is at r * rowStride + j * elemStride of x (xOff first) and of y (yOff), so a [rows, n] matrix
// and the columns of an [n, rows] one are both rows. One workgroup of 256 invocations for each row.
// Bindings: args, x, gamma, y (beta is read from the gamma buffer, at its own offset: mode 1 uses gamma[gOff + j] and
// gamma[bOff + j], mode 2 gamma[gOff + j] and gamma[gOff + n + j]).
//
// Args words: 0 n, 1 rows, 2 elemStride, 3 rowStride, 4 x offset, 5 y offset, 6 eps (float), 7 mode, 8 gamma offset,
// 9 beta offset, 10 slope (float), 11 groupsX (rows across), 12 to 15 unused.
//
// slangc: -fp-mode precise

import speechcommon;

[[vk::binding(0, 0)]] StructuredBuffer<Args> args;
[[vk::binding(1, 0)]] StructuredBuffer<float> x;
[[vk::binding(2, 0)]] StructuredBuffer<float> gamma;
[[vk::binding(3, 0)]] RWStructuredBuffer<float> y;

groupshared float partial[256];

[shader("compute")]
[numthreads(256, 1, 1)]
void layernormMain(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex) {
    Args p = args[0];
    uint n = p.v[0];
    uint rows = p.v[1];
    uint es = p.v[2];
    uint rs = p.v[3];
    uint xOff = p.v[4];
    uint yOff = p.v[5];
    float eps = asfloat(p.v[6]);
    uint mode = p.v[7];
    uint gOff = p.v[8];
    uint bOff = p.v[9];
    float slope = asfloat(p.v[10]);
    uint row = foldedGroup(gid, p.v[11]);
    if (row >= rows) { return; }
    uint xb = xOff + row * rs;
    uint yb = yOff + row * rs;

    float s = 0.0;
    for (uint j = tid; j < n; j += 256u) { s += x[xb + j * es]; }
    partial[tid] = s;
    GroupMemoryBarrierWithGroupSync();
    for (uint st = 128u; st > 0u; st >>= 1u) {
        if (tid < st) { partial[tid] += partial[tid + st]; }
        GroupMemoryBarrierWithGroupSync();
    }
    float mean = partial[0] / float(n);
    GroupMemoryBarrierWithGroupSync();
    float v = 0.0;
    for (uint j = tid; j < n; j += 256u) {
        float d = x[xb + j * es] - mean;
        v += d * d;
    }
    partial[tid] = v;
    GroupMemoryBarrierWithGroupSync();
    for (uint st = 128u; st > 0u; st >>= 1u) {
        if (tid < st) { partial[tid] += partial[tid + st]; }
        GroupMemoryBarrierWithGroupSync();
    }
    float inv = 1.0 / sqrt(partial[0] / float(n) + eps);
    for (uint j = tid; j < n; j += 256u) {
        float r = (x[xb + j * es] - mean) * inv;
        if (mode == 1u) { r = r * gamma[gOff + j] + gamma[bOff + j]; }
        if (mode == 2u) { r = r * (1.0 + gamma[gOff + j]) + gamma[gOff + n + j]; }
        if (r < 0.0) { r = r * slope; }
        y[yb + j * es] = r;
    }
}
