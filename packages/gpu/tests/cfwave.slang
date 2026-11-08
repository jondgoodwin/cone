// Subgroup operations (a GPU's warp or wave), for computefeatures.cone: each
// invocation writes what it saw of its subgroup, seven words, so that the
// test's CPU twin can group the invocations by the subgroup size and lane the
// GPU reports and compute the same answers. SPIR-V's GroupNonUniform
// capabilities (basic, arithmetic, ballot, shuffle); integer arithmetic only,
// so every answer is exact. Compiled ahead of time to cfwave.spv by
// tools/shaders/shaders.py.

[[vk::binding(0, 0)]] StructuredBuffer<uint> values;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> result;

[shader("compute")]
[numthreads(64, 1, 1)]
void waveMain(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    uint v = values[i];
    uint o = i * 7;
    result[o + 0] = WaveGetLaneCount();                      // the subgroup's size
    result[o + 1] = WaveGetLaneIndex();                      // this invocation's place in it
    result[o + 2] = WaveActiveSum(v);                        // arithmetic: sum over the subgroup
    result[o + 3] = WavePrefixSum(v);                        // arithmetic: sum of the lanes before this one
    result[o + 4] = WaveActiveMax(v);                        // arithmetic: maximum
    result[o + 5] = WaveActiveCountBits((v & 1) != 0);       // ballot: how many lanes have an odd value
    result[o + 6] = WaveReadLaneAt(v, 3);                    // shuffle: lane 3's value
}
