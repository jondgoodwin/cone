// The matmul of speechmatmul.slang (Y = X W^T, W F16, a 64 x 64 tile of Y to a workgroup of 16 x 16 invocations, each output
// added in column order one product at a time: the same sums, bit for bit) with the loads widened: a weight row is read 8 halves
// (16 bytes, one uint4) at a time and an input row 4 floats (one float4) at a time, so a step of 32 columns is one weight load
// and two input loads an invocation, where the scalar form makes eight and eight. It needs what the wide loads need, which
// speechkernels.cone checks before it chooses this form: F16 weights, a column count that is a multiple of 32, the matrix
// at a byte offset that is a multiple of 16, the input at an element offset that is a multiple of 4. Bindings: params (by
// value, llmkernels' MatmulParams), the weights as uint4s, X as float4s, Y.
//
// slangc: -fp-mode precise

import llmcommon;

struct MatmulParams {
    uint rows;          // W's rows: Y's columns
    uint cols;          // W's columns: X's columns (a multiple of 32)
    uint batch;         // X's and Y's rows
    uint rowBytes;      // bytes from one weight row to the next
    uint wByteOffset;   // byte offset of weight row 0 in the weights buffer (a multiple of 16)
    uint xOffset;       // X[xOffset ..] is the input (a multiple of 4)
    uint yOffset;       // Y[yOffset ..] is the output
    uint qtype;         // QTYPE_F16
};

[[vk::binding(0, 0)]] StructuredBuffer<MatmulParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<uint4> weights;
[[vk::binding(2, 0)]] StructuredBuffer<float4> X;
[[vk::binding(3, 0)]] RWStructuredBuffer<float> Y;

// the two floats in the halves of a 32-bit word
float2 halves(uint w) { return float2(halfBitsToFloat(w & 0xFFFFu), halfBitsToFloat(w >> 16u)); }

groupshared float tileW[32][65];   // [k in tile][weight row in tile]
groupshared float tileX[32][65];   // [k in tile][batch row in tile]

[shader("compute")]
[numthreads(16, 16, 1)]
void matmulVecMain(uint3 gid : SV_GroupID, uint3 lid : SV_GroupThreadID) {
    MatmulParams p = params[0];
    uint r0 = gid.x * 64u;
    uint b0 = gid.y * 64u;
    uint tx = lid.x;
    uint ty = lid.y;
    uint tid = ty * 16u + tx;
    float acc[4][4];
    for (uint i = 0u; i < 4u; i++) {
        for (uint j = 0u; j < 4u; j++) { acc[i][j] = 0.0; }
    }
    // this invocation's weight row and column group within a step: row tid / 4, halves 8 (tid % 4) .. 8 (tid % 4) + 7
    uint wrow = tid >> 2u;
    uint wcg = (tid & 3u) * 8u;
    bool wValid = (r0 + wrow) < p.rows;
    uint wRowU4 = (p.wByteOffset + min(r0 + wrow, p.rows - 1u) * p.rowBytes) >> 4u;      // uint4 index of the row's start
    for (uint k0 = 0u; k0 < p.cols; k0 += 32u) {
        uint4 wq = weights[wRowU4 + ((k0 + wcg) >> 3u)];
        float4 xq0;
        float4 xq1;
        uint xr0 = tid >> 3u;                  // input rows tid / 8 and tid / 8 + 32, columns 4 (tid % 8) .. +3
        uint xr1 = xr0 + 32u;
        uint xc = (tid & 7u) * 4u;
        bool xv0 = (b0 + xr0) < p.batch;
        bool xv1 = (b0 + xr1) < p.batch;
        xq0 = X[((p.xOffset + min(b0 + xr0, p.batch - 1u) * p.cols + k0 + xc) >> 2u)];
        xq1 = X[((p.xOffset + min(b0 + xr1, p.batch - 1u) * p.cols + k0 + xc) >> 2u)];
        float2 h0 = halves(wq.x);
        float2 h1 = halves(wq.y);
        float2 h2 = halves(wq.z);
        float2 h3 = halves(wq.w);
        if (!wValid) { h0 = float2(0.0, 0.0); h1 = h0; h2 = h0; h3 = h0; }
        if (!xv0) { xq0 = float4(0.0, 0.0, 0.0, 0.0); }
        if (!xv1) { xq1 = float4(0.0, 0.0, 0.0, 0.0); }
        tileW[wcg + 0u][wrow] = h0.x; tileW[wcg + 1u][wrow] = h0.y;
        tileW[wcg + 2u][wrow] = h1.x; tileW[wcg + 3u][wrow] = h1.y;
        tileW[wcg + 4u][wrow] = h2.x; tileW[wcg + 5u][wrow] = h2.y;
        tileW[wcg + 6u][wrow] = h3.x; tileW[wcg + 7u][wrow] = h3.y;
        tileX[xc + 0u][xr0] = xq0.x; tileX[xc + 1u][xr0] = xq0.y; tileX[xc + 2u][xr0] = xq0.z; tileX[xc + 3u][xr0] = xq0.w;
        tileX[xc + 0u][xr1] = xq1.x; tileX[xc + 1u][xr1] = xq1.y; tileX[xc + 2u][xr1] = xq1.z; tileX[xc + 3u][xr1] = xq1.w;
        GroupMemoryBarrierWithGroupSync();
        for (uint kk = 0u; kk < 32u; kk++) {
            float wcol[4];
            float xcol[4];
            for (uint i = 0u; i < 4u; i++) {
                wcol[i] = tileW[kk][tx + 16u * i];
                xcol[i] = tileX[kk][ty + 16u * i];
            }
            for (uint j = 0u; j < 4u; j++) {
                for (uint i = 0u; i < 4u; i++) { acc[j][i] += wcol[i] * xcol[j]; }
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
    for (uint j = 0u; j < 4u; j++) {
        uint b = b0 + ty + 16u * j;
        if (b < p.batch) {
            for (uint i = 0u; i < 4u; i++) {
                uint r = r0 + tx + 16u * i;
                if (r < p.rows) { Y[p.yOffset + b * p.rows + r] = acc[j][i]; }
            }
        }
    }
}
