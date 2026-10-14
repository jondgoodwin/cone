// Reads words from deep inside a storage buffer larger than 2 GiB, for
// bigbuffers.cone: probes[i] is a word index into 'big' (and so a byte offset
// of four times it, past 2^31 for the later ones), result[i] the word there.
// Compiled ahead of time to bigbuffer.spv by tools/shaders/shaders.py.

[[vk::binding(0, 0)]] StructuredBuffer<uint> big;
[[vk::binding(1, 0)]] StructuredBuffer<uint> probes;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> result;

[shader("compute")]
[numthreads(64, 1, 1)]
void probeMain(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    result[i] = big[probes[i]];
}
