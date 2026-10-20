// A gpuwork test fixture: farMain(inv, params, src, out) reads a run of a storage buffer wherever it lies in it:
// out[i] = src[params[0] + i] for i < params[1], and out[params[1]] = the number of 32-bit words the shader sees
// the whole of src as (what the binding ranges over). The test binds src to a buffer larger than WebGPU's default
// 128 MiB storage binding and reads its far end. A fixture only: gpuwork does not depend on Slang.

[[vk::binding(0, 0)]] StructuredBuffer<uint> params;
[[vk::binding(1, 0)]] StructuredBuffer<uint> src;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> dst;

[shader("compute")]
[numthreads(64, 1, 1)]
void farMain(uint3 id : SV_DispatchThreadID) {
    uint first = params[0];
    uint count = params[1];
    if (id.x == 0) {
        uint words;
        uint stride;
        src.GetDimensions(words, stride);
        dst[count] = words;
    }
    if (id.x >= count) { return; }
    dst[id.x] = src[first + id.x];
}
