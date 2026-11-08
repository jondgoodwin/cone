// Y = X W^T for a batch of input rows, W a matrix of F32 or F16 weights: Y[b][r] = sum over c of W[r][c] X[b][c]. llmkernels'
// matmul serves any quantised type and reaches about 700 GFLOP/s; this one is for the encoder's matrices, which are only F32
// or F16, and for a batch of tens of rows or more reaches 2 to 2.8 TFLOP/s (examples/bench.cone). A 64 x 64 tile of Y to a
// workgroup. Each output is added in column order, one product at a time, so the sums are llmkernels' matmul's, bit for bit.
// Bindings: params (by value, llmkernels' MatmulParams), the weights' words, X, Y.
//
// THE LOADS. A workgroup loads a step's weights and inputs into workgroup memory before it computes them. The loads are
// straight-line code with no branch round a load (an index past the matrix's edge is clamped to a valid one and the value
// zeroed afterwards), so a thread's loads can all be in flight together.
//
// slangc: -fp-mode precise

import llmcommon;

struct MatmulParams {
    uint rows;          // W's rows: Y's columns
    uint cols;          // W's columns: X's columns
    uint batch;         // X's and Y's rows
    uint rowBytes;      // bytes from one weight row to the next
    uint wByteOffset;   // byte offset of weight row 0 in the weights buffer
    uint xOffset;       // X[xOffset ..] is the input
    uint yOffset;       // Y[yOffset ..] is the output
    uint qtype;         // QTYPE_F32 or QTYPE_F16
};

[[vk::binding(0, 0)]] StructuredBuffer<MatmulParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<uint> weights;
[[vk::binding(2, 0)]] StructuredBuffer<float> X;
[[vk::binding(3, 0)]] RWStructuredBuffer<float> Y;

// The weight at row wr, column k of the matrix, or 0 where (wr, k) is outside it; the load is made either way
float loadWeight(MatmulParams p, uint wr, uint k) {
    bool valid = (wr < p.rows) && (k < p.cols);
    uint wrc = min(wr, p.rows - 1u);
    uint kc = min(k, p.cols - 1u);
    bool half16 = (p.qtype == QTYPE_F16);
    uint byteOff = p.wByteOffset + wrc * p.rowBytes + kc * (half16 ? 2u : 4u);
    uint word = weights[byteOff >> 2u];
    float v = half16 ? halfBitsToFloat((word >> ((byteOff & 2u) * 8u)) & 0xFFFFu) : asfloat(word);
    return valid ? v : 0.0;
}

// The input at batch row xb, column k, or 0 outside
float loadInput(MatmulParams p, uint xb, uint k) {
    bool valid = (xb < p.batch) && (k < p.cols);
    uint xbc = min(xb, p.batch - 1u);
    uint kc = min(k, p.cols - 1u);
    float v = X[p.xOffset + xbc * p.cols + kc];
    return valid ? v : 0.0;
}

// A workgroup of 16 x 16 invocations makes 64 weight rows by 64 batch rows; each invocation makes 16 outputs (its weight rows
// tx, tx + 16, tx + 32, tx + 48 and batch rows ty, ty + 16, ty + 32, ty + 48, so that the workgroup's reads of workgroup
// memory and its writes of Y are contiguous across tx). The shared dimension is walked 16 columns at a time.

// 65 wide, not 64: the loads write 16 consecutive k of a row at once, which would hit one memory bank 16 times
groupshared float tileW[32][65];   // [k in tile][weight row in tile]
groupshared float tileX[32][65];   // [k in tile][batch row in tile]

[shader("compute")]
[numthreads(16, 16, 1)]
void matmulTiledMain(uint3 gid : SV_GroupID, uint3 lid : SV_GroupThreadID) {
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
    for (uint k0 = 0u; k0 < p.cols; k0 += 32u) {
        // 2048 weights and 2048 inputs, eight of each for every invocation; consecutive invocations take consecutive columns
        float wl[8];
        float xl[8];
        for (uint s = 0u; s < 8u; s++) {
            uint idx = tid + s * 256u;
            uint kk = idx & 31u;
            uint rr = idx >> 5u;
            wl[s] = loadWeight(p, r0 + rr, k0 + kk);
            xl[s] = loadInput(p, b0 + rr, k0 + kk);
        }
        for (uint s = 0u; s < 8u; s++) {
            uint idx = tid + s * 256u;
            tileW[idx & 31u][idx >> 5u] = wl[s];
            tileX[idx & 31u][idx >> 5u] = xl[s];
        }
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
