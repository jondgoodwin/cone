// Elementwise kernels over two inputs and one output. Bindings (both entry points): args, a, b, y.
//
//   addScaleMain  y[i] = (a[i] + b[i]) * scale for i < n. Args: 0 n, 1 a offset, 2 b offset, 3 y offset,
//                 4 scale (float), 5 groupsX.
//   biasActMain   y[r, c] = act(a[r, c] + b[c]) for r < rows and c < cols (a matrix plus a bias row): act 0 none,
//                 1 leaky ReLU with slope 'slope', 2 the tanh GELU (what ALBERT's "gelu_new" is).
//                 Args: 0 rows, 1 cols, 2 a offset, 3 b offset, 4 y offset, 5 act, 6 slope (float), 7 groupsX.
//
// 256 invocations to a workgroup, one for each element written.
//
// slangc: -fp-mode precise

import speechcommon;

[[vk::binding(0, 0)]] StructuredBuffer<Args> args;
[[vk::binding(1, 0)]] StructuredBuffer<float> a;
[[vk::binding(2, 0)]] StructuredBuffer<float> b;
[[vk::binding(3, 0)]] RWStructuredBuffer<float> y;

[shader("compute")]
[numthreads(256, 1, 1)]
void addScaleMain(uint3 gid : SV_GroupID, uint lid : SV_GroupIndex) {
    Args p = args[0];
    uint i = foldedGroup(gid, p.v[5]) * 256u + lid;
    if (i >= p.v[0]) { return; }
    y[p.v[3] + i] = (a[p.v[1] + i] + b[p.v[2] + i]) * asfloat(p.v[4]);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void biasActMain(uint3 gid : SV_GroupID, uint lid : SV_GroupIndex) {
    Args p = args[0];
    uint i = foldedGroup(gid, p.v[7]) * 256u + lid;
    uint rows = p.v[0];
    uint cols = p.v[1];
    if (i >= rows * cols) { return; }
    uint c = i % cols;
    float v = a[p.v[2] + i] + b[p.v[3] + c];
    uint act = p.v[5];
    if (act == 1u) {
        if (v < 0.0) { v = v * asfloat(p.v[6]); }
    } else if (act == 2u) {
        v = 0.5 * v * (1.0 + tanh(0.79788456080286535588 * (v + 0.044715 * v * v * v)));
    }
    y[p.v[4] + i] = v;
}
