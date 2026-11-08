// Y = X W^T for a batch of input vectors: Y[b][r] = sum over c of W[r][c] X[b][c],
// W a weight matrix of any of the five types, dequantised as it is read.
// X is batch rows of 'cols' floats, Y is batch rows of 'rows' floats (one
// row for each token, as an activation is kept). A 16 x 16 tile of Y for
// each workgroup of 256 invocations: the workgroup walks the shared
// dimension 16 columns at a time, each invocation loading one weight and one
// input into workgroup memory (zero past the end of cols), then adding 16
// products. Each output is therefore added in column order, one at a time,
// the order the CPU twin uses. Bindings: params (by value), the weights'
// words, X, Y.
//
// slangc: -fp-mode precise

import llmcommon;

struct MatmulParams {
    uint rows;          // W's rows: Y's columns
    uint cols;          // W's columns: X's columns, a whole number of blocks of the type
    uint batch;         // X's and Y's rows
    uint rowBytes;      // bytes from one weight row to the next
    uint wByteOffset;   // byte offset of weight row 0 in the weights buffer
    uint xOffset;       // X[xOffset ..] is the input
    uint yOffset;       // Y[yOffset ..] is the output
    uint qtype;         // QTYPE_*
};

[[vk::binding(0, 0)]] StructuredBuffer<MatmulParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<uint> weights;
[[vk::binding(2, 0)]] StructuredBuffer<float> X;
[[vk::binding(3, 0)]] RWStructuredBuffer<float> Y;

groupshared float tileW[16][16];   // [weight row in tile][k in tile]
groupshared float tileX[16][16];   // [batch row in tile][k in tile]

[shader("compute")]
[numthreads(16, 16, 1)]
void matmulMain(uint3 gid : SV_GroupID, uint3 lid : SV_GroupThreadID) {
    MatmulParams p = params[0];
    uint r0 = gid.x * 16u;
    uint b0 = gid.y * 16u;
    uint lx = lid.x;
    uint ly = lid.y;
    float acc = 0.0;
    for (uint k0 = 0u; k0 < p.cols; k0 += 16u) {
        uint k = k0 + lx;
        uint wr = r0 + ly;
        uint xb = b0 + ly;
        float wv = 0.0;
        float xv = 0.0;
        if (k < p.cols) {
            if (wr < p.rows) { wv = weightAt(weights, p.qtype, p.wByteOffset + wr * p.rowBytes, k); }
            if (xb < p.batch) { xv = X[p.xOffset + xb * p.cols + k]; }
        }
        tileW[ly][lx] = wv;
        tileX[ly][lx] = xv;
        GroupMemoryBarrierWithGroupSync();
        // this invocation's output: weight row r0 + lx, batch row b0 + ly
        for (uint kk = 0u; kk < 16u; kk++) {
            acc += tileW[lx][kk] * tileX[ly][kk];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    uint r = r0 + lx;
    uint b = b0 + ly;
    if (r < p.rows && b < p.batch) { Y[p.yOffset + b * p.rows + r] = acc; }
}
