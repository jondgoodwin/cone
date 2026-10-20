// A gpuwork test fixture: rangeMain(inv, params, src, dst), the three bindings read as the ranges they are bound to:
// dst[i] = src[i] * params[1] + params[0] for i < params[2]; and dst[params[2]] = the number of 32-bit words the shader
// sees src as, dst[params[2] + 1] = the same for dst (what each binding ranges over); and dst[params[2] + 2] = the
// number of words it sees params as. The test binds each to a range of a bigger buffer, and the parameters to a
// struct of 12 bytes packed in a slab. A fixture only: gpuwork does not depend on Slang.

[[vk::binding(0, 0)]] StructuredBuffer<uint> params;
[[vk::binding(1, 0)]] StructuredBuffer<uint> src;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> dst;

[shader("compute")]
[numthreads(64, 1, 1)]
void rangeMain(uint3 id : SV_DispatchThreadID) {
    uint count = params[2];
    if (id.x == 0) {
        uint words;
        uint stride;
        src.GetDimensions(words, stride);
        dst[count] = words;
        dst.GetDimensions(words, stride);
        dst[count + 1] = words;
        params.GetDimensions(words, stride);
        dst[count + 2] = words;
    }
    if (id.x >= count) { return; }
    dst[id.x] = src[id.x] * params[1] + params[0];
}
