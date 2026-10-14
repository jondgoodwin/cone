// Rotary position embedding, in place, on 'tokens' vectors of 'nHeads' heads
// of 'headDim' floats (token-major: x[(token * nHeads + head) * headDim + d]).
// The first 'ropeDim' dimensions of each head are rotated in pairs, by the
// angle (pos0 + token) * freqs[pair] with freqs the caller's table of
// ropeDim / 2 frequencies (base^(-2 pair / ropeDim) for Llama, any scaling
// baked in by the caller, so the CPU and the GPU start from the same
// numbers); the rest are left as they are. The pairs are (2 pair, 2 pair + 1)
// in mode 0 (ggml's "normal" order, the one a Llama GGUF's permuted weights
// want) or (pair, pair + ropeDim / 2) in mode 1 (the "neox" order, Qwen and
// the Hugging Face Llama layout). One invocation for each pair of each head
// of each token, so no two touch the same element. Bindings: params (by
// value), freqs, x (read and written).
//
// slangc: -fp-mode precise

import llmcommon;

struct RopeParams {
    uint tokens;
    uint nHeads;
    uint headDim;
    uint ropeDim;       // even, at most headDim
    uint pos0;          // the position of token 0
    uint mode;          // 0 adjacent pairs, 1 split halves
    uint groupsX;
    uint pad;
};

[[vk::binding(0, 0)]] StructuredBuffer<RopeParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<float> freqs;
[[vk::binding(2, 0)]] RWStructuredBuffer<float> x;

[shader("compute")]
[numthreads(64, 1, 1)]
void ropeMain(uint3 gid : SV_GroupID, uint lid : SV_GroupIndex) {
    RopeParams p = params[0];
    uint half = p.ropeDim / 2u;
    uint total = p.tokens * p.nHeads * half;
    uint i = foldedGroup(gid, p.groupsX) * 64u + lid;
    if (i >= total) { return; }
    uint pair = i % half;
    uint th = i / half;
    uint head = th % p.nHeads;
    uint token = th / p.nHeads;
    uint basis = (token * p.nHeads + head) * p.headDim;
    uint a = (p.mode == 0u) ? (basis + 2u * pair) : (basis + pair);
    uint b = (p.mode == 0u) ? (a + 1u) : (basis + pair + half);
    float angle = float(p.pos0 + token) * freqs[pair];
    float c;
    float s;
    sincosReduced(angle, s, c);
    float x0 = x[a];
    float x1 = x[b];
    x[a] = x0 * c - x1 * s;
    x[b] = x0 * s + x1 * c;
}
