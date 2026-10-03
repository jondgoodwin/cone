// A gpuwork test fixture: scrambleMain(inv, words, out), out[i] =
// pcg(words[i] ^ i), 64 invocations a workgroup. The tests chain it after
// noisebake.slang's bake, reading the bake's output where it lies on the GPU.
// A fixture only: gpuwork does not depend on Slang.

import noise;

[[vk::binding(0, 0)]] StructuredBuffer<uint> words;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> scrambled;

[shader("compute")]
[numthreads(64, 1, 1)]
void scrambleMain(uint3 id : SV_DispatchThreadID) {
    uint count;
    uint stride;
    words.GetDimensions(count, stride);
    if (id.x >= count) { return; }
    scrambled[id.x] = pcg(words[id.x] ^ id.x);
}
