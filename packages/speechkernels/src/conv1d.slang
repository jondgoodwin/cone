// A 1-D convolution (PyTorch's Conv1d with groups = 1): y[co, o] = bias[co] + sum over ci and tap k of
// w[co, ci, k] * x[ci, o * stride - pad + k * dilation], a position outside 0 .. lin zero. x is [cin, lin], y is
// [cout, lout], w is [cout, cin, k], all row-major f32.
//
// One workgroup of 256 invocations makes a tile of 64 output channels by 64 positions: each invocation four channels
// by four positions (sixteen sums in registers), the input channels taken 8 at a time through workgroup memory (the
// weights of the tile's channels, laid out with the channel fastest so that the invocations' reads do not collide,
// and the span of input the tile's positions read). Bindings: args, x, w, y. The bias is read from the same buffer
// as the weights, at its own offset (a dispatch binds a buffer once, and a model's weights are one buffer).
//
// Args words: 0 cin, 1 cout, 2 lin, 3 lout, 4 k, 5 stride, 6 pad, 7 dilation, 8 x offset (elements), 9 y offset,
// 10 w offset, 11 bias offset, 12 hasBias, 13 groupsX (the position tiles across, as the dispatch folds them),
// 14 leaky slope applied to the OUTPUT (as a float; 1.0 for none), 15 unused.
//
// slangc: -fp-mode precise

import speechcommon;

[[vk::binding(0, 0)]] StructuredBuffer<Args> args;
[[vk::binding(1, 0)]] StructuredBuffer<float> x;
[[vk::binding(2, 0)]] StructuredBuffer<float> w;
[[vk::binding(3, 0)]] RWStructuredBuffer<float> y;

static const uint TCO = 64u;
static const uint TL = 64u;
static const uint CI = 8u;
static const uint MAXK = 12u;
static const uint MAXSPAN = 392u;     // (TL - 1) * stride + (k - 1) * dilation + 1, at most this

groupshared float xTile[CI * MAXSPAN];
groupshared float wTile[CI * MAXK * TCO];

[shader("compute")]
[numthreads(256, 1, 1)]
void conv1dMain(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex) {
    Args p = args[0];
    uint cin = p.v[0];
    uint cout = p.v[1];
    uint lin = p.v[2];
    uint lout = p.v[3];
    uint k = p.v[4];
    uint stride = p.v[5];
    uint pad = p.v[6];
    uint dil = p.v[7];
    uint xOff = p.v[8];
    uint yOff = p.v[9];
    uint wOff = p.v[10];
    uint bOff = p.v[11];
    bool hasBias = p.v[12] != 0u;
    uint groupsX = p.v[13];
    float slope = asfloat(p.v[14]);

    uint tile = foldedGroup(gid, groupsX);
    uint l0 = tile * TL;
    uint co0 = gid.z * TCO;
    uint span = (TL - 1u) * stride + (k - 1u) * dil + 1u;

    uint cg = tid / 16u;     // 0 .. 15: channels 4 cg .. 4 cg + 3 of the tile
    uint pg = tid % 16u;     // 0 .. 15: positions pg, pg + 16, pg + 32, pg + 48

    float acc[4][4];
    for (uint a = 0u; a < 4u; a++) {
        for (uint b = 0u; b < 4u; b++) { acc[a][b] = 0.0; }
    }

    int inStart = int(l0 * stride) - int(pad);
    for (uint c0 = 0u; c0 < cin; c0 += CI) {
        uint nc = min(CI, cin - c0);
        // the input span of this tile for these channels
        for (uint i = tid; i < nc * span; i += 256u) {
            uint cc = i / span;
            uint s = i % span;
            int pos = inStart + int(s);
            float v = 0.0;
            if (pos >= 0 && pos < int(lin)) { v = x[xOff + (c0 + cc) * lin + uint(pos)]; }
            xTile[cc * MAXSPAN + s] = v;
        }
        // the weights of these channels for the tile's output channels
        for (uint i = tid; i < TCO * nc * k; i += 256u) {
            uint oc = i / (nc * k);
            uint r = i % (nc * k);
            uint cc = r / k;
            uint kk = r % k;
            float v = 0.0;
            if (co0 + oc < cout) { v = w[wOff + ((co0 + oc) * cin + c0 + cc) * k + kk]; }
            wTile[(cc * MAXK + kk) * TCO + oc] = v;
        }
        GroupMemoryBarrierWithGroupSync();
        for (uint cc = 0u; cc < nc; cc++) {
            for (uint kk = 0u; kk < k; kk++) {
                uint wb = (cc * MAXK + kk) * TCO + cg * 4u;
                float w0 = wTile[wb];
                float w1 = wTile[wb + 1u];
                float w2 = wTile[wb + 2u];
                float w3 = wTile[wb + 3u];
                for (uint j = 0u; j < 4u; j++) {
                    float xv = xTile[cc * MAXSPAN + (pg + 16u * j) * stride + kk * dil];
                    acc[0][j] += w0 * xv;
                    acc[1][j] += w1 * xv;
                    acc[2][j] += w2 * xv;
                    acc[3][j] += w3 * xv;
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }

    for (uint a = 0u; a < 4u; a++) {
        uint co = co0 + cg * 4u + a;
        if (co >= cout) { continue; }
        float bv = 0.0;
        if (hasBias) { bv = w[bOff + co]; }
        for (uint j = 0u; j < 4u; j++) {
            uint o = l0 + pg + 16u * j;
            if (o < lout) {
                float v = acc[a][j] + bv;
                if (v < 0.0) { v = v * slope; }
                y[yOff + co * lout + o] = v;
            }
        }
    }
}
