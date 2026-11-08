// What a quantised language model's matrix-vector product does with these
// features, for computefeatures.cone: the weights are 8-bit integers in
// blocks of 32 (packed four to a word) with a half-precision scale a block,
// the activations the same with a float scale a block (llama.cpp's Q8_0 with
// activations quantised to Q8_1), and each block's integer dot product is done
// with the dot product instruction and scaled. y[row] is the sum over the
// row's blocks of  weightScale * activationScale * (sum of the 32 products).
// Two kernels: one invocation a row, adding the blocks in order (the CPU twin
// does the same sums in the same order, so the answers are bit for bit the
// same); and one subgroup a row, each lane adding blocks lane, lane + 32, ...,
// then WaveActiveSum (a sum in another order, so the twin compares within a
// tolerance). Compiled ahead of time to cfquant.spv by
// tools/shaders/shaders.py.
// slangc: -fp-mode precise

static const uint BLOCKS = 64;      // blocks a row (2048 weights)
static const uint WORDS = 8;        // packed words a block

[[vk::binding(0, 0)]] StructuredBuffer<uint> weights;         // rows * BLOCKS * WORDS
[[vk::binding(1, 0)]] StructuredBuffer<half> weightScales;    // rows * BLOCKS
[[vk::binding(2, 0)]] StructuredBuffer<uint> activations;     // BLOCKS * WORDS
[[vk::binding(3, 0)]] StructuredBuffer<float> activationScales;   // BLOCKS
[[vk::binding(4, 0)]] RWStructuredBuffer<float> result;       // rows

int blockDot(uint row, uint block) {
    int s = 0;
    for (uint j = 0; j < WORDS; j++) {
        s = dot4add_i8packed(weights[(row * BLOCKS + block) * WORDS + j], activations[block * WORDS + j], s);
    }
    return s;
}

// One invocation a row (the test's rows are a multiple of 64)
[shader("compute")]
[numthreads(64, 1, 1)]
void q8Rows(uint3 id : SV_DispatchThreadID) {
    uint row = id.x;
    float acc = 0.0;
    for (uint b = 0; b < BLOCKS; b++) {
        float scale = float(weightScales[row * BLOCKS + b]) * activationScales[b];
        acc = acc + float(blockDot(row, b)) * scale;
    }
    result[row] = acc;
}

// One workgroup of 32 invocations a row: the subgroup is at least 32 wide, or the test does not run it
[shader("compute")]
[numthreads(32, 1, 1)]
void q8Subgroup(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID) {
    uint row = group.x;
    float acc = 0.0;
    for (uint b = local.x; b < BLOCKS; b += 32) {
        float scale = float(weightScales[row * BLOCKS + b]) * activationScales[b];
        acc = acc + float(blockDot(row, b)) * scale;
    }
    float total = WaveActiveSum(acc);
    if (local.x == 0) {
        result[row] = total;
    }
}
