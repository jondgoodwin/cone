// The volume demo's shaders: noise.slang's fBm baked by compute into a 64^3
// r32float 3-D texture (bakeMain), then drawn each frame (vertexMain,
// fragmentMain): a slice of it, 4 x 4 pixels a voxel, and a turning sphere
// textured through it. volume.cone says what is checked.
//
// slangc: -fp-mode precise

import noise;

// The immediates, one block for both pipelines: the bake reads the first
// 24 bytes, the drawing the rest
struct Immediates {
    float ox;                      // bytes 0-15: the voxel (0, 0, 0)'s point, and the step
    float oy;
    float oz;
    float step;
    uint seed;                     // bytes 16-19
    uint size;                     // bytes 20-23: voxels each way
    float turn;                    // bytes 24-27: the sphere's turn, in radians
    uint slice;                    // bytes 28-31: the slice shown
    float width;                   // bytes 32-39: the window's size in pixels
    float height;
};

[[vk::push_constant]]
ConstantBuffer<Immediates> immediates;

// ---- The bake ----

[[vk::binding(0, 0)]] [format("r32f")] WTexture3D<float> baked;

// Voxel 'id': fBm of 4 octaves at origin + id step, as the bake test has it
[shader("compute")]
[numthreads(4, 4, 4)]
void bakeMain(uint3 id : SV_DispatchThreadID) {
    float3 p = float3(immediates.ox + float(id.x) * immediates.step, immediates.oy + float(id.y) * immediates.step,
        immediates.oz + float(id.z) * immediates.step);
    baked.Store(id, fbm3(p, immediates.seed, 4, 2.0, 0.5).value);
}

// ---- The drawing ----

// The baked texture, read by 'Load': r32float is not filtered (WebGPU's
// rule), so the sphere interpolates its eight neighbours itself
[[vk::binding(1, 0)]] Texture3D<float> volume;

struct VertexOutput {
    float4 position : SV_Position;
};

[shader("vertex")]
VertexOutput vertexMain(uint id : SV_VulkanVertexID) {
    VertexOutput output;
    float2 corner = float2(float((id << 1u) & 2u), float(id & 2u));
    output.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

// The volume at 'p', from 0 to size - 1 each way, trilinearly
float trilinear(float3 p) {
    int n = int(immediates.size) - 1;
    float3 f = floor(p);
    float3 t = p - f;
    int3 a = clamp(int3(f), int3(0, 0, 0), int3(n, n, n));
    int3 b = min(a + int3(1, 1, 1), int3(n, n, n));
    float c000 = volume.Load(int4(a.x, a.y, a.z, 0));
    float c100 = volume.Load(int4(b.x, a.y, a.z, 0));
    float c010 = volume.Load(int4(a.x, b.y, a.z, 0));
    float c110 = volume.Load(int4(b.x, b.y, a.z, 0));
    float c001 = volume.Load(int4(a.x, a.y, b.z, 0));
    float c101 = volume.Load(int4(b.x, a.y, b.z, 0));
    float c011 = volume.Load(int4(a.x, b.y, b.z, 0));
    float c111 = volume.Load(int4(b.x, b.y, b.z, 0));
    float x00 = lerp(c000, c100, t.x);
    float x10 = lerp(c010, c110, t.x);
    float x01 = lerp(c001, c101, t.x);
    float x11 = lerp(c011, c111, t.x);
    return lerp(lerp(x00, x10, t.y), lerp(x01, x11, t.y), t.z);
}

// A value of the noise as a color: the slice's grey, from -0.5 to 0.5 (most
// of fBm's values)
float grey(float v) { return saturate(v + 0.5); }

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    uint2 px = uint2(input.position.xy);
    uint side = 4u * immediates.size;

    // The slice, 4 x 4 pixels a voxel, its top left at (16, 16)
    if (px.x >= 16u && px.x < 16u + side && px.y >= 16u && px.y < 16u + side) {
        uint3 voxel = uint3((px.x - 16u) / 4u, (px.y - 16u) / 4u, immediates.slice);
        float g = grey(volume.Load(int4(int3(voxel), 0)));
        return float4(g, g, g, 1.0);
    }

    // The sphere, in the rest of the window: its surface point's direction,
    // turned about y, reads the volume at that point of the ball inside it
    float left = float(side) + 32.0;
    float2 centre = float2((left + immediates.width) * 0.5, immediates.height * 0.5);
    float radius = 0.42 * min(immediates.width - left, immediates.height);
    float2 q = (input.position.xy - centre) / radius;
    float r2 = dot(q, q);
    if (r2 >= 1.0 || radius <= 0.0) {
        return float4(0.02, 0.025, 0.05, 1.0);
    }
    float3 n = float3(q.x, -q.y, sqrt(1.0 - r2));
    float c = cos(immediates.turn);
    float s = sin(immediates.turn);
    float3 turned = float3(c * n.x + s * n.z, n.y, c * n.z - s * n.x);
    float3 p = (turned * 0.48 + 0.5) * float(immediates.size - 1u);
    float v = trilinear(p);
    float3 warm = lerp(float3(0.05, 0.08, 0.2), float3(1.0, 0.62, 0.25), grey(v));
    float light = max(dot(n, normalize(float3(-0.5, 0.6, 0.65))), 0.0) * 0.85 + 0.15;
    return float4(warm * light, 1.0);
}
