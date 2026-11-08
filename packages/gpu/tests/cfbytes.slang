// 8-bit integers kept in storage buffers and widened in the shader (SPIR-V's
// Int8 and UniformAndStorageBuffer8BitAccess capabilities), for
// computefeatures.cone: the same bytes read as signed and as unsigned, so that
// the sign extension is checked both ways. Compiled ahead of time to
// cfbytes.spv by tools/shaders/shaders.py.

[[vk::binding(0, 0)]] StructuredBuffer<int8_t> signedBytes;
[[vk::binding(1, 0)]] StructuredBuffer<uint8_t> unsignedBytes;     // the same bytes
[[vk::binding(2, 0)]] RWStructuredBuffer<int> result;

// result[i] = 3 * signed(byte i) + unsigned(byte i xor 1)
[shader("compute")]
[numthreads(64, 1, 1)]
void byteMain(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    result[i] = 3 * int(signedBytes[i]) + int(unsignedBytes[i ^ 1]);
}
