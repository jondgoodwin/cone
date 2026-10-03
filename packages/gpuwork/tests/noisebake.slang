// A gpuwork test fixture: noise.slang's fBm baked into storage buffers, in
// the shape Cone's compute entry points will have (each slice one storage
// buffer, binding = parameter position, group 0; a by-value struct a small
// read-only storage buffer; no push constants). gpuwork itself does not
// depend on Slang: this file only stands in until kernels are written in
// Cone. Its CPU twin is in the tests, over noise's Cone functions.
//
// bakeMain(inv, bake, out, hashes): a voxel an invocation, 4 x 4 x 4 a
// workgroup: out[4v .. 4v+3] the fBm's value and derivative at voxel v,
// hashes[v] its lattice hash.
//
// slangc: -fp-mode precise

import noise;

struct Bake {
    float ox;                      // bytes 0-11: voxel (0, 0, 0)'s point
    float oy;
    float oz;
    float step;                    // bytes 12-15: from one voxel to the next
    uint seed;                     // bytes 16-19
    uint size;                     // bytes 20-23: voxels each way
};

[[vk::binding(0, 0)]] StructuredBuffer<Bake> bake;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> out;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> hashes;

// Voxel 'id' at the point origin + id step, each coordinate one multiply and
// one add, in the order the twin writes them: fBm of 4 octaves, lacunarity
// 2, gain 0.5
[shader("compute")]
[numthreads(4, 4, 4)]
void bakeMain(uint3 id : SV_DispatchThreadID) {
    Bake b = bake[0];
    if (id.x >= b.size || id.y >= b.size || id.z >= b.size) { return; }
    float3 p = float3(b.ox + float(id.x) * b.step, b.oy + float(id.y) * b.step, b.oz + float(id.z) * b.step);
    Noise3 n = fbm3(p, b.seed, 4, 2.0, 0.5);
    uint v = id.x + b.size * (id.y + b.size * id.z);
    out[4 * v] = n.value;
    out[4 * v + 1] = n.d.x;
    out[4 * v + 2] = n.d.y;
    out[4 * v + 3] = n.d.z;
    hashes[v] = hash3(int(id.x), int(id.y), int(id.z), b.seed).x;
}
