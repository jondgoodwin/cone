// The bake test's compute shader: noise.slang's fBm baked into a 64^3 3-D
// texture, a voxel an invocation, with its hash and its derivative. bake.cone
// computes the same with the Cone package and compares, voxel by voxel.
//
// slangc: -fp-mode precise

import noise;

struct Bake {
    float ox;                      // bytes 0-11: the voxel (0, 0, 0)'s point
    float oy;
    float oz;
    float step;                    // bytes 12-15: from one voxel to the next
    uint seed;                     // bytes 16-19
    uint size;                     // bytes 20-23: voxels each way
};

[[vk::push_constant]]
ConstantBuffer<Bake> bake;

[[vk::binding(0, 0)]] [format("r32f")] WTexture3D<float> values;       // the fBm
[[vk::binding(1, 0)]] [format("r32ui")] WTexture3D<uint> hashes;       // the voxel's lattice hash
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> derivatives;          // (d, value), x fastest

// Voxel 'id' at the point origin + id step, each coordinate one multiply and
// one add, in the order bake.cone writes them: fBm of 4 octaves, lacunarity
// 2, gain 0.5
[shader("compute")]
[numthreads(4, 4, 4)]
void bakeMain(uint3 id : SV_DispatchThreadID) {
    float3 p = float3(bake.ox + float(id.x) * bake.step, bake.oy + float(id.y) * bake.step,
        bake.oz + float(id.z) * bake.step);
    Noise3 n = fbm3(p, bake.seed, 4, 2.0, 0.5);
    values.Store(id, n.value);
    hashes.Store(id, hash3(int(id.x), int(id.y), int(id.z), bake.seed).x);
    derivatives[id.x + bake.size * (id.y + bake.size * id.z)] = float4(n.d, n.value);
}
