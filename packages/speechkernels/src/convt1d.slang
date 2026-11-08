// A transposed 1-D convolution (PyTorch's ConvTranspose1d, with groups): every input position scatters
// x[ci, i] * w[ci, co - group's first, k] to the output position o = i * stride - pad + k; this gathers instead: the
// output o takes the (i, k) with i * stride + k = o + pad. x is [cin, lin], y is [cout, lout], w is
// [cin, cout / groups, k]; lout = (lin - 1) * stride - 2 pad + k + outPad. One invocation for each output element,
// 256 to a workgroup. Bindings: args, x, w, y (the bias is in the weights' buffer, at its own offset).
//
// Args words: 0 cin, 1 cout, 2 lin, 3 lout, 4 k, 5 stride, 6 pad, 7 groups, 8 x offset, 9 y offset, 10 w offset,
// 11 bias offset, 12 hasBias, 13 groupsX, 14 and 15 unused.
//
// slangc: -fp-mode precise

import speechcommon;

[[vk::binding(0, 0)]] StructuredBuffer<Args> args;
[[vk::binding(1, 0)]] StructuredBuffer<float> x;
[[vk::binding(2, 0)]] StructuredBuffer<float> w;
[[vk::binding(3, 0)]] RWStructuredBuffer<float> y;

[shader("compute")]
[numthreads(256, 1, 1)]
void convt1dMain(uint3 gid : SV_GroupID, uint lid : SV_GroupIndex) {
    Args p = args[0];
    uint cin = p.v[0];
    uint cout = p.v[1];
    uint lin = p.v[2];
    uint lout = p.v[3];
    uint k = p.v[4];
    uint stride = p.v[5];
    uint pad = p.v[6];
    uint groups = p.v[7];
    uint xOff = p.v[8];
    uint yOff = p.v[9];
    uint wOff = p.v[10];
    uint bOff = p.v[11];
    bool hasBias = p.v[12] != 0u;
    uint groupsX = p.v[13];

    uint idx = foldedGroup(gid, groupsX) * 256u + lid;
    if (idx >= cout * lout) { return; }
    uint co = idx / lout;
    uint o = idx % lout;
    uint cinPerG = cin / groups;
    uint coutPerG = cout / groups;
    uint g = co / coutPerG;
    uint cl = co - g * coutPerG;
    float acc = 0.0;
    if (hasBias) { acc = w[bOff + co]; }
    int t = int(o + pad);
    // the taps: kk with (t - kk) divisible by stride and 0 <= (t - kk) / stride < lin
    for (uint kk = 0u; kk < k; kk++) {
        int d = t - int(kk);
        if (d < 0) { break; }
        if (uint(d) % stride != 0u) { continue; }
        uint i = uint(d) / stride;
        if (i >= lin) { continue; }
        for (uint c = 0u; c < cinPerG; c++) {
            uint ci = g * cinPerG + c;
            acc += x[xOff + ci * lin + i] * w[wOff + (ci * coutPerG + cl) * k + kk];
        }
    }
    y[yOff + co * lout + o] = acc;
}
