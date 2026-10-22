// Bidirectional multi-head attention for one sequence (ALBERT's, with every position visible to every other and no
// mask): out[t, h, :] = sum over j of softmax_j(q[t, h, :] . k[j, h, :] * scale) * v[j, h, :], for 'heads' heads of
// 'headDim' (64) dimensions, q, k, v and out all [steps, heads * headDim]. One workgroup of 64 invocations for each
// (head, step); the scores are held in workgroup memory (steps at most 512).
// Bindings: args, q, k, v, out.
//
// Args words: 0 steps, 1 heads, 2 headDim (at most 64), 3 scale (float), 4 to 15 unused.
//
// slangc: -fp-mode precise

import speechcommon;

[[vk::binding(0, 0)]] StructuredBuffer<Args> args;
[[vk::binding(1, 0)]] StructuredBuffer<float> q;
[[vk::binding(2, 0)]] StructuredBuffer<float> k;
[[vk::binding(3, 0)]] StructuredBuffer<float> v;
[[vk::binding(4, 0)]] RWStructuredBuffer<float> outA;

groupshared float score[512];
groupshared float qv[64];
groupshared float red[64];

[shader("compute")]
[numthreads(64, 1, 1)]
void bertAttentionMain(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex) {
    Args p = args[0];
    uint steps = p.v[0];
    uint heads = p.v[1];
    uint hd = p.v[2];
    float scale = asfloat(p.v[3]);
    uint h = gid.x;
    uint t = gid.y;
    uint width = heads * hd;
    if (tid < hd) { qv[tid] = q[t * width + h * hd + tid]; }
    GroupMemoryBarrierWithGroupSync();
    float mx = -3.0e38;
    for (uint j = tid; j < steps; j += 64u) {
        float d = 0.0;
        for (uint e = 0u; e < hd; e++) { d += qv[e] * k[j * width + h * hd + e]; }
        d = d * scale;
        score[j] = d;
        mx = max(mx, d);
    }
    red[tid] = mx;
    GroupMemoryBarrierWithGroupSync();
    for (uint st = 32u; st > 0u; st >>= 1u) {
        if (tid < st) { red[tid] = max(red[tid], red[tid + st]); }
        GroupMemoryBarrierWithGroupSync();
    }
    float m = red[0];
    GroupMemoryBarrierWithGroupSync();
    float sum = 0.0;
    for (uint j = tid; j < steps; j += 64u) {
        float e = exp(score[j] - m);
        score[j] = e;
        sum += e;
    }
    red[tid] = sum;
    GroupMemoryBarrierWithGroupSync();
    for (uint st = 32u; st > 0u; st >>= 1u) {
        if (tid < st) { red[tid] += red[tid + st]; }
        GroupMemoryBarrierWithGroupSync();
    }
    float total = red[0];
    GroupMemoryBarrierWithGroupSync();
    if (tid < hd) {
        float acc = 0.0;
        for (uint j = 0u; j < steps; j++) { acc += score[j] * v[j * width + h * hd + tid]; }
        outA[t * width + h * hd + tid] = acc / total;
    }
}
