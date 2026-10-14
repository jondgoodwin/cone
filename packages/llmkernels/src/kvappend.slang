// Appends 'nTokens' new keys (or values) to a KV cache: cache[(pos0 + t) *
// kvDim + i] = src[t * kvDim + i], with kvDim = nKvHeads * headDim. One
// invocation for each element. Bindings: params (by value), src, cache
// (written).
//
// slangc: -fp-mode precise

import llmcommon;

struct KvAppendParams {
    uint nTokens;
    uint kvDim;
    uint pos0;
    uint groupsX;
};

[[vk::binding(0, 0)]] StructuredBuffer<KvAppendParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<float> src;
[[vk::binding(2, 0)]] RWStructuredBuffer<float> cache;

[shader("compute")]
[numthreads(64, 1, 1)]
void kvAppendMain(uint3 gid : SV_GroupID, uint lid : SV_GroupIndex) {
    KvAppendParams p = params[0];
    uint i = foldedGroup(gid, p.groupsX) * 64u + lid;
    if (i >= p.nTokens * p.kvDim) { return; }
    uint t = i / p.kvDim;
    uint e = i % p.kvDim;
    cache[(p.pos0 + t) * p.kvDim + e] = src[i];
}
