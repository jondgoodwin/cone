// Integer dot products, for computefeatures.cone: four bytes packed in each
// 32-bit word, multiplied pairwise and added to an accumulator by one
// instruction (SPIR-V's OpSDot and OpUDot, with the DotProduct capabilities;
// Slang's dot4add_i8packed and dot4add_u8packed). Compiled ahead of time to
// cfdot.spv by tools/shaders/shaders.py.

[[vk::binding(0, 0)]] StructuredBuffer<uint> x;
[[vk::binding(1, 0)]] StructuredBuffer<uint> y;
[[vk::binding(2, 0)]] RWStructuredBuffer<int> result;

// result[i] = i + sum over the four bytes of x[i] and y[i], each a signed byte
[shader("compute")]
[numthreads(64, 1, 1)]
void dotSigned(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    result[i] = dot4add_i8packed(x[i], y[i], int(i));
}

// The same with each an unsigned byte
[shader("compute")]
[numthreads(64, 1, 1)]
void dotUnsigned(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    result[i] = int(dot4add_u8packed(x[i], y[i], i));
}
