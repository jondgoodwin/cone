// Causal attention of 'nTokens' new tokens over a KV cache, one sequence, with
// grouped-query heads (nKvHeads divides nHeads; nKvHeads = nHeads is plain
// multi-head attention). Token t sits at position pos0 + t and attends to
// positions 0 .. pos0 + t of the cache, so the cache must already hold the
// keys and values of the new tokens (kvAppend puts them there). Layouts, all
// f32: q and out are [token][head][d]; the key and value caches are
// [position][kvHead][d] with room for maxSeq positions.
//
// One workgroup of 128 invocations for each (head, token). Three passes in
// the group: (1) invocation t computes the scaled scores s[j] = scale * q.k[j]
// for j = t, t + 128 ..., into a scratch row of maxSeq floats that the
// workgroup owns ('scores'); (2) the row's maximum and then the sum of
// exp(s - max) are found by per-invocation partials and trees, the exps left
// in the scratch; (3) invocation t takes dimensions d = t, t + 128 ..., each
// sum_j e[j] v[j][d], divided by the sum. The scratch costs nTokens * nHeads *
// maxSeq floats; it is what lets any length of sequence through (workgroup
// memory is 16 KB at least). No flash-attention tiling, no batching of
// sequences. Bindings: params (by value), q, kCache, vCache, scores (scratch,
// written), out.
//
// slangc: -fp-mode precise

struct AttentionParams {
    uint nTokens;
    uint nHeads;
    uint nKvHeads;
    uint headDim;
    uint pos0;
    uint maxSeq;        // the caches' capacity in positions, and the scratch row's length
    float scale;        // usually 1 / sqrt(headDim)
    uint pad;
};

[[vk::binding(0, 0)]] StructuredBuffer<AttentionParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<float> q;
[[vk::binding(2, 0)]] StructuredBuffer<float> kCache;
[[vk::binding(3, 0)]] StructuredBuffer<float> vCache;
[[vk::binding(4, 0)]] RWStructuredBuffer<float> scores;
[[vk::binding(5, 0)]] RWStructuredBuffer<float> outv;

groupshared float partial[128];

[shader("compute")]
[numthreads(128, 1, 1)]
void attentionMain(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex) {
    AttentionParams p = params[0];
    uint head = gid.x;
    uint token = gid.y;
    if (head >= p.nHeads || token >= p.nTokens) { return; }
    uint kvHead = head / (p.nHeads / p.nKvHeads);
    uint seqLen = p.pos0 + token + 1u;
    uint qBase = (token * p.nHeads + head) * p.headDim;
    uint kvStride = p.nKvHeads * p.headDim;
    uint sBase = (token * p.nHeads + head) * p.maxSeq;

    // (1) scores
    for (uint j = tid; j < seqLen; j += 128u) {
        uint kBase = j * kvStride + kvHead * p.headDim;
        float dotp = 0.0;
        for (uint d = 0u; d < p.headDim; d++) { dotp += q[qBase + d] * kCache[kBase + d]; }
        scores[sBase + j] = dotp * p.scale;
    }

    // (2) the maximum, then the exps and their sum; each invocation reads
    // only the scores it wrote
    float m = -3.4e38;
    for (uint j = tid; j < seqLen; j += 128u) { m = max(m, scores[sBase + j]); }
    partial[tid] = m;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 64u; s > 0u; s >>= 1u) {
        if (tid < s) { partial[tid] = max(partial[tid], partial[tid + s]); }
        GroupMemoryBarrierWithGroupSync();
    }
    float rowMax = partial[0];
    GroupMemoryBarrierWithGroupSync();
    float sum = 0.0;
    for (uint j = tid; j < seqLen; j += 128u) {
        float e = exp(scores[sBase + j] - rowMax);
        scores[sBase + j] = e;
        sum += e;
    }
    partial[tid] = sum;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 64u; s > 0u; s >>= 1u) {
        if (tid < s) { partial[tid] += partial[tid + s]; }
        GroupMemoryBarrierWithGroupSync();
    }
    float total = partial[0];
    // the exps are written by one invocation and read by all
    DeviceMemoryBarrierWithGroupSync();

    // (3) the weighted sum of the values
    for (uint d = tid; d < p.headDim; d += 128u) {
        float acc = 0.0;
        for (uint j = 0u; j < seqLen; j++) {
            acc += scores[sBase + j] * vCache[j * kvStride + kvHead * p.headDim + d];
        }
        outv[qBase + d] = acc / total;
    }
}
