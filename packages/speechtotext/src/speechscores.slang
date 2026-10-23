// The Conformer's relative-position attention scores (Transformer-XL's, as NeMo's RelPositionMultiHeadAttention):
// for head h, query position i and key position j (T positions),
//   scores[h][i][j] = ( (q[i][h] + u[h]) . k[j][h]  +  (q[i][h] + v[h]) . p[T - 1 - i + j][h] ) * scale
// q, k are T rows of nHeads * dk floats; p is 2 T - 1 rows of the same width (the relative position embeddings,
// already through the layer's linear_pos; row m stands for the offset T - 1 - m, so key j of query i uses the
// offset i - j); u, v are nHeads * dk floats each at float offsets uOff and vOff of 'small'. NeMo's rel_shift is
// this indexing. One invocation for each (h, i, j), 256 a workgroup (folds past 65535 workgroups). 'scale' is
// 1 / sqrt(dk). Bindings: params (by value), small, q, k, p, scores (nHeads * T * T floats, [h][i][j]).
//
// slangc: -fp-mode precise

import llmcommon;

struct ScoreParams {
    uint t;
    uint nHeads;
    uint dk;
    uint uOff;
    uint vOff;
    float scale;
    uint total;         // nHeads * t * t
    uint groupsX;
};

[[vk::binding(0, 0)]] StructuredBuffer<ScoreParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<float> small;
[[vk::binding(2, 0)]] StructuredBuffer<float> q;
[[vk::binding(3, 0)]] StructuredBuffer<float> k;
[[vk::binding(4, 0)]] StructuredBuffer<float> pos;
[[vk::binding(5, 0)]] RWStructuredBuffer<float> scores;

[shader("compute")]
[numthreads(256, 1, 1)]
void relScoresMain(uint3 gid : SV_GroupID, uint lid : SV_GroupIndex) {
    ScoreParams p = params[0];
    uint idx = foldedGroup(gid, p.groupsX) * 256u + lid;
    if (idx >= p.total) { return; }
    uint j = idx % p.t;
    uint rest = idx / p.t;
    uint i = rest % p.t;
    uint h = rest / p.t;
    uint width = p.nHeads * p.dk;
    uint qBase = i * width + h * p.dk;
    uint kBase = j * width + h * p.dk;
    uint pBase = (p.t - 1u - i + j) * width + h * p.dk;
    float ac = 0.0;
    float bd = 0.0;
    for (uint d = 0u; d < p.dk; d++) {
        float qv = q[qBase + d];
        ac += (qv + small[p.uOff + h * p.dk + d]) * k[kBase + d];
        bd += (qv + small[p.vOff + h * p.dk + d]) * pos[pBase + d];
    }
    scores[idx] = (ac + bd) * p.scale;
}
