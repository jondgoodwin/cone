// The recurrence of a bidirectional LSTM layer with 256 hidden units (PyTorch's LSTM; gates i, f, g, o in that order).
// The input half of every gate is computed first for all the steps as one matrix product (G, [steps, 2 directions,
// 1024] with the biases of both of PyTorch's bias vectors added in), so this kernel does only the part that must run
// in order: one workgroup of 256 invocations for each direction, invocation u the hidden unit u, holding the hidden
// state in workgroup memory. The forward direction goes from step 0 up, the backward one from the last step down;
// the output is [steps, 2 * 256] (forward units, then backward). Bindings: args, gates, whhT, out. The biases
// ([2, 1024], the two of PyTorch's added) are read from the same buffer as whhT, at their own offset.
//
// whhT is the hidden-to-hidden weights of both directions transposed: whhT[dir][k][g * 256 + u] is PyTorch's
// weight_hh[g * 256 + u, k], so that the invocations of a workgroup read consecutive words.
//
// Args words: 0 steps, 1 gates offset (elements), 2 out offset, 3 whhT offset, 4 bias offset (2 * 1024 words),
// 5 to 15 unused.
//
// slangc: -fp-mode precise

import speechcommon;

[[vk::binding(0, 0)]] StructuredBuffer<Args> args;
[[vk::binding(1, 0)]] StructuredBuffer<float> gates;
[[vk::binding(2, 0)]] StructuredBuffer<float> whhT;
[[vk::binding(3, 0)]] RWStructuredBuffer<float> outH;

groupshared float h[256];

float sigmoid(float v) { return 1.0 / (1.0 + exp(-v)); }

[shader("compute")]
[numthreads(256, 1, 1)]
void lstmMain(uint3 gid : SV_GroupID, uint u : SV_GroupIndex) {
    Args p = args[0];
    uint steps = p.v[0];
    uint gOff = p.v[1];
    uint oOff = p.v[2];
    uint wOff = p.v[3];
    uint bOff = p.v[4];
    uint dir = gid.x;

    uint wBase = wOff + dir * 256u * 1024u;
    h[u] = 0.0;
    float c = 0.0;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 0u; s < steps; s++) {
        uint t = (dir == 0u) ? s : (steps - 1u - s);
        uint gBase = gOff + t * 2048u + dir * 1024u;
        float pi = gates[gBase + u] + whhT[bOff + dir * 1024u + u];
        float pf = gates[gBase + 256u + u] + whhT[bOff + dir * 1024u + 256u + u];
        float pg = gates[gBase + 512u + u] + whhT[bOff + dir * 1024u + 512u + u];
        float po = gates[gBase + 768u + u] + whhT[bOff + dir * 1024u + 768u + u];
        for (uint k = 0u; k < 256u; k++) {
            float hk = h[k];
            uint wr = wBase + k * 1024u + u;
            pi += hk * whhT[wr];
            pf += hk * whhT[wr + 256u];
            pg += hk * whhT[wr + 512u];
            po += hk * whhT[wr + 768u];
        }
        float ig = sigmoid(pi);
        float fg = sigmoid(pf);
        float gg = tanh(pg);
        float og = sigmoid(po);
        c = fg * c + ig * gg;
        float hn = og * tanh(c);
        GroupMemoryBarrierWithGroupSync();
        h[u] = hn;
        outH[oOff + t * 512u + dir * 256u + u] = hn;
        GroupMemoryBarrierWithGroupSync();
    }
}
