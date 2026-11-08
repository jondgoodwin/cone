// Small elementwise and copying kernels over one input and one output. Bindings (all entry points): args, x, y.
//
//   leakyMain    y[i] = x[i], times 'slope' where negative, for i < n.
//                Args: 0 n, 1 x offset, 2 y offset, 3 slope (float), 4 groupsX.
//   copy2dMain   y[dOff + r * dRowStride + c * dColStride] = x[sOff + r * sRowStride + c * sColStride] for r < rows and
//                c < cols: a copy of a block, a channel concatenation (a destination offset), a transpose (strides).
//                Args: 0 rows, 1 cols, 2 sOff, 3 sRowStride, 4 sColStride, 5 dOff, 6 dRowStride, 7 dColStride,
//                8 groupsX.
//   repeat2Main  y[c, 2 i] = y[c, 2 i + 1] = x[c, i]: nearest-neighbour upsampling by 2 of each channel.
//                Args: 0 channels, 1 length (of x), 2 x offset, 3 y offset, 4 groupsX.
//
// 256 invocations to a workgroup, one for each element written.
//
// slangc: -fp-mode precise

import speechcommon;

[[vk::binding(0, 0)]] StructuredBuffer<Args> args;
[[vk::binding(1, 0)]] StructuredBuffer<float> x;
[[vk::binding(2, 0)]] RWStructuredBuffer<float> y;

[shader("compute")]
[numthreads(256, 1, 1)]
void leakyMain(uint3 gid : SV_GroupID, uint lid : SV_GroupIndex) {
    Args p = args[0];
    uint i = foldedGroup(gid, p.v[4]) * 256u + lid;
    if (i >= p.v[0]) { return; }
    float v = x[p.v[1] + i];
    if (v < 0.0) { v = v * asfloat(p.v[3]); }
    y[p.v[2] + i] = v;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void copy2dMain(uint3 gid : SV_GroupID, uint lid : SV_GroupIndex) {
    Args p = args[0];
    uint i = foldedGroup(gid, p.v[8]) * 256u + lid;
    uint rows = p.v[0];
    uint cols = p.v[1];
    if (i >= rows * cols) { return; }
    uint r = i / cols;
    uint c = i % cols;
    y[p.v[5] + r * p.v[6] + c * p.v[7]] = x[p.v[2] + r * p.v[3] + c * p.v[4]];
}

[shader("compute")]
[numthreads(256, 1, 1)]
void repeat2Main(uint3 gid : SV_GroupID, uint lid : SV_GroupIndex) {
    Args p = args[0];
    uint i = foldedGroup(gid, p.v[4]) * 256u + lid;
    uint channels = p.v[0];
    uint len = p.v[1];
    if (i >= channels * len * 2u) { return; }
    uint c = i / (2u * len);
    uint o = i % (2u * len);
    y[p.v[3] + i] = x[p.v[2] + c * len + o / 2u];
}
