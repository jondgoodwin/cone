// A 2-D convolution with a square kernel, stride, padding, groups and an optional ReLU, for the front of the
// encoder (the 8x subsampling: a 3x3 convolution of one channel to 256, then twice a 3x3 depthwise convolution at
// stride 2 and a 1x1 one). The input is cin channels of h x w floats, channel-major ([ci][y][x]); the weights
// [cout][cin / groups][k][k] at float offset wOff of 'small', the biases cout floats at bOff.
// Output size: oh = (h + 2 pad - k) / stride + 1, likewise ow. The output is written [co][oy][ox] (outMode 0), or
// time-major with channels and columns flattened, [oy][co * ow + ox] (outMode 1), which is the layout the encoder's
// linear layer takes. One invocation for each output, 256 a workgroup (folds past 65535 workgroups).
// Bindings: params (by value), small, x, y.
//
// slangc: -fp-mode precise

import llmcommon;

struct ConvParams {
    uint cin;
    uint cout;
    uint groups;
    uint h;
    uint w;
    uint oh;
    uint ow;
    uint k;
    uint stride;
    uint pad;
    uint wOff;
    uint bOff;
    uint relu;
    uint outMode;
    uint total;         // cout * oh * ow
    uint groupsX;
};

[[vk::binding(0, 0)]] StructuredBuffer<ConvParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<float> small;
[[vk::binding(2, 0)]] StructuredBuffer<float> x;
[[vk::binding(3, 0)]] RWStructuredBuffer<float> y;

[shader("compute")]
[numthreads(256, 1, 1)]
void conv2dMain(uint3 gid : SV_GroupID, uint lid : SV_GroupIndex) {
    ConvParams p = params[0];
    uint i = foldedGroup(gid, p.groupsX) * 256u + lid;
    if (i >= p.total) { return; }
    uint ox = i % p.ow;
    uint rest = i / p.ow;
    uint oy = rest % p.oh;
    uint co = rest / p.oh;
    uint cpg = p.cin / p.groups;          // input channels of a group
    uint opg = p.cout / p.groups;         // output channels of a group
    uint g = co / opg;
    float acc = small[p.bOff + co];
    for (uint ci = 0u; ci < cpg; ci++) {
        uint chan = g * cpg + ci;
        for (uint ky = 0u; ky < p.k; ky++) {
            int iy = int(oy * p.stride + ky) - int(p.pad);
            if (iy < 0 || iy >= int(p.h)) { continue; }
            for (uint kx = 0u; kx < p.k; kx++) {
                int ix = int(ox * p.stride + kx) - int(p.pad);
                if (ix < 0 || ix >= int(p.w)) { continue; }
                acc += small[p.wOff + ((co * cpg + ci) * p.k + ky) * p.k + kx] * x[(chan * p.h + uint(iy)) * p.w + uint(ix)];
            }
        }
    }
    if (p.relu != 0u) { acc = max(acc, 0.0); }
    uint o = (p.outMode == 0u) ? i : (oy * p.cout * p.ow + co * p.ow + ox);
    y[o] = acc;
}
