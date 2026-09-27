// The volume test's shaders: 3-D storage textures written by compute
// (r32float, r32uint, rgba16float), one read and written in place, and a
// 3-D texture sampled by a render pass. volume.cone says what each checks.
// Compiled ahead of time to volume.spv by tools/shaders/shaders.py.

struct Immediates {
    float base;                    // bytes 0-3: added to every value
};

[[vk::push_constant]]
ConstantBuffer<Immediates> immediates;

// The compute bind group: four storage textures, 16 x 8 x 4, the first three
// written only (WTexture3D: WGSL's 'write' access, which WebGPU requires of
// rgba16float), the fourth read and written (r32float may be)
[[vk::binding(0, 0)]] [format("r32f")] WTexture3D<float> values;
[[vk::binding(1, 0)]] [format("r32ui")] WTexture3D<uint> hashes;
[[vk::binding(2, 0)]] [format("rgba16f")] WTexture3D<float4> colors;
[[vk::binding(3, 0)]] [format("r32f")] RWTexture3D<float> doubled;

// Each texel's value, hash and color from its coordinates
[shader("compute")]
[numthreads(4, 4, 4)]
void fillMain(uint3 id : SV_DispatchThreadID) {
    values.Store(id, float(id.x + 16u * id.y + 128u * id.z) + immediates.base);
    hashes.Store(id, (id.x * 73856093u) ^ (id.y * 19349663u) ^ (id.z * 83492791u));
    colors.Store(id, float4(float(id.x), float(id.y), float(id.z), 0.5));
}

// Each texel of 'doubled' read, and written back twice itself plus 1
[shader("compute")]
[numthreads(4, 4, 4)]
void doubleMain(uint3 id : SV_DispatchThreadID) {
    doubled[id] = doubled[id] * 2.0 + 1.0;
}

// The render bind group: the values, sampled (r32float is not filtered)
[[vk::binding(4, 0)]] Texture3D<float> volume;
[[vk::binding(5, 0)]] SamplerState nearest;

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

// Pixel (x, y) of the 16 x 8 target reads texel (x, y, 1): x + 16 y + 128,
// less 128, as a fraction of 255
[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float3 uvw = float3(input.position.x / 16.0, input.position.y / 8.0, 1.5 / 4.0);
    float v = volume.SampleLevel(nearest, uvw, 0.0);
    return float4((v - 128.0) / 255.0, 0.0, 0.0, 1.0);
}
