// Adaptive instance normalisation, as StyleTTS2's AdaIN1d: each channel of x [channels, length] normalised over its
// length (mean and biased variance, eps 1e-5, no affine of its own), then scaled and shifted by the style:
// y = (1 + gamma[c]) * (x - mean) / sqrt(var + eps) + beta[c], with gamma and beta the two halves of 'gb' (the style
// vector through the layer's fc: 2 * channels floats, gamma first). Then an activation: 0 none; 1 leaky ReLU with
// the given slope; 2 Snake, v + sin(alpha v)^2 / alpha with alpha[c] the channel's.
// One workgroup of 256 invocations for each channel. Bindings: args, x, gb, alpha, y.
//
// Args words: 0 channels, 1 length, 2 x offset, 3 y offset, 4 gb offset, 5 alpha offset, 6 activation, 7 slope
// (float), 8 eps (float), 9 groupsX, 10 to 15 unused.
//
// slangc: -fp-mode precise

import speechcommon;

[[vk::binding(0, 0)]] StructuredBuffer<Args> args;
[[vk::binding(1, 0)]] StructuredBuffer<float> x;
[[vk::binding(2, 0)]] StructuredBuffer<float> gb;
[[vk::binding(3, 0)]] StructuredBuffer<float> alpha;
[[vk::binding(4, 0)]] RWStructuredBuffer<float> y;

groupshared float partial[256];

[shader("compute")]
[numthreads(256, 1, 1)]
void adainMain(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex) {
    Args p = args[0];
    uint channels = p.v[0];
    uint len = p.v[1];
    uint xOff = p.v[2];
    uint yOff = p.v[3];
    uint gbOff = p.v[4];
    uint aOff = p.v[5];
    uint act = p.v[6];
    float slope = asfloat(p.v[7]);
    float eps = asfloat(p.v[8]);
    uint c = foldedGroup(gid, p.v[9]);
    if (c >= channels) { return; }
    uint base = c * len;

    float s = 0.0;
    for (uint i = tid; i < len; i += 256u) { s += x[xOff + base + i]; }
    partial[tid] = s;
    GroupMemoryBarrierWithGroupSync();
    for (uint st = 128u; st > 0u; st >>= 1u) {
        if (tid < st) { partial[tid] += partial[tid + st]; }
        GroupMemoryBarrierWithGroupSync();
    }
    float mean = partial[0] / float(len);
    GroupMemoryBarrierWithGroupSync();
    float v = 0.0;
    for (uint i = tid; i < len; i += 256u) {
        float d = x[xOff + base + i] - mean;
        v += d * d;
    }
    partial[tid] = v;
    GroupMemoryBarrierWithGroupSync();
    for (uint st = 128u; st > 0u; st >>= 1u) {
        if (tid < st) { partial[tid] += partial[tid + st]; }
        GroupMemoryBarrierWithGroupSync();
    }
    float inv = 1.0 / sqrt(partial[0] / float(len) + eps);
    float g = 1.0 + gb[gbOff + c];
    float b = gb[gbOff + channels + c];
    float a = 1.0;
    if (act == 2u) { a = alpha[aOff + c]; }
    for (uint i = tid; i < len; i += 256u) {
        float r = (x[xOff + base + i] - mean) * inv * g + b;
        if (act == 1u) {
            if (r < 0.0) { r = r * slope; }
        } else if (act == 2u) {
            float sn = sin(a * r);
            r = r + (1.0 / a) * sn * sn;
        }
        y[yOff + base + i] = r;
    }
}
